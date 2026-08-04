# tracy-query implementation plan and feature checklist

This is the working implementation plan for querying Tracy 0.13.1 capture files. It defines the CLI contract, required Tracy data coverage, immutable acceptance criteria, staged work, and verification evidence.

## Goals

- Query one or more Tracy captures without opening the Tracy GUI.
- Report the usable time range of each capture.
- Query supported timeline data over a whole capture or inclusive time range.
- Find the latest, next, or active matching data at a time point.
- Combine multiple data kinds in one deterministically ordered query.
- Filter applicable text, names, and metadata with regular expressions.
- Discover Tracy's typed data sources and filter by natural scopes such as thread, GPU context, plot, frame set, lock, memory pool, and CPU.
- Preserve true nesting for CPU/GPU/ghost zones and ordered callstacks without inventing a universal trace hierarchy.
- Count matching records without emitting or retaining potentially large result sets.
- Produce deterministic, line-oriented output suitable for people and scripts.

## Scope

### In scope

- Offline `.tracy` files compatible with the pinned Tracy 0.13.1 parser.
- Capture validation, time ranges, capture metadata, and source discovery.
- Timestamped data kinds listed in the coverage matrix below.
- Natural source ownership, nested-zone structure, callstack order, and CPU topology.
- JSON Lines and readable one-record-per-line text output.
- Multiple input traces in one query.

### Out of scope for this plan

- Live capture or connecting to a running Tracy client.
- Modifying or rewriting captures.
- Recreating Tracy's GUI, statistics views, or SQL interface.
- Treating unrelated Tracy collections as hierarchical sub-traces.
- Supporting unreviewed Tracy server API versions. A version upgrade requires a separate compatibility review.

## Immutable acceptance criteria

These criteria must not change without explicit user approval.

1. `check` successfully loads valid Tracy 0.13.1 captures and clearly rejects missing, invalid, legacy, and unsupported captures.
2. `range` reports each trace's first timestamp, last timestamp, and duration.
3. Every timestamped kind in the coverage matrix can be queried over the whole capture and over an inclusive time range.
4. Point queries support latest-at-or-before and next-at-or-after per selected natural source stream; interval kinds also support active-at-time.
5. Any data query supports `--count`, and count mode uses exactly the same selection/filtering semantics without emitting matching records.
6. Multiple kinds and multiple traces can be combined in one query with deterministic ordering.
7. Applicable fields support validated ECMAScript regular-expression filters, including message text, plot/zone names, thread names, source files, and symbols.
8. `sources` reports typed natural Tracy sources. Scope filters operate on those sources; structural parent/ancestor/depth filters are limited to genuinely nested zones and callstacks.
9. Every normal data-result line includes `timestamp_ns`, `trace`, `source`, and `kind`. Interval records additionally include end and duration.
10. Non-timestamped metadata is exposed without inventing timestamps. Binary attachments are represented by queryable metadata and never mixed into line-oriented stdout.
11. Results and counts are verified against small deterministic fixtures and the checked-in `traces/monkey-playground` reference capture.
12. Query and count implementations remain bounded enough to process the 35.9 MB reference capture without collecting all matching output records in memory.

## Design rules and constraints

- Tracy is pinned to v0.13.1 commit `05cceee0df3b8d7c6fa87e9638af311dbabc63cb`; its server API is internal and version-specific.
- Keep Tracy-specific traversal behind project-owned adapters and project-owned `Trace`, `Source`, `Record`, `Interval`, and `Query` models.
- Tracy does not provide a universal hierarchy of sub-traces. Preserve only native ownership and real nested structures.
- Canonical source IDs are stable within a loaded trace and do not contain display names. Names are separate fields and may be duplicated.
- Query times are relative to each trace's first usable timestamp. Bare integers are nanoseconds; supported units are `ns`, `us`, `ms`, `s`, `m`, and `h`.
- Range boundaries are inclusive. Interval matching defaults to overlap and may explicitly use start or contained semantics.
- JSON Lines is the default format. Embedded newlines and other control characters must be escaped so one record always occupies one physical line.
- Normal output goes to stdout; diagnostics go to stderr. Broken pipes are handled quietly.
- Count mode must reuse the normal matching pipeline but skip record formatting and avoid retaining matches.
- Regexes are compiled once before traversal. Invalid regexes or unsupported fields fail before emitting partial results.
- Results are ordered by normalized timestamp, input trace order, kind, canonical source ID, and original record sequence.
- Duplicate Tracy storage views must not duplicate logical records; for example, choose one canonical traversal for globally and per-thread indexed messages.
- Incomplete or unavailable Tracy fields are omitted or explicitly marked; they are never guessed.
- [`README.md`](README.md) is the primary user-facing guide. Keep it concise, task-oriented, and current with supported installation, commands, examples, output, and limitations; implementation planning stays in this document.

## CLI contract

```text
tracy-query [GLOBAL OPTIONS] <COMMAND> [COMMAND OPTIONS] <TRACE>...

Commands:
  check       Load and validate captures, then exit
  range       Print the usable time range of each capture
  info        Print capture metadata and data-kind counts
  sources     List typed data sources present in captures
  query       Query timestamped records
```

The current form remains a compatibility shorthand for `check`:

```sh
tracy-query capture.tracy
tracy-query check capture.tracy another.tracy
```

### Global options

```text
--format <jsonl|text>     Output format; default: jsonl
--trace-label <P=L>      Use label L for trace path P; repeatable
--quiet                  Suppress non-result diagnostics
-h, --help               Show contextual help
--version                Show tracy-query and supported Tracy versions
```

### Capture range and metadata

```sh
tracy-query range capture.tracy
tracy-query info capture.tracy
tracy-query info --detail full capture.tracy
```

`info` includes trace version, capture/program name, wall-clock capture time, host, PID, timer resolution, CPU architecture, sampling period, app-info strings, parameters, CPU topology, and counts by data kind. Full detail summarizes source locations, symbols, callstacks, source cache, and symbol code without assigning fake timestamps.

### Typed source discovery and scoping

```sh
tracy-query sources capture.tracy
tracy-query sources --kind plot --kind message capture.tracy
tracy-query sources --source-type thread capture.tracy
tracy-query sources --filter 'name=Render|Main' capture.tracy
tracy-query sources --source-regex '^plot:' capture.tracy
```

Representative canonical source IDs:

```text
thread:42
gpu-context:0/thread:42
plot:3
frame-set:0
lock:17
memory-pool:0
cpu:3
hardware-counter:cycles
capture
```

`gpu-context:0/thread:42` reflects real GPU-context ownership. It is not a generic hierarchical path.

```text
--source <ID>             Select an exact source ID; repeat for OR
--source-regex <REGEX>    Match canonical source IDs
--source-type <TYPE>      Select source types; repeat for OR
--thread <REGEX>          Match thread ID or name
--gpu-context <REGEX>     Match GPU-context ID or name
--plot <REGEX>            Match plot name
--frame-set <REGEX>       Match frame-set name
--lock <REGEX>            Match lock ID or name
--memory-pool <REGEX>     Match memory-pool ID or name
--cpu <ID>                Match a scheduler CPU
```

Selectors of one type are ORed; different selector types are ANDed.

### Data queries

```sh
# Whole capture
tracy-query query --kind message capture.tracy

# Inclusive range and mixed kinds
tracy-query query --kind message --kind cpu-zone --kind plot \
  --from 2.5s --to 4s capture.tracy

# Multiple traces
tracy-query query --kind message,plot first.tracy second.tracy

# Latest and next plot value per matching plot
tracy-query query --kind plot --plot '^FPS$' \
  --at 10s --latest --next capture.tracy

# Zones active at a point
tracy-query query --kind cpu-zone --at 10s --active capture.tracy

# Scoped regex
tracy-query query --kind message \
  --filter 'message.text=timeout|retry' --ignore-case capture.tracy

# Natural thread scope
tracy-query query --kind cpu-zone --kind message \
  --thread '^Main$|^Render-' --from 1s --to 2s capture.tracy

# Real nested-zone structure
tracy-query query --kind cpu-zone --zone-ancestor '^Frame$' \
  --zone-depth 2:4 --from 1s --to 2s capture.tracy

# Count only
tracy-query query --kind all --from start --to 30s --count capture.tracy
tracy-query query --kind all --count --group-by trace --group-by kind capture.tracy
```

Kind and temporal options:

```text
--kind <KIND[,KIND...]>              Repeatable; values are ORed
--kind all                           Select every timestamped kind
--from <TIME>                        Omitted endpoint means capture start
--to <TIME>                          Omitted endpoint means capture end
--at <TIME> --latest                 Latest match at or before time, per source
--at <TIME> --next                   Next match at or after time, per source
--at <TIME> --active                 Intervals containing time
--range-match <overlap|start|contained>  Default: overlap
```

`--from`/`--to` and `--at` are mutually exclusive. If latest and next select the same exact record, it is emitted once.

Regex and structural options:

```text
--filter <[KIND.]FIELD=REGEX>  Repeatable; constraints are ANDed
--ignore-case                  Case-insensitive regex matching
--root-zones                   Select top-level CPU/GPU/ghost zones
--zone-depth <N|MIN:MAX>       Select zones by nesting depth
--zone-parent <REGEX>          Match a direct parent zone name
--zone-ancestor <REGEX>        Match any ancestor zone name
--stack-frame <REGEX>          Require a matching callstack frame/symbol
```

A kind-scoped filter affects only that kind, allowing mixed queries such as:

```sh
tracy-query query --kind message,plot \
  --filter 'message.text=error|warning' \
  --filter 'plot.name=FPS|Frame Time' capture.tracy
```

Count and output controls:

```text
--count                         Emit no records; print one total by default
--group-by <trace|kind|source>  Group counts; repeatable
--limit <N>                     Stop after N merged output records
--detail <basic|full>           Control callstack/source-location expansion
```

### Output contract

Every JSONL data record has these fields:

```text
timestamp_ns   Time relative to this trace's first usable timestamp
trace          Input path or --trace-label value
source         Canonical natural source ID
kind           Canonical data kind
```

Point-record example:

```json
{"timestamp_ns":24500123,"trace":"capture.tracy","source":"thread:42","kind":"message","text":"Loading level 7","thread_id":42,"thread_name":"Main"}
```

Interval example:

```json
{"timestamp_ns":25000000,"end_timestamp_ns":26700000,"duration_ns":1700000,"trace":"capture.tracy","source":"thread:42","kind":"cpu-zone","zone_id":918,"parent_id":901,"name":"Render","depth":2}
```

Frame images use the associated frame timestamp. Full-detail output may include the original Tracy timestamp, source location, symbols, callstacks, and attachment metadata.

## Tracy 0.13.1 coverage matrix

| Kind | Tracy data | Required output/detail |
|---|---|---|
| `message` | `MessageData` | text, thread, color, optional callstack |
| `plot` | `PlotData::data` / `PlotItem` | name, plot type/format, value |
| `cpu-zone` | nested thread `ZoneEvent` timelines | start/end, depth, parent, name/text, source location, thread, callstack |
| `gpu-zone` | GPU context/thread `GpuEvent` timelines | CPU/GPU times, parent, context, thread, source location, callstack, GPU notes |
| `frame` | `FrameData::frames` / `FrameEvent` | frame set/index, start/end, continuity |
| `frame-image` | `FrameImage` associated with a frame | frame timestamp/index, dimensions, flip, compressed size |
| `lock` | `LockMap::timeline` / `LockEvent` | operation, shared/exclusive, owner/waiters, contention |
| `memory` | default/named `MemData` / `MemEvent` | alloc/free, address, size, pool, thread, callstacks |
| `sample` | thread and context-switch samples | timestamp, thread, callstack/IP, resolved symbols |
| `ghost-zone` | sample-derived `GhostZone` | start/end, parent/depth, thread, reconstructed frame |
| `context-switch` | per-thread `ContextSwitchData` | interval, CPU, wakeup, reason/state |
| `cpu-slice` | per-CPU `ContextSwitchCpu` | interval, CPU, scheduled thread/process |
| `cpu-usage` | derived `ContextSwitchUsage` | timestamp, own/other CPU usage, derived marker |
| `hardware-sample` | `HwSampleData` | timestamp, address/symbol, cycles/retired/cache/branch counter |
| `crash` | `CrashEvent` | timestamp, thread, message, callstack |
| metadata | capture fields, app info, parameters, CPU topology | `info` output, without fake timestamps |
| reference metadata | strings, source locations, symbols, callstack frames | resolved into records and summarized by full `info` |
| attachments | frame pixels, source cache, symbol code | metadata only in this plan; never binary stdout |

## Checked-in reference capture

The end-to-end fixture is under [`traces/monkey-playground`](traces/monkey-playground):

- `0001-application.tracy` — Tracy 0.13.1 capture, 35,942,354 bytes.
- `0001-application.txt` — provenance, workload, known values, suggested investigations, and caveats.
- `zones.csv` — aggregate output generated by Tracy's official `tracy-csvexport`.
- `manifest.tsv`, `app.log`, `tracy-capture.log`, `sha256.txt`, and `csvexport.log` — capture evidence.

Known verification facts:

- SHA-256: `7b20e50534b1b3f2da094602af0a2f6c8049056a9e9bfcc91fae1964de07c765`.
- Tracy version: 0.13.1.
- Reported duration: 16.31 seconds.
- Reported zones: 617,218.
- Reported timer resolution: 10 ns.
- `zones.csv` reports 2,369 `engine.rt.cycle` zones.
- Tracy 0.13.1 `tracy-csvexport` parsed the capture with empty stderr.
- The current load-only `./build/tracy-query traces/monkey-playground/0001-application.tracy` exits 0; a local Release run completed in approximately 1.0 second. Timing is evidence, not a fixed performance threshold.

The `.txt` description is the human-readable oracle for representative zones, frame marks, plots, and expected final plot values. Tests must not assume the monkey workload itself is repeatable; they operate on the fixed capture bytes and verify its checksum first.

## Execution contract

- Keep this plan updated as work progresses and check off completed items.
- Keep `README.md` user-focused and update it in the same milestone whenever user-visible behavior, setup, examples, output, or limitations change.
- Commit each completed stage or meaningful milestone.
- Implementation steps may be revised when new evidence warrants it.
- Design rules may be revised for a documented, well-supported reason.
- Goals and acceptance criteria must not be changed without explicit user approval.

Stages are ordered. A stage starts only after its listed dependencies and verification are complete, unless the plan is explicitly revised with supporting evidence.

## Staged implementation checklist

### Stage 0 — foundation and fixture baseline

Dependencies: none.

- [x] Pin Tracy 0.13.1 and matching dependencies by commit and archive hash.
- [x] Build Tracy's internal server parser behind `TracyQuery::TracyServer`.
- [x] Load one capture and return useful invalid-file errors.
- [x] Add and checksum the `traces/monkey-playground` reference fixture and description.
- [x] Run the current CLI successfully against the reference `.tracy` file.

Verification:

- [x] `cmake --build build` succeeds.
- [x] Existing CTest tests pass.
- [x] `sha256sum -c traces/monkey-playground/sha256.txt` passes.
- [x] The load-only CLI exits 0 for the reference capture.

### Stage 1 — CLI and project-owned query model

Dependencies: Stage 0.

- [ ] Add `check`, `range`, `info`, `sources`, and `query` parsing while preserving the bare-trace shorthand.
- [ ] Support multiple trace inputs, labels, contextual help, and documented exit codes.
- [ ] Implement overflow-checked time parsing and normalized timestamp conversion.
- [ ] Add project-owned trace/source/record/query types and an adapter registry independent of Tracy headers.
- [ ] Centralize diagnostics, stdout/stderr behavior, and broken-pipe handling.

Verification:

- [ ] Unit-test every accepted/rejected command form and time literal.
- [ ] Verify single- and multi-trace `check` against valid, missing, and invalid files.
- [ ] Commit the completed stage.

### Stage 2 — range, info, and typed source discovery

Dependencies: Stage 1.

- [ ] Implement exact capture range and normalized/original timestamp fields.
- [ ] Implement basic/full `info`, including metadata and per-kind counts.
- [ ] Inventory natural source types without duplicating logical records.
- [ ] Assign stable canonical source IDs independent of display names.
- [ ] Implement source ID/type regexes and explicit scope selectors.

Verification:

- [ ] Compare reference duration, zone count, timer resolution, and trace version with the companion evidence.
- [ ] Test empty collections, duplicate source names, unnamed sources, and multi-trace IDs.
- [ ] Verify source inventory against representative threads, plots, frame sets, locks, memory pools, and CPUs in the reference capture.
- [ ] Commit the completed stage.

### Stage 3 — shared filtering, output, merge, and count pipeline

Dependencies: Stages 1–2.

- [ ] Implement JSONL and escaped one-record-per-line text writers.
- [ ] Implement deterministic chronological merging across adapters and traces.
- [ ] Implement scoped/unscoped regex filters with preflight field validation.
- [ ] Implement `--count`, optional grouping, and `--limit` using the same matching semantics.
- [ ] Ensure count mode and merge iterators do not retain all matching records.

Verification:

- [ ] Unit-test JSON escaping, multiline values, regex errors/scoping, ordering ties, grouping, truncation, and broken pipes.
- [ ] Prove normal and count modes return identical match totals on generated fixtures.
- [ ] Measure memory behavior on the reference capture and document evidence.
- [ ] Commit the completed stage.

### Stage 4 — core event adapters

Dependencies: Stage 3.

- [ ] Implement messages and all plot types.
- [ ] Implement nested CPU and GPU zones, including incomplete ends, extras, source locations, callstacks, calibration, and GPU notes.
- [ ] Implement frames and frame-image metadata.
- [ ] Implement lock operations and contention state.
- [ ] Implement default/named memory-pool allocation and free records.

Verification:

- [ ] Add deterministic fixtures for each adapter and boundary behavior.
- [ ] On the reference capture, count `^engine\.rt\.cycle$` CPU zones and match the expected 2,369.
- [ ] Query representative frame marks and latest plot values described in `0001-application.txt`.
- [ ] Compare representative records/counts with `zones.csv` and official exporter output.
- [ ] Commit the completed stage.

### Stage 5 — scheduler, sampling, crash, and reference detail

Dependencies: Stage 3; may proceed in parallel with Stage 4, but Stage 6 requires both.

- [ ] Implement thread context-switch intervals and per-CPU scheduler slices.
- [ ] Implement derived CPU-usage points with an explicit derived marker.
- [ ] Implement thread/context-switch samples and sample-derived ghost zones.
- [ ] Implement all Tracy hardware-sample counter categories.
- [ ] Implement crash records and full source-location/symbol/callstack expansion.

Verification:

- [ ] Add deterministic fixtures for present, absent, partial, and unresolved scheduler/sampling data.
- [ ] Cross-check representative scheduler/sample counts against Tracy's profiler where available.
- [ ] Verify full detail preserves callstack order and does not invent unresolved names.
- [ ] Commit the completed stage.

### Stage 6 — range, point, structural, and mixed-kind semantics

Dependencies: Stages 4–5.

- [ ] Implement whole-capture and inclusive range traversal for every adapter.
- [ ] Implement overlap/start/contained interval rules.
- [ ] Implement latest and next lookup per natural source stream.
- [ ] Implement active interval lookup.
- [ ] Implement zone root/depth/direct-parent/ancestor filters and stable zone/parent IDs.
- [ ] Implement ordered callstack-frame filtering.
- [ ] Combine every kind through the shared filtering, count, merge, and output pipeline.

Verification:

- [ ] Test exact boundaries, outside-capture points, incomplete intervals, nested active zones, and latest/next deduplication.
- [ ] Verify mixed-kind results are globally deterministic and individual-kind counts sum to the combined count.
- [ ] Run representative range/latest/next/active/count queries against the reference capture.
- [ ] Commit the completed stage.

### Stage 7 — final end-to-end validation and release readiness

Dependencies: all prior stages.

- [ ] Run formatting, static checks, unit tests, integration tests, and supported-platform CI.
- [ ] Review `README.md` as a new user: verify setup, core commands, examples, output, and limitations match the implemented release.
- [ ] Validate all acceptance criteria against small fixtures and the checksummed reference capture.
- [ ] Compare representative results with Tracy 0.13.1's GUI or official exporters.
- [ ] Benchmark whole-capture, narrow-range, point, mixed-kind, and count queries.
- [ ] Document memory behavior, performance evidence, unsupported data, and Tracy API limitations.
- [ ] Verify install output contains only intended project artifacts.
- [ ] Freeze the documented CLI/output contract for the first stable release.

Final verification surface:

- [ ] `check`, `range`, `info`, `sources`, and all query temporal modes succeed on the reference capture.
- [ ] Every coverage-matrix adapter has deterministic fixture coverage.
- [ ] Reference metadata matches 0.13.1, 10 ns resolution, 617,218 zones, and approximately 16.31 seconds.
- [ ] The `engine.rt.cycle` filtered CPU-zone count is exactly 2,369.
- [ ] Mixed-kind normal output and grouped/ungrouped count output satisfy the immutable acceptance criteria.
- [ ] The final end-to-end evidence is recorded in the plan or linked release notes.
- [ ] `README.md` contains only current user-facing guidance and has no stale CLI examples or behavior claims.
- [ ] Commit the completed stage or release milestone.
