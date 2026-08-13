# Tracy in-process capture implementation plan

## Status and execution contract

This is an implementation plan, not an implementation. Paths are relative to the
`tracy-query` repository.

- Current status: implementation complete and verified on 2026-08-13. Native,
  Rust, panic, collector/query regression, sanitizer, static-link, and six-target
  matrix evidence is recorded below.
- Keep this plan updated as work progresses and check off completed items.
- Commit each completed stage or meaningful milestone.
- Implementation steps may be revised when new evidence warrants it.
- Design rules may be revised for a documented, well-supported reason.
- Goals and acceptance criteria must not be changed without explicit user approval.
- Record verification commands and relevant CI/artifact links in this document as
  stages are completed.

## Purpose

Add an opt-in Tracy 0.13.1 client mode that captures profiling data entirely
inside the instrumented process and writes a normal `.tracy` file. The existing
Tracy instrumentation APIs remain the event producers. A background client
thread serializes those events using Tracy's existing protocol, a bounded
in-memory duplex transport carries the protocol in both directions, and an
in-process `tracy::Worker` builds the normal server-side trace model. Controlled
shutdown drains the client, disconnects the Worker, and publishes the capture
through `Worker::Write` and `TracyFileWrite`.

```text
application threads
    |
    | existing Tracy C/C++ instrumentation calls
    v
existing Tracy producer queues
    |
    v
existing client dequeue/serialization thread
    |
    | Tracy protocol + LZ4, over bounded memory instead of a socket
    v
in-process tracy::Worker threads
    |
    v
Worker::Write -> capture.tracy.partial -> atomic rename -> capture.tracy
```

Expose this implementation through a repository-maintained, patched
`tracy-client-sys` package. Cargo's `[patch.crates-io]` mechanism will substitute
that package beneath unmodified `tracy-client` and `tracing-tracy` crates. The
final milestone is a runnable Rust example which demonstrates direct
`tracy-client` events, `tracing` spans/events through `tracing-tracy`, normal
capture finalization, and capture finalization after an unwind panic.

## Foundation and investigation evidence

The repository already supplies the server-side and validation pieces needed by
this feature:

- [`cmake/TracyServer.cmake`](cmake/TracyServer.cmake) hash-pins Tracy 0.13.1
  (`05cceee0df3b8d7c6fa87e9638af311dbabc63cb`) and builds
  `TracyWorker.cpp`, `TracyFileWrite`, zstd, Capstone, and PPQSort.
- [`src/collector/collector.cpp`](src/collector/collector.cpp) establishes the
  supported Worker lifecycle: connect, wait for data, disconnect, retain the
  Worker, call `Worker::Write`, and atomically publish a capture.
- The live collector fixtures already prove Tracy 0.13.1 client-to-Worker capture
  and semantic validation with `tracy-query`.
- The locked Rust fixture uses `tracy-client` 0.18.4 and
  `tracy-client-sys` 0.28.0. That sys crate contains Tracy 0.13.1 and protocol
  version 76, matching this repository.
- Planning investigation linked the repository's Tracy client and server
  libraries into one process, connected them over loopback, and produced a
  valid capture containing one CPU zone and 100 messages. This proves that the
  client and Worker can coexist in one executable; the implementation must
  replace the temporary socket path with memory and turn this into a checked-in
  regression test.

The protocol must remain duplex. `tracy::Worker` sends requests for strings,
thread names, source locations, callstack frames, symbols, source code, and
termination acknowledgements. Directly feeding producer `QueueItem`s into
`Worker::Process` would bypass those semantics and is therefore not the planned
boundary.

## Goals and scope

1. Add a bounded, closeable, in-memory duplex byte transport suitable for the
   complete Tracy client/server protocol.
2. Add a compile-time `TRACY_EMBEDDED_CAPTURE` mode to the pinned Tracy client
   and a matching embedded endpoint constructor/path for `tracy::Worker`.
3. Preserve Tracy's handshake, event serialization, dynamic metadata queries,
   termination protocol, and initial LZ4 framing. Optimization that removes
   serialization or compression is deferred until after correctness.
4. Add a process-global embedded-capture coordinator with deterministic
   configure, running, finish, and failure states and a narrow C ABI.
5. Publish captures only after client drain and Worker shutdown, using a
   same-directory partial file followed by atomic rename.
6. Add a repository-local fork of `tracy-client-sys` 0.28.0 which preserves the
   upstream package name, Rust bindings, existing feature surface, and ordinary
   instrumentation ABI while adding an `embedded-capture` feature and bindings
   for the new lifecycle API.
7. Demonstrate that crates.io `tracy-client` 0.18.4 and `tracing-tracy` 0.11.4
   work unchanged when Cargo resolves the patched sys package.
8. Support Rust `panic=unwind` through an outer `catch_unwind` boundary which
   finalizes the capture and then resumes the original panic.
9. Keep the existing `tracy-query`, `tracy-collector`, network Tracy client,
   fixtures, static-linking guarantees, and release behavior passing.
10. Test and document the supported lifecycle and its observer effect on Linux,
    macOS, and Windows for the architectures in the existing CI matrix.

## Non-goals

- Do not embed Tracy's GUI.
- Do not invent a replacement Rust instrumentation API or fork
  `tracing-tracy`/`tracy-client` for the initial integration.
- Do not directly expose `tracy::Worker` or other C++ layouts through Rust FFI.
- Do not bypass Tracy's protocol by invoking private Worker event handlers from
  producer threads.
- Do not initially remove protocol serialization or LZ4 compression.
- Do not support multiple simultaneous captures or stop/restart cycles in one
  process. A single configured capture followed by process termination is the
  initial lifecycle.
- Do not claim capture recovery for `panic=abort`, signals, access violations,
  unhandled SEH, `SIGKILL`, `_exit`, OOM, power loss, double panic, or failure
  while the capture finalizer itself is running. These cases remain explicitly
  undefined.
- Do not make `.tracy` incrementally recoverable in this work. A journal or
  flight-recorder format is a separate project.
- Do not require successful captures to be readable before controlled
  finalization completes.
- Do not publish the patched sys crate to crates.io in the initial work. It must
  be usable as a path or git `[patch.crates-io]` dependency and structured so
  later publication/upstreaming remains possible.

## Immutable acceptance criteria

1. All new native code, the patched sys crate, and generated captures use the
   repository's exact Tracy 0.13.1 revision and protocol version 76; client and
   Worker version skew fails clearly rather than producing a capture.
2. Embedded mode transports the handshake, compressed event stream, Worker
   queries, acknowledgements, and termination entirely through memory. It does
   not bind, connect, discover, or require a TCP/UDP port, named pipe, inherited
   handle, shared-memory name, or helper process.
3. The in-memory transport is bounded, applies backpressure instead of silently
   dropping protocol bytes, wakes blocked readers/writers on close, and has
   deterministic end-of-stream and cancellation behavior.
4. Existing Tracy instrumentation APIs and exported `___tracy_*` symbols retain
   their behavior. Existing C++ fixture source and unmodified `tracy-client`
   source continue to emit zones, messages, plots, frame marks, locks, memory
   events, and supported callstack/symbol metadata in embedded mode.
5. A successful finish stops new instrumentation under a documented caller
   precondition, drains committed producer and serial queues, completes pending
   Worker metadata queries, observes client/server termination, writes through
   `Worker::Write`, closes the writer, and atomically publishes a non-empty
   `.tracy` file.
6. The final output passes `tracy-query check`. Semantic queries prove the
   presence of events emitted directly by `tracy-client` and spans/events
   emitted by the unmodified `tracing-tracy::TracyLayer`.
7. Output is written to a same-directory `.partial` path and renamed only after
   successful completion. Existing final files are not overwritten. Invalid or
   unwritable paths return a stable error and do not publish a misleading final
   capture.
8. The lifecycle C ABI is narrow, versioned/documented, does not expose C++
   types, never lets a C++ exception cross FFI, and reports invalid ordering,
   duplicate configuration/finalization, transport failure, and file failure
   deterministically.
9. The patched package remains named `tracy-client-sys`, remains semver-compatible
   with `tracy-client` 0.18.4's `>=0.23,<0.29` requirement, preserves upstream
   features/bindings, and adds an opt-in `embedded-capture` feature. A Cargo
   resolution check proves that `tracy-client`, `tracing-tracy`, and the example
   all use exactly one patched sys package.
10. The normal Rust example exits successfully and leaves one valid capture.
    Its source uses crates.io `tracy-client` and `tracing-tracy`, with only
    `tracy-client-sys` replaced through `[patch.crates-io]`.
11. The unwind-panic example runs application code under `catch_unwind`, lets
    Rust unwind and drop active spans/guards, records a post-catch panic marker,
    finalizes one valid capture, and calls `resume_unwind`. The harness observes
    the expected non-zero panic exit and validates events from before the panic.
12. Capture finalization is never attempted from the panic hook or while an
    active application span/guard from the protected closure is still live.
    The supported panic path has bounded automated tests and does not deadlock.
13. Existing `tracy-query` and `tracy-collector` commands, tests, installed
    binaries, network capture behavior, and checked-in trace validation remain
    passing.
14. Native and Rust integration tests pass on Linux, macOS, and Windows on
    x86-64 and ARM64 in the existing CI matrix, subject only to a documented CI
    runner limitation approved by the user. Sanitizer tests pass on Linux.
15. Documentation states the one-capture lifecycle, thread-safety preconditions,
    panic strategy requirement, undefined failures, memory/CPU/I/O observer
    effect, file publication behavior, Cargo patch procedure, and exact example
    commands.

## Design rules and constraints

### Version and source ownership

- Keep one authoritative Tracy pin in this repository. Native CMake targets and
  the patched sys crate must consume the same revision and shared patch set.
- Store substantive Tracy changes as reviewable patch/overlay files under a
  versioned integration directory rather than accumulating fragile CMake
  `string(REPLACE)` operations.
- Keep patches narrow and separate by concern: transport seam, embedded Worker
  constructor/path, coordinator/C ABI, and existing compatibility fixes.
- Record upstream file/line assumptions and re-run patch, protocol, lifecycle,
  and trace-compatibility tests before any Tracy upgrade.
- Preserve copyright and license notices from Tracy, `tracy-client-sys`, and all
  bundled dependencies. Document the provenance of the local sys fork.

### Transport boundary

- Use a duplex stream abstraction at the serialized protocol boundary, not at
  producer `QueueItem` or final trace-file boundaries.
- Model the duplex connection as two bounded unidirectional byte streams sharing
  explicit ownership and close state. Reads must support Tracy's exact-length,
  timeout, and shutdown-predicate needs; writes must preserve byte order.
- Use mutexes/condition variables initially. Correctness, bounded memory, and
  shutdown behavior take priority over a lock-free implementation.
- Keep the existing socket transport behavior unchanged. Select the memory path
  only when `TRACY_EMBEDDED_CAPTURE` is enabled or when the Worker is explicitly
  constructed with an embedded endpoint.
- Avoid per-event virtual dispatch on application producer threads. Transport
  adaptation belongs to the client serializer and Worker network threads.
- Retain the existing Tracy handshake, welcome/on-demand payloads, server query
  packets, LZ4 frame boundaries, and termination exchange in the first release.

### Ownership and lifecycle

- A process-global coordinator owns the channel pair, embedded Worker, capture
  configuration, and finalization result. The Tracy client owns its ordinary
  producer and serializer threads.
- Configuration must complete before `tracy_client::Client::start()` invokes
  `___tracy_startup_profiler`. The sys crate's `embedded-capture` feature must
  imply its own `enable` and `manual-lifetime`/`delayed-init` features and reject
  incompatible native combinations clearly. Because Cargo features do not
  propagate upward from a sys dependency, consumers must also enable
  `manual-lifetime` on `tracy-client` (directly or through the forwarding
  `tracing-tracy` feature) so `Client::start()` actually calls the startup ABI.
- Initial state transitions are:

  ```text
  unconfigured -> configured -> starting -> capturing
                                   |            |
                                   +-> failed   +-> finishing -> finished
                                                        |       |
                                                        +-> failed
  ```

- Only one transition through this lifecycle is supported per process. Repeated
  status/error queries may be idempotent, but repeated configure/start/finish
  calls must not recreate Tracy client state silently.
- The finalizer's caller must ensure that no other thread can invoke Tracy and
  that all application-owned Tracy/tracing guards have been dropped. Enforce
  what can be checked and document the remainder as an unsafe lifecycle
  precondition at the sys layer.
- Finalization order must be derived from Tracy 0.13.1 source and covered by a
  test. The expected order is: request client shutdown, drain client queues,
  complete protocol termination and metadata queries, join client transport
  work, wait for/disconnect and join the Worker, write the capture, close the
  writer, validate the partial, and rename.
- Never hold coordinator or channel-state mutexes while joining Tracy threads or
  calling `Worker::Write`.

### Capture files and errors

- Accept a caller-supplied UTF-8 path through the C ABI, copy it during
  configuration, and define Windows/non-UTF-8 limitations explicitly.
- Refuse overwrite by default. Create only a generated same-directory partial
  path and atomically rename it to the requested final path.
- Catch every exception at the C boundary and store a stable status plus a
  bounded diagnostic retrievable without borrowing C++ memory.
- Do not promise crash durability. Optional `fsync`/`FlushFileBuffers` policy may
  be added only after its cost and semantics are documented; ordinary writer
  close plus atomic rename is the initial requirement.

### Rust dependency and panic integration

- Fork `tracy-client-sys` 0.28.0 under a dedicated repository directory while
  retaining its package identity. Do not create a differently named package and
  assume Cargo will substitute it transitively.
- Use `[patch.crates-io] tracy-client-sys = { path = ... }` in the example and a
  direct sys dependency to activate `embedded-capture` and call its lifecycle
  functions. Cargo feature unification must carry that feature to the one sys
  crate used by `tracy-client` and `tracing-tracy`. Separately activate
  `manual-lifetime` on the unmodified `tracy-client` package (and use
  `tracing-tracy`'s forwarding feature consistently); sys features cannot alter
  `tracy-client`'s own `cfg(feature = "manual-lifetime")`.
- Keep the sys API low-level. The example may define a small local RAII/error
  wrapper, but a polished safe public wrapper crate is outside initial scope.
- Use `tracing::dispatcher::with_default` (or an equivalently scoped subscriber)
  inside the protected closure so dispatcher/span guards unwind before
  finalization. Do not rely on a global subscriber that may still emit while
  shutdown occurs.
- A panic hook may annotate a lock-free/preallocated diagnostic in later work,
  but must not flush, join threads, allocate a trace model, or write the file.
  The required example records its panic marker after `catch_unwind` returns.
- Build and test with Rust's unwind panic strategy. Abort and hard-failure paths
  remain undefined and must not be implied by API naming or documentation.

### Compatibility and verification

- Use `tracy-query check` and semantic queries as the compatibility oracle, not
  only file existence or size.
- Keep a socket-based same-process fixture as a control until memory transport
  reaches parity; then retain whichever control gives useful upgrade evidence.
- Cover transport concurrency and lifecycle transitions with deterministic unit
  tests. Avoid timing-only sleeps where barriers/status transitions can be used.
- Apply the repository warning policy, static MSVC runtime policy, sanitizer
  policy, and existing six-target CI expectations to new native code.
- Measure capture-thread CPU, peak RSS, application wall time, channel high-water
  mark, and output size in a representative fixture. Document measurements
  without making an unverified overhead guarantee.

## Staged implementation

### Stage 1 — Freeze interfaces and make the baseline reproducible

Dependencies: none. This stage turns planning assumptions into checked evidence
before modifying Tracy's transport.

- [x] Add an architecture document describing the serialized duplex boundary,
  channel semantics, lifecycle state machine, C ABI, file publication, Cargo
  substitution, and panic contract.
- [x] Define proposed public constants/status codes and opaque C functions in a
  small project-owned header without exposing Tracy server declarations.
- [x] Add a checked-in C++ same-process loopback fixture that links the pinned
  client and Worker, emits representative data, finalizes a trace, and exits
  without relying on an external profiler process.
- [x] Resolve duplicate common Tracy symbols deliberately in CMake; document
  which client-provided common objects satisfy server references instead of
  relying on accidental static archive extraction order.
- [x] Query the loopback fixture capture for at least CPU zones, messages, plots,
  frame marks, locks, and memory events, and include a metadata/callstack case
  supported on each platform.
- [x] Add a compatibility assertion for Tracy version 0.13.1 and protocol 76 in
  both native and future Cargo build paths.
- [x] Move existing source mutations and new transport changes toward one
  versioned, idempotent patch/overlay mechanism shared by CMake and the sys
  crate; preserve existing query accessor behavior.

Verification:

- [x] Configure/build with `BUILD_TESTING=ON` and full-static mode both enabled
  and disabled on Linux.
- [x] Run all existing CTests unchanged.
- [x] Run the loopback fixture repeatedly and validate its file with
  `tracy-query check`, `info`, and fixture-specific count queries.
- [x] Prove the fixture links and runs under ASan/UBSan without duplicate-symbol,
  ODR, leak, or shutdown errors.

### Stage 2 — Implement and verify the bounded duplex memory transport

Dependencies: Stage 1 interface and ownership decisions.

- [x] Implement a platform-neutral byte-stream core with configurable capacity,
  ordered writes, exact and partial reads, bounded backpressure, timeout/shutdown
  predicates, half-close/full-close, and diagnostic counters.
- [x] Build a duplex pair from two independently closeable streams and define
  endpoint copy/move/ownership rules that cannot leave dangling channel state.
- [x] Ensure a blocked writer wakes and fails when its peer closes and a blocked
  reader wakes with EOF/cancellation when its peer or coordinator closes.
- [x] Ensure shutdown never requires a producer thread to consume bytes and does
  not hold a channel mutex while invoking callbacks.
- [x] Add deterministic unit tests for ordering, wraparound/chunk boundaries,
  capacity backpressure, concurrent readers/writers, timeout, peer close,
  coordinator cancellation, destruction with blocked operations, and repeated
  close.
- [x] Add high-water-mark and transferred-byte counters for diagnosis and later
  observer-effect measurements; do not put instrumentation for these counters
  back into Tracy itself.

Verification:

- [x] Run transport unit tests repeatedly with small capacities that force
  wraparound and backpressure.
- [x] Run ThreadSanitizer where supported plus ASan/UBSan on Linux.
- [x] Demonstrate bounded allocation under a sustained writer/slow-reader test
  and clean completion under injected close at every blocking point.

### Stage 3 — Route Tracy client and Worker protocol traffic through memory

Dependencies: Stage 2 transport.

- [x] Add a narrow transport facade to the Tracy client serializer path covering
  accept/acquire, send, exact read, `HasData`, close, and send-buffer sizing.
- [x] Preserve the existing `ListenSocket`/`Socket` behavior when embedded mode
  is disabled and compile an endpoint-backed path under
  `TRACY_EMBEDDED_CAPTURE` without adding work to producer-thread fast paths.
- [x] Add an embedded Worker path which initializes the same trace model as the
  network constructor while selecting the endpoint-backed internal Socket
  implementation and skipping every OS address/port operation.
- [x] Route Worker compressed-frame reads and priority/ordinary server query
  writes through its endpoint while preserving current flow-control counters,
  locking, and termination checks.
- [x] Implement the in-process handshake rendezvous and ensure the client welcome
  and optional on-demand payload are processed before capture reports ready.
- [x] Preserve dynamic string/thread/source-location/callstack/symbol queries and
  termination acknowledgements in both directions.
- [x] Ensure Worker shutdown closes/wakes the memory transport before joining
  network/processing/background threads.
- [x] Cover relevant internal failure surfaces: compile-time protocol/version
  mismatch rejection, channel peer-close/read/write cancellation, bounded
  capacity pressure, no-handshake/no-data finalization, output races, and writer
  errors. Malformed/truncated protocol injection is excluded because the two
  pinned in-process endpoints cannot accept external bytes.
- [x] Keep the Stage 1 socket control fixture passing to detect regressions in
  normal Tracy mode.

Verification:

- [x] Replace the same-process fixture's loopback endpoint with memory and prove
  that it runs with no configured `TRACY_PORT` and while customary Tracy ports
  are occupied.
- [x] Validate semantic parity between socket-control and memory captures for the
  deterministic fixture's event-kind counts and identifying names/messages.
- [x] Add a test hook/counter proving that embedded capture used the memory
  backend and performed no socket bind/connect/broadcast operation.
- [x] Run repeated startup, sustained capture, forced backpressure, and shutdown
  tests under sanitizers.

### Stage 4 — Implement capture coordination, finalization, and C ABI

Dependencies: Stage 3 complete protocol path.

- [x] Implement the process-global coordinator and its explicit state machine,
  with one owner for channel endpoints, Worker, path/configuration, and stored
  error information.
- [x] Define and implement versioned C functions for configure/start status,
  finish, active/state query, and bounded error retrieval. Final names and
  signatures must be frozen in the architecture document before Rust bindings.
- [x] Make every C entry point validate pointers/lengths and catch all C++
  exceptions before returning a stable status code.
- [x] Start the endpoint-backed Worker early enough that manual Tracy client
  startup can complete its handshake without discovery, ports, or external
  orchestration.
- [x] Implement controlled shutdown from a caller that has quiesced
  instrumentation: request Tracy client shutdown, allow its existing drain and
  termination loops to finish, close endpoints on error, join both sides, and
  only then call `Worker::Write`.
- [x] Audit shutdown ordering against `Profiler::Worker`,
  `ShutdownProfiler`, `Worker::Exec`, `Worker::Network`, `Worker::Disconnect`,
  and both destructors in pinned Tracy 0.13.1. Record the result in the
  architecture document.
- [x] Implement generated same-directory partial names, no-overwrite checks,
  `TracyFileWrite` close/finish, non-empty validation, semantic load validation
  where appropriate, atomic rename, and partial cleanup/error retention.
- [x] Return deterministic errors for calls in the wrong order, repeated
  configuration/finish, no handshake/data, transport failure, unwritable path,
  existing output, writer failure, and rename failure.
- [x] Add lifecycle tests covering normal completion, no client/data, concurrent
  producers, documented quiescence of zones/dispatchers, duplicate ordering,
  output-exists races, missing output directories, and partial cleanup.

Verification:

- [x] A standalone C++ fixture configures a path, starts embedded Tracy, emits
  representative data, finishes, and produces exactly one valid final file and
  no partial.
- [x] `tracy-query check` plus semantic queries validate every successful test
  capture; failed finalizations publish no final capture.
- [x] Repeated lifecycle and injected-failure suites finish without deadlock or
  leaked threads/files under ASan/UBSan and ThreadSanitizer where supported.
- [x] Existing socket client, collector, and query suites remain passing.

### Stage 5 — Build the patched `tracy-client-sys` package

Dependencies: frozen Stage 4 C ABI and shared native build boundary.

- [x] Add `rust/tracy-client-sys/` as a provenance-documented fork of upstream
  0.28.0, retaining package name/version compatibility, existing generated
  bindings, feature names, license files, and disabled-feature no-op behavior.
- [x] Add an `embedded-capture` Cargo feature which implies `enable` and
  `manual-lifetime`/`delayed-init`, defines `TRACY_EMBEDDED_CAPTURE`, and gives a
  clear build error for unsupported/incompatible combinations.
- [x] Add Rust declarations and status constants for the new C ABI only when the
  feature is active; preserve every upstream `___tracy_*` declaration used by
  `tracy-client`.
- [x] Make the sys build consume the repository's exact Tracy archive, shared
  patch set, server source list, zstd/Capstone/PPQSort pins, compiler settings,
  and required platform libraries. Do not introduce a second unreviewed Tracy
  source or dependency revision.
- [x] Make path and git dependency builds reproducible from a clean checkout,
  including clear first-build network/cache requirements consistent with the
  repository's existing CMake FetchContent policy.
- [x] Ensure native static libraries and transitive dependencies are linked once
  and in a deterministic order on GNU/Clang, Apple, and MSVC toolchains.
- [x] Add sys-level compile/link smoke tests for feature disabled, ordinary
  network client features, embedded capture, and the existing manual-lifetime
  symbols.
- [x] Add an ABI test that calls configure/start/finish through Rust FFI and
  validates the resulting trace with the repository's query executable.
- [x] Use `cargo tree -d` and `cargo tree -i tracy-client-sys` fixtures to reject
  accidental coexistence of registry and patched sys packages.

Verification:

- [x] `cargo test` for the patched crate passes with no default features, default
  features, manual lifetime, and embedded capture in supported combinations.
- [x] A clean Cargo build proves that unmodified `tracy-client` 0.18.4 links
  against the patched package and can emit a valid embedded capture.
- [x] Existing `tests/nextest-fixture` remains on its intended network collector
  mode and passes, proving that Cargo/native refactoring did not force embedded
  mode globally.
- [x] Run native and Rust sys smoke tests across the existing six-platform CI
  matrix.

### Stage 6 — Add unwind-panic lifecycle coverage

Dependencies: Stage 5 Rust FFI package.

- [x] Add a Rust integration fixture using the default unwind panic strategy,
  `catch_unwind(AssertUnwindSafe(...))`, and a scoped tracing dispatcher.
- [x] Keep the capture/client owner outside the protected closure; create and
  drop all application spans and Tracy guards inside it so unwinding quiesces
  instrumentation before finalization.
- [x] After `catch_unwind` returns an error, emit a fixed panic-caught marker from
  the now-quiescent controlling thread, call the embedded finalizer, preserve
  the original panic payload, and invoke `resume_unwind` only after successful
  publication.
- [x] Define behavior when finalization itself fails during panic handling:
  report both the capture error and original panic without a second Rust panic,
  then terminate through one documented path.
- [x] Add concurrent producer coverage where worker threads are explicitly
  joined before the protected closure unwinds to the finalization boundary.
- [x] Add negative/documentation tests or compile-time fixtures showing that
  `panic=abort` is unsupported and that flushing from a panic hook is not part
  of the API contract.
- [x] Bound the integration harness duration and capture diagnostics on timeout
  so a shutdown deadlock is actionable.

Verification:

- [x] The normal fixture exits zero and produces a valid trace.
- [x] The unwind fixture exits with the expected resumed-panic status, leaves one
  valid atomically published trace, and contains pre-panic zones/messages plus
  the post-catch marker.
- [x] Repeated normal and panic process fixtures, with nested direct/tracing
  zones and joined concurrent producers, complete in the six-target matrix
  without deadlock, partial publication, or loss of pre-panic events.

### Stage 7 — Cross-platform CI, compatibility, and documentation

Dependencies: Stages 1–6 functionally complete.

- [x] Add native transport/coordinator tests and patched-sys build tests to all
  existing Linux, macOS, and Windows x86-64/ARM64 matrix jobs.
- [x] Extend the Linux sanitizer job with memory-transport concurrency,
  coordinator lifecycle, normal Rust finalization, and unwind-panic finalization.
- [x] Add bounded stress coverage for an eight-byte wraparound/backpressure
  channel, blocked close paths, concurrent application threads, dynamic Tracy
  metadata queries, and repeated process runs.
- [x] Preserve installed `tracy-query`/`tracy-collector` artifacts and release
  asset naming; do not publish the example or sys crate as a binary release
  artifact.
- [x] Update `README.md` with embedded capture purpose, supported lifecycle,
  Cargo patch setup, comparison with the external collector, and links to the
  architecture/example documentation.
- [x] Document the C ABI and low-level Rust safety requirements, including
  process-global state, no concurrent instrumentation during finish, one
  capture per process, no restart, and path/error ownership.
- [x] Document `panic=unwind` usage and explicitly list undefined abort, signal,
  kill, OOM, power-loss, double-panic, and finalizer-failure cases.
- [x] Measure and record wall time, CPU, peak RSS, channel high-water mark, trace
  size, and binary-size impact for socket-control versus embedded fixtures.
- [x] Document how to update the Tracy pin and local sys fork, reapply/review the
  patch set, refresh generated bindings if necessary, and rerun compatibility
  evidence.

Verification:

- [x] All existing and new CTest/Cargo tests pass on every required native CI
  target.
- [x] Linux sanitizer/stress jobs pass repeatedly with actionable timeout limits.
- [x] Installed query and collector binaries retain static-link and version smoke
  tests, and existing reference captures remain queryable.
- [x] Documentation commands work from a clean checkout and do not rely on an
  installed Tracy SDK, profiler GUI, or daemon.

### Stage 8 — Final Rust example and end-to-end demonstration

Dependencies: all previous stages. This is the final milestone and the feature
is not complete until this demonstration is reproducible.

- [x] Add `examples/rust-embedded-capture/` with a locked Cargo manifest that
  depends on crates.io `tracy-client` 0.18.4 and `tracing-tracy` 0.11.4 with
  their existing `enable`/`manual-lifetime` feature forwarding enabled, directly
  depends on `tracy-client-sys` only for lifecycle FFI and
  `embedded-capture` activation, and replaces that package through
  `[patch.crates-io]` with `rust/tracy-client-sys/`.
- [x] Configure a scoped `tracing_subscriber` registry with
  `tracing_tracy::TracyLayer`, emit nested `tracing` spans/events (including an
  instrumented function), and emit independently identifiable messages/zones
  through `tracy-client`.
- [x] Provide `normal` and `unwind-panic` modes. Both accept an explicit output
  path and refuse overwrite; panic mode follows catch, quiesce, finalize, and
  resume-unwind ordering.
- [x] Print concise lifecycle diagnostics to stderr without treating trace data
  as logs, and provide a helper command/script that handles panic mode's
  expected non-zero exit.
- [x] Add an end-to-end harness that builds from a clean Cargo target directory,
  verifies only one patched sys package in the dependency graph, occupies Tracy
  network ports to prove they are irrelevant, and runs both modes.
- [x] Validate the normal and panic captures with `tracy-query check`, `range`,
  and `info`, then query exact names/messages proving data from both
  `tracing-tracy` and direct `tracy-client` integration.
- [x] Assert that each mode leaves exactly one requested `.tracy` file, no
  partial file, no child/helper process, and no listener requirement.
- [x] Add the exact demonstration commands and representative query output to
  the example README and root documentation.

Final verification commands (exact options may be refined without weakening the
acceptance criteria):

```sh
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON \
  -DTRACY_QUERY_FULLY_STATIC=OFF
cmake --build build --parallel
ctest --test-dir build --output-on-failure

cargo tree --manifest-path examples/rust-embedded-capture/Cargo.toml \
  -i tracy-client-sys

cargo run --manifest-path examples/rust-embedded-capture/Cargo.toml -- \
  normal out/rust-normal.tracy
build/tracy-query check out/rust-normal.tracy
build/tracy-query query --kind cpu-zone,message --count --group-by kind \
  out/rust-normal.tracy

# The harness expects the resumed panic and validates the file afterward.
python3 examples/rust-embedded-capture/run_demo.py \
  --mode unwind-panic \
  --output out/rust-panic.tracy \
  --query build/tracy-query
build/tracy-query check out/rust-panic.tracy
build/tracy-query query --kind cpu-zone,message \
  --filter 'message.text=embedded-example|panic-caught' \
  out/rust-panic.tracy
```

Completion evidence:

- Verification environment: branch `plan/in-process-capture`, final audited
  code/test/CI commit `87bc78433a14c382fcbba3eba394ace4341a8b6a`. The following
  commit only updates this recorded evidence.
- Complete native regression evidence: local Debug build ran all 55 then-current
  CTests successfully; the final audit split transport shutdown into explicit
  core, peer-destroy, and coordinator-cancel process tests, and local plus
  six-platform CI revalidated the resulting 60 tests.
- Sanitizer evidence: local ASan/UBSan embedded native and Rust normal/panic
  suites passed repeatedly; the CI sanitizer job includes all of them and its
  prior native run passed at
  <https://github.com/SanderVocke/tracy-query/actions/runs/31654475556/job/94305767406>.
  ThreadSanitizer was attempted locally but is unsupported by the local Nix/glibc
  runtime (`unexpected memory mapping`); close/backpressure concurrency remains
  covered by deterministic tests and ASan/UBSan.
- Cargo feature evidence: patched sys `--no-default-features`, default features,
  and `enable,manual-lifetime` tests passed locally; the matrix built the
  `embedded-capture` feature beneath unmodified `tracy-client` and
  `tracing-tracy` on every target.
- Dependency evidence: `cargo tree -i tracy-client-sys` resolves one path-patched
  0.28.0 package shared by the example, `tracy-client`, and `tracing-tracy`;
  `cargo tree -d` is empty.
- End-to-end evidence: `run_demo.py` validates normal exit and resumed unwind
  panic, occupies Tracy TCP/UDP ports, checks no partials, and queries exact
  direct-client and tracing-layer semantic markers.
- Cross-platform/static/packaging evidence: CI run
  <https://github.com/SanderVocke/tracy-query/actions/runs/31654475556> passed
  Linux, macOS, and Windows on x86-64 and ARM64, including Linux full-static
  verification, installation and installed executable smoke tests. The pinned
  nextest/network collector contract passed at
  <https://github.com/SanderVocke/tracy-query/actions/runs/31654475556/job/94305767477>.
- Observer-effect measurements and limitations are recorded in
  `docs/in-process-capture.md`; exact demonstration commands are in
  `examples/rust-embedded-capture/README.md` and the root README.

- [x] Clean-checkout native build/test/install evidence is recorded.
- [x] Clean Cargo dependency/build evidence shows one patched
  `tracy-client-sys` and unmodified higher-level crates.
- [x] Normal example trace and exact semantic query output are recorded.
- [x] Resumed-unwind example exit status, valid trace, and exact pre-panic and
  post-catch query output are recorded.
- [x] Six-platform CI and Linux sanitizer links are recorded.
- [x] Every immutable acceptance criterion is checked against command output,
  CI evidence, or validated capture artifacts before the feature is declared
  complete.
