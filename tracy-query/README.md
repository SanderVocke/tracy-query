# tracy-query

`tracy-query` is a command-line reader for Tracy 0.13.1 capture files. It validates captures, reports ranges/metadata/sources, and performs deterministic timestamped queries without opening Tracy's GUI.

## Build and install

From the repository root:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target tracy-query
cmake --install build --prefix stage
stage/bin/tracy-query --version
```

The installed command name and release asset names remain `tracy-query`.

## Quick use

```sh
tracy-query check capture.tracy
tracy-query range capture.tracy
tracy-query info --detail full capture.tracy
tracy-query sources capture.tracy
tracy-query query --kind cpu-zone,message --from 2s --to 3s capture.tracy
tracy-query query --kind all --count --group-by kind capture.tracy
```

See [the complete CLI reference](docs/cli.md), [architecture and testing](docs/architecture.md), and the [agent skill](SKILL.md).

## Interfaces and ownership

- `src/` and `include/tracy_query/`: parser adapter, model, query engine, output, and CLI.
- `tests/`: unit, CLI, semantic, checksum, multi-trace, and structural fixture tests.
- `traces/`: checksummed reference and synthetic Tracy captures with provenance.
- `SKILL.md`: an LLM-agent workflow for debugging existing captures.

The component uses the repository's single pinned Tracy server target. It is also used by the nextest integration tests to validate generated captures semantically.

## Limitations

Tracy's server-side parser is internal and version-specific. The component is pinned to Tracy 0.13.1; upgrades require explicit parser/model and fixture review. Large globally ordered output may use temporary disk space, and full symbol/callstack resolution is more expensive than basic output.
