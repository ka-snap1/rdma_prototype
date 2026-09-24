use rdma_prototype::transport::{Connection, Event, Fabric, Info, Listener, Result, finish};
use std::thread;
use std::time::{Duration, Instant};

#[derive(Debug, PartialEq)]
enum Phase {
    Hello,
    Reply,
    Ack,
    Done,
}

/* Application state is independent from resource ownership and FFI polling. */
struct Exchange {
    phase: Phase,
    connected: bool,
    sent: bool,
    peer_closed: bool,
}

impl Exchange {
    fn new() -> Self {
        Self {
            phase: Phase::Hello,
            connected: false,
            sent: false,
            peer_closed: false,
        }
    }

    fn on_event(&mut self, event: Event) -> Result<()> {
        match event {
            Event::Connected => self.connected = true,
            Event::Sent => self.sent = true,
            Event::PeerClosed => self.peer_closed = true,
            Event::Received(message) => match self.phase {
                Phase::Hello if message == b"hello" => self.phase = Phase::Reply,
                Phase::Ack if message == b"ack" => self.phase = Phase::Done,
                _ => {
                    return Err(format!(
                        "unexpected message in {:?}: {:?}",
                        self.phase, message
                    ));
                }
            },
        }
        Ok(())
    }

    fn complete(&self) -> bool {
        self.phase == Phase::Done && self.sent
    }
}

fn serve(conn: &mut Connection<'_>, timeout: Duration) -> Result<()> {
    let mut exchange = Exchange::new();
    let deadline = Instant::now() + timeout;
    while Instant::now() < deadline {
        for event in conn.poll()? {
            println!("event: {event:?}");
            exchange.on_event(event)?;
        }
        if exchange.complete() {
            return Ok(());
        }
        if exchange.connected && !exchange.peer_closed && exchange.phase == Phase::Reply {
            // Receive ACK before sending world. Retry EAGAIN in the next iteration.
            if !conn.receive_pending() && !conn.post_recv()? {
                continue;
            }
            if conn.send(b"world")? {
                exchange.phase = Phase::Ack;
            }
        }
        // A shutdown event can precede the last CQ observation. Drain until the
        // exchange finishes or the deadline expires, but do not submit new work.
        thread::sleep(Duration::from_millis(1));
    }
    Err(format!(
        "exchange timeout in {:?}; peer_closed={}",
        exchange.phase, exchange.peer_closed
    ))
}

fn run(ip: &str, port: &str, domain: Option<&str>, timeout: Duration) -> Result<()> {
    let info = Info::local(ip, port, domain)?;
    let mut fabric = Fabric::open(&info)?;
    let result = (|| {
        let mut listener = Listener::bind(&fabric, &info)?;
        println!(
            "listening on {ip}:{port}; waiting up to {} seconds",
            timeout.as_secs()
        );
        let result = (|| {
            let mut request = listener.wait_request(timeout)?;
            let mut conn = Connection::from_request(&mut request, timeout)?;
            // Accept succeeded; release request metadata while keeping the EP alive.
            drop(request);
            let result = serve(&mut conn, timeout);
            finish(result, conn.close())
        })();
        finish(result, listener.close())
    })();
    finish(result, fabric.close())
}

fn main() -> Result<()> {
    let args: Vec<String> = std::env::args().skip(1).collect();
    if args.is_empty() || args.len() > 4 {
        return Err(
            "usage: server <local-rxe-ip> [port=7471] [local-domain] [timeout-seconds=30]".into(),
        );
    }
    let seconds = args
        .get(3)
        .map_or(Ok(30_u64), |s| s.parse::<u64>())
        .map_err(|_| "invalid timeout")?;
    if !(1..=3600).contains(&seconds) {
        return Err("timeout must be between 1 and 3600 seconds".into());
    }
    let (major, minor) = rdma_prototype::fabric::runtime_fabric_version();
    println!("libfabric runtime version: {major}.{minor}");
    run(
        &args[0],
        args.get(1).map_or("7471", String::as_str),
        args.get(2).map(String::as_str),
        Duration::from_secs(seconds),
    )?;
    println!("hello/world/ack completed; resources closed");
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn ack_and_tx_completion_can_arrive_in_either_order() {
        for ack_first in [true, false] {
            let mut exchange = Exchange::new();
            // Receive completion may be observed before the connected EQ event.
            exchange
                .on_event(Event::Received(b"hello".to_vec()))
                .unwrap();
            exchange.on_event(Event::Connected).unwrap();
            assert_eq!(exchange.phase, Phase::Reply);
            exchange.phase = Phase::Ack; // world was successfully submitted
            if ack_first {
                exchange.on_event(Event::Received(b"ack".to_vec())).unwrap();
                assert!(!exchange.complete());
                exchange.on_event(Event::Sent).unwrap();
            } else {
                exchange.on_event(Event::Sent).unwrap();
                assert!(!exchange.complete());
                exchange.on_event(Event::Received(b"ack".to_vec())).unwrap();
            }
            assert!(exchange.complete());
        }
    }

    #[test]
    fn invalid_message_or_disconnect_does_not_complete_exchange() {
        let mut exchange = Exchange::new();
        assert!(exchange.on_event(Event::Received(b"ack".to_vec())).is_err());
        exchange.on_event(Event::PeerClosed).unwrap();
        assert!(exchange.peer_closed);
        assert!(!exchange.complete());
    }
}
