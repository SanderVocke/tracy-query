# Tracy nextest in-process per-test capture plan

## Status and execution contract

This plan is implemented and audited. Paths are relative to the `tracy-query`
repository.

- Current status: implementation complete and audited; all local and CI gates pass.
- Keep this plan updated as work progresses and check off completed items.
- Commit each completed stage or meaningful milestone.
- Implementation steps may be revised when new evidence warrants it.
- Design rules may be revised for a documented, well-supported reason.
- Goals and acceptance criteria must not be changed without explicit user approval.
- Record verification commands and CI/artifact links as stages complete.

## Purpose

Integrate the in-process Tracy 0.13.1 backend with cargo-nextest's
process-per-test model. Each opted-in test attempt owns one embedded Tracy
client/Worker pair and retains its model in bounded process memory. At an
explicit test boundary:

- success drains and discards the model without opening a trace file;
- an unwind panic or `Result::Err` drains and atomically publishes one trace,
  then preserves the original test failure for nextest.

```text
cargo nextest
├── attempt A process -> embedded Tracy -> pass -> discard
├── attempt B process -> embedded Tracy -> unwind panic -> save -> resume panic
└── attempt C process -> embedded Tracy -> Result::Err -> save -> return Err
```

Abort, signals, nextest timeout termination, OOM, `SIGKILL`, power loss, and
finalizer failure cannot reliably run an in-process finalizer. The existing
external `tracy-collector` remains the solution when those failures must be
retained.

## Existing foundation and key constraint

- cargo-nextest 0.9.116 provides process-per-test execution and per-attempt
  identity variables including `NEXTEST_TEST_NAME` and `NEXTEST_ATTEMPT_ID`.
  The repository already pins this version in its external collector fixture.
- `examples/rust-embedded-capture` proves the required
  `catch_unwind -> drop guards -> finish -> resume_unwind` ordering with
  unmodified `tracy-client` 0.18.4 and `tracing-tracy` 0.11.4.
- `___tracy_embedded_capture_finish()` currently always writes. Fail-only mode
  requires an orderly finalize-and-discard operation that never enters file
  publication code.
- Outcome selection must happen inside the test process before it exits. JUnit
  reconciliation is too late because the in-memory Worker model is then gone.
- Ordinary `cargo test` can execute several tests in one process and is
  incompatible with the one-capture-per-process lifecycle. The integration must
  be inert unless a nextest attempt identity is present.

## Goals and scope

1. Add a disposition-aware native finalizer supporting save and no-write discard.
2. Add a repository-local Rust test helper, with an attribute macro if needed,
   for synchronous tests returning `()` or `Result<(), E>`.
3. Support `off`, `failure`, and `always` policies; `failure` is the target mode.
4. Preserve panic payloads, `Result::Err` values, and nextest exit/retry behavior.
5. Finalize only after test-owned Tracy zones, tracing spans/dispatchers, and
   producer threads are quiescent.
6. Generate path-confined, collision-resistant filenames from nextest attempt
   identity so concurrent tests and retries cannot overwrite one another.
7. Validate direct `tracy-client` and unmodified `tracing-tracy` events in saved
   per-test captures.
8. Keep existing query, collector, external-nextest, static-link, and release
   behavior passing on the six-platform matrix.
9. Document when to use in-process fail-only capture versus the external daemon.

## Non-goals

- Do not modify cargo-nextest, libtest, `tracy-client`, or `tracing-tracy`.
- Do not add a daemon, helper process, network port, or per-test process wrapper.
- Do not infer retention from JUnit after process exit.
- Do not finalize from a panic hook, destructor, or while guards are active.
- Do not support ordinary multi-test-per-process `cargo test` capture.
- Do not claim traces for aborts, signals, timeouts, forced termination, OOM,
  double panic, or finalizer failure.
- Do not initially support async/custom harnesses, parameterized-test frameworks,
  subprocess tests, or `#[should_panic]`; reject unsupported combinations clearly.
- Do not make discarded successes recoverable later or publish the helper crate
  to crates.io in this milestone.

## Immutable acceptance criteria

1. Every active attempt uses exact Tracy 0.13.1/protocol 76 over the existing
   bounded serialized duplex memory transport, with no network/helper process.
2. Under `failure`, passing `()` and `Ok(())` tests create no trace, partial, or
   sidecar file.
3. Discard performs the same orderly client drain and Worker shutdown as save,
   destroys the model, and provably never invokes `FileWrite::Open`,
   `Worker::Write`, rename, or output creation.
4. Panic tests execute inside `catch_unwind`; unwinding drops test locals and
   guards, the controller emits a panic-caught marker, publishes one valid trace,
   and resumes the original payload.
5. `Result::Err` publishes one valid trace and returns the original error.
6. Finalizer failure reports capture and original-test context without a second
   panic and publishes no misleading final file.
7. Saved traces pass `tracy-query check`, `range`, and `info`; semantic queries
   prove attempt identity and pre-failure direct/tracing events.
8. Concurrent attempts and retries produce unique no-overwrite filenames from a
   bounded sanitized display fragment plus a digest of `NEXTEST_ATTEMPT_ID`.
   Test metadata cannot escape the configured output root.
9. Without a complete nextest attempt identity, including cargo test and
   list/discovery, the helper performs no capture or file operation.
10. `off`, `failure`, and `always` have deterministic precedence and validation;
    invalid configuration fails before the test body with an actionable error.
11. Enabled capture requires `panic=unwind`. Async tests, `#[should_panic]`, and
    incompatible harness attributes are rejected rather than misclassified.
12. Instrumentation-producing fixture threads are joined and guards dropped
    before finalization; consumers receive the same documented precondition.
13. Nextest remains the direct supervisor. Pass suites exit zero; panic/`Err`
    suites retain normal failure status; retries, fail-fast, and scheduling stay
    nextest-owned.
14. Existing external collector tests remain passing, including abort and timeout
    retention as the documented fallback.
15. Native, Rust, and process-level tests pass on Linux, macOS, and Windows
    x86-64/ARM64. Linux ASan/UBSan covers save and discard.
16. Documentation includes Cargo patch/features, test annotation, nextest policy
    and output variables, CI artifact upload, safety rules, observer effect, and
    unsupported failures.

## Design rules and constraints

### Outcome boundary

- Keep the capture owner outside `catch_unwind`; invoke the complete test body
  inside it and finalize only after unwinding returns control.
- Generate explicit wrappers for `()` and `Result<(), E>` and preserve their
  original failure values. Do not classify via formatted output or panic hooks.
- Treat `#[should_panic]` as unsupported initially because libtest can convert an
  expected panic into success outside a naive wrapper.
- Scope any helper-owned tracing dispatcher inside the protected body. Consumer
  threads and guards remain the consumer's quiescence responsibility.

### Native lifecycle

- Add a disposition-aware C ABI operation with stable save/discard constants.
  Keep `___tracy_embedded_capture_finish()` as a save-compatible wrapper.
- Share validation, shutdown, protocol drain, metadata completion, disconnect,
  and join logic. Branch only after shutdown: save writes atomically; discard
  destroys the Worker directly.
- Represent discarded completion distinctly from published completion. Never
  hold coordinator or transport locks while joining/destroying Tracy work.
- Add writer-entry test hooks/counters to prove discard never touches output.

### Rust integration and activation

- Add a small repository-local lifecycle crate and a proc-macro companion only
  if required for robust test expansion. Existing instrumentation APIs remain
  unchanged.
- Patch only `tracy-client-sys` through `[patch.crates-io]`; activate
  `embedded-capture` there and `manual-lifetime` on higher-level crates.
- Activate only when policy enables capture and nextest provides complete
  attempt identity. Missing identity is a strict no-op.
- Reject non-unwind enabled builds at compile time.

### Identity, files, and policy

- Define `TRACY_NEXTEST_CAPTURE=off|failure|always` and
  `TRACY_NEXTEST_OUTPUT_DIR`. Require a fresh, valid output directory.
- Build filenames from bounded sanitized binary/test text plus a digest of the
  opaque attempt ID. Reject separators, traversal, drive prefixes, and reserved
  names from metadata.
- Embed attempt/test/retry markers in trace data so content independently proves
  association.
- Do not coordinate concurrent processes through a shared mutable manifest.
  If sidecars are later necessary, publish one atomically only beside a saved
  trace, never for discarded success.
- Bound readiness/finalization waits and include attempt identity, state, and
  native diagnostics in errors.

### Compatibility and verification

- Reuse the existing sys fork, native target, Tracy pin, warning policy, and
  static runtime behavior.
- Keep `tests/nextest-fixture` as the network collector control and add a separate
  in-process fixture with independent features/environment.
- Use process-level tests for one-shot global lifecycles and `tracy-query`
  semantic validation as the capture oracle.
- Measure pass/failure wall time, CPU, peak RSS, binary size, channel high-water
  marks, and output size without promising an overhead bound.

## Staged implementation

### Stage 1 — Freeze test, activation, and ABI contracts

Dependencies: completed in-process capture backend.

- [x] Add an architecture document covering supported signatures, policies,
  paths, panic/`Err` ordering, and external-collector fallback.
- [x] Freeze the disposition C ABI, terminal states, Rust helper API, and
  compatibility behavior of the current finish function.
- [x] Add compile fixtures for `()`, `Result<(), E>`, async, `#[should_panic]`,
  incompatible attributes, and non-unwind configuration.
- [x] Add a minimal manual nextest proof that a resumed panic is reported only
  after in-process publication completes.
- [x] Record and test nextest 0.9.116 process/environment assumptions.

Verification:

- [x] Review wrapper behavior against libtest `Termination`, panic, and existing
  Rust example behavior.
- [x] Prove cargo test and nextest list/discovery remain inert.
- [x] Confirm no retention decision depends on post-exit JUnit data.

### Stage 2 — Implement native save/discard finalization

Dependencies: Stage 1 ABI decision.

- [x] Refactor shutdown into a common drain/disconnect phase followed by a
  save/discard disposition.
- [x] Implement the additive C ABI operation and keep current finish-as-save.
- [x] Add discarded terminal state and deterministic invalid/duplicate errors.
- [x] Add writer/write/publish hooks or counters proving discard avoids disk.
- [x] Cover no-data, invalid disposition, transport timeout, output races, writer
  failure, and cleanup without exceptions crossing FFI.
- [x] Update sys bindings and ABI/version assertions.

Verification:

- [x] Process fixtures prove save creates one valid file and discard creates no
  output/partial and records zero writer calls.
- [x] Repeated save/discard tests pass under ASan/UBSan.
- [x] Existing C++ and Rust normal/panic examples remain passing unchanged.

### Stage 3 — Implement the Rust test helper

Dependencies: Stage 2 native ABI.

- [x] Add lifecycle and optional proc-macro crates with locked manifests,
  provenance, licenses, and feature forwarding.
- [x] Implement nextest-only activation, policy parsing, output validation,
  filename generation, and attempt marker emission.
- [x] Start Tracy and wait for readiness before invoking the test body.
- [x] For `()`, discard normal return under `failure`; on unwind save, mark, and
  resume the original payload.
- [x] For `Result<(), E>`, discard `Ok` and save before returning original `Err`.
- [x] Implement `off`, `always`, bounded diagnostics, and non-double-panic
  finalizer-failure behavior.
- [x] Ensure helper-owned dispatchers/guards drop before finalization.

Verification:

- [x] Unit/compile tests cover policy, path confinement, retry uniqueness,
  no-op activation, supported signatures, and compile rejection.
- [x] `cargo tree -i tracy-client-sys` shows one patched package shared by helper,
  `tracy-client`, and `tracing-tracy`; `cargo tree -d` is empty.
- [x] Process fixtures preserve original panic and `Err` reporting.

### Stage 4 — Add a dedicated in-process nextest fixture

Dependencies: Stage 3 helper.

- [x] Add `tests/nextest-in-process-fixture/` with locked dependencies, patched
  sys, pinned nextest profile, and unwind strategy.
- [x] Include passing `()`, passing `Result`, panic, `Result::Err`, failed retry
  followed by pass, nested direct/tracing events, and joined producers.
- [x] Add exact identity/retry and pre-failure markers.
- [x] Exercise concurrent attempts and publication races.
- [x] Add a suite harness that invokes nextest directly, expects native status,
  and validates outputs without wrapping individual tests.
- [x] Keep abort/timeout as explicit negative cases and validate their retention
  only through the external collector control.

Verification:

- [x] `failure` writes no pass/successful-retry files and exactly one distinct
  valid trace for each panic, `Err`, and failed retry attempt.
- [x] `always` writes each supported executed attempt; `off` writes none.
- [x] Nextest continues later tests under `--no-fail-fast` and remains supervisor.

### Stage 5 — Semantic and failure-path validation

Dependencies: Stage 4 fixture.

- [x] Run `check`, `range`, `info`, and exact semantic queries on every saved
  capture for identity, direct-client, tracing-layer, panic, and `Err` markers.
- [x] Verify zones, spans, dispatchers, and producer threads are quiescent before
  finalization.
- [x] Inject output-exists, missing/unwritable directory, writer, transport, and
  finalization-timeout failures; assert no misleading final output.
- [x] Verify ignored/filtered tests and list/discovery perform no file operation.
- [x] Occupy Tracy ports to prove no network dependency.
- [x] Add explicit negative tests/documentation for abort, timeout, async,
  custom harnesses, `#[should_panic]`, and `panic=abort`.

Verification:

- [x] Repeat panic/`Err` suites under bounded timeouts with no partials or helper
  processes left behind.
- [x] Compare in-process and external collector behavior and document why hard
  failures differ.
- [x] Run native and Rust save/discard paths under Linux ASan/UBSan.

### Stage 6 — Cross-platform CI and regressions

Dependencies: Stages 2–5 complete.

- [x] Add native disposition and Rust process fixtures to all Linux/macOS/Windows
  x86-64/ARM64 jobs.
- [x] Run the pinned in-process nextest suite on every supported runner; use an
  approved equivalent process fixture only where nextest is unavailable.
- [x] Extend Linux sanitizer CI with repeated discard, panic-save, Err-save,
  concurrent-attempt, and publication-failure tests.
- [x] Keep external collector nextest/JUnit, query/collector, install/static, and
  release tests passing.
- [x] Add bounded timeouts and failure-only diagnostic artifact publication.

Verification:

- [x] Six-platform, sanitizer, external-nextest, and in-process-nextest jobs pass
  on one exact commit.
- [x] Linux static and Windows static-runtime guarantees remain intact.
- [x] Matrix Cargo resolution contains exactly one patched sys package.

### Stage 7 — Documentation and final demonstration

Dependencies: stable API and CI behavior.

- [x] Document exact Cargo patch/features, test annotation/wrapper, nextest
  profile, policy/output variables, CI artifact upload, and query commands.
- [x] Document output-root freshness, naming/retries/concurrency, quiescence,
  unwind-only support, finalizer failure, and unsupported hard failures.
- [x] Add a decision table comparing in-process fail-only capture with
  `tracy-collector`.
- [x] Record observer-effect measurements and upgrade steps for nextest
  assumptions, macro expansion, C ABI bindings, and Tracy pin.
- [x] From a clean checkout, run native/Cargo tests and both nextest architectures
  under `off`, `failure`, and `always`.
- [x] Prove discard performs zero writer operations and failure mode publishes
  only unique, valid, semantically associated failure traces with no partials.
- [x] Record final six-platform, sanitizer, static/install, dependency, and
  external-collector evidence.
- [x] Audit every immutable criterion before declaring completion.

## Completion evidence

- Native ABI and no-write proof: `include/tracy_embedded_capture/embedded_capture.h`,
  `src/embedded/embedded_capture.cpp`, `tests/embedded_capture_errors.cpp`, and
  `tests/fixtures/tracy_embedded_fixture.cpp` cover ABI v2, save compatibility,
  discarded terminal state, invalid/repeated operations, output races, I/O
  cleanup, and zero writer/write/publish counters for discard.
- Rust boundary: `rust/tracy-nextest-capture{,-macros}` provides the unwind-only
  wrappers with locked manifests, explicit provenance, and MIT/Apache license
  files. Unit and compile-contract tests cover strict policies, confined unique
  names, both supported signatures, and rejection of async, `#[should_panic]`,
  unsupported return types, and `panic=abort`.
- Process contract: `tests/nextest_in_process.py` and
  `tests/nextest_failure_modes.py` directly invoke pinned cargo-nextest 0.9.116,
  prove ordinary cargo/list/incomplete identity inertness, policy behavior,
  retries/concurrency, preserved panic and `Err`, controlled finalizer failure,
  occupied network ports, no partials, and exact per-attempt semantic
  `check`/`range`/`info`/query validity. A test-only native timeout injection
  proves the transport-error path performs no writer/write/publish operation.
  `tests/nextest_orchestrator.py` remains the abort/timeout fallback.
- Dependency topology: local `cargo tree -i tracy-client-sys` reports one patched
  0.28.0 package shared by the helper, `tracy-client` 0.18.4, and
  `tracing-tracy` 0.11.4; `cargo tree -d` is empty.
- Local final contract: four `nextest-in-process-*` CTests passed, including all
  `off`, `failure`, `always`, and injected failure modes. Helper unit and
  compile-fail tests passed.
- CI: final implementation commit `4bbdf83` passed sanitizer, pinned
  external-nextest, and Linux/macOS/Windows x86-64/ARM64 jobs at
  <https://github.com/SanderVocke/tracy-query/actions/runs/31687190388>.
- ThreadSanitizer is unavailable locally because the runtime terminates with
  `FATAL: ThreadSanitizer: unexpected memory mapping`; deterministic concurrency
  tests and CI ASan/UBSan are the supported sanitizer evidence.

## Expected end-to-end commands

Exact package/profile names may be refined in Stage 1 without weakening the
acceptance criteria:

```sh
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON \
  -DTRACY_QUERY_FULLY_STATIC=OFF
cmake --build build --parallel
ctest --test-dir build --output-on-failure

cargo tree --manifest-path tests/nextest-in-process-fixture/Cargo.toml \
  -i tracy-client-sys

rm -rf out/nextest-in-process
mkdir -p out/nextest-in-process
TRACY_NEXTEST_CAPTURE=failure \
TRACY_NEXTEST_OUTPUT_DIR="$PWD/out/nextest-in-process" \
  cargo nextest run \
    --manifest-path tests/nextest-in-process-fixture/Cargo.toml \
    --profile tracy-in-process --no-fail-fast

# The fixture intentionally fails. Only finalized failure captures should exist.
for trace in out/nextest-in-process/*.tracy; do
  build/tracy-query check "$trace"
  build/tracy-query range "$trace"
  build/tracy-query info "$trace"
  build/tracy-query query --kind cpu-zone,message \
    --filter 'message.text=nextest-in-process|panic-caught|result-error' \
    "$trace"
done

test -z "$(find out/nextest-in-process -name '*.partial' -print -quit)"
```
