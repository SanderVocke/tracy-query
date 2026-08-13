# tracy-query

`tracy-query` is a command-line reader for [Tracy Profiler](https://github.com/wolfpld/tracy) capture files. It validates captures, reports metadata and sources, and queries timestamped profiling data without opening Tracy's GUI. It also provides an opt-in in-process backend for failure-only per-test cargo-nextest captures.

The project is intentionally pinned to Tracy 0.13.1.

## Download

The [latest GitHub release](https://github.com/SanderVocke/tracy-query/releases/latest) provides uncompressed executables on Linux, macOS, and Windows on x86-64 and ARM64:

| Platform | x86-64 | ARM64 |
|---|---|---|
| Linux | `tracy-query-linux-x86_64` | `tracy-query-linux-arm64` |
| macOS | `tracy-query-macos-x86_64` | `tracy-query-macos-arm64` |
| Windows | `tracy-query-windows-x86_64.exe` | `tracy-query-windows-arm64.exe` |

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

Dependencies are commit- and hash-pinned and cached under `build/_deps`. On Linux, the installed `tracy-query` executable is fully static by default, and the build fails if it has a dynamic dependency. Use `-DTRACY_QUERY_FULLY_STATIC=OFF` only when building locally on a system without static runtime archives.

## Capture in process

An experimental opt-in backend embeds Tracy's 0.13.1 server-side Worker in the
instrumented process. Existing Tracy producer queues and serialization remain
unchanged, but the complete bidirectional protocol travels over bounded memory
streams instead of TCP. Controlled shutdown drains the client, writes a
same-directory partial capture, and atomically publishes a normal `.tracy` file.
No profiler GUI, helper process, discovery, or port is involved.

The runnable [`examples/rust-embedded-capture`](examples/rust-embedded-capture)
uses unmodified crates.io `tracy-client` and `tracing-tracy` packages with only a
repository-patched `tracy-client-sys`. It demonstrates normal completion and a
`panic=unwind` boundary that drops scoped spans, finalizes the trace, and resumes
the original panic:

```sh
python3 examples/rust-embedded-capture/run_demo.py \
  --mode normal --output out/rust-normal.tracy --query build/tracy-query
python3 examples/rust-embedded-capture/run_demo.py \
  --mode unwind-panic --output out/rust-panic.tracy --query build/tracy-query
```

See [`docs/in-process-capture.md`](docs/in-process-capture.md) for architecture,
lifecycle and safety requirements. For per-test cargo-nextest capture that
writes only unwind-panic and `Result::Err` failures, see
[`docs/nextest-in-process-capture.md`](docs/nextest-in-process-capture.md).
Only one capture/start/finish lifecycle is
supported per process. Every instrumentation-producing thread must be quiescent
before finish. Abort, fatal signals/exceptions, `SIGKILL`, OOM, power loss,
double panic, and concurrent instrumentation during finish remain undefined.
The in-process Worker consumes CPU, memory bandwidth, memory, and filesystem I/O
inside the profiled process. Hard process termination cannot publish an in-memory
capture; no trace is claimed for aborts, fatal signals, timeouts, or forced exits.

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
- In-process captures cannot finalize after aborts, fatal signals, timeouts, forced termination, OOM, or power loss.
- Begin with low traced-test concurrency and measure CPU, RSS, wall time, channel occupancy, and artifact size before expanding scope.
- A result kind is empty when the capture did not record that category.
- Frame images, cached source files, and symbol code are represented by metadata; binary extraction is not provided.
- Normal globally ordered queries use bounded in-memory chunks and temporary files. Very large output may require substantial temporary disk space. Count mode does not format or retain matching records.
- Full-detail callstack and symbol resolution is more expensive than basic output.

## Tracy integration

The project uses CMake `FetchContent` declarations in [`cmake/TracyServer.cmake`](cmake/TracyServer.cmake). Tracy is pinned to the exact commit behind **v0.13.1** (`05cceee0df3b8d7c6fa87e9638af311dbabc63cb`), and each downloaded archive is protected by a SHA-256 hash.

The standard Tracy package exposes the instrumentation client but not `TracyFileRead` and `TracyWorker`, which are needed to parse and create captures. This project compiles the exact upstream server source set into private static libraries used by `tracy-query` and the embedded capture backend. It never searches for, loads, or invokes an installed Tracy package or profiler binary.

## Reference capture

[`traces/monkey-playground`](traces/monkey-playground) contains a checksummed Tracy 0.13.1 capture and detailed provenance used for end-to-end validation.
