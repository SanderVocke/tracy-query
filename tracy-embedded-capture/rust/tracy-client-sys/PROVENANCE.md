# Fork provenance

This directory is based on `tracy-client-sys` 0.28.0 from
<https://github.com/nagisa/rust_tracy_client>. The Rust declarations and
upstream feature names are retained under the upstream MIT/Apache-2.0 licenses.

Unlike the crates.io package, the build script compiles Tracy from the
`tracy-extensions` repository's single hash-pinned Tracy 0.13.1 source declaration in
`cmake/TracyServer.cmake`. The additional `embedded-capture` feature links the
project-owned in-memory transport, server Worker, capture coordinator, and C ABI.
The bundled Tracy source from the upstream sys crate is intentionally not copied
here, preventing an independent source/version from drifting.

The package name and 0.28.0 version are retained so Cargo `[patch.crates-io]`
substitution satisfies `tracy-client` 0.18.4's `>=0.23,<0.29` dependency. This
fork is initially intended only as a path or git patch and is not published to
crates.io.
