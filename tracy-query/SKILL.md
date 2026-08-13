---
name: tracy-query
description: Inspect and diagnose applications from existing Tracy Profiler 0.13.1-compatible .tracy capture files using the tracy-query CLI. Use for trace validation, metadata and source discovery, time-range and point queries, CPU/GPU zones, frames, plots, messages, locks, memory, sampling, scheduler activity, hardware samples, crashes, regex filtering, multi-trace comparisons, and grouped counts. Capture creation is out of scope.
compatibility: Requires a tracy-query executable and one or more existing Tracy 0.13.1-compatible capture files.
---

# Debug applications with `tracy-query`

Use `tracy-query` to investigate **existing** `.tracy` captures without opening Tracy's GUI. The parser is pinned to Tracy 0.13.1. Do not provide instructions for instrumenting an application, starting Tracy, or creating a capture; capture acquisition is outside this skill.

## Operating rules

1. Ask for the capture path and the symptom, regression, or time of interest if they are not already known.
2. Locate the executable. Prefer `tracy-query` on `PATH`; in this repository it may be `./build/tracy-query` or `./build-static/tracy-query`.
3. Run `--version`, then `check`, before interpreting a capture.
4. Inventory the capture with `range`, `info`, and `sources` before choosing filters. Do not assume thread names, plot names, frame sets, or source IDs.
5. Start with counts and narrow time windows. Emit full records only after reducing the search space.
6. Treat missing categories as “not recorded in this capture,” not proof that the application had no such activity.
7. Keep stdout as machine-readable evidence. Diagnostics are on stderr. Prefer JSON Lines unless the user asks for text.
8. Quote regular expressions so the shell does not expand them.
9. Report the exact command, trace label, normalized time range, filters, and relevant output used for each conclusion.

A convenient shell variable is:

```sh
TQ="${TRACY_QUERY:-tracy-query}"
"$TQ" --version
```

If `tracy-query` is not on `PATH`, set `TQ=./build/tracy-query` or use the actual executable path. The executable contains its Tracy parser and does not require an installed Tracy profiler binary.

## Investigation workflow

### 1. Validate and identify captures

```sh
"$TQ" check app.tracy
"$TQ" check before.tracy after.tracy
```

`check` loads every input and exits without data output. A bare path is shorthand for `check`:

```sh
"$TQ" app.tracy
```

Do not continue with a capture that fails validation. Status 1 indicates a loading, reading, output, or execution failure; status 2 indicates invalid CLI usage.

### 2. Establish time and capture context

```sh
"$TQ" range app.tracy
"$TQ" info app.tracy
"$TQ" info --detail full app.tracy
```

Use `range` to obtain normalized `first_timestamp_ns`, `last_timestamp_ns`, and `duration_ns`, plus Tracy's original timestamps. Query times and emitted timestamps are relative to the trace's first usable timestamp, so normalized time starts at zero.

Use `info` to inspect capture identity, program, host, process, timer resolution, sampling period, duration, and counts for the available data categories. `--detail full` additionally reports source-location, string, callstack, symbol/code, source-cache, application-info, parameter, and CPU-topology metadata.

### 3. Discover natural sources

```sh
"$TQ" sources app.tracy
"$TQ" sources --count app.tracy
"$TQ" sources --kind cpu-zone app.tracy
"$TQ" sources --source-type thread app.tracy
"$TQ" sources --plot 'latency|queue|memory' --ignore-case app.tracy
```

Source records include `trace`, `source`, `source_type`, optional `name`, comma-separated `kinds`, per-kind counts, and available first/last timestamps. Common source types are `thread`, `plot`, `gpu-thread`, `frame-set`, `lock`, `memory-pool`, `cpu`, `capture`, and `hardware-counter`.

Tracy uses typed natural collections, not one universal hierarchy. Discover source IDs and names first, then scope later queries with them.

### 4. Form a narrow, testable query

Prefer this progression:

1. Count a category over the whole capture.
2. Count it in the suspected time window.
3. Group by trace, kind, or source.
4. Select the relevant sources or names.
5. Emit basic records.
6. Use `--detail full` only when callstacks or resolved symbols are needed.

Example:

```sh
"$TQ" query --kind cpu-zone --from 4.5s --to 5s --count app.tracy
"$TQ" query --kind cpu-zone --from 4.5s --to 5s \
  --group-by source --count app.tracy
"$TQ" query --kind cpu-zone --from 4.5s --to 5s \
  --thread '^Render$' --limit 200 app.tracy
```

## Command and option reference

### Global options

These can be used with the applicable commands:

- `--format jsonl|text`: select JSON Lines (default) or escaped key/value text.
- `--trace-label PATH=LABEL`: replace an input path in output with a stable label; repeat for multiple traces.
- `--quiet`: suppress non-error diagnostics such as output-truncation notices.
- `--help`, `-h`: show help.
- `--version`: show the tool and pinned parser version.
- `--`: end option parsing, useful when a trace path starts with `-`.

Commands accept one or more trace paths. Multiple traces are loaded independently and results are merged deterministically by normalized timestamp, input order, kind, source, and record sequence.

### `check`

```sh
"$TQ" check TRACE...
```

Load and validate captures. It emits no data on success; use the exit status.

### `range`

```sh
"$TQ" range TRACE...
```

Emit one line per trace with normalized range/duration and original Tracy timestamps.

### `info`

```sh
"$TQ" info [--detail basic|full] TRACE...
```

Emit one metadata object per trace. Use the category counts to distinguish populated, empty, and sampling-heavy captures before querying.

### `sources`

```sh
"$TQ" sources [SOURCE OPTIONS] TRACE...
```

Relevant options:

- `--kind KIND[,KIND...]`: retain sources that provide any selected kind.
- `--source ID`: exact source ID; repeat to select alternatives.
- `--source-regex REGEX`: source-ID regex.
- `--source-type TYPE`: exact natural source type; repeat to select alternatives.
- `--thread REGEX`: thread ID or name.
- `--gpu-context REGEX`: GPU context/thread ID or name.
- `--plot REGEX`: plot name.
- `--frame-set REGEX`: frame-set name.
- `--lock REGEX`: lock ID or name.
- `--memory-pool REGEX`: memory-pool ID or name.
- `--cpu N`: exact CPU number; repeat for alternatives.
- `--filter name=REGEX`, `--filter source=REGEX`, or `--filter source_type=REGEX`.
- `--ignore-case`: make all regex selectors case-insensitive.
- `--count`: emit only the number of selected sources.

Repeated values within one selector type are alternatives (OR). Different selector types are combined (AND).

### `query`

```sh
"$TQ" query --kind KIND[,KIND...] [QUERY OPTIONS] TRACE...
```

`--kind` is required. Repeat it, pass a comma-separated list, or use `--kind all`.

Supported kinds:

- `message`: application messages, text, thread, and color.
- `plot`: numeric plot points and plot metadata.
- `cpu-zone`: nested CPU source scopes.
- `gpu-zone`: nested GPU scopes by GPU context/thread.
- `frame`: frame intervals by frame set.
- `frame-image`: frame-image metadata; binary image extraction is not provided.
- `lock`: lock wait/obtain/release-style events.
- `memory`: allocation and release events by memory pool.
- `sample`: callstack samples by thread.
- `ghost-zone`: sampled/reconstructed nested zones.
- `context-switch`: thread scheduling intervals.
- `cpu-slice`: intervals showing which thread/process ran on a CPU.
- `cpu-usage`: reconstructed usage points.
- `hardware-sample`: hardware-counter samples.
- `crash`: crash message, thread, and callstack metadata.

## Time selection

### Whole capture or inclusive range

Without a time option, `query` scans the whole capture.

```sh
"$TQ" query --kind message --from start --to end app.tracy
"$TQ" query --kind cpu-zone --from 2.5s --to 3s app.tracy
```

Accepted non-negative units are `ns`, `us`, `ms`, `s`, `m`, and `h`; no suffix means nanoseconds. Fractional values are accepted when they resolve exactly to nanoseconds. `start` and `end` are valid for range endpoints. Ranges are inclusive.

For interval kinds, choose how records relate to the range:

- `--range-match overlap` (default): interval intersects the range.
- `--range-match start`: interval starts inside the range.
- `--range-match contained`: entire interval is inside the range.

Point records simply need their timestamp inside the inclusive range.

### Point lookup

```sh
"$TQ" query --kind plot --at 10s --latest --next app.tracy
"$TQ" query --kind cpu-zone --at 10s --active app.tracy
```

`--at TIME` requires at least one of:

- `--latest`: newest matching record at or before the point, per trace/source/kind.
- `--next`: earliest matching record at or after the point, per trace/source/kind.
- `--active`: every matching interval spanning the point.

The flags may be combined; duplicate records are removed. `--active` is valid only for interval kinds (`cpu-zone`, `gpu-zone`, `frame`, `ghost-zone`, `context-switch`, and `cpu-slice`). Do not combine `--at` with `--from` or `--to`.

## Source, structural, and callstack selectors

The source selectors documented for `sources` also apply to `query`:

```sh
"$TQ" query --kind message,cpu-zone --thread '^Main$|Worker-[0-3]$' app.tracy
"$TQ" query --kind plot --source plot:3 app.tracy
"$TQ" query --kind cpu-slice --cpu 6 --from 1s --to 2s app.tracy
```

Zone structure options:

- `--root-zones`: only depth-zero zones.
- `--zone-depth N` or `--zone-depth MIN:MAX`: exact depth or inclusive depth range.
- `--zone-parent REGEX`: direct parent name.
- `--zone-ancestor REGEX`: any ancestor name.

Structure options apply only to `cpu-zone`, `gpu-zone`, and `ghost-zone`. Parent-name and ancestor-name filters are unavailable for `ghost-zone`; root/depth remain available.

Use `--stack-frame REGEX` to match expanded callstack text. This forces callstack expansion even without `--detail full`:

```sh
"$TQ" query --kind sample --stack-frame 'Physics::Step|integrate' \
  --from 7s --to 7.1s app.tracy
```

## Field regex filters

Syntax:

```sh
--filter FIELD=REGEX
--filter KIND.FIELD=REGEX
```

Filtering uses ECMAScript regex search. Add `^...$` for an exact match. `--ignore-case` applies to all regex filters and regex source selectors. Multiple filters are ANDed.

In a mixed-kind query, prefer kind-scoped filters. A scoped filter applies only to that kind and allows records of the other selected kinds to pass:

```sh
"$TQ" query --kind cpu-zone,message \
  --filter 'cpu-zone.name=^Update$' \
  --filter 'message.text=warning|error' app.tracy
```

An unscoped field must be valid for every selected kind. The valid fields are:

| Kind | Filter fields |
|---|---|
| `message` | `trace`, `source`, `kind`, `text`, `thread`, `color` |
| `plot` | `trace`, `source`, `kind`, `name`, `plot_type`, `format` |
| `cpu-zone` | `trace`, `source`, `kind`, `name`, `text`, `function`, `file`, `thread`, `color` |
| `gpu-zone` | `trace`, `source`, `kind`, `name`, `function`, `file`, `thread`, `context`, `note` |
| `frame`, `frame-image` | `trace`, `source`, `kind`, `frame_set` |
| `lock` | `trace`, `source`, `kind`, `name`, `operation`, `thread`, `function`, `file` |
| `memory` | `trace`, `source`, `kind`, `pool`, `operation`, `thread`, `symbol` |
| `sample`, `ghost-zone`, `hardware-sample` | `trace`, `source`, `kind`, `thread`, `symbol`, `function`, `file`, `counter` |
| `context-switch`, `cpu-slice`, `cpu-usage` | `trace`, `source`, `kind`, `thread`, `process`, `cpu`, `reason`, `state`, `derived` |
| `crash` | `trace`, `source`, `kind`, `message`, `thread`, `symbol` |

Examples:

```sh
"$TQ" query --kind message \
  --filter 'message.text=error|timeout' --ignore-case app.tracy

"$TQ" query --kind cpu-zone \
  --filter 'cpu-zone.name=^engine\.rt\.' \
  --filter 'cpu-zone.file=/src/audio/' app.tracy

"$TQ" query --kind hardware-sample --detail full \
  --filter 'hardware-sample.symbol=Scheduler|Worker' app.tracy
```

## Counting, grouping, limits, and detail

Use `--count` to run the same matching logic without formatting or retaining normal result records:

```sh
"$TQ" query --kind all --count app.tracy
"$TQ" query --kind all --count --group-by kind app.tracy
"$TQ" query --kind cpu-zone --count --group-by source app.tracy
```

Repeat `--group-by trace|kind|source` for compound groups:

```sh
"$TQ" query --kind message,cpu-zone --count \
  --group-by trace --group-by kind before.tracy after.tracy
```

`--limit N` limits normal record output after deterministic ordering. It does not change counts. Unless `--quiet` is used, truncation is reported on stderr. `--detail basic` is the default; `--detail full` resolves additional callstack and symbol information and may be substantially slower.

## Output contract and safe processing

JSON Lines is the default. Every timestamped record is one physical line and includes:

- `timestamp_ns`
- `trace`
- `source`
- `kind`

Intervals also include `end_timestamp_ns` and `duration_ns`. Kind-specific fields follow. Metadata, source, and count objects are also one physical line but are not timestamped records.

Use streaming tools when possible:

```sh
"$TQ" info app.tracy | jq .

"$TQ" query --kind cpu-zone --from 2s --to 2.1s app.tracy |
  jq -c 'select(.duration_ns > 1000000)'

"$TQ" query --kind message --from 2s --to 3s app.tracy |
  jq -r '[.timestamp_ns, .source, .text] | @tsv'
```

Do not use `jq -s` or load all output into memory for broad, high-volume queries. Narrow first, count first, or maintain a bounded top-N in a streaming script. Globally ordered normal output uses bounded memory plus temporary files; very large output may require substantial temporary disk space. Capture loading itself still uses Tracy's parser memory regardless of count mode.

`--format text` emits one escaped `key=value` record per line. Prefer JSONL for robust agent parsing.

## Multi-trace comparison

Assign concise labels so evidence does not depend on long paths:

```sh
"$TQ" query --kind cpu-zone --count --group-by trace \
  --trace-label before.tracy=before \
  --trace-label after.tracy=after \
  before.tracy after.tracy
```

Each trace is normalized to its own start. Multi-trace output compares relative capture time, not wall-clock alignment. Use grouped counts for a first comparison, then apply identical time/source/name filters to both traces. Do not claim a regression from raw event counts alone when capture durations differ; normalize or compare equivalent windows.

## Debugging recipes

### Slow frame or latency spike

1. Discover frame sets and relevant plots.
2. Query frames/plots around the reported time.
3. Find CPU/GPU zones overlapping the long frame.
4. Inspect active intervals at the spike.
5. Resolve callstacks only for the narrowed set.

```sh
"$TQ" sources --kind frame,plot app.tracy
"$TQ" query --kind frame,plot --from 12s --to 12.1s app.tracy
"$TQ" query --kind cpu-zone,gpu-zone --at 12.05s --active app.tracy
```

### CPU hotspot or unexpected work

```sh
"$TQ" query --kind cpu-zone --from 5s --to 6s \
  --count --group-by source app.tracy
"$TQ" query --kind cpu-zone --from 5s --to 6s \
  --thread '^Worker' --detail full app.tracy
```

Use `duration_ns` from emitted intervals for duration analysis. Duration is not a built-in regex filter field; filter numeric duration downstream after narrowing.

### Logs around a failure

```sh
"$TQ" query --kind message --from 8s --to 8.5s \
  --filter 'message.text=fail|error|timeout|warning' --ignore-case app.tracy
```

Correlate messages with active zones, plots, scheduler events, or memory events in the same normalized window.

### Lock contention

```sh
"$TQ" sources --kind lock app.tracy
"$TQ" query --kind lock --from 3s --to 4s \
  --filter 'lock.operation=wait|obtain|release' --detail full app.tracy
```

Inspect operation sequences and threads; do not infer wait duration unless the event fields support pairing or the surrounding evidence establishes it.

### Memory behavior

```sh
"$TQ" sources --kind memory app.tracy
"$TQ" query --kind memory --memory-pool '^Default$' \
  --from 1s --to 2s --detail full app.tracy
```

Compare allocation and release operations, addresses, sizes, threads, and symbols. The metadata count is allocation-oriented; emitted memory records represent timestamped operations.

### Scheduler starvation or migration

```sh
"$TQ" query --kind context-switch,cpu-slice,cpu-usage \
  --from 6s --to 6.2s --count --group-by kind app.tracy
"$TQ" query --kind context-switch,cpu-slice \
  --from 6s --to 6.2s --cpu 4 app.tracy
```

Correlate CPU slices/context switches with active CPU zones and thread samples. Scheduler collections may be absent even when zones exist.

### Sampling and hardware evidence

```sh
"$TQ" query --kind sample,ghost-zone --from 9s --to 9.1s \
  --thread '^Main$' --detail full app.tracy
"$TQ" query --kind hardware-sample --from 9s --to 9.1s \
  --count --group-by source app.tracy
```

Samples are observations, not exact duration measurements. Sampling period and missing symbols affect interpretation.

### Crash analysis

```sh
"$TQ" query --kind crash app.tracy
"$TQ" query --kind crash --detail full app.tracy
"$TQ" query --kind message --at 10s --latest app.tracy
```

Use the crash timestamp to query preceding messages, latest plot values, active zones, memory events, and scheduler state. A crash record may have unresolved or incomplete callstack data.

## Interpretation pitfalls

- The parser is pinned to Tracy 0.13.1; reject unsupported captures rather than guessing.
- All query times are normalized relative times, not capture wall-clock timestamps.
- Range matching for intervals defaults to overlap, which may include intervals that started before `--from`.
- Latest/next are selected per trace/source/kind, not once globally.
- Source IDs are typed and capture-specific; discover them instead of hard-coding assumptions.
- Regex matching searches substrings unless anchored.
- Scoped filters in mixed queries do not filter the other kinds.
- Empty results can mean absent instrumentation, disabled sampling/scheduler collection, an incorrect source name, or a genuinely absent event.
- `--detail full` can change symbol/callstack visibility and cost; basic output may contain IDs without resolved text.
- Frame images, cached source files, and symbol code are metadata only; binary extraction is not supported.
- Counts establish frequency, not duration or causal relationships.
- Temporal correlation supports a hypothesis but does not by itself prove causality.

When concluding an investigation, distinguish observed facts from interpretations and list any missing trace categories that limit confidence.
