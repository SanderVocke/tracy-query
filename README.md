# tracy-query

A command-line trace query tool for [Tracy Profiler](https://github.com/wolfpld/tracy), currently at the initial milestone: load a trace file and exit.

The planned CLI, query semantics, data-source coverage, and implementation checklist are in [`FEATURE_CHECKLIST.md`](FEATURE_CHECKLIST.md).

## Build

Requirements:

- CMake 3.24 or newer
- Ninja (recommended), Make, or another CMake-supported build tool
- A C++20 compiler
- Internet access on the first configure

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Dependencies are cached by CMake under `build/_deps` after the first configure.

## Use

```sh
./build/tracy-query capture.tracy
```

A successful load has no output and exits with status 0. Invalid, unreadable, or unsupported captures produce an error and status 1. Invalid command-line usage exits with status 2.

## Tracy integration and versioning

The project uses CMake `FetchContent` declarations in [`cmake/TracyServer.cmake`](cmake/TracyServer.cmake). Tracy is pinned to the exact commit behind **v0.13.1** (`05cceee0df3b8d7c6fa87e9638af311dbabc63cb`), and the downloaded archive is protected by a SHA-256 hash. Tracy's matching Capstone, zstd, and PPQSort dependencies are pinned in the same way.

This is preferable to the standard Tracy vcpkg port for this use case. The normal Tracy package exposes `TracyClient`, which instruments applications, but does not install the internal server headers and implementation (`TracyFileRead` and `TracyWorker`) needed to parse trace files. This project therefore builds a private `TracyQuery::TracyServer` target from the exact upstream server source set. Keeping that integration in one CMake module gives future Tracy upgrades an explicit review boundary, which is important because these server APIs are not stable public APIs.
