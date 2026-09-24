//! Check that Rust can link and call the new shim entry points without RDMA.
use rdma_prototype::ffi;
use std::ffi::{CStr, CString};
use std::ptr;

#[test]
fn invalid_query_clears_the_output_handle() {
    let port = CString::new("7471").unwrap();
    let mut info = ptr::dangling_mut::<ffi::Info>();
    // Invalid node is rejected before device discovery; no handle is borrowed.
    let ret = unsafe {
        ffi::shim_info_open(ptr::null(), port.as_ptr(), ptr::null(), 1, &mut info)
    };
    assert!(ret < 0);
    assert!(info.is_null());
    let text = unsafe { CStr::from_ptr(ffi::shim_error_string(ret)) };
    assert!(!text.to_bytes().is_empty());
}

#[test]
fn operation_handle_crosses_the_ffi_boundary() {
    let mut op = ptr::null_mut();
    // The operation was never submitted, so it can be freed immediately.
    assert_eq!(unsafe { ffi::shim_op_create(1_u64 << 40, &mut op) }, 0);
    assert!(!op.is_null());
    assert_eq!(unsafe { ffi::shim_op_free(op) }, 0);
}
