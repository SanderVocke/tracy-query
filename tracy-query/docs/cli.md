# tracy-query CLI reference

## Commands

- `check FILE...`: validate one or more captures. A bare `tracy-query FILE` is shorthand.
- `range FILE...`: print usable timestamp ranges.
- `info [--detail full] FILE...`: print capture metadata and record counts.
- `sources [filters] FILE...`: discover natural source IDs and names.
- `query [selectors] FILE...`: stream matching records as JSON Lines or text.

Run `tracy-query --help` or `tracy-query <command> --help` for the authoritative option list.

## Record kinds

`message`, `plot`, `cpu-zone`, `gpu-zone`, `frame`, `frame-image`, `lock`, `memory`, `sample`, `ghost-zone`, `context-switch`, `cpu-slice`, `cpu-usage`, `hardware-sample`, and `crash`.

## Time and selection

Times are relative to each trace's first usable timestamp. Units are `ns`, `us`, `ms`, `s`, `m`, and `h`; ranges are inclusive.

```sh
tracy-query query --kind message,plot --from 2.5s --to 4s capture.tracy
tracy-query query --kind plot --plot '^Backend/cycles$' --at 10s --latest --next capture.tracy
tracy-query query --kind cpu-zone --at 10s --active capture.tracy
```

Interval records overlap the selected range by default. Use `--range-match start` or `--range-match contained` for other behavior.

## Filters and structure

Filters use ECMAScript regular expressions on applicable fields:

```sh
tracy-query query --kind message --filter 'message.text=error|warning' --ignore-case capture.tracy
tracy-query query --kind cpu-zone --filter 'cpu-zone.name=^engine\.rt\.' capture.tracy
tracy-query query --kind cpu-zone,message --thread '^Main$|Render-' capture.tracy
tracy-query query --kind cpu-zone --zone-ancestor '^Frame$' --zone-depth 2:4 capture.tracy
```

Use `sources` before guessing IDs/names. CPU/GPU zones are nested; plots, messages, locks, and frames are typed collections rather than universal children of zones.

## Counts, detail, and multiple traces

```sh
tracy-query query --kind all --count capture.tracy
tracy-query query --kind all --count --group-by trace --group-by kind a.tracy b.tracy
```

`--detail full` resolves additional callstack and symbol data. `--limit N` limits normal output, not the population considered by count mode. Multiple traces and kinds are merged deterministically by normalized timestamp.

## Output and status

JSON Lines is the default; `--format text` emits escaped key/value records. Data goes to stdout and diagnostics to stderr.

- 0: success
- 1: capture loading, reading, output, or execution failure
- 2: invalid command-line usage
