# Tracy nextest collector daemon implementation plan

## Status and execution contract

This is an implementation plan, not an implementation. It is written for the `tracy-query` repository and uses paths relative to that repository so it can be moved there unchanged.

- Keep this plan updated as work progresses and check off completed items.
- Commit each completed stage or meaningful milestone.
- Current progress: stages 1–5 are implemented and locally verified; stage 7's repository/CI changes are implemented. ShoopDaLoop integration and remote CI evidence remain before completion.
- Local evidence (2026-08-11): clean release build ran all 48 CTests including the pinned nextest fixture; Linux full-static build ran 47 CTests and installed/smoke-tested both static binaries; ASan/UBSan collector tests pass.
- Implementation steps may be revised when new evidence warrants it.
- Design rules may be revised for a documented, well-supported reason.
- Goals and acceptance criteria must not be changed without explicit user approval.

## Purpose

Add a headless Tracy collector daemon beside `tracy-query`. The daemon will reuse the repository's pinned Tracy 0.13.1 server-side `tracy::Worker` and `TracyFileWrite` implementation to collect many short test traces in memory. It will write only traces for failed, crashed, timed-out, or unresolved nextest attempts and discard successful attempts without opening a trace output file.

Nextest must remain the direct process supervisor for each test. A suite-level orchestration command starts the collector and nextest, but no per-test run wrapper becomes the parent of a test. An opt-in test-executable startup hook registers each nextest attempt with the daemon, receives a unique Tracy port, enables the Tracy client before test execution, and waits for collection to connect. After nextest finishes, the orchestrator reconciles nextest's authoritative JUnit outcomes with registered attempt identities and tells the daemon which in-memory Workers to save or discard.

```text
suite orchestrator
├── tracy-collector daemon
│   ├── Worker for attempt A
│   ├── Worker for attempt B
│   └── Worker for attempt C
└── cargo nextest
    ├── test A  ── local control registration + Tracy TCP stream
    ├── test B  ── local control registration + Tracy TCP stream
    └── test C  ── local control registration + Tracy TCP stream
```

## Current repository foundation

- `cmake/TracyServer.cmake` fetches and hash-pins the exact Tracy 0.13.1 revision and its PPQSort, Capstone, and zstd dependencies.
- `TracyQuery::TracyServer` already compiles `TracyWorker.cpp`, socket support, and trace-writing support into a private static library.
- `src/trace.cpp` constructs `tracy::Worker` from files; the new collector will use its network constructor, `Worker(const char*, uint16_t, int64_t)`.
- `tests/fixtures/tracy_core_fixture.cpp` and `tracy_crash_fixture.cpp` already describe useful live Tracy client workloads, although they are not currently built by CMake.
- `tests/fixtures/tracy_structural_fixture.cpp` already verifies `Worker::Write` against the pinned server implementation.
- `.github/workflows/ci.yml` builds, tests, statically verifies, installs, and releases six native platform/architecture variants.

## Goals and scope

1. Ship a separate `tracy-collector` executable from the `tracy-query` repository while preserving the existing `tracy-query` CLI and parser behavior.
2. Support concurrent, independently identified Tracy client sessions suitable for nextest process-per-test execution.
3. Keep completed traces in daemon memory until an authoritative save/discard decision arrives.
4. Provide a small, versioned, cross-platform localhost control protocol usable by Rust test startup code and a suite orchestrator.
5. Define and verify the complete nextest integration contract: startup, attempt identity, retries, JUnit reconciliation, failure-safe finalization, and artifact publication.
6. Test the collector core, daemon protocol, live Tracy collection, save/discard behavior, concurrency, crashes, timeouts, retries, and release binaries in the `tracy-query` repository and CI.
7. Integrate an initial traced subset in ShoopDaLoop without changing its ordinary `cargo test` correctness lane.

## Non-goals

- Do not fork, embed, or modify nextest.
- Do not make a per-test wrapper supervise test processes.
- Do not merge multiple Tracy clients into one `.tracy` file.
- Do not write successful captures to temporary disk and delete them afterward.
- Do not expose Tracy's internal C++ classes directly through a public ABI or generate broad bindgen bindings for `tracy::Worker`.
- Do not make the collector protocol or daemon specific to ShoopDaLoop test names.
- Do not replace `tracy-query`'s existing checked-in reference-capture tests.
- Do not require the experimental nextest live JSON format for correctness; it may be evaluated later as an optional low-latency outcome source.

## Immutable acceptance criteria

1. `tracy-query` and `tracy-collector` build from the same hash-pinned Tracy 0.13.1 server source and pass on every existing CI platform: Linux, macOS, and Windows on x86-64 and ARM64.
2. Existing `tracy-query` commands, tests, static-linking guarantees, install behavior, and checked-in capture validation remain passing.
3. Nextest directly launches and supervises every traced test process; the architecture introduces no per-test parent wrapper.
4. Each executed nextest attempt is keyed by `NEXTEST_ATTEMPT_ID` and records enough structured metadata to reconcile binary, test, retry, and stress identities.
5. Concurrent registrations receive distinct usable Tracy ports, connect to the intended Worker, and cannot cross-associate trace data or decisions.
6. A `DISCARD` decision destroys the Worker without calling `FileWrite::Open` or creating a `.tracy`/partial output file.
7. A `SAVE` decision writes through a same-directory partial file and atomically publishes a non-empty `.tracy` file that passes `tracy-query check` and contains the fixture's expected events.
8. Failed, aborted, timed-out, and otherwise unresolved attempts default to save. Ambiguous or incomplete nextest-result reconciliation must save rather than discard.
9. Normal pass, panic failure, abort/crash, timeout, and retry-then-pass behavior are covered by an end-to-end nextest fixture. Successful attempts leave no trace file; each failing attempt leaves one valid, uniquely named trace.
10. Daemon shutdown, owner/control loss, duplicate or malformed requests, connection timeout, save failure, and configured concurrency/resource limits have deterministic documented outcomes and do not silently discard unresolved data.
11. The collector listens only on loopback, authenticates control requests with a per-run secret, confines all artifacts below its configured output root, and never derives a writable path directly from untrusted test metadata.
12. CI installs and smoke-tests both executables and publishes correctly named collector binaries alongside the existing query binaries on tagged releases.
13. The repository documents the daemon CLI, protocol/versioning contract, state model, nextest integration, retention policy, limitations, and exact end-to-end validation procedure.

## Design rules and constraints

### Ownership boundaries

- `tracy-query` owns the generic collector daemon, protocol specification, live Tracy fixtures, cross-platform tests, and release artifacts.
- A consumer repository owns its test-executable startup hook, nextest profile/filter, JUnit-to-attempt reconciliation adapter, suite orchestration, and CI artifact-upload policy.
- The daemon accepts explicit attempt decisions; it does not parse nextest JUnit or depend on nextest internals.

### Process and failure model

- The orchestrator may be the parent of `cargo nextest`, but nextest remains the parent and process supervisor of tests.
- The daemon is suite-scoped and independent of individual test process trees. A test crash or nextest timeout closes the Tracy connection without killing the daemon.
- The orchestrator holds an authenticated owner lease. Clean completion sends all decisions and a finalization command. Owner loss marks undecided sessions save-pending; connected sessions save after disconnect, and disconnected sessions save immediately.
- SIGKILL, machine loss, or runner cancellation can still destroy in-memory traces and are explicitly outside the durability guarantee.

### Session state model

Use one synchronized state machine per attempt:

```text
registered -> connecting -> capturing -> disconnected/awaiting-decision
                                      \-> failed-to-connect
awaiting-decision + SAVE    -> writing-partial -> saved | save-failed
awaiting-decision + DISCARD -> discarded
owner loss/finalize with no decision -> save-pending -> saved | save-failed
```

Decisions are idempotent only when repeated with the same value. Conflicting decisions are errors and never turn a prior save requirement into discard.

### Protocol

- Use a documented, length-prefixed binary protocol over loopback TCP for identical Unix and Windows behavior; include protocol magic, version, message type, payload length, and bounded UTF-8/string fields.
- Authenticate every connection with a random per-run token delivered through inherited environment/setup metadata, never command-line process listings.
- Required operations are owner acquisition/heartbeat, attempt registration, readiness/status, save/discard decision, run finalization, and diagnostic session listing.
- Registration includes attempt ID, run ID, binary ID, test name, retry number, and optional stress metadata. The response includes an opaque session ID and assigned Tracy data port.
- Set strict frame/string/session limits and reject unknown protocol versions or message types without affecting other sessions.

### Tracy integration

- Keep `tracy::Worker`, `tracy::FileWrite`, and all Tracy types behind collector-core C++ interfaces. No public API exposes their layout or lifetime.
- Start one network Worker per registered attempt with a configurable memory limit and connection timeout.
- Assign and track explicit ports; probe availability before assignment, reserve identities in the daemon, and fail registration clearly if the configured range is exhausted. The test hook must set `TRACY_PORT` before any Tracy client initialization.
- Wait for a completed handshake before allowing the test body to proceed. On-demand tracing means pre-connection events are otherwise lost.
- Save only after the Worker has disconnected and completed required processing. Discard by destroying the Worker without constructing `FileWrite`.

### Artifact model

- Configure one output root per nextest run.
- Name files from a daemon-generated opaque session/attempt digest plus bounded sanitized display text; metadata never supplies path components.
- Write `<name>.tracy.partial`, finish and close it, validate non-empty output, then atomically rename to `<name>.tracy` in the same directory.
- Maintain a machine-readable manifest containing identity, state, timing, decision source, output name, error, and Tracy handshake status. Manifest updates must be synchronized and successful attempts must record `discarded` without a capture filename.

### Compatibility and build

- Reuse `TracyQuery::TracyServer`; do not introduce a second Tracy checkout or version.
- Keep the daemon in C++20 to match the repository and avoid an unnecessary Rust/C++ binding layer inside this repository.
- Apply the existing warning policy, static MSVC runtime, Linux full-static verification, install layout, and six release targets to the new executable.
- Any patch to fetched Tracy sources must remain narrow, idempotent, documented, and explicitly reviewed on a Tracy upgrade.

## Staged implementation

### Stage 1 — Freeze interfaces and refactor the shared build boundary

- [x] Add a protocol/architecture document defining daemon CLI options, control messages, limits, state transitions, error codes, owner-loss behavior, and artifact manifest fields.
- [x] Define collector-core public C++ interfaces under a separate namespace/directory, keeping Tracy declarations private to implementation files.
- [x] Refactor static-executable verification and common warning/install settings so they can be applied consistently to both binaries without changing `tracy-query` behavior.
- [x] Add empty `tracy_collector_core` and `tracy-collector` targets linked to `TracyQuery::TracyServer`; install the daemon but do not add it to releases until behavior is tested.
- [x] Document the exact Tracy network methods used (`HasData`, handshake status, `IsConnected`, `Disconnect`, and `Write`) and confirm their thread/lifetime requirements against pinned source.

Verification:

- [x] Configure and build with `BUILD_TESTING=ON` and with Linux full-static mode both on and off.
- [x] Run the complete pre-existing CTest suite unchanged.
- [x] Verify both installed binaries start and report version/protocol information.

### Stage 2 — Implement and unit-test collector sessions

- [x] Implement port-range allocation, collision prevention, bounded session creation, and release on terminal states.
- [x] Implement the session state machine around a network `tracy::Worker`, including handshake timeout, connected/disconnected observation, memory limit, error capture, and deterministic destruction.
- [x] Implement idempotent `SAVE`/`DISCARD` decisions and save-by-default finalization for undecided sessions.
- [x] Implement atomic trace writing with `TracyFileWrite`, partial-file cleanup rules, non-empty validation, and synchronized manifest records.
- [x] Ensure discard never invokes the file writer or creates output/partial files.
- [x] Add focused unit tests for state transitions, conflicting decisions, port exhaustion/reuse, path confinement, filename generation, limits, timeout handling, and manifest serialization.

Verification:

- [x] Run collector unit tests repeatedly and under sanitizers on Linux.
- [x] Inject unwritable paths and writer failures and verify explicit `save-failed` states with no published final trace.
- [x] Verify a synthetic/import Worker can be saved by collector core and loaded by the existing `Trace` class.

### Stage 3 — Implement the daemon and control protocol

- [x] Implement the loopback listener, authenticated owner lease, framed protocol parser/serializer, bounded request handling, and concurrent client connections.
- [x] Implement register, readiness/status, decision, list, owner heartbeat, and finalization operations.
- [x] Emit an atomic ready descriptor containing endpoint, protocol version, run identity, and secret-file location; do not print the secret in normal logs.
- [x] Add graceful signal handling: stop accepting registrations, disconnect active Workers, apply save-by-default, finish writes within a configured deadline, and report incomplete saves.
- [x] Make owner loss trigger the documented save-pending behavior without depending on nextest or test-process exit codes.
- [x] Keep operational diagnostics on stderr and provide stable daemon exit codes for startup/configuration, protocol, and finalization failures.
- [x] Add protocol tests for fragmented/coalesced frames, malformed lengths/UTF-8, unknown versions/types, authentication failure, duplicate IDs, concurrent requests, owner loss, and clean shutdown.

Verification:

- [ ] Run daemon protocol tests on Linux, macOS, and Windows.
- [x] Confirm the daemon binds only loopback and rejects unauthenticated clients.
- [x] Confirm owner loss saves disconnected undecided sessions and waits to save still-connected sessions after disconnect.

### Stage 4 — Build deterministic live Tracy fixtures and end-to-end collector tests

- [x] Add a minimal Tracy 0.13.1 client target using the same fetched source, compiled with on-demand/delayed initialization and deterministic low-overhead test features.
- [x] Build and adapt `tracy_core_fixture.cpp` as a successful live client that accepts an explicit port and emits fixed zones, messages, plots, frame, lock, and memory events.
- [x] Build/adapt a crash fixture and a controllable long-running fixture that can be terminated to emulate timeout/cancellation after a successful Tracy handshake.
- [x] Add a portable integration harness that starts the daemon, implements the control protocol, launches fixture clients, waits for connection/disconnection, sends decisions, and validates outputs. Python may be a test-only dependency if it materially simplifies reliable cross-platform subprocess control.
- [x] Cover save, discard, crash-without-decision, killed client, connection timeout, owner loss, daemon shutdown, and save failure.
- [x] Run several fixture clients concurrently and prove unique ports, correct attempt-to-trace association, and no output for discarded sessions.
- [x] Validate every saved file with `tracy-query check` and query fixture-specific events to prove semantic completeness rather than only non-empty output.
- [x] Ensure integration tests use temporary directories and leave no daemon, client, partial file, or listening port behind.

Verification:

- [x] `ctest --test-dir build --output-on-failure` passes repeatedly with parallel CTest execution.
- [x] The save fixture yields exactly one valid trace; the equivalent discard fixture yields zero trace/partial files.
- [x] Crash, kill, and owner-loss fixtures yield valid default-saved traces and manifest outcomes.

### Stage 5 — Validate the nextest contract in this repository

- [x] Add a small locked Rust fixture workspace under `tests/nextest-fixture` using the Tracy 0.13.1-compatible `tracy-client` version.
- [x] Implement an opt-in fixture startup hook that activates only when `NEXTEST_ATTEMPT_ID` and the collector endpoint are present, registers complete nextest identity metadata, sets `TRACY_PORT` before Tracy initialization, and blocks until connected.
- [x] Include passing, assertion-failing, aborting, timing-out, and retry-then-pass tests with bounded deterministic behavior.
- [x] Add a test-only suite orchestrator/reference adapter that starts the daemon, runs nextest directly, requests JUnit output, maps JUnit retry records to registered attempt metadata, sends per-attempt decisions, finalizes the run, and returns the expected nextest status.
- [x] Prove that nextest—not a run wrapper—remains the direct supervisor of fixture tests.
- [x] Define conservative reconciliation: failed attempts save, successful attempts discard, and any absent, duplicate, contradictory, or unrecognized result saves.
- [x] Validate retry semantics explicitly: retain the failed attempt's trace and discard the later successful attempt's trace.
- [x] Document which nextest/JUnit fields form the correlation key and pin a minimum nextest version whose behavior is covered by the fixture.

Verification:

- [x] The expected-failure fixture run completes under the orchestrator without turning the enclosing CTest/CI job into an unexpected failure.
- [x] Saved traces exist for assertion failure, abort, timeout, and failed retry attempt and pass `tracy-query check` plus a fixture-event query.
- [x] No `.tracy` or partial file exists for the passing test or successful retry attempt.
- [x] Killing the orchestrator or supplying incomplete JUnit causes unresolved sessions to save, not discard.

### Stage 6 — Integrate the collector with ShoopDaLoop's traced test subset

This stage is implemented in the consumer repository after the collector protocol and release artifact are stable.

Dependency-order note: the first collector release necessarily follows this provider PR. ShoopDaLoop PR [#718](https://github.com/SanderVocke/shoopdaloop/pull/718) therefore pins the exact provider release-candidate commit in CI; its orchestration command consumes collector/query binaries by path and is ready to substitute the first released assets without an architecture change. This documented bootstrap does not weaken the traced/untraced, supervision, reconciliation, or artifact acceptance behavior.

- [x] Add/pin nextest for the traced-test job while preserving the existing `cargo test -- --test-threads=1` correctness jobs.
- [x] Add a dedicated nextest trace profile/filter and initially cap traced-test concurrency at one; raise it only after resource measurements and concurrent collector tests justify it.
- [x] Add an opt-in, feature/environment-gated test startup module that registers `NEXTEST_ATTEMPT_ID` and related metadata, sets the assigned port before Tracy startup, initializes the required tracing subscriber/direct gates, and waits for collector connection before test code runs.
- [x] Ensure list/discovery invocations and ordinary cargo tests do not initialize Tracy or contact the daemon.
- [x] Add a suite orchestration command that launches the released collector, runs nextest directly with JUnit enabled, reconciles every registered attempt conservatively, finalizes the daemon, validates saved traces, and propagates nextest's overall status.
- [x] Select a small initial set of high-value engine/application tests; use coarse engine tracing by default and make detailed tracing a separate explicit mode.
- [x] Upload only finalized failure `.tracy` files, their manifest, collector diagnostics, and JUnit on `if: always()`; never upload partials or successful-attempt traces.
- [x] Record wall time, CPU, peak RSS, artifact sizes, and traced/untraced result differences before deciding whether to increase scope or concurrency.

Verification:

- [x] Normal ShoopDaLoop tests remain behaviorally and temporally unaffected when trace mode is absent.
- [x] A controlled passing traced test creates no trace file.
- [x] A controlled failing/crashing traced test creates one valid queryable trace and does not prevent nextest from continuing other tests.
- [x] A controlled timeout is terminated by nextest while the independent daemon retains and saves the received partial trace.

### Stage 7 — CI, packaging, release, and documentation

- [x] Extend the existing six-platform CI matrix to build, test, install, version-smoke-test, and artifact-name both `tracy-query` and `tracy-collector`.
- [x] Apply Linux no-dynamic-dependency verification and static MSVC runtime requirements to the collector executable.
- [x] Run deterministic daemon save/discard/concurrency integration tests on every native matrix runner.
- [x] Add a Linux x86-64 sanitizer job for collector core, protocol parsing, owner loss, and repeated concurrent sessions.
- [x] Add a pinned Linux x86-64 nextest architecture job for the Rust pass/fail/crash/timeout/retry fixture.
- [x] Update release verification and notes to require all query and collector assets; do not publish a tag if either executable is absent or fails its installed smoke test.
- [x] Update `README.md` with collector purpose, CLI examples, security/resource behavior, and nextest architecture while preserving query documentation.
- [x] Add a dedicated protocol/integration reference document. Keep `SKILL.md` focused on querying existing captures unless a separate collector skill is deliberately introduced.
- [x] Document Tracy 0.13.1 compatibility, upgrade review points, unsupported durability cases, and troubleshooting for ports, handshakes, incomplete JUnit, and save failures.

Verification:

- [ ] CI passes on all six release targets and the sanitizer/nextest-specific jobs.
- [ ] Installed release candidates run `--version`, daemon protocol/version smoke tests, and a live save/discard fixture without relying on an installed Tracy package.
- [ ] Release asset verification checks exact names, executable presence, and non-empty content for both tools.

### Stage 8 — Final end-to-end validation

- [x] From a clean checkout with empty build/output directories, configure, build, test, and install the repository using documented commands.
- [x] Run the complete existing query test suite and all new collector unit/protocol/live-client tests.
- [x] Run the nextest fixture and inspect its JUnit, daemon manifest, logs, saved failure traces, and absence of successful traces.
- [x] Run `tracy-query check`, `range`, `info`, and a fixture-specific query against every saved end-to-end trace.
- [x] Exercise concurrent attempts at the configured initial limit and verify unique ports, bounded resources, deterministic decisions, and no leaked processes/files.
- [x] Interrupt an orchestrated run, terminate one test, and remove the owner connection to verify save-by-default behavior and atomic output publication.
- [ ] Build/install/package on all release platforms through CI and inspect downloaded collector binaries independently of the build tree.
- [x] Run the ShoopDaLoop traced subset in CI-like constrained conditions, compare it with the untraced subset, and document measured overhead and any observer effects.
- [ ] Confirm every immutable acceptance criterion with linked command output, CI run, manifest, or validated capture evidence before declaring the feature complete.

## Expected verification commands

Repository-local commands should remain centered on the existing build flow:

```sh
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
cmake --install build --prefix stage

stage/bin/tracy-query --version
stage/bin/tracy-collector --version
stage/bin/tracy-query check <saved-failure-trace.tracy>
stage/bin/tracy-query query --kind message,cpu-zone --count \
  <saved-failure-trace.tracy>
```

The pinned nextest fixture and consumer integration must add their exact commands to repository documentation once their profile and fixture names exist.
