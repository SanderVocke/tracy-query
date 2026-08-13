# Provenance

`tracy-nextest-capture` is repository-owned code first implemented for
`tracy-extensions`; it is not derived from a published third-party helper crate.

It integrates the unmodified crates.io packages `tracy-client` 0.18.4 and the
repository's package-identical `tracy-client-sys` 0.28.0 patch. Native fork and
Tracy source provenance are documented in `../tracy-client-sys/PROVENANCE.md`.
The crate is not published to crates.io.
