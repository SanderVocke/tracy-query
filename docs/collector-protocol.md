# Tracy collector daemon protocol and nextest integration

This document defines protocol version **1** for `tracy-collector`. The daemon and
`tracy-query` are built from the same hash-pinned Tracy 0.13.1 server source.
Tracy's C++ types are private implementation details and are not part of this
protocol or any installed API.

## Architecture and ownership

`tracy-collector` is a suite-scoped process. It owns one in-memory
`tracy::Worker` per registered attempt. The suite orchestrator may start the
daemon and `cargo nextest`, but nextest remains the direct parent and process
supervisor of every test. An opt-in hook inside each test executable registers
before Tracy initializes, puts the returned port in `TRACY_PORT`, initializes
the Tracy client, and waits for `STATUS` to report `capturing`.

The daemon does not parse JUnit. After nextest exits, the consumer-owned adapter
maps JUnit attempts to registrations and sends `SAVE` or `DISCARD`. It must use
`NEXTEST_ATTEMPT_ID` as the primary key and retain binary ID, test name, retry
number, and optional stress identity as correlation evidence. Failed, aborted,
timed-out, duplicate, contradictory, absent, or otherwise unresolved results
must be saved. Only one unambiguous successful result may be discarded.

## Command line

```text
tracy-collector --output-root DIR --ready-file FILE [options]
  --control-port PORT       loopback control port (default: 9327)
  --data-port-first PORT    first Tracy client port (default: 9400)
  --data-port-last PORT     last Tracy client port (default: 9499)
  --max-sessions N          registration limit (default: 256)
  --memory-limit BYTES      per-Worker memory limit (default: 536870912)
  --connect-timeout-ms MS   Tracy handshake deadline (default: 30000)
  --owner-timeout-ms MS     owner lease deadline (default: 15000)
  --finalize-timeout-ms MS  graceful finalization deadline (default: 30000)
  --version                 executable, protocol, and Tracy versions
```

`DIR` is created and canonicalized. All artifacts remain immediately below it.
The daemon binds control and Tracy data connections to loopback only. Set
`TRACY_COLLECTOR_TOKEN` in the daemon environment to provide the run secret;
otherwise the daemon creates 32 random bytes and writes a secret file beside
the ready file, then refers to it from the ready descriptor. On POSIX the daemon
sets mode 0600. On Windows the file inherits its containing directory's ACL, so
the ready file must be placed in a private per-user work directory. The secret
is never accepted on the command line and is never printed to normal logs.

After the listener is active, the daemon atomically publishes the UTF-8 JSON
ready descriptor. Fields are `protocol_version`, `endpoint`, `run_id`, and
`secret_file`. Diagnostics go to stderr. Exit 0 means clean finalization; 2 is
CLI/configuration error, 3 startup/ready-publication failure, 4 finalization had
a save failure or exceeded its deadline, and 5 is an internal failure.

## Framing and encoding

Control uses TCP over loopback for identical Unix and Windows behavior. Every
request and response is exactly one frame per connection:

```text
byte 0..3    ASCII "TCOL"
byte 4..5    unsigned protocol version, big endian (1)
byte 6..7    unsigned message type, big endian
byte 8..11   unsigned payload length, big endian
byte 12..    payload
```

Payloads consist only of unsigned integers in big endian and bounded UTF-8
strings encoded as `u16 byte_length` followed by bytes. Every request starts
with the authentication token string. Responses use request type OR `0x8000`
and begin with `u16 status`, then a status-message string, followed by the
operation-specific fields on success. A response with status 0 is successful.
Unknown versions/types, invalid UTF-8, truncated/coalesced extra data, strings
over 4096 bytes, payloads over 65536 bytes, and unauthenticated requests are
rejected without changing any session.

Status codes: 0 success; 1 malformed; 2 unauthenticated; 3 conflict/duplicate;
4 not found; 5 limit/exhaustion; 6 invalid state; 7 I/O/save failure; 8 version
or message unsupported; 9 owner lease unavailable.

### Operations

| Type | Name | Request after token | Successful response after status/message |
|---:|---|---|---|
| 1 | `OWNER_ACQUIRE` | run ID string | owner lease ID string |
| 2 | `OWNER_HEARTBEAT` | owner lease ID string | none |
| 3 | `REGISTER` | run ID, attempt ID, binary ID, test name strings; retry `u32`; stress metadata string | session ID string; Tracy port `u16` |
| 4 | `STATUS` | session ID string | state, handshake, error, output-name strings |
| 5 | `DECIDE` | session ID string; decision `u16` (1 save, 2 discard); source string | resulting state string |
| 6 | `FINALIZE` | owner lease ID string | unresolved count `u32` |
| 7 | `LIST` | none | count `u32`, then for each: session ID, attempt ID, state, output name, error strings |

A duplicate attempt ID is rejected. Repeating the same decision is idempotent;
a conflicting decision is rejected and can never weaken save to discard.
Authentication failures do not reveal whether a run or session exists.

## Session and failure model

```text
registered -> connecting -> capturing -> awaiting-decision
                    |             |
                    +-> failed-to-connect
awaiting + SAVE    -> save-pending -> writing-partial -> saved | save-failed
awaiting + DISCARD -> discard-pending -> discarded
owner loss/finalize with no decision -> save-pending -> saved | save-failed
```

A decision received while connected is retained and applied after disconnect.
Handshake timeout calls `Worker::Disconnect`; an unresolved timeout is still a
save decision and reports `save-failed` if no usable trace was received. Owner
acquisition starts a lease. Missing heartbeats, control/owner loss beyond the
lease, `FINALIZE`, SIGINT, or SIGTERM stop registrations and apply save to every
undecided session. Connected Workers are disconnected during explicit/signal
finalization; ordinary owner expiry waits for client disconnect up to the
finalization deadline, then disconnects. Duplicate IDs, malformed requests,
port/session exhaustion, and save errors are explicit and never affect other
sessions.

SIGKILL, machine loss, abrupt runner cancellation, and loss of the daemon's
volatile memory are outside the durability guarantee.

## Tracy Worker lifecycle

The network constructor `Worker("127.0.0.1", port, memory_limit)` repeatedly
connects to the on-demand Tracy client. `GetHandshakeStatus()` detects protocol,
availability, and dropped-handshake failures. `HasData()` becomes true only
after the welcome payload is processed; this is readiness. `IsConnected()`
tracks capture lifetime. `Disconnect()` asks the Worker threads to shut down;
the Worker destructor joins network, processing, and background threads.
`Worker::Write(FileWrite&, false)` is called only after disconnection and while
the Worker remains alive. These rules match Tracy 0.13.1's `capture` utility.

`DISCARD` destroys the Worker directly. It never constructs `FileWrite` and
therefore never opens any trace path. `SAVE` opens only
`<generated-name>.tracy.partial` via `FileWrite::Open`, writes and closes it,
checks non-zero size, and atomically renames it in the same directory to
`<generated-name>.tracy`. Existing outputs are never overwritten. Partial files
are removed after an ordinary write failure; a failed cleanup is recorded.

Names are generated from the opaque session ID and a bounded sanitized display
fragment. Metadata never supplies a path component. Paths are checked against
the canonical output root.

## Manifest and retention

`manifest.json` is atomically rewritten under the same synchronization used for
session transitions. It contains protocol/run metadata and, for every attempt:
identity fields, retry/stress metadata, state, registration/update timestamps,
decision and source, output name (only for a published capture), error,
handshake status, and assigned port. Discarded sessions have an empty output
name. Captures and the manifest are retained until the output directory is
removed by the suite/CI retention policy. Partials are never publication
artifacts.

## Exact nextest validation contract

The reference fixture uses a pinned `cargo-nextest` release and requests JUnit.
The adapter correlates `NEXTEST_ATTEMPT_ID` registrations with JUnit binary/test
and retry records, saves every non-successful or ambiguous registration,
discards only clear successes, finalizes, then validates every published file:

```sh
tracy-query check failure.tracy
tracy-query range failure.tracy
tracy-query info failure.tracy
tracy-query query --kind message,cpu-zone --count failure.tracy
```

Expected cases are normal pass (no trace), panic/assertion, abort, nextest
timeout, and retry-then-pass (failed retry retained; later pass discarded).
Killing the adapter or providing incomplete JUnit must retain unresolved
captures. Ordinary `cargo test` and nextest list/discovery do not set the
collector variables, so the opt-in hook must remain inert.

## Security, limits, and troubleshooting

The per-run secret protects all requests, listeners are loopback-only, frame and
string lengths are checked before allocation, registrations and ports are
bounded, memory is bounded per Worker, and generated paths cannot escape the
root. This is local same-user authentication, not a sandbox against a process
that can read the owner's environment or files.

The sanitizer job enables AddressSanitizer and UndefinedBehaviorSanitizer. It
disables UBSan's alignment check because Tracy's packed wire-event payloads
intentionally bind fields at unaligned addresses, and disables only the enum
check for Tracy's bundled client/libbacktrace target because its DWARF reader
loads raw tag values through an enum. Collector core/protocol code retains the
enum check, and all other undefined-behavior checks remain fatal.

A port-exhaustion error means increase the configured range or reduce nextest
concurrency. A handshake timeout usually means Tracy initialized before
`TRACY_PORT`, the client was not built with Tracy on-demand support, or versions
differ. Incomplete JUnit is intentionally resolved as save. `save-failed` in
the manifest preserves the path/error diagnosis; check output permissions and
free space. A Tracy upgrade must re-audit constructor/thread behavior,
handshake protocol 76, `HasData`, `IsConnected`, `Disconnect`, `Write`, and the
narrow source patches in `cmake/TracyServer.cmake`.
