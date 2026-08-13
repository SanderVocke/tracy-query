#![deny(unsafe_op_in_unsafe_fn, missing_docs)]
//! Failure-only in-process Tracy capture for synchronous cargo-nextest tests.

#[cfg(not(panic = "unwind"))]
compile_error!("tracy-nextest-capture requires panic=unwind");

use sha2::{Digest, Sha256};
use std::any::Any;
use std::ffi::OsString;
use std::fmt::Debug;
use std::path::{Path, PathBuf};
use std::time::{Duration, Instant};

pub use tracy_nextest_capture_macros::tracy_capture_test;

const CAPACITY: usize = 256 * 1024;
const MEMORY_LIMIT: i64 = 256 * 1024 * 1024;
const READY_TIMEOUT: Duration = Duration::from_secs(10);

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum Policy {
    Off,
    Failure,
    Always,
}

struct Capture {
    policy: Policy,
    client: tracy_client::Client,
    output: PathBuf,
    diagnostic_identity: String,
}

impl Capture {
    fn start() -> Result<Option<Self>, String> {
        let identity = (
            std::env::var_os("NEXTEST_ATTEMPT_ID"),
            std::env::var_os("NEXTEST_TEST_NAME"),
            std::env::var_os("NEXTEST_BINARY_ID"),
            std::env::var_os("NEXTEST_ATTEMPT"),
        );
        let (Some(attempt_id), Some(test_name), Some(binary), Some(attempt)) = identity else {
            return Ok(None);
        };
        let policy = parse_policy(std::env::var_os("TRACY_NEXTEST_CAPTURE"))?;
        if policy == Policy::Off {
            return Ok(None);
        }
        if !cfg!(panic = "unwind") {
            return Err("Tracy nextest capture requires panic=unwind".into());
        }
        let root = PathBuf::from(std::env::var_os("TRACY_NEXTEST_OUTPUT_DIR")
            .ok_or("TRACY_NEXTEST_OUTPUT_DIR is required when capture is enabled")?);
        if !root.is_dir() {
            return Err(format!("capture output directory does not exist: {}", root.display()));
        }
        let test_name = test_name.into_string()
            .map_err(|_| "NEXTEST_TEST_NAME must be valid UTF-8")?;
        let binary = binary.into_string()
            .map_err(|_| "NEXTEST_BINARY_ID must be valid UTF-8")?;
        let attempt = attempt.into_string()
            .map_err(|_| "NEXTEST_ATTEMPT must be valid UTF-8")?;
        let output = output_path(&root, &binary, &test_name, &attempt, &attempt_id);
        let diagnostic_identity = format!(
            "test={} attempt={} id-digest={}",
            sanitize(&test_name),
            sanitize(&attempt),
            attempt_digest(&attempt_id),
        );
        verify_abi().map_err(|error| format!("{diagnostic_identity}: {error}"))?;
        configure(&output).map_err(|error| format!("{diagnostic_identity}: {error}"))?;
        let client = tracy_client::Client::start();
        wait_until_capturing()
            .map_err(|error| format!("{diagnostic_identity}: {error}"))?;
        client.message(&format!("nextest-in-process:{test_name}:attempt:{attempt}"), 0);
        Ok(Some(Self { policy, client, output, diagnostic_identity }))
    }

    fn finish(self, failed: bool) -> Result<(), String> {
        let disposition = if failed || self.policy == Policy::Always {
            tracy_client_sys::TRACY_EMBEDDED_CAPTURE_SAVE
        } else {
            tracy_client_sys::TRACY_EMBEDDED_CAPTURE_DISCARD
        };
        let status = unsafe {
            tracy_client_sys::___tracy_embedded_capture_finish_with_disposition(disposition)
        };
        if status != tracy_client_sys::TRACY_EMBEDDED_CAPTURE_OK {
            return Err(format!("{}: {}", self.diagnostic_identity, capture_error(status)));
        }
        if disposition == tracy_client_sys::TRACY_EMBEDDED_CAPTURE_SAVE {
            eprintln!("Tracy failure capture published: {}", self.output.display());
        }
        Ok(())
    }
}

/// Run a synchronous unit-returning test with nextest capture when enabled.
pub fn run<F>(body: F)
where
    F: FnOnce(),
{
    let capture = Capture::start().unwrap_or_else(|error| configuration_failure(error));
    let outcome = std::panic::catch_unwind(std::panic::AssertUnwindSafe(body));
    match outcome {
        Ok(()) => {
            if let Some(capture) = capture {
                capture.finish(false).unwrap_or_else(|error| configuration_failure(error));
            }
        }
        Err(payload) => {
            if let Some(capture) = capture {
                capture.client.message("nextest-in-process:panic-caught", 0);
                if let Err(error) = capture.finish(true) {
                    finalization_failure(&error, payload.as_ref());
                }
            }
            std::panic::resume_unwind(payload);
        }
    }
}

/// Run a synchronous `Result`-returning test with nextest capture when enabled.
pub fn run_result<F, E>(body: F) -> Result<(), E>
where
    F: FnOnce() -> Result<(), E>,
    E: Debug,
{
    let capture = Capture::start().unwrap_or_else(|error| configuration_failure(error));
    let outcome = std::panic::catch_unwind(std::panic::AssertUnwindSafe(body));
    match outcome {
        Ok(Ok(())) => {
            if let Some(capture) = capture {
                capture.finish(false).unwrap_or_else(|error| configuration_failure(error));
            }
            Ok(())
        }
        Ok(Err(error)) => {
            if let Some(capture) = capture {
                capture.client.message("nextest-in-process:result-error", 0);
                if let Err(capture_error) = capture.finish(true) {
                    eprintln!("capture finalization failed while returning test error: {capture_error}; original error: {error:?}");
                    std::process::exit(70);
                }
            }
            Err(error)
        }
        Err(payload) => {
            if let Some(capture) = capture {
                capture.client.message("nextest-in-process:panic-caught", 0);
                if let Err(error) = capture.finish(true) {
                    finalization_failure(&error, payload.as_ref());
                }
            }
            std::panic::resume_unwind(payload);
        }
    }
}

fn parse_policy(value: Option<OsString>) -> Result<Policy, String> {
    match value.as_deref().and_then(|value| value.to_str()) {
        None | Some("off") => Ok(Policy::Off),
        Some("failure") => Ok(Policy::Failure),
        Some("always") => Ok(Policy::Always),
        Some(value) => Err(format!("invalid TRACY_NEXTEST_CAPTURE policy: {value}")),
    }
}

fn sanitize(value: &str) -> String {
    let mut result = String::new();
    for character in value.chars().take(48) {
        result.push(if character.is_ascii_alphanumeric() || matches!(character, '-' | '_') {
            character
        } else {
            '_'
        });
    }
    if result.is_empty() { "test".into() } else { result }
}

fn attempt_digest(attempt_id: &std::ffi::OsStr) -> String {
    let mut digest = Sha256::new();
    digest.update(attempt_id.to_string_lossy().as_bytes());
    format!("{:x}", digest.finalize())[..16].into()
}

fn output_path(root: &Path, binary: &str, test: &str, attempt: &str, attempt_id: &std::ffi::OsStr) -> PathBuf {
    root.join(format!("{}--{}--attempt-{}--{}.tracy", sanitize(binary), sanitize(test), sanitize(attempt), attempt_digest(attempt_id)))
}

fn verify_abi() -> Result<(), String> {
    let actual = unsafe { tracy_client_sys::___tracy_embedded_capture_abi_version() };
    if actual == tracy_client_sys::TRACY_EMBEDDED_CAPTURE_ABI_VERSION {
        Ok(())
    } else {
        Err(format!(
            "embedded capture ABI mismatch: helper requires {}, native library reports {actual}",
            tracy_client_sys::TRACY_EMBEDDED_CAPTURE_ABI_VERSION
        ))
    }
}

fn configure(path: &Path) -> Result<(), String> {
    let path = path.to_str().ok_or("capture path must be UTF-8")?;
    let status = unsafe {
        tracy_client_sys::___tracy_embedded_capture_configure(
            path.as_ptr().cast(), path.len(), CAPACITY, MEMORY_LIMIT,
        )
    };
    if status == tracy_client_sys::TRACY_EMBEDDED_CAPTURE_OK { Ok(()) } else { Err(capture_error(status)) }
}

fn wait_until_capturing() -> Result<(), String> {
    let deadline = Instant::now() + READY_TIMEOUT;
    loop {
        let state = unsafe { tracy_client_sys::___tracy_embedded_capture_get_state() };
        if state == tracy_client_sys::TRACY_EMBEDDED_CAPTURE_CAPTURING { return Ok(()); }
        if state == tracy_client_sys::TRACY_EMBEDDED_CAPTURE_FAILED { return Err(capture_error(state)); }
        if Instant::now() >= deadline { return Err(format!("timed out waiting for capture; state={state}")); }
        std::thread::sleep(Duration::from_millis(1));
    }
}

fn capture_error(status: i32) -> String {
    let length = unsafe { tracy_client_sys::___tracy_embedded_capture_get_error(std::ptr::null_mut(), 0) };
    let mut bytes = vec![0_u8; length + 1];
    unsafe { tracy_client_sys::___tracy_embedded_capture_get_error(bytes.as_mut_ptr().cast(), bytes.len()); }
    format!("embedded capture status {status}: {}", String::from_utf8_lossy(&bytes[..length]))
}

fn configuration_failure(error: String) -> ! {
    eprintln!("Tracy nextest capture setup/finalization failed: {error}");
    std::process::exit(70)
}

fn finalization_failure(error: &str, payload: &(dyn Any + Send)) -> ! {
    let context = if let Some(message) = payload.downcast_ref::<&'static str>() {
        format!("panic payload: {message}")
    } else if let Some(message) = payload.downcast_ref::<String>() {
        format!("panic payload: {message}")
    } else {
        "non-string panic payload".into()
    };
    eprintln!("capture finalization failed while handling original {context}: {error}");
    std::process::exit(70)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn policies_are_strict() {
        assert_eq!(parse_policy(None).unwrap(), Policy::Off);
        assert_eq!(parse_policy(Some("failure".into())).unwrap(), Policy::Failure);
        assert!(parse_policy(Some("sometimes".into())).is_err());
    }

    #[test]
    fn names_are_confined_and_unique() {
        let root = Path::new("root");
        let first = output_path(root, "../binary", "../../bad:test", "1", "id-one".as_ref());
        let second = output_path(root, "../binary", "../../bad:test", "1", "id-two".as_ref());
        assert_eq!(first.parent(), Some(root));
        assert_ne!(first, second);
        assert!(!first.file_name().unwrap().to_string_lossy().contains('/'));
    }
}
