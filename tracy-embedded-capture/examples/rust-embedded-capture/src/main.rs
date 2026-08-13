use std::any::Any;
use std::path::Path;
use std::time::{Duration, Instant};
use tracing_subscriber::prelude::*;

const CAPACITY: usize = 256 * 1024;
const MEMORY_LIMIT: i64 = 256 * 1024 * 1024;

fn capture_error(status: i32) -> String {
    let length =
        unsafe { tracy_client_sys::___tracy_embedded_capture_get_error(std::ptr::null_mut(), 0) };
    let mut bytes = vec![0_u8; length + 1];
    unsafe {
        tracy_client_sys::___tracy_embedded_capture_get_error(
            bytes.as_mut_ptr().cast(),
            bytes.len(),
        );
    }
    let message = String::from_utf8_lossy(&bytes[..length]);
    format!("embedded capture status {status}: {message}")
}

fn configure(path: &Path) -> Result<(), String> {
    let path = path
        .to_str()
        .ok_or_else(|| "output path must be UTF-8".to_owned())?;
    let status = unsafe {
        tracy_client_sys::___tracy_embedded_capture_configure(
            path.as_ptr().cast(),
            path.len(),
            CAPACITY,
            MEMORY_LIMIT,
        )
    };
    if status == tracy_client_sys::TRACY_EMBEDDED_CAPTURE_OK {
        Ok(())
    } else {
        Err(capture_error(status))
    }
}

fn wait_until_capturing() -> Result<(), String> {
    let deadline = Instant::now() + Duration::from_secs(10);
    loop {
        let state = unsafe { tracy_client_sys::___tracy_embedded_capture_get_state() };
        if state == tracy_client_sys::TRACY_EMBEDDED_CAPTURE_CAPTURING {
            return Ok(());
        }
        if state == tracy_client_sys::TRACY_EMBEDDED_CAPTURE_FAILED {
            return Err(capture_error(state));
        }
        if Instant::now() >= deadline {
            return Err(format!(
                "timed out waiting for embedded capture; state={state}"
            ));
        }
        std::thread::sleep(Duration::from_millis(1));
    }
}

fn finish() -> Result<(), String> {
    let status = unsafe { tracy_client_sys::___tracy_embedded_capture_finish() };
    if status != tracy_client_sys::TRACY_EMBEDDED_CAPTURE_OK {
        return Err(capture_error(status));
    }
    let mut statistics = tracy_client_sys::tracy_embedded_capture_statistics::default();
    let status =
        unsafe { tracy_client_sys::___tracy_embedded_capture_get_statistics(&mut statistics) };
    if status != tracy_client_sys::TRACY_EMBEDDED_CAPTURE_OK {
        return Err(capture_error(status));
    }
    eprintln!(
        "embedded transport: c2s={} bytes (high-water {}), s2c={} bytes (high-water {})",
        statistics.client_to_server_bytes,
        statistics.client_to_server_high_water,
        statistics.server_to_client_bytes,
        statistics.server_to_client_high_water,
    );
    Ok(())
}

#[tracing::instrument(skip(client), fields(example.work_units = units))]
fn traced_work(client: &tracy_client::Client, units: u64, panic_at_end: bool) {
    let _direct = client.clone().span_alloc(
        Some("direct.tracy-client.zone"),
        "traced_work",
        file!(),
        line!(),
        0,
    );
    client.message("direct.tracy-client.message", 0);
    tracing::info!(units, "tracing-tracy.event");
    {
        let child = tracing::info_span!("tracing-tracy.child", units);
        let _entered = child.enter();
        std::thread::sleep(Duration::from_millis(20));
    }
    if panic_at_end {
        panic!("intentional unwind-panic example");
    }
}

fn run_scoped(client: &tracy_client::Client, panic_at_end: bool) {
    let subscriber = tracing_subscriber::registry().with(tracing_tracy::TracyLayer::default());
    let dispatch = tracing::Dispatch::new(subscriber);
    tracing::dispatcher::with_default(&dispatch, || {
        let workers: Vec<_> = (0..4)
            .map(|worker| {
                let client = client.clone();
                let dispatch = dispatch.clone();
                std::thread::spawn(move || {
                    tracing::dispatcher::with_default(&dispatch, || {
                        let span = tracing::info_span!("tracing-tracy.worker", worker);
                        let _entered = span.enter();
                        client.message("direct.tracy-client.worker", 0);
                    });
                })
            })
            .collect();
        for worker in workers {
            worker.join().expect("example worker must not panic");
        }
        traced_work(client, 7, panic_at_end);
    });
    drop(dispatch);
}

fn panic_summary(payload: &(dyn Any + Send)) -> &'static str {
    if payload.is::<&'static str>() || payload.is::<String>() {
        "rust unwind panic caught"
    } else {
        "non-string rust unwind panic caught"
    }
}

fn main() {
    let mut arguments = std::env::args_os().skip(1);
    let mode = arguments.next().unwrap_or_default();
    let output = arguments.next().unwrap_or_default();
    if arguments.next().is_some()
        || output.is_empty()
        || (mode != "normal" && mode != "unwind-panic")
    {
        eprintln!("usage: rust-embedded-capture-example <normal|unwind-panic> OUTPUT.tracy");
        std::process::exit(2);
    }

    let output = Path::new(&output);
    if let Err(error) = configure(output) {
        eprintln!("{error}");
        std::process::exit(3);
    }
    let client = tracy_client::Client::start();
    if let Err(error) = wait_until_capturing() {
        eprintln!("{error}");
        std::process::exit(4);
    }

    if mode == "normal" {
        run_scoped(&client, false);
        if let Err(error) = finish() {
            eprintln!("{error}");
            std::process::exit(5);
        }
        eprintln!("embedded capture published: {}", output.display());
        return;
    }

    let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        run_scoped(&client, true);
    }));
    let payload = match result {
        Ok(()) => {
            eprintln!("unwind-panic mode unexpectedly returned");
            std::process::exit(6);
        }
        Err(payload) => payload,
    };

    // Unwinding has dropped the scoped dispatcher and every span/zone guard.
    // This controlling thread is now the only instrumentation producer.
    client.message(panic_summary(payload.as_ref()), 0);
    if let Err(error) = finish() {
        // Avoid a second panic while handling the original payload.
        eprintln!("capture finalization failed while handling panic: {error}");
        std::process::exit(70);
    }
    eprintln!("panic capture published; resuming original panic");
    std::panic::resume_unwind(payload);
}
