# tracy-query

`tracy-query` is a command-line reader for [Tracy Profiler](https://github.com/wolfpld/tracy) capture files. It validates captures, reports metadata and sources, and queries timestamped profiling data without opening Tracy's GUI. The companion `tracy-collector` daemon collects concurrent test-attempt traces in memory and publishes only captures selected by an authenticated suite orchestrator.

Both tools are intentionally pinned to Tracy 0.13.1.

## Download

Starting with v0.2.0, the [latest GitHub release](https://github.com/SanderVocke/tracy-query/releases/latest) provides uncompressed executables for both tools on Linux, macOS, and Windows on x86-64 and ARM64:

| Platform | x86-64 | ARM64 |
|---|---|---|
| Linux | `tracy-query-linux-x86_64` | `tracy-query-linux-arm64` |
| macOS | `tracy-query-macos-x86_64` | `tracy-query-macos-arm64` |
| Windows | `tracy-query-windows-x86_64.exe` | `tracy-query-windows-arm64.exe` |

Collector assets use the same suffixes with the `tracy-collector-` prefix, for example `tracy-collector-linux-x86_64` and `tracy-collector-windows-arm64.exe`.

Tracy and all bundled third-party dependencies are statically linked into every executable. Linux binaries are fully static, Windows binaries use the static MSVC runtime, and macOS binaries retain only operating-system runtime dependencies because macOS does not support fully static user executables. On Linux and macOS, mark a downloaded binary executable with `chmod +x <file>`.

## Build

Requirements:

- CMake 3.24 or newer
- A C++20 compiler
- Ninja, Make, or another CMake-supported build tool
- Internet access during the first configure
- On Linux, static C and C++ runtime archives (for example, the `libc6-dev` and compiler development packages)

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Dependencies are commit- and hash-pinned and cached under `build/_deps`. On Linux, the installed `tracy-query` and `tracy-collector` executables are fully static by default, and the build fails if either has a dynamic dependency. Use `-DTRACY_QUERY_FULLY_STATIC=OFF` only when building locally on a system without static runtime archives.

## Collect nextest attempt traces

Start one suite-scoped daemon with an output root and atomic ready descriptor:

```sh
./build/tracy-collector \
  --output-root artifacts/tracy \
  --ready-file artifacts/collector-ready.json \
  --control-port 9327 \
  --data-port-first 9400 --data-port-last 9499
```

The descriptor identifies the loopback endpoint, run ID, protocol version, and secret file. The daemon restricts that file to mode 0600 on POSIX; on Windows it inherits the containing directory's ACL, so the ready file should be placed in a private per-user work directory. A consumer startup hook activates only under nextest, registers `NEXTEST_ATTEMPT_ID` plus binary/test/retry metadata, sets the returned `TRACY_PORT` before Tracy delayed/manual initialization, and waits for a completed handshake. After nextest exits, the suite adapter reconciles authoritative JUnit records and sends decisions: clear successes are discarded; failed, crashed, timed-out, missing, duplicate, contradictory, and otherwise unresolved attempts are saved.

`DISCARD` destroys the in-memory Worker without opening a trace file. `SAVE` writes a same-directory `.partial`, validates it, and atomically publishes a `.tracy` file. `manifest.json` records every identity, state, decision, handshake, output, and error. The daemon listens only on loopback, authenticates every request, bounds frames/sessions/memory/ports, and never forms paths from untrusted metadata. SIGINT, SIGTERM, owner lease loss, and finalization default unresolved sessions to save; SIGKILL or machine loss can still lose in-memory data.

The complete versioned wire format, state model, retention rules, limits, exit codes, Rust fixture contract, JUnit correlation policy, and troubleshooting procedure are in [`docs/collector-protocol.md`](docs/collector-protocol.md). The repository's reference adapter is [`tests/nextest_orchestrator.py`](tests/nextest_orchestrator.py), and its locked fixture is under [`tests/nextest-fixture`](tests/nextest-fixture). [ShoopDaLoop PR #718](https://github.com/SanderVocke/shoopdaloop/pull/718) provides a complete consumer integration, including its startup hook, nextest profile, suite adapter, CI job, and operational guide.

Exact end-to-end validation:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
cmake --install build --prefix stage
stage/bin/tracy-query --version
stage/bin/tracy-collector --version
```

When `cargo-nextest` 0.9.116 is on `PATH` during configure, CTest also runs the pass/fail/abort/timeout/retry JUnit contract. CI pins that version in a dedicated job and publishes the JUnit, manifest, and only finalized failure traces.

## Inspect captures

Validate one or more captures:

```sh
./build/tracy-query check capture.tracy another.tracy
./build/tracy-query capture.tracy                 # shorthand
```

Print capture ranges, metadata, or natural sources:

```sh
./build/tracy-query range capture.tracy
./build/tracy-query info --detail full capture.tracy
./build/tracy-query sources capture.tracy
./build/tracy-query sources --kind plot --plot 'callback|cycle' capture.tracy
./build/tracy-query sources --source-type thread capture.tracy
```

Tracy data is organized into typed collections rather than universal hierarchical sub-traces. Sources therefore use stable IDs such as `thread:42`, `plot:3`, `frame-set:0`, and `gpu-context:0/thread:42`.

## Query data

Supported kinds:

```text
message, plot, cpu-zone, gpu-zone, frame, frame-image, lock, memory,
sample, ghost-zone, context-switch, cpu-slice, cpu-usage, hardware-sample, crash
```

Query a time range. Times are relative to each trace's first usable timestamp:

```sh
./build/tracy-query query --kind message,plot \
  --from 2.5s --to 4s capture.tracy
```

Accepted units are `ns`, `us`, `ms`, `s`, `m`, and `h`. Ranges are inclusive. Interval records overlap the range by default; use `--range-match start` or `--range-match contained` for other behavior.

Find values around a time point:

```sh
./build/tracy-query query --kind plot --plot '^BackendWrapper/cycles$' \
  --at 10s --latest --next capture.tracy

./build/tracy-query query --kind cpu-zone --at 10s --active capture.tracy
```

Filter applicable fields with ECMAScript regular expressions:

```sh
./build/tracy-query query --kind message \
  --filter 'message.text=error|warning' --ignore-case capture.tracy

./build/tracy-query query --kind cpu-zone \
  --filter 'cpu-zone.name=^engine\.rt\.' --from 3s --to 5s capture.tracy
```

Scope queries to sources:

```sh
./build/tracy-query query --kind cpu-zone,message --thread '^Main$|Render-' capture.tracy
./build/tracy-query query --kind plot --source plot:3 capture.tracy
```

CPU and GPU zones are genuinely nested. Structural filters apply only to zone kinds:

```sh
./build/tracy-query query --kind cpu-zone --zone-ancestor '^Frame$' \
  --zone-depth 2:4 capture.tracy
```

Count matches instead of producing large output:

```sh
./build/tracy-query query --kind all --count capture.tracy
./build/tracy-query query --kind all --count \
  --group-by trace --group-by kind capture.tracy
```

Use `--limit N` to limit normal output and `--detail full` to resolve additional callstack and symbol information. Multiple traces and kinds are merged deterministically by normalized timestamp.

## Output and exit status

The default format is JSON Lines: one data record per physical line. Every data record includes:

- `timestamp_ns`
- `trace`
- `source`
- `kind`

Intervals also include `end_timestamp_ns` and `duration_ns`. Use `--format text` for escaped key/value output. Normal results go to stdout and diagnostics go to stderr.

- Status 0: success
- Status 1: capture loading, reading, output, or execution failed
- Status 2: invalid command-line usage

Run `tracy-query --help` or `tracy-query <command> --help` for contextual help.

## Agent skill

[`SKILL.md`](SKILL.md) provides LLM agents with a self-contained workflow and complete CLI reference for debugging applications from existing Tracy captures. It covers source discovery, focused queries, multi-trace comparisons, common debugging recipes, and interpretation pitfalls; capture creation is intentionally out of scope.

## Limitations

- Tracy's server-side parser and network Worker are internal, version-specific APIs. New Tracy versions require an explicit compatibility and lifecycle review.
- Collector captures remain in daemon memory until decided. SIGKILL, machine loss, abrupt runner cancellation, and memory loss are outside its durability guarantee.
- Per-session memory, port, session, handshake, owner-lease, and finalization limits must be sized for the traced suite; begin with low nextest concurrency and measure CPU, RSS, wall time, and artifact size before expanding scope.
- A result kind is empty when the capture did not record that category.
- Frame images, cached source files, and symbol code are represented by metadata; binary extraction is not provided.
- Normal globally ordered queries use bounded in-memory chunks and temporary files. Very large output may require substantial temporary disk space. Count mode does not format or retain matching records.
- Full-detail callstack and symbol resolution is more expensive than basic output.

## Tracy integration

The project uses CMake `FetchContent` declarations in [`cmake/TracyServer.cmake`](cmake/TracyServer.cmake). Tracy is pinned to the exact commit behind **v0.13.1** (`05cceee0df3b8d7c6fa87e9638af311dbabc63cb`), and each downloaded archive is protected by a SHA-256 hash.

The standard Tracy package exposes the instrumentation client but not `TracyFileRead` and `TracyWorker`, which are needed to parse and collect captures. This project compiles the exact upstream server source set once into a private static library and links that same target into `tracy-query` and `tracy-collector`. It never searches for, loads, or invokes an installed Tracy package or profiler binary.

## Reference capture

[`traces/monkey-playground`](traces/monkey-playground) contains a checksummed Tracy 0.13.1 capture and detailed provenance used for end-to-end validation.
