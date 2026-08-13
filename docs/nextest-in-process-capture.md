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

## Upgrade and observer-effect notes

Nextest 0.9.116's process-per-test model and `NEXTEST_ATTEMPT_ID`,
`NEXTEST_TEST_NAME`, `NEXTEST_BINARY_ID`, and `NEXTEST_ATTEMPT` variables are
part of the tested contract. On a nextest upgrade, rerun list/no-op, retries,
concurrency, and all three policy fixtures and inspect generated macro wrappers
against libtest `Termination` behavior. On a lifecycle ABI change, update
`embedded_capture.h`, generated sys declarations, ABI assertions, helper calls,
and native save/discard fixtures together. Tracy pin upgrades also require the
full protocol/shutdown audit in `docs/in-process-capture.md`.

Representative local Linux Debug measurements on 2026-08-13 used
`/run/current-system/sw/bin/time -f 'wall=%e user=%U sys=%S rss_kib=%M'` around
pinned nextest. A warm capture-off two-pass run measured 1.01 s wall, 1.04 s user,
0.66 s system, and 198,140 KiB process-tree peak RSS (including concurrent Cargo
bookkeeping). A one-test unwind-failure run measured 0.82 s wall, 0.52 s user,
0.33 s system, and 34,112 KiB peak RSS; its trace was 2,010 bytes and its Debug
test executable was 43,311,408 bytes. The native transport statistics API in a
representative embedded fixture reported 1,187-byte client-to-server and
104-byte server-to-client channel high-water marks; see
`docs/in-process-capture.md` for that measurement's full environment and
methodology.

These observations are not an overhead comparison or guarantee: the runs have
different test counts and outcomes, and symbols, enabled instrumentation,
channel occupancy, scheduling, test duration, Cargo state, and platform dominate
all measurements. Repeat both policies with an external timing/RSS tool and read
the transport statistics API for the consumer's actual workload before
increasing traced-test concurrency.
