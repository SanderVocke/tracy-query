#[cfg(test)]
mod tests {
    use std::error::Error;
    use std::fmt::{Display, Formatter};
    use std::time::Duration;
    use tracing_subscriber::prelude::*;

    use tracy_nextest_capture::tracy_capture_test;

    fn emit(marker: &str) {
        let Some(client) = tracy_client::Client::running() else { return; };
        let subscriber = tracing_subscriber::registry().with(tracing_tracy::TracyLayer::default());
        let dispatch = tracing::Dispatch::new(subscriber);
        tracing::dispatcher::with_default(&dispatch, || {
            let _direct = client.clone().span_alloc(
                Some("nextest-in-process.direct-zone"), "emit", file!(), line!(), 0,
            );
            client.message(marker, 0);
            let span = tracing::info_span!("nextest-in-process.tracing-span", marker);
            let _entered = span.enter();
            tracing::info!(marker, "nextest-in-process.tracing-event");
            let workers: Vec<_> = (0..2).map(|worker| {
                let client = client.clone();
                std::thread::spawn(move || client.message(&format!("nextest-in-process.worker:{worker}"), 0))
            }).collect();
            for worker in workers { worker.join().expect("producer must join"); }
            std::thread::sleep(Duration::from_millis(10));
        });
    }

    #[derive(Debug)]
    struct FixtureError;
    impl Display for FixtureError {
        fn fmt(&self, formatter: &mut Formatter<'_>) -> std::fmt::Result { formatter.write_str("intentional result fixture error") }
    }
    impl Error for FixtureError {}

    #[tracy_capture_test]
    fn passes_unit() {
        emit("nextest-in-process:pass-unit");
    }

    #[tracy_capture_test]
    fn passes_result() -> Result<(), FixtureError> {
        emit("nextest-in-process:pass-result");
        Ok(())
    }

    #[tracy_capture_test]
    fn panic_failure() {
        emit("nextest-in-process:panic-before");
        panic!("intentional nextest in-process panic");
    }

    #[tracy_capture_test]
    fn result_failure() -> Result<(), FixtureError> {
        emit("nextest-in-process:result-before");
        Err(FixtureError)
    }

    #[tracy_capture_test]
    fn retry_then_passes() {
        emit("nextest-in-process:retry-before");
        let attempt = std::env::var("NEXTEST_ATTEMPT").ok()
            .and_then(|value| value.parse::<u32>().ok()).unwrap_or(1);
        assert!(attempt > 1, "intentional first retry failure");
    }

    #[tracy_capture_test]
    #[ignore = "dedicated harness injects finalizer I/O failure"]
    fn finalizer_failure_during_panic() {
        emit("nextest-in-process:finalizer-failure-before");
        let output = std::env::var_os("TRACY_NEXTEST_OUTPUT_DIR").unwrap();
        std::fs::remove_dir_all(output).unwrap();
        panic!("intentional panic before finalizer failure");
    }

    #[test]
    #[ignore = "abort is intentionally unsupported by in-process finalization"]
    fn abort_is_unsupported() { std::process::abort(); }

    #[test]
    #[ignore = "nextest timeout is intentionally unsupported by in-process finalization"]
    fn timeout_is_unsupported() { std::thread::sleep(Duration::from_secs(60)); }
}
