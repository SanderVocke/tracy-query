# cargo-nextest in-process failure capture

The repository-local `tracy-nextest-capture` helper gives each cargo-nextest
attempt its own bounded in-process Tracy 0.13.1 capture. It writes on an unwind
panic or `Result::Err` and drains/discards successful traces without opening an
output file.

## Configuration

Use exact unmodified `tracy-client` 0.18.4 and `tracing-tracy` 0.11.4, patching
only `tracy-client-sys` 0.28.0. Enable `manual-lifetime` on both higher-level
crates and `embedded-capture` on the sys crate. See
`tests/nextest-in-process-fixture/Cargo.toml` for a complete manifest.

Annotate synchronous tests returning `()` or `Result<(), E>`:

```rust
use tracy_nextest_capture::tracy_capture_test;

#[tracy_capture_test]
fn example() {
    let client = tracy_client::Client::running().unwrap();
    client.message("example marker", 0);
}
```

Set a fresh existing output directory and policy:

```sh
mkdir -p out/nextest-traces
TRACY_NEXTEST_CAPTURE=failure \
TRACY_NEXTEST_OUTPUT_DIR="$PWD/out/nextest-traces" \
  cargo nextest run --profile tracy-in-process --no-fail-fast
```

Policies are `off`, `failure`, and `always`. Missing
`NEXTEST_ATTEMPT_ID` is always inert, including ordinary `cargo test` and list
operations. Filenames contain bounded sanitized test metadata and a SHA-256
prefix of the opaque nextest attempt ID, so retries and concurrent attempts do
not collide. Existing outputs are never overwritten.

## Lifecycle and safety

The attribute starts capture before the test body and scopes that body inside
`catch_unwind`. On panic, Rust first drops body locals, zones, spans, and scoped
dispatchers; the helper then emits a panic marker, finalizes, and resumes the
original payload. `Result::Err` is finalized before returning the original
error. Passing tests use the native discard disposition, which performs normal
Tracy drain/termination but never opens a trace writer.

Tests must join every instrumentation-producing thread and drop all guards
before returning. Async tests, custom harnesses, parameterized frameworks, and
`#[should_panic]` are not supported initially. Capture requires `panic=unwind`.
A finalizer failure reports context and exits 70 rather than starting a second
panic.

Abort, fatal signals/exceptions, nextest timeout termination, `SIGKILL`, OOM,
`_exit`, power loss, and double panic cannot reliably finalize in process. Use
`tracy-collector` when retaining these failures is required. The external
collector costs a daemon and loopback connection but survives the test process;
in-process capture avoids those components but can only publish controlled
outcomes.

## CI artifacts

Upload only finalized traces, with `if: always()` if failures should retain them:

```yaml
- uses: actions/upload-artifact@v7
  if: always()
  with:
    name: tracy-nextest-failures
    path: out/nextest-traces/*.tracy
    if-no-files-found: ignore
```

Validate artifacts with `tracy-query check`, `range`, `info`, and semantic
queries. `.partial` files indicate an interrupted/failed finalizer and must not
be published as traces.
