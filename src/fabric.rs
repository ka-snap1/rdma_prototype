// use std::ffi::c_char;
use std::ffi::{CString, c_char};
unsafe extern  "C"{
    fn shim_fabric_version() -> u32;
    fn shim_print_info(
        local_ip: *const c_char,
        domain_name: *const c_char,
    ) -> i32;
}
pub fn runtime_fabric_version() -> (u32, u32) {
    let version = unsafe { shim_fabric_version() };
    (version >> 16, version & 0xFFFF)
}
pub fn print_info(
    local_ip: &str,
    domain_name: Option<&str>,
) -> Result<(), String> {
    let ip = CString::new(local_ip)
        .map_err(|_| "local_ip include NUL ".to_string())?;

    let domain = domain_name
        .map(CString::new)
        .transpose()
        .map_err(|_| "domain_name include NUL ".to_string())?;

    let domain_ptr = domain
        .as_ref()
        .map_or(std::ptr::null(), |name| name.as_ptr());

    let ret = unsafe {
        shim_print_info(ip.as_ptr(), domain_ptr)
    };

    if ret != 0 {
        return Err(format!("can not print libfabric info, error code:{ret}"));
    }

    Ok(())
}
