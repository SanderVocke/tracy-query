# tracy-extensions

A collection of focused extensions around [Tracy Profiler](https://github.com/wolfpld/tracy), pinned to Tracy **0.13.1** and protocol **76**.

## Component inventory

| Component | Deliverable | Use it when | Documentation |
|---|---|---|---|
| [`tracy-query`](tracy-query/) | `tracy-query` CLI and query library | Inspect, validate, and query existing `.tracy` captures without the GUI | [README](tracy-query/README.md) · [CLI reference](tracy-query/docs/cli.md) · [architecture](tracy-query/docs/architecture.md) |
| [`tracy-embedded-capture`](tracy-embedded-capture/) | Native embedded capture library, C ABI v2, patched `tracy-client-sys` 0.28.0, and Rust example | Capture a normal Tracy client/server protocol session entirely inside one process | [README](tracy-embedded-capture/README.md) · [architecture and lifecycle](tracy-embedded-capture/docs/architecture.md) · [Rust integration](tracy-embedded-capture/docs/rust.md) |
| [`tracy-nextest-capture`](tracy-nextest-capture/) | Rust runtime and attribute macro for cargo-nextest | Retain per-attempt traces on unwind panic or `Result::Err`, while discarding successful attempts without writing | [README](tracy-nextest-capture/README.md) · [usage and policy](tracy-nextest-capture/docs/usage.md) · [architecture](tracy-nextest-capture/docs/architecture.md) |

The nextest component depends on embedded capture and uses `tracy-query` as its semantic test oracle. The root CMake project is a superbuild: Tracy pinning and static-link policy are shared, while each component owns its targets, tests, and detailed documentation.

## Build everything

Requirements are CMake 3.24+, a C++20 compiler, a CMake-supported build tool, Python 3 for process tests, and Cargo for Rust integrations. CI pins cargo-nextest 0.9.116.

```sh
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON \
  -DTRACY_QUERY_FULLY_STATIC=OFF
cmake --build build --parallel
ctest --test-dir build --output-on-failure
cmake --install build --prefix stage
stage/bin/tracy-query --version
```

Linux release builds are fully static by default; Windows uses the static MSVC runtime. Use `-DTRACY_QUERY_FULLY_STATIC=OFF` for local Linux systems without static C/C++ runtime archives.

## Release binaries

Release 0.4.0 provides the `tracy-query` executable for Linux, macOS, and Windows on x86-64 and ARM64:

| Platform | x86-64 | ARM64 |
|---|---|---|
| Linux | `tracy-query-linux-x86_64` | `tracy-query-linux-arm64` |
| macOS | `tracy-query-macos-x86_64` | `tracy-query-macos-arm64` |
| Windows | `tracy-query-windows-x86_64.exe` | `tracy-query-windows-arm64.exe` |

The embedded and nextest components are source integrations rather than standalone executables.

## Shared guarantees and limits

- Third-party sources are commit- and hash-pinned in [`cmake/TracyServer.cmake`](cmake/TracyServer.cmake).
- The embedded boundary preserves Tracy's serialized, bidirectional protocol over bounded memory.
- Capture finalization requires all instrumentation producers and guards to be quiescent.
- Only one embedded capture lifecycle is supported per process.
- Abort, fatal signals, timeouts, forced termination, OOM, and power loss cannot run an in-process finalizer; no trace is claimed for them.

## Repository migration

This repository is the full-history successor to the archived `SanderVocke/tracy-query` repository. GitHub does not permit a same-owner fork relationship, so history and tags were duplicated without a `fork: true` relationship.
