# Rust integration

The local `tracy-client-sys` directory remains package name/version `tracy-client-sys` 0.28.0 so Cargo can substitute it for the dependency used by unmodified `tracy-client` 0.18.4 and `tracing-tracy` 0.11.4.

```toml
[dependencies]
tracy-client = { version = "=0.18.4", default-features = false, features = ["enable", "manual-lifetime"] }
tracing-tracy = { version = "=0.11.4", default-features = false, features = ["enable", "manual-lifetime"] }
tracy-client-sys = { version = "=0.28.0", default-features = false, features = ["embedded-capture"] }

[patch.crates-io]
tracy-client-sys = { path = "../path/to/tracy-extensions/tracy-embedded-capture/rust/tracy-client-sys" }

[profile.release]
panic = "unwind"
```

Configure the C ABI before `tracy_client::Client::start()`, wait for capturing state, emit instrumentation, drop dispatchers/zones and join producers, then call the disposition-aware finalizer. `___tracy_embedded_capture_finish()` remains save-compatible. The nextest component provides a tested wrapper around this ordering.

`build.rs` can configure the root superbuild itself, or reuse a prepared native build through `TRACY_QUERY_CMAKE_BUILD_DIR`. The latter must contain `tracy_embedded_capture_native` built with matching Cargo features/runtime settings.

Use `cargo tree -i tracy-client-sys` to prove one patched package is shared by every higher-level Tracy crate. Provenance and license details are adjacent to the patch.
