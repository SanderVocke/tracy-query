# tracy-nextest-capture

Failure-only in-process Tracy capture for synchronous cargo-nextest tests. Each opted-in nextest attempt owns one embedded capture lifecycle. Passing attempts drain and discard without opening a writer; unwind panics and `Result::Err` atomically save a trace before preserving the original test result.

## Deliverables

- `crates/tracy-nextest-capture`: lifecycle/runtime API.
- `crates/tracy-nextest-capture-macros`: `#[tracy_capture_test]` attribute.
- `tests/fixture`: pinned cargo-nextest 0.9.116 process-per-test fixture.
- `tests/*.py`: policy, semantic trace, retry/concurrency, no-op, and failure-mode contracts.

The component consumes [`tracy-embedded-capture`](../tracy-embedded-capture/) and uses [`tracy-query`](../tracy-query/) as its trace oracle.

## Use

```rust
use tracy_nextest_capture::tracy_capture_test;

#[tracy_capture_test]
fn profiled_test() {
    let client = tracy_client::Client::running().unwrap();
    client.message("test marker", 0);
}
```

```sh
mkdir -p out/nextest-traces
TRACY_NEXTEST_CAPTURE=failure \
TRACY_NEXTEST_OUTPUT_DIR="$PWD/out/nextest-traces" \
  cargo nextest run --profile tracy-in-process --no-fail-fast
```

Policies are `off`, `failure`, and `always`. See [usage/policy](docs/usage.md) and [architecture/testing](docs/architecture.md).

## Supported boundary

Initially supported: synchronous `()` and `Result<(), E>` libtest tests with `panic=unwind`. Async tests, `#[should_panic]`, incompatible custom/parameterized harnesses, panic-abort, and unsupported returns are rejected. Ordinary multi-test-per-process `cargo test` and incomplete nextest identity are strict no-ops.
