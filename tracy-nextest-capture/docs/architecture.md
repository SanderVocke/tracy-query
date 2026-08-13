# Nextest capture architecture and testing

The attribute macro keeps the capture owner outside `catch_unwind` and invokes the complete synchronous test body inside it. After normal return or unwinding has dropped body locals, guards, spans, and scoped dispatchers, the runtime selects save/discard. Panic payloads are resumed; `Result::Err` values are returned unchanged.

Activation requires all of `NEXTEST_ATTEMPT_ID`, `NEXTEST_TEST_NAME`, `NEXTEST_BINARY_ID`, and `NEXTEST_ATTEMPT`, plus an enabled policy. Filenames contain bounded sanitized display fragments and a SHA-256 prefix of the opaque attempt ID. Metadata cannot create path components.

## Component dependencies

- The embedded component supplies ABI v2, bounded protocol transport, and save/discard finalization.
- The query component validates every saved trace with `check`, `range`, `info`, and exact semantic queries.
- No network port, helper process, JUnit reconciliation, or post-exit decision is involved.

## Test contracts

From a root build configured with cargo-nextest 0.9.116:

```sh
ctest --test-dir build -R 'nextest-in-process-' --output-on-failure
```

The tests cover supported and rejected signatures, `off|failure|always`, complete/incomplete identity, ordinary Cargo/list inertness, pass/panic/Err, retries, concurrent producers, occupied Tracy ports, path confinement, unique publication, no partials, writer/finalizer failures, and direct/tracing semantic markers.

Dependency gates:

```sh
cargo tree --locked --manifest-path tracy-nextest-capture/tests/fixture/Cargo.toml -i tracy-client-sys
cargo tree --locked --manifest-path tracy-nextest-capture/tests/fixture/Cargo.toml -d
```

Expect one local patched sys package and no duplicate packages.

## Hard failures and upgrades

Abort, fatal signal, nextest timeout termination, forced exit, OOM, and power loss cannot run an in-process finalizer. No trace is claimed for them.

On a nextest upgrade, recheck process-per-test behavior and all identity variables, list/no-op behavior, retries, scheduling, and every policy. On a macro/runtime ABI change, update macro expansion tests, generated sys declarations, ABI assertions, lifecycle calls, docs, and process fixtures together.
