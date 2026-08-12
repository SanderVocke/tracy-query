fn main() {
    #[cfg(feature = "manual-lifetime")]
    unsafe {
        tracy_client_sys::___tracy_startup_profiler();
    }

    let connected = unsafe { tracy_client_sys::___tracy_connected() };
    assert_eq!(connected, 0);

    #[cfg(feature = "manual-lifetime")]
    unsafe {
        tracy_client_sys::___tracy_shutdown_profiler();
    }
}
