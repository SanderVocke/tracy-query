# tracy-query architecture and testing

The component wraps Tracy 0.13.1's server-side file reader and Worker model behind repository-owned C++ interfaces.

- `trace.cpp` owns capture loading and Tracy model lifetime.
- `adapter.cpp` translates Tracy internals into stable component model records.
- `model.cpp`, `time.cpp`, and `query.cpp` implement typed selection, time normalization, structural filters, ordering, and counts.
- `output.cpp` writes JSON Lines or text.
- `cli.cpp`, `commands.cpp`, and `main.cpp` implement the command surface and status behavior.

The root [`cmake/TracyServer.cmake`](../../cmake/TracyServer.cmake) is the only dependency pin. The query component links `TracyQuery::TracyServer`; it does not search for or run an installed Tracy profiler.

## Tests

`tests/` covers CLI parsing, known reference counts/ranges, source discovery, time boundaries, structural relationships, mixed kinds, multi-trace ordering, checksums, and generated structural traces. `traces/` contains reference/synthetic captures and provenance.

From the root build:

```sh
ctest --test-dir build -R 'unit|fixture|cli-' --output-on-failure
```

The embedded and nextest components also invoke `tracy-query check`, `range`, `info`, and semantic queries as an independent capture oracle.

## Upgrade constraints

A Tracy update requires changing the one pinned source declaration, reviewing file format/Worker API changes, regenerating or replacing fixtures where justified, and revalidating every semantic count. Never accept a green build that silently omits moved tests or trace fixtures.
