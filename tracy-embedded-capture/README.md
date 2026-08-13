# tracy-embedded-capture

An in-process Tracy 0.13.1 capture backend. The normal Tracy producer queues and complete serialized protocol remain intact, but bidirectional traffic uses bounded memory streams instead of TCP. On orderly shutdown the embedded Worker can atomically save or discard the model.

## Deliverables

- Native target `tracy_embedded_capture_native` and compatibility alias `TracyQuery::EmbeddedCapture`.
- Public versioned C ABI in `include/tracy_embedded_capture/embedded_capture.h`.
- Package-identical `tracy-client-sys` 0.28.0 patch under `rust/`.
- Rust example under `examples/rust-embedded-capture/` using unmodified `tracy-client` 0.18.4 and `tracing-tracy` 0.11.4.
- Native transport/lifecycle/error/semantic tests under `tests/`.

## Build and test

From the repository root:

```sh
cmake -S . -B build -G Ninja -DBUILD_TESTING=ON -DTRACY_QUERY_FULLY_STATIC=OFF
cmake --build build --target tracy_embedded_capture_native
ctest --test-dir build -R 'embedded-' --output-on-failure
```

Rust demonstration:

```sh
python3 tracy-embedded-capture/examples/rust-embedded-capture/run_demo.py \
  --mode normal --output out/normal.tracy --query build/tracy-query
python3 tracy-embedded-capture/examples/rust-embedded-capture/run_demo.py \
  --mode unwind-panic --output out/panic.tracy --query build/tracy-query
```

See [architecture/lifecycle](docs/architecture.md) and [Rust integration](docs/rust.md).

## Safety contract

Only one configure/start/finish lifecycle is supported per process. Every instrumentation-producing thread must be joined and all Tracy/tracing guards dropped before finalization. `panic=unwind` can be caught and finalized; aborts, fatal signals, timeouts, forced termination, OOM, and power loss cannot run the finalizer and produce no claimed trace.
