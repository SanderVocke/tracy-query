pub const TRACY_EMBEDDED_CAPTURE_ABI_VERSION: u32 = 1;
pub const TRACY_EMBEDDED_CAPTURE_OK: i32 = 0;
pub const TRACY_EMBEDDED_CAPTURE_INVALID_ARGUMENT: i32 = 1;
pub const TRACY_EMBEDDED_CAPTURE_INVALID_STATE: i32 = 2;
pub const TRACY_EMBEDDED_CAPTURE_OUTPUT_EXISTS: i32 = 3;
pub const TRACY_EMBEDDED_CAPTURE_IO_ERROR: i32 = 4;
pub const TRACY_EMBEDDED_CAPTURE_TRANSPORT_ERROR: i32 = 5;
pub const TRACY_EMBEDDED_CAPTURE_NO_DATA: i32 = 6;
pub const TRACY_EMBEDDED_CAPTURE_INTERNAL_ERROR: i32 = 7;

pub const TRACY_EMBEDDED_CAPTURE_UNCONFIGURED: i32 = 0;
pub const TRACY_EMBEDDED_CAPTURE_CONFIGURED: i32 = 1;
pub const TRACY_EMBEDDED_CAPTURE_CAPTURING: i32 = 2;
pub const TRACY_EMBEDDED_CAPTURE_FINISHING: i32 = 3;
pub const TRACY_EMBEDDED_CAPTURE_FINISHED: i32 = 4;
pub const TRACY_EMBEDDED_CAPTURE_FAILED: i32 = 5;

#[repr(C)]
#[derive(Debug, Copy, Clone, Default)]
pub struct tracy_embedded_capture_statistics {
    pub client_to_server_bytes: u64,
    pub server_to_client_bytes: u64,
    pub client_to_server_high_water: u64,
    pub server_to_client_high_water: u64,
}

extern "C" {
    pub fn ___tracy_embedded_capture_configure(
        path: *const ::std::os::raw::c_char,
        path_len: usize,
        channel_capacity: usize,
        worker_memory_limit: i64,
    ) -> i32;
    pub fn ___tracy_embedded_capture_finish() -> i32;
    pub fn ___tracy_embedded_capture_abi_version() -> u32;
    pub fn ___tracy_embedded_capture_get_state() -> i32;
    pub fn ___tracy_embedded_capture_get_statistics(
        statistics: *mut tracy_embedded_capture_statistics,
    ) -> i32;
    pub fn ___tracy_embedded_capture_get_error(
        destination: *mut ::std::os::raw::c_char,
        capacity: usize,
    ) -> usize;
}
