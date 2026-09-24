//! Shared single-threaded resource ownership for the prototype client and server.
//! Transport methods submit or poll once; application loops own their deadlines.

use crate::ffi;
use std::ffi::{CStr, CString};
use std::mem::MaybeUninit;
use std::ptr;
use std::thread;
use std::time::{Duration, Instant};

pub type Result<T> = std::result::Result<T, String>;
const BUFFER_SIZE: usize = 64;
const TX_ID: u64 = 1;
const RX_ID: u64 = 2;

fn check(code: i32, operation: &str) -> Result<()> {
    if code == 0 {
        return Ok(());
    }
    // The shim returns a library-owned, NUL-terminated error string.
    let text = unsafe { CStr::from_ptr(ffi::shim_error_string(code)) };
    Err(format!("{operation}: {} ({code})", text.to_string_lossy()))
}

/* Close consumes a handle only on success. Keep failed handles for a retry. */
macro_rules! close {
    ($handle:expr, $function:ident) => {
        if !$handle.is_null() {
            check(unsafe { ffi::$function($handle) }, stringify!($function))?;
            $handle = ptr::null_mut();
        }
    };
}

/* Own one query result. This is metadata, not an open fabric or endpoint. */
pub struct Info {
    raw: *mut ffi::Info,
}

impl Info {
    pub fn local(ip: &str, port: &str, domain: Option<&str>) -> Result<Self> {
        Self::query(ip, port, domain, true)
    }

    /* domain still selects a local device when resolving the remote address. */
    pub fn remote(ip: &str, port: &str, domain: Option<&str>) -> Result<Self> {
        Self::query(ip, port, domain, false)
    }

    fn query(ip: &str, port: &str, domain: Option<&str>, is_source: bool) -> Result<Self> {
        let node = CString::new(ip).map_err(|_| "IP contains NUL")?;
        let service = CString::new(port).map_err(|_| "port contains NUL")?;
        let domain = domain
            .map(CString::new)
            .transpose()
            .map_err(|_| "domain contains NUL")?;
        let mut info = Self {
            raw: ptr::null_mut(),
        };
        // Strings remain valid until the synchronous query returns.
        check(
            unsafe {
                ffi::shim_info_open(
                    node.as_ptr(),
                    service.as_ptr(),
                    domain.as_ref().map_or(ptr::null(), |s| s.as_ptr()),
                    i32::from(is_source),
                    &mut info.raw,
                )
            },
            "shim_info_open",
        )?;
        Ok(info)
    }
}

impl Drop for Info {
    fn drop(&mut self) {
        unsafe { ffi::shim_info_free(self.raw) };
    }
}

/* Own the C shim handle, not the contents of ffi::Fabric.
 * raw is the address returned by shim_fabric_open through its output argument.
 * Returning this Rust object keeps the handle alive for later resource opens.
 */
pub struct Fabric {
    raw: *mut ffi::Fabric,
}

impl Fabric {
    pub fn open(info: &Info) -> Result<Self> {
        let mut fabric = Self {
            raw: ptr::null_mut(),
        };
        check(
            unsafe { ffi::shim_fabric_open(info.raw, &mut fabric.raw) },
            "shim_fabric_open",
        )?;
        Ok(fabric)
    }

    pub fn close(&mut self) -> Result<()> {
        close!(self.raw, shim_fabric_close);
        Ok(())
    }
}

impl Drop for Fabric {
    fn drop(&mut self) {
        if let Err(error) = self.close() {
            eprintln!("fabric cleanup: {error}; C handle retained until process exit");
        }
    }
}

/* Borrow the parent fabric and own a listening EQ and passive endpoint.
 * The lifetime prevents Rust from dropping fabric while this listener exists.
 */
pub struct Listener<'f> {
    fabric: &'f Fabric,
    eq: *mut ffi::Eq,
    raw: *mut ffi::Listener,
}

impl<'f> Listener<'f> {
    pub fn bind(fabric: &'f Fabric, info: &Info) -> Result<Self> {
        let mut listener = Self {
            fabric,
            eq: ptr::null_mut(),
            raw: ptr::null_mut(),
        };
        check(
            unsafe { ffi::shim_eq_open(fabric.raw, &mut listener.eq) },
            "shim_eq_open",
        )?;
        // A failed composite open can leave a cleanup-only handle in raw.
        // Drop also covers this partially initialized object.
        check(
            unsafe {
                ffi::shim_listener_open(fabric.raw, info.raw, listener.eq, &mut listener.raw)
            },
            "shim_listener_open",
        )?;
        Ok(listener)
    }

    pub fn poll(&self) -> Result<Option<Request<'_, 'f>>> {
        if self.raw.is_null() {
            return Err("listener is closed".into());
        }
        let Some(event) = poll_eq(self.eq)? else {
            return Ok(None);
        };
        if event.event_type == ffi::EVENT_CONNREQ && !event.info.is_null() {
            // Take ownership of event.info, preserving its provider request handle.
            return Ok(Some(Request {
                listener: self,
                info: Info { raw: event.info },
                handled: false,
            }));
        }
        Err(format!(
            "unexpected listener event {}, error {}",
            event.event_type, event.err
        ))
    }

    pub fn wait_request(&self, timeout: Duration) -> Result<Request<'_, 'f>> {
        let deadline = Instant::now() + timeout;
        while Instant::now() < deadline {
            if let Some(request) = self.poll()? {
                return Ok(request);
            }
            thread::sleep(Duration::from_millis(1));
        }
        Err("timeout waiting for a connection request".into())
    }

    pub fn close(&mut self) -> Result<()> {
        close!(self.raw, shim_listener_close);
        close!(self.eq, shim_eq_close);
        Ok(())
    }
}

impl Drop for Listener<'_> {
    fn drop(&mut self) {
        if let Err(error) = self.close() {
            eprintln!("listener cleanup: {error}");
        }
    }
}

/* A request borrows its listener so rejection remains possible on failure.
 * Drop rejects unaccepted requests before Info releases the metadata.
 */
pub struct Request<'l, 'f> {
    listener: &'l Listener<'f>,
    info: Info,
    handled: bool,
}

impl Request<'_, '_> {
    fn reject(&mut self) -> Result<()> {
        if self.handled {
            return Ok(());
        }
        // verbs 1.11 consumes the request handle even if rdma_reject fails.
        // Never attempt rejection twice with the same provider handle.
        self.handled = true;
        check(
            unsafe { ffi::shim_reject(self.listener.raw, self.info.raw) },
            "shim_reject",
        )
    }
}

impl Drop for Request<'_, '_> {
    fn drop(&mut self) {
        if let Err(error) = self.reject() {
            eprintln!("request cleanup: {error}");
        }
    }
}

/* The allocation behind Box does not move when this Rust struct moves.
 * bytes stays private so callers cannot touch memory borrowed by the provider.
 */
struct Slot {
    bytes: Option<Box<[u8]>>,
    mr: *mut ffi::Mr,
    op: *mut ffi::Op,
    pending: bool,
}

impl Slot {
    fn empty() -> Self {
        Self {
            bytes: Some(vec![0; BUFFER_SIZE].into_boxed_slice()),
            mr: ptr::null_mut(),
            op: ptr::null_mut(),
            pending: false,
        }
    }

    pub fn close(&mut self) -> Result<()> {
        close!(self.op, shim_op_free);
        close!(self.mr, shim_mr_close);
        self.bytes = None;
        self.pending = false;
        Ok(())
    }
}

/* Own the connection resources, while borrowing the shared parent fabric.
 * A separate connection EQ keeps listener and active endpoint events apart.
 */
pub struct Connection<'f> {
    _fabric: &'f Fabric,
    domain: *mut ffi::Domain,
    eq: *mut ffi::Eq,
    tx_cq: *mut ffi::Cq,
    rx_cq: *mut ffi::Cq,
    ep: *mut ffi::Endpoint,
    tx: Slot,
    rx: Slot,
    cm_started: bool,
    closing: bool,
}

#[derive(Debug)]
pub enum Event {
    Connected,
    Received(Vec<u8>),
    Sent,
    PeerClosed,
}

impl<'f> Connection<'f> {
    fn empty(fabric: &'f Fabric) -> Self {
        Self {
            _fabric: fabric,
            domain: ptr::null_mut(),
            eq: ptr::null_mut(),
            tx_cq: ptr::null_mut(),
            rx_cq: ptr::null_mut(),
            ep: ptr::null_mut(),
            tx: Slot::empty(),
            rx: Slot::empty(),
            cm_started: false,
            closing: false,
        }
    }

    /* Prepare resources and a posted receive before connect or accept. */
    fn initialize(&mut self, info: &Info, timeout: Duration) -> Result<()> {
        // The server passes request info; the client passes destination query info.
        check(
            unsafe { ffi::shim_domain_open(self._fabric.raw, info.raw, &mut self.domain) },
            "shim_domain_open",
        )?;
        check(
            unsafe { ffi::shim_eq_open(self._fabric.raw, &mut self.eq) },
            "shim_eq_open",
        )?;
        check(
            unsafe { ffi::shim_cq_open(self.domain, 16, &mut self.tx_cq) },
            "TX CQ open",
        )?;
        check(
            unsafe { ffi::shim_cq_open(self.domain, 16, &mut self.rx_cq) },
            "RX CQ open",
        )?;
        check(
            unsafe {
                ffi::shim_endpoint_open(
                    self.domain,
                    info.raw,
                    self.eq,
                    self.tx_cq,
                    self.rx_cq,
                    &mut self.ep,
                )
            },
            "shim_endpoint_open",
        )?;
        for (slot, access, id) in [
            (&mut self.tx, ffi::ACCESS_SEND, TX_ID),
            (&mut self.rx, ffi::ACCESS_RECV, RX_ID),
        ] {
            let bytes = slot.bytes.as_mut().unwrap();
            check(
                unsafe {
                    ffi::shim_mr_register(
                        self.domain,
                        bytes.as_mut_ptr().cast(),
                        bytes.len(),
                        access,
                        &mut slot.mr,
                    )
                },
                "shim_mr_register",
            )?;
            check(
                unsafe { ffi::shim_op_create(id, &mut slot.op) },
                "shim_op_create",
            )?;
        }
        // Make receive memory available before accepting or initiating a connection.
        let deadline = Instant::now() + timeout;
        while !self.post_recv()? {
            if Instant::now() >= deadline {
                return Err("receive submission timeout".into());
            }
            self.poll()?;
            thread::sleep(Duration::from_millis(1));
        }
        Ok(())
    }

    pub fn from_request(request: &mut Request<'_, 'f>, timeout: Duration) -> Result<Self> {
        if request.handled {
            return Err("connection request is already handled".into());
        }
        let mut conn = Self::empty(request.listener.fabric);
        let setup = (|| -> Result<()> {
            conn.initialize(&request.info, timeout)?;
            check(unsafe { ffi::shim_accept(conn.ep) }, "shim_accept")?;
            request.handled = true;
            conn.cm_started = true;
            Ok(())
        })();
        if let Err(error) = setup {
            // Reject before closing the request endpoint and its CM identifier.
            return match request.reject() {
                Ok(()) => Err(error),
                Err(rejection) => Err(format!("{error}; rejection: {rejection}")),
            };
        }
        Ok(conn)
    }

    /* Starts connection establishment; wait for Event::Connected before sending. */
    pub fn connect(fabric: &'f Fabric, info: &Info, timeout: Duration) -> Result<Self> {
        let mut conn = Self::empty(fabric);
        conn.initialize(info, timeout)?;
        check(
            unsafe { ffi::shim_connect(conn.ep, info.raw) },
            "shim_connect",
        )?;
        conn.cm_started = true;
        Ok(conn)
    }

    pub fn receive_pending(&self) -> bool {
        self.rx.pending
    }

    /* Return false on EAGAIN; no operation was submitted in that case. */
    pub fn post_recv(&mut self) -> Result<bool> {
        self.ensure_open()?;
        if self.rx.pending {
            return Err("receive slot is already in flight".into());
        }
        let bytes = self.rx.bytes.as_mut().unwrap();
        let ret = unsafe {
            ffi::shim_post_recv(
                self.ep,
                self.rx.op,
                self.rx.mr,
                bytes.len(),
                bytes.as_mut_ptr().cast(),
            )
        };
        if unsafe { ffi::shim_error_is_again(ret) } != 0 {
            return Ok(false);
        }
        check(ret, "shim_post_recv")?;
        self.rx.pending = true;
        Ok(true)
    }

    pub fn send(&mut self, message: &[u8]) -> Result<bool> {
        self.ensure_open()?;
        if self.tx.pending {
            return Err("send slot is already in flight".into());
        }
        let bytes = self.tx.bytes.as_mut().unwrap();
        if message.len() > bytes.len() {
            return Err("message exceeds send buffer capacity".into());
        }
        bytes[..message.len()].copy_from_slice(message);
        let ret = unsafe {
            ffi::shim_post_send(
                self.ep,
                self.tx.op,
                self.tx.mr,
                message.len(),
                bytes.as_ptr().cast(),
            )
        };
        if unsafe { ffi::shim_error_is_again(ret) } != 0 {
            return Ok(false);
        }
        check(ret, "shim_post_send")?;
        self.tx.pending = true;
        Ok(true)
    }

    /* Poll once without waiting. Copy received bytes only after RX completion.
     * The caller runs its callback after poll returns, avoiding reentrant access.
     */
    pub fn poll(&mut self) -> Result<Vec<Event>> {
        self.ensure_open()?;
        let mut events = Vec::new();
        if let Some(done) = poll_cq(self.tx_cq)? {
            if !self.tx.pending || done.op_id != TX_ID || done.flags != ffi::COMPLETION_SEND {
                return Err("unexpected TX completion".into());
            }
            self.tx.pending = false;
            events.push(Event::Sent);
        }
        if let Some(done) = poll_cq(self.rx_cq)? {
            if !self.rx.pending || done.op_id != RX_ID || done.flags != ffi::COMPLETION_RECV {
                return Err("unexpected RX completion".into());
            }
            self.rx.pending = false;
            let bytes = self.rx.bytes.as_ref().unwrap();
            let message = bytes
                .get(..done.len)
                .ok_or("RX length exceeds buffer capacity")?;
            events.push(Event::Received(message.to_vec()));
        }
        if let Some(event) = poll_eq(self.eq)? {
            match event.event_type {
                ffi::EVENT_CONNECTED => events.push(Event::Connected),
                ffi::EVENT_SHUTDOWN => events.push(Event::PeerClosed),
                _ => return Err(format!("unexpected connection event {}", event.event_type)),
            }
        }
        Ok(events)
    }

    fn ensure_open(&self) -> Result<()> {
        if self.ep.is_null() || self.closing {
            return Err("connection is closing or closed".into());
        }
        Ok(())
    }

    pub fn close(&mut self) -> Result<()> {
        self.closing = true;
        if self.cm_started && !self.ep.is_null() {
            // Shutdown is advisory; close still runs after peer disconnect errors.
            unsafe { ffi::shim_shutdown(self.ep) };
            self.cm_started = false;
        }
        close!(self.ep, shim_endpoint_close);
        // A CQ can still reference an unfinished op after endpoint close.
        close!(self.tx_cq, shim_cq_close);
        close!(self.rx_cq, shim_cq_close);
        self.tx.close()?;
        self.rx.close()?;
        close!(self.eq, shim_eq_close);
        close!(self.domain, shim_domain_close);
        Ok(())
    }
}

impl Drop for Connection<'_> {
    fn drop(&mut self) {
        if let Err(error) = self.close() {
            eprintln!("connection cleanup: {error}; retain buffers until process exit");
            // Never let Rust release storage if the provider may still use it.
            // C parent reference counts also prevent premature domain/fabric close.
            if let Some(bytes) = self.tx.bytes.take() {
                std::mem::forget(bytes);
            }
            if let Some(bytes) = self.rx.bytes.take() {
                std::mem::forget(bytes);
            }
        }
    }
}

fn poll_eq(eq: *mut ffi::Eq) -> Result<Option<ffi::ConnectEvent>> {
    let mut event = MaybeUninit::<ffi::ConnectEvent>::uninit();
    let ret = unsafe { ffi::shim_eq_poll(eq, event.as_mut_ptr()) };
    if ret == 0 {
        return Ok(None);
    }
    if ret < 0 {
        check(ret, "shim_eq_poll")?;
    }
    // A returned event has all fields initialized by the shim.
    let event = unsafe { event.assume_init() };
    if event.err != 0 {
        return Err(format!(
            "EQ error {} (provider {}): {}",
            event.err,
            event.prov_err,
            error_text(&event.message)
        ));
    }
    Ok(Some(event))
}

fn poll_cq(cq: *mut ffi::Cq) -> Result<Option<ffi::Completion>> {
    let mut done = MaybeUninit::<ffi::Completion>::uninit();
    let ret = unsafe { ffi::shim_cq_poll(cq, done.as_mut_ptr()) };
    if ret == 0 {
        return Ok(None);
    }
    if ret < 0 {
        check(ret, "shim_cq_poll")?;
    }
    let done = unsafe { done.assume_init() };
    if done.err != 0 {
        return Err(format!(
            "CQ op {} error {} (provider {}): {}",
            done.op_id,
            done.err,
            done.prov_err,
            error_text(&done.message)
        ));
    }
    Ok(Some(done))
}

fn error_text(text: &[std::ffi::c_char]) -> String {
    let bytes: Vec<u8> = text
        .iter()
        .take_while(|&&c| c != 0)
        .map(|&c| c as u8)
        .collect();
    String::from_utf8_lossy(&bytes).into_owned()
}

/* Report the primary error and an explicit cleanup error without hiding either. */
pub fn finish(result: Result<()>, cleanup: Result<()>) -> Result<()> {
    match (result, cleanup) {
        (Err(error), Err(cleanup)) => Err(format!("{error}; cleanup: {cleanup}")),
        (Err(error), _) | (_, Err(error)) => Err(error),
        _ => Ok(()),
    }
}
