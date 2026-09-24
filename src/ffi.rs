//! Raw C shim bindings. Use each resource graph from one driving thread.
//!
//! Callers must preserve buffer and handle lifetimes described in fabric_shim.h.
//! Successful close consumes a handle; failed close keeps it alive. A failed
//! listener/endpoint open may return a cleanup-only handle when rollback fails.
//! Failed request endpoint setup also retains its handle: reject the request
//! before closing that endpoint.

use std::ffi::{c_char, c_int, c_void};

macro_rules! opaque_handles {
    ($($name:ident),+ $(,)?) => {$(
        #[repr(C)]
        pub struct $name {
            _private: [u8; 0],
        }
    )+};
}

opaque_handles!(Info, Fabric, Domain, Eq, Cq, Listener, Endpoint, Mr, Op);

pub const ACCESS_SEND: u32 = 1;
pub const ACCESS_RECV: u32 = 2;
pub const EVENT_CONNREQ: u32 = 1;
pub const EVENT_CONNECTED: u32 = 2;
pub const EVENT_SHUTDOWN: u32 = 3;
pub const EVENT_ERROR: u32 = 4;
pub const COMPLETION_SEND: u64 = 1;
pub const COMPLETION_RECV: u64 = 2;
pub const ERROR_TEXT_SIZE: usize = 256;

#[repr(C)]
pub struct Completion {
    pub op_id: u64,
    pub flags: u64,
    pub len: usize,
    pub err: i32,
    pub prov_err: i32,
    pub message: [c_char; ERROR_TEXT_SIZE],
}

#[repr(C)]
pub struct ConnectEvent {
    pub event_type: u32,
    pub source: usize,
    pub info: *mut Info,
    pub err: i32,
    pub prov_err: i32,
    pub message: [c_char; ERROR_TEXT_SIZE],
}

unsafe extern "C" {
    pub fn shim_fabric_version() -> u32;
    pub fn shim_error_string(error: c_int) -> *const c_char;
    pub fn shim_error_is_again(error: c_int) -> c_int;
    pub fn shim_print_info(local_ip: *const c_char, domain_name: *const c_char) -> c_int;
    pub fn shim_info_open(
        node: *const c_char,
        service: *const c_char,
        domain_name: *const c_char,
        is_source: c_int,
        out: *mut *mut Info,
    ) -> c_int;
    pub fn shim_info_free(info: *mut Info);
    pub fn shim_fabric_open(info: *mut Info, out: *mut *mut Fabric) -> c_int;
    pub fn shim_domain_open(fabric: *mut Fabric, info: *mut Info, out: *mut *mut Domain) -> c_int;
    pub fn shim_eq_open(fabric: *mut Fabric, out: *mut *mut Eq) -> c_int;
    pub fn shim_cq_open(domain: *mut Domain, capacity: usize, out: *mut *mut Cq) -> c_int;
    pub fn shim_listener_open(
        fabric: *mut Fabric,
        info: *mut Info,
        eq: *mut Eq,
        out: *mut *mut Listener,
    ) -> c_int;
    pub fn shim_endpoint_open(
        domain: *mut Domain,
        info: *mut Info,
        eq: *mut Eq,
        tx_cq: *mut Cq,
        rx_cq: *mut Cq,
        out: *mut *mut Endpoint,
    ) -> c_int;
    pub fn shim_connect(ep: *mut Endpoint, info: *mut Info) -> c_int;
    pub fn shim_accept(ep: *mut Endpoint) -> c_int;
    pub fn shim_reject(listener: *mut Listener, info: *mut Info) -> c_int;
    pub fn shim_eq_poll(eq: *mut Eq, out: *mut ConnectEvent) -> c_int;
    pub fn shim_cq_poll(cq: *mut Cq, out: *mut Completion) -> c_int;
    pub fn shim_mr_register(
        domain: *mut Domain,
        buf: *mut c_void,
        capacity: usize,
        access: u32,
        out: *mut *mut Mr,
    ) -> c_int;
    pub fn shim_op_create(id: u64, out: *mut *mut Op) -> c_int;
    pub fn shim_op_free(op: *mut Op) -> c_int;
    pub fn shim_post_send(
        ep: *mut Endpoint,
        op: *mut Op,
        mr: *mut Mr,
        len: usize,
        buf: *const c_void,
    ) -> c_int;
    pub fn shim_post_recv(
        ep: *mut Endpoint,
        op: *mut Op,
        mr: *mut Mr,
        capacity: usize,
        buf: *mut c_void,
    ) -> c_int;
    pub fn shim_shutdown(ep: *mut Endpoint) -> c_int;
    pub fn shim_endpoint_close(ep: *mut Endpoint) -> c_int;
    pub fn shim_listener_close(listener: *mut Listener) -> c_int;
    pub fn shim_mr_close(mr: *mut Mr) -> c_int;
    pub fn shim_cq_close(cq: *mut Cq) -> c_int;
    pub fn shim_eq_close(eq: *mut Eq) -> c_int;
    pub fn shim_domain_close(domain: *mut Domain) -> c_int;
    pub fn shim_fabric_close(fabric: *mut Fabric) -> c_int;
}
