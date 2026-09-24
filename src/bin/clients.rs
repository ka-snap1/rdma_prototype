use rdma_prototype::transport::{Connection, Event, Fabric, Info, Result, finish};
use std::thread;
use std::time::{Duration, Instant};

#[derive(Clone, Copy, Debug, PartialEq)]
enum Message {
    Hello,
    Ack,
}

impl Message {
    fn bytes(self) -> &'static [u8] {
        match self {
            Self::Hello => b"hello",
            Self::Ack => b"ack",
        }
    }
}

/* Application state does not own an endpoint or registered memory.
 * The hello TX completion and the world RX completion can arrive in either order.
 */
#[derive(Debug, Default)]
struct Exchange {
    connected: bool,
    hello_posted: bool,
    hello_completed: bool,
    world_received: bool,
    ack_posted: bool,
    ack_completed: bool,
    peer_closed: bool,
}

impl Exchange {
    fn on_event(&mut self, event: Event) -> Result<()> {
        match event {
            Event::Connected => self.connected = true,
            Event::Received(message) => {
                if !self.hello_posted || self.world_received || message != b"world" {
                    return Err(format!(
                        "unexpected reply: {message:?}; expected one world message"
                    ));
                }
                self.world_received = true;
            }
            Event::Sent => {
                if self.ack_posted && !self.ack_completed {
                    self.ack_completed = true;
                } else if self.hello_posted && !self.hello_completed {
                    self.hello_completed = true;
                } else {
                    return Err("unexpected or duplicate send completion".into());
                }
            }
            Event::PeerClosed => self.peer_closed = true,
        }
        Ok(())
    }

    fn next_message(&self) -> Option<Message> {
        if !self.connected || self.peer_closed {
            return None;
        }
        if !self.hello_posted {
            Some(Message::Hello)
        } else if self.hello_completed && self.world_received && !self.ack_posted {
            Some(Message::Ack)
        } else {
            None
        }
    }

    /* Only advance after send returns true. EAGAIN leaves the state unchanged. */
    fn posted(&mut self, message: Message) {
        match message {
            Message::Hello => self.hello_posted = true,
            Message::Ack => self.ack_posted = true,
        }
    }

    fn complete(&self) -> bool {
        self.world_received && self.hello_completed && self.ack_completed
    }
}

/* Keep the event loop testable without requiring a verbs device. */
trait ClientIo {
    fn poll(&mut self) -> Result<Vec<Event>>;
    fn send(&mut self, message: &[u8]) -> Result<bool>;
}

impl ClientIo for Connection<'_> {
    fn poll(&mut self) -> Result<Vec<Event>> {
        Connection::poll(self)
    }

    fn send(&mut self, message: &[u8]) -> Result<bool> {
        Connection::send(self, message)
    }
}

fn exchange(io: &mut impl ClientIo, timeout: Duration) -> Result<()> {
    let deadline = Instant::now() + timeout;
    let mut state = Exchange::default();
    while Instant::now() < deadline {
        for event in io.poll()? {
            println!("event: {event:?}");
            state.on_event(event)?;
        }
        if state.complete() {
            return Ok(());
        }
        if let Some(message) = state.next_message()
            && io.send(message.bytes())?
        {
            state.posted(message);
        }
        // After peer shutdown, drain final CQ events without submitting new work.
        // A lost completion or a stalled peer still reaches the fixed deadline.
        thread::sleep(Duration::from_millis(1));
    }
    Err(format!("client exchange timeout: {state:?}"))
}

fn run(ip: &str, port: &str, domain: Option<&str>, timeout: Duration) -> Result<()> {
    // The destination is the server IP, while domain selects the local RXE device.
    let info = Info::remote(ip, port, domain)?;
    let mut fabric = Fabric::open(&info)?;
    let result = (|| {
        // connect creates domain/EQ/CQs/EP/MRs and posts receive before connecting.
        // It returns before CONNECTED; exchange drives EQ and CQs together.
        let mut conn = Connection::connect(&fabric, &info, timeout)?;
        let result = exchange(&mut conn, timeout);
        finish(result, conn.close())
    })();
    finish(result, fabric.close())
}

fn main() -> Result<()> {
    let args: Vec<String> = std::env::args().skip(1).collect();
    if args.is_empty() || args.len() > 4 {
        return Err(
            "usage: clients <server-rxe-ip> [port=7471] [local-domain] [timeout-seconds=30]".into(),
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
    println!("hello sent, world verified, ack sent; resources closed");
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::VecDeque;

    #[test]
    fn reply_and_hello_completion_can_arrive_in_either_order() {
        for reply_first in [true, false] {
            let mut state = Exchange::default();
            assert_eq!(state.next_message(), None);
            state.on_event(Event::Connected).unwrap();
            assert_eq!(state.next_message(), Some(Message::Hello));
            state.posted(Message::Hello);
            let reply = Event::Received(b"world".to_vec());
            let events = if reply_first {
                [reply, Event::Sent]
            } else {
                [Event::Sent, reply]
            };
            for event in events {
                state.on_event(event).unwrap();
            }
            assert_eq!(state.next_message(), Some(Message::Ack));
            state.posted(Message::Ack);
            assert!(!state.complete());
            state.on_event(Event::Sent).unwrap();
            assert!(state.complete());
        }
    }

    #[test]
    fn invalid_or_duplicate_reply_is_rejected() {
        for message in [b"hello".as_slice(), b"world\0", b"", b"wor"] {
            let mut state = Exchange {
                hello_posted: true,
                ..Exchange::default()
            };
            assert!(state.on_event(Event::Received(message.to_vec())).is_err());
        }
        let mut state = Exchange {
            hello_posted: true,
            ..Exchange::default()
        };
        state.on_event(Event::Received(b"world".to_vec())).unwrap();
        assert!(state.on_event(Event::Received(b"world".to_vec())).is_err());
    }

    struct FakeIo {
        events: VecDeque<Vec<Event>>,
        attempts: Vec<Vec<u8>>,
        again: bool,
    }

    impl ClientIo for FakeIo {
        fn poll(&mut self) -> Result<Vec<Event>> {
            Ok(self.events.pop_front().unwrap_or_default())
        }

        fn send(&mut self, message: &[u8]) -> Result<bool> {
            self.attempts.push(message.to_vec());
            if self.again {
                self.again = false;
                return Ok(false);
            }
            if message == b"hello" {
                self.events
                    .push_back(vec![Event::Received(b"world".to_vec())]);
                self.events.push_back(vec![Event::Sent]);
            } else if message == b"ack" {
                // SHUTDOWN can be observed before the final local TX completion.
                self.events.push_back(vec![Event::PeerClosed]);
                self.events.push_back(vec![Event::Sent]);
            } else {
                panic!("unexpected payload");
            }
            Ok(true)
        }
    }

    #[test]
    fn event_loop_retries_eagain_and_drains_ack_after_shutdown() {
        let mut io = FakeIo {
            events: VecDeque::from([vec![Event::Connected]]),
            attempts: Vec::new(),
            again: true,
        };
        exchange(&mut io, Duration::from_secs(1)).unwrap();
        assert_eq!(
            io.attempts,
            [b"hello".to_vec(), b"hello".to_vec(), b"ack".to_vec()]
        );
    }

    #[test]
    fn stalled_or_closed_peer_reaches_deadline_without_sending() {
        for events in [vec![], vec![Event::Connected, Event::PeerClosed]] {
            let mut io = FakeIo {
                events: VecDeque::from([events]),
                attempts: Vec::new(),
                again: false,
            };
            let err = exchange(&mut io, Duration::from_millis(5)).unwrap_err();
            assert!(err.contains("timeout"));
            assert!(io.attempts.is_empty());
        }
    }

    #[test]
    fn transport_error_is_not_reported_as_timeout_or_success() {
        struct BrokenIo;
        impl ClientIo for BrokenIo {
            fn poll(&mut self) -> Result<Vec<Event>> {
                Err("injected CQ error".into())
            }
            fn send(&mut self, _: &[u8]) -> Result<bool> {
                panic!("must not send")
            }
        }
        assert_eq!(
            exchange(&mut BrokenIo, Duration::from_secs(1)),
            Err("injected CQ error".into())
        );
    }
}
