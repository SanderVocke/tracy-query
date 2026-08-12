use std::io::{Read, Write};
use std::net::TcpStream;
use std::time::{Duration, Instant};

const MAGIC: &[u8; 4] = b"TCOL";
const VERSION: u16 = 1;

fn string_field(out: &mut Vec<u8>, value: &str) {
    let bytes = value.as_bytes();
    assert!(bytes.len() <= 4096);
    out.extend_from_slice(&(bytes.len() as u16).to_be_bytes());
    out.extend_from_slice(bytes);
}

fn take_string(data: &[u8], offset: &mut usize) -> String {
    let length = u16::from_be_bytes(data[*offset..*offset + 2].try_into().unwrap()) as usize;
    *offset += 2;
    let result = std::str::from_utf8(&data[*offset..*offset + length]).unwrap().to_owned();
    *offset += length;
    result
}

fn request(kind: u16, fields: &[u8]) -> Vec<u8> {
    let endpoint = std::env::var("TRACY_COLLECTOR_ENDPOINT").unwrap();
    let token = std::env::var("TRACY_COLLECTOR_TOKEN").unwrap();
    let mut payload = Vec::new();
    string_field(&mut payload, &token);
    payload.extend_from_slice(fields);
    let mut frame = Vec::new();
    frame.extend_from_slice(MAGIC);
    frame.extend_from_slice(&VERSION.to_be_bytes());
    frame.extend_from_slice(&kind.to_be_bytes());
    frame.extend_from_slice(&(payload.len() as u32).to_be_bytes());
    frame.extend_from_slice(&payload);
    let mut stream = TcpStream::connect(endpoint).unwrap();
    stream.set_read_timeout(Some(Duration::from_secs(10))).unwrap();
    stream.write_all(&frame).unwrap();
    let mut header = [0_u8; 12];
    stream.read_exact(&mut header).unwrap();
    assert_eq!(&header[..4], MAGIC);
    assert_eq!(u16::from_be_bytes(header[4..6].try_into().unwrap()), VERSION);
    assert_eq!(u16::from_be_bytes(header[6..8].try_into().unwrap()), kind | 0x8000);
    let length = u32::from_be_bytes(header[8..12].try_into().unwrap()) as usize;
    let mut response = vec![0; length];
    stream.read_exact(&mut response).unwrap();
    let status = u16::from_be_bytes(response[..2].try_into().unwrap());
    let mut offset = 2;
    let message = take_string(&response, &mut offset);
    assert_eq!(status, 0, "collector status {status}: {message}");
    response[offset..].to_vec()
}

/// Opt-in startup hook. It is inert under ordinary cargo test and discovery.
fn collector_startup_hook() -> Option<tracy_client::Client> {
    let attempt_id = std::env::var("NEXTEST_ATTEMPT_ID").ok()?;
    let run_id = std::env::var("TRACY_COLLECTOR_RUN_ID")
        .expect("collector run ID must accompany NEXTEST_ATTEMPT_ID");
    let binary = std::env::var("NEXTEST_BINARY_ID").unwrap_or_else(|_| env!("CARGO_PKG_NAME").into());
    let test_name = std::env::var("NEXTEST_TEST_NAME").unwrap_or_else(|_| "unknown-test".into());
    let attempt = std::env::var("NEXTEST_ATTEMPT")
        .ok().and_then(|value| value.parse::<u32>().ok()).unwrap_or(1);
    let stress = match (std::env::var("NEXTEST_STRESS_CURRENT"),
                        std::env::var("NEXTEST_STRESS_TOTAL")) {
        (Ok(current), Ok(total)) => format!("{current}/{total}"),
        _ => String::new(),
    };

    let mut registration = Vec::new();
    string_field(&mut registration, &run_id);
    string_field(&mut registration, &attempt_id);
    string_field(&mut registration, &binary);
    string_field(&mut registration, &test_name);
    registration.extend_from_slice(&attempt.saturating_sub(1).to_be_bytes());
    string_field(&mut registration, &stress);
    let response = request(3, &registration);
    let mut offset = 0;
    let session = take_string(&response, &mut offset);
    let port = u16::from_be_bytes(response[offset..offset + 2].try_into().unwrap());

    // delayed-init guarantees this is set before Tracy initializes.
    std::env::set_var("TRACY_PORT", port.to_string());
    let client = tracy_client::Client::start();
    // delayed-init starts the listener on first instrumentation. This startup
    // marker is intentionally not relied on as captured data.
    client.message("nextest collector startup", 0);
    let deadline = Instant::now() + Duration::from_secs(10);
    loop {
        let mut status = Vec::new();
        string_field(&mut status, &session);
        let response = request(4, &status);
        let mut offset = 0;
        let state = take_string(&response, &mut offset);
        let handshake = take_string(&response, &mut offset);
        if state == "capturing" {
            break;
        }
        assert!(Instant::now() < deadline,
                "collector did not connect: state={state} handshake={handshake}");
        std::thread::sleep(Duration::from_millis(10));
    }
    let marker = format!("nextest:{test_name}:attempt:{attempt}:id:{attempt_id}");
    // Repetition makes abrupt termination deterministic even while several
    // Tracy clients are concurrently resolving their first dynamic string.
    for _ in 0..100 {
        client.message(&marker, 0);
        std::thread::sleep(Duration::from_millis(20));
    }
    Some(client)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn passes() {
        let client = collector_startup_hook();
        if let Some(client) = client {
            let _span = client.span_alloc(Some("nextest.pass"), "passes", file!(), line!(), 0);
            std::thread::sleep(Duration::from_millis(30));
        }
    }

    #[test]
    fn assertion_failure() {
        let _client = collector_startup_hook();
        std::thread::sleep(Duration::from_millis(30));
        panic!("intentional assertion fixture");
    }

    #[test]
    fn aborts() {
        let _client = collector_startup_hook();
        std::thread::sleep(Duration::from_millis(30));
        std::process::abort();
    }

    #[test]
    fn times_out() {
        let _client = collector_startup_hook();
        std::thread::sleep(Duration::from_secs(10));
    }

    #[test]
    fn retry_then_passes() {
        let _client = collector_startup_hook();
        std::thread::sleep(Duration::from_millis(30));
        let attempt = std::env::var("NEXTEST_ATTEMPT")
            .ok().and_then(|value| value.parse::<u32>().ok()).unwrap_or(1);
        assert!(attempt > 1, "intentional first-attempt retry failure");
    }
}
