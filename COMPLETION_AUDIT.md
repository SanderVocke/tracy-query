# Completion audit

## Objective

Complete every deliverable and verification gate in [`FEATURE_CHECKLIST.md`](FEATURE_CHECKLIST.md): a user-documented Tracy 0.13.1 CLI that validates captures, discovers metadata/sources, queries every covered timeline kind, supports range/point/filter/count/mixed queries, and is backed by reproducible evidence.

## Prompt-to-artifact checklist

### Immutable acceptance criteria

| # | Requirement | Implementation evidence | Verification evidence |
|---|---|---|---|
| 1 | Validate valid captures and reject missing, invalid, legacy, or unsupported input | `Trace` in `src/trace.cpp`; `check` parser/dispatch in `src/cli.cpp` and `src/commands.cpp` | CTest: `cli-check-reference`, `cli-check-multiple`, `cli-check-synthetic`, `cli-rejects-invalid-trace`; unit parser rejection tests |
| 2 | Report first/last timestamps and duration | `run_range()` in `src/commands.cpp` | CTest: `cli-range-reference`, expected duration `16310852272` ns |
| 3 | Query every coverage-matrix kind over whole captures and ranges | Adapter traversal in `src/query.cpp`; registry in `src/adapter.cpp` | Coverage table below; `cli-query-all-count`; fixed synthetic captures and structural round trip |
| 4 | Latest, next, and active point queries per natural source | Point selection and deduplication in `run_query()` | CTest: `cli-query-plot-point`, `cli-query-active-zone`, `cli-query-synthetic-next-outside`, `cli-query-synthetic-active-nesting`, `cli-query-synthetic-point-deduplication` |
| 5 | Count mode shares normal matching without emitting records | Shared `Matcher`/temporal path; streaming `count_record` path | CTest: `cli-query-count-equivalence`, grouped and ungrouped count tests |
| 6 | Combine kinds and traces deterministically | 8,192-record external-sort runs and deterministic comparator in `src/query.cpp` | CTest: `cli-query-mixed-range`, `cli-query-synthetic-mixed-count`, `cli-query-multiple-traces` |
| 7 | Validated ECMAScript regex filters for applicable fields | Preflight in `src/cli.cpp`; compiled matcher in `src/query.cpp` | Unit invalid-regex/field tests; CTest: scoped message/plot, file, thread, zone-name, and symbol regex tests |
| 8 | Typed natural sources; hierarchy only for genuinely nested records | `discover_sources()` in `src/commands.cpp`; zone/ghost recursion in `src/query.cpp` | CTest source inventory, zone-parent/ancestor/depth/active nesting, GPU/scheduler structural fixture |
| 9 | Every data line has timestamp, trace, source, kind; intervals add end/duration | `emit_record()` in `src/output.cpp` | Unit JSONL/text contract and physical-line tests; integration output assertions |
| 10 | Metadata has no fake timestamps; binary attachments stay out of stdout | `info` and frame-image metadata in `src/commands.cpp`/`src/query.cpp` | `cli-info-reference`, `cli-query-synthetic-frame-image`; install/user documentation limitations |
| 11 | Verify against deterministic fixtures and checked reference capture | `traces/monkey-playground`, `traces/synthetic`, `tests/fixtures` | CTest checksum test, 45-test Release suite, official `zones.csv` comparison |
| 12 | Bounded matching/output memory on the 35.9 MB reference capture | Streaming count; bounded external-sort chunks and temporary runs | Measured whole `all` count and ordered mixed query; evidence recorded in the plan |

### Data-kind coverage

| Kind | Non-empty evidence |
|---|---|
| `message` | `0001-core.tracy`; `cli-query-synthetic-messages` |
| `plot` | `0001-core.tracy`; `cli-query-synthetic-plot`; latest/next reference test |
| `cpu-zone` | core and monkey captures; nested/boundary tests; exact 2,369 `engine.rt.cycle` count |
| `gpu-zone` | `0004-structural.tracy`; `cli-query-synthetic-structural`; generated writer/parser round trip |
| `frame` | `0001-core.tracy`; `cli-query-synthetic-frame` |
| `frame-image` | `0001-core.tracy`; 4×4 image metadata test |
| `lock` | `0001-core.tracy`; wait/obtain/release count test |
| `memory` | `0001-core.tracy`; named pool alloc/free test |
| `sample` | `0003-sampling.tracy`; exact 1,757 count |
| `ghost-zone` | `0003-sampling.tracy`; exact 1,705 count |
| `context-switch` | `0004-structural.tracy`; exact generated interval count |
| `cpu-slice` | `0004-structural.tracy`; exact generated CPU-slice count |
| `cpu-usage` | `0004-structural.tracy`; exact three-point reconstructed count |
| `hardware-sample` | `0003-sampling.tracy`; exact 8,059 count and symbol-regex test |
| `crash` | `0002-crash.tracy`; abort message and callstack metadata test |
| metadata/reference data/attachments | `info --detail full`, source location/callstack/symbol resolution, frame-image metadata |

### Named commands and user-visible contract

| Surface | Evidence |
|---|---|
| `check` and bare-trace shorthand | CLI parser tests and reference checks |
| `range` | Reference duration assertion |
| `info [--detail full]` | Reference metadata test and manual output comparison |
| `sources` with type/name/regex scopes | Source test plus reference/synthetic inventories |
| `query --from/--to` | Inclusive and mixed-range tests |
| `query --at --latest/--next/--active` | Point and active tests |
| `--range-match overlap/start/contained` | Synthetic interval boundary tests |
| `--filter`, `--ignore-case`, source scopes | Unit preflight and integration regex/scope tests |
| `--count`, `--group-by`, `--limit` | Count equivalence, grouped counts, truncation diagnostic |
| `--format jsonl|text`, `--detail basic|full` | Unit output tests and full callstack/symbol integration queries |
| Multiple traces and labels | Parser tests and `cli-query-multiple-traces` |
| Exit status and broken pipes | Unit/CLI failure tests; pipeline exits `0 0` when consumed by `head` |

## Verification runs

- Release configure/build: clean, with no project compiler warnings.
- Release CTest: **45/45 passed**.
- Debug CTest: **45/45 passed**.
- ASan/UBSan synthetic subset: **29/29 passed** after fixing the Tracy post-load background-task race.
- ASan/UBSan reference checks: capture load, CPU-zone count, and plot point tests all passed.
- Clean out-of-tree Release build with `BUILD_TESTING=OFF`: succeeded and queried the structural fixture correctly.
- Install audit: only `bin/tracy-query` installed; installed binary successfully queried a fixture.
- Reference checksum and every synthetic checksum: passed.
- Official-exporter comparison: `engine.rt.cycle` count is exactly 2,369, matching `zones.csv`; trace metadata matches Tracy 0.13.1, 10 ns resolution, 617,218 zones, and 16.31 seconds.

## Performance evidence

Linux x86-64 Release measurements on the fixed 35.9 MB monkey capture:

| Query | Elapsed | Maximum RSS |
|---|---:|---:|
| `query --kind all --count` | 5.36 s | 523,232 KiB |
| ordered `message,plot --limit 100` | 1.73 s | 528,596 KiB |
| latest/next plot at 15 s | 1.23 s | 522,992 KiB |
| narrow mixed count | 1.17 s | 522,632 KiB |

The Tracy worker owns most resident memory. Count matching does not retain matching records; normal global ordering uses bounded chunks and temporary files.

## CI and repository state

- `.github/workflows/ci.yml` configures Release build and full CTest on Ubuntu, macOS, and Windows.
- This local repository has no configured remote or PR, so no hosted status check exists to inspect. The executable's currently verified release platform is Linux x86-64 with GCC 14; hosted matrix execution begins when the repository is pushed.
- Implementation milestones are committed in `b932011` and `589b0b9`; the final audit/plan update is committed separately.

## Completion finding

No acceptance-criterion gap remains in the inspected local artifacts. Every timestamped adapter has non-empty fixed coverage, all temporal/filter/count/source/output contracts have direct tests, the large reference agrees with independent exporter evidence, bounded-memory behavior is implemented and measured, and the README reflects current behavior and limitations.
