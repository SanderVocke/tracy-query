# Tracy extensions repository restructure and migration plan

## Status and execution contract

This is an implementation and migration plan, not an implementation. Paths are
relative to the current `SanderVocke/tracy-query` checkout unless a stage says
otherwise.

- Current status: planning complete; implementation has not started.
- Keep the plan updated as work progresses and check off completed items.
- Commit each completed stage or meaningful milestone.
- Implementation steps may be revised when new evidence warrants it.
- Design rules may be revised for a documented, well-supported reason.
- Goals and acceptance criteria must not be changed without explicit user approval.
- Do not archive or otherwise make the source repository read-only until the
  successor repository, exact release commit, CI, release, and assets are proven.

## Current evidence and migration constraint

- The current public repository is `SanderVocke/tracy-query`, default branch
  `master`, and it is not archived. `SanderVocke/tracy-extensions` does not
  currently exist.
- The current tree combines three distinct products in root-level `src/`,
  `include/`, `rust/`, `examples/`, `tests/`, and `docs/` directories:
  `tracy-query`, the embedded capture client/server backend, and cargo-nextest
  lifecycle integration.
- The existing release workflow produces six `tracy-query` executables: Linux,
  macOS, and Windows on x86-64 and ARM64. The embedded and nextest components are
  source integrations, not standalone executables.
- GitHub does not permit a repository owner to create a fork of that owner's own
  repository in the same personal account, even with a different repository
  name. Therefore `SanderVocke/tracy-extensions` must be created as a full-history
  duplicate/successor rather than as a GitHub fork-network child. Preserve the
  commit graph and tags and document this supported equivalent. If a literal
  GitHub `fork: true` relationship is required, stop before repository creation
  and request a different destination organization/account.

## Goals and scope

1. Restructure the checkout into three obvious top-level component directories:
   `tracy-query/`, `tracy-embedded-capture/`, and `tracy-nextest-capture/`.
2. Give every component its own `README.md` and owned `docs/` tree, while keeping
   shared Tracy pinning, CMake policy, and CI logic centralized where reuse is
   real.
3. Turn the root `README.md` into a concise inventory and navigation page rather
   than a combined manual.
4. Preserve current query behavior, embedded capture ABI/protocol behavior, Rust
   dependency topology, nextest policy behavior, tests, platform guarantees, and
   release binary names.
5. Rename the repository-level product/version identity to `tracy-extensions`
   0.4.0 without renaming the `tracy-query` executable or package-compatible
   `tracy-client-sys` 0.28.0 patch.
6. Create the public successor repository `SanderVocke/tracy-extensions` with the
   current history and tags, validate it, and publish release `v0.4.0` there.
7. Archive `SanderVocke/tracy-query` only after the successor and release are
   independently usable and verifiably complete.

## Non-goals

- Do not change Tracy 0.13.1/protocol 76 or unpin third-party dependencies.
- Do not reintroduce the removed external collector daemon.
- Do not publish the repository-local Rust helper crates to crates.io in this
  migration.
- Do not invent standalone binaries for the embedded or nextest components.
- Do not rename the `tracy-query` command, its six release asset stems, the
  public embedded C ABI symbols, or the patched `tracy-client-sys` package.
- Do not delete source repository history, tags, releases, issues, or other
  historical GitHub records; archiving is the terminal source-repository action.
- Do not archive the source as a proxy for successful migration.

## Immutable acceptance criteria

1. The successor source tree has top-level `tracy-query/`,
   `tracy-embedded-capture/`, and `tracy-nextest-capture/` directories. Product
   source, public headers, tests, examples, fixtures, Rust crates, and detailed
   docs have one documented owning component; intentional cross-component tests
   are explicitly identified.
2. Each component contains a useful `README.md` and a `docs/` directory covering
   its interfaces, build/use workflow, architecture/lifecycle, limitations,
   testing, and links to dependencies or consumers.
3. Root `README.md` is an inventory table/navigation page covering all three
   components, their deliverables, support status, relationships, and entry
   points. Detailed CLI, C ABI, Cargo, and nextest instructions live under the
   owning component.
4. Shared Tracy FetchContent declarations, exact 0.13.1/protocol 76 assertions,
   common static-link policy, and reusable test/release helpers have one source
   of truth; component CMake files do not copy dependency definitions.
5. Root CMake remains a usable superbuild and delegates component targets/tests
   through component CMake files. Existing direct Cargo workflows remain usable
   with locked manifests, and no build script depends accidentally on the old
   directory depth.
6. `tracy-query` retains its CLI, query semantics, reference/synthetic trace
   coverage, installed command name, and six release asset names.
7. Embedded capture retains its bounded serialized duplex transport, versioned C
   ABI, save/discard semantics, package-identical `tracy-client-sys` 0.28.0
   patch, Rust example, sanitizer coverage, and one-lifecycle safety contract.
8. Nextest capture retains synchronous `()`/`Result<(), E>` support,
   `off|failure|always`, complete-identity activation, panic/`Err` preservation,
   retry/concurrency-safe naming, exact semantic trace validation, compile-time
   exclusions, and no activity under ordinary multi-test `cargo test`.
9. Repository-owned product metadata and documentation identify
   `SanderVocke/tracy-extensions` and version 0.4.0. The sys patch remains 0.28.0;
   repository-local helper crates may become 0.4.0. Stale operational links to
   `SanderVocke/tracy-query` remain only in an explicit migration/history note.
10. Clean local builds, all CTests, locked Cargo tests/trees, install/smoke tests,
    static checks, sanitizers, and Linux/macOS/Windows x86-64/ARM64 CI pass after
    the move. Verifiers must use the relocated paths rather than skip coverage.
11. `SanderVocke/tracy-extensions` is public, has `master` as default, contains
    the release commit and prior history/tags, has Actions enabled, and exposes
    working repository metadata and component links. Because same-owner literal
    forking is impossible, API evidence records it as the documented
    full-history successor.
12. Tag `v0.4.0` points to one exact successful successor commit. Release 0.4.0
    is published from that tag with six non-empty, correctly named
    `tracy-query` binaries. Native matrix jobs run each installed binary's
    `--version`; Linux remains fully static and Windows retains static runtime.
13. Downloaded release metadata/assets are audited through the GitHub API for
    tag/commit association, names, count, non-zero sizes, and successful
    producing jobs before source archival.
14. Only after criteria 1–13 pass, `SanderVocke/tracy-query` has its description
    and homepage pointed to the successor and GitHub reports `archived: true`.
    The archived repository's historical tags/releases remain available.
15. Both repositories have the intended final state with no uncommitted local
    changes, no unpushed migration commit, no dangling partial release, and a
    recorded cutover/evidence log in this plan.

## Target layout and ownership

Freeze exact filenames in Stage 1, preserving these boundaries:

```text
README.md                         # inventory and common entry points only
CMakeLists.txt                    # superbuild/common policy and add_subdirectory
cmake/                            # one Tracy pin and shared build/release helpers
.github/workflows/                # repository-wide matrix and release automation

tracy-query/
  README.md
  docs/                           # CLI/reference, architecture, agent workflow
  CMakeLists.txt
  include/tracy_query/
  src/
  tests/
  traces/
  SKILL.md

tracy-embedded-capture/
  README.md
  docs/                           # ABI, transport/lifecycle, Rust integration
  CMakeLists.txt
  include/tracy_embedded_capture/
  src/
  rust/tracy-client-sys/
  examples/rust-embedded-capture/
  tests/

tracy-nextest-capture/
  README.md
  docs/                           # policy, annotation, CI, limitations/upgrades
  crates/tracy-nextest-capture/
  crates/tracy-nextest-capture-macros/
  tests/fixture/
  tests/                          # process harnesses and failure modes
```

The nextest component may consume embedded capture and use `tracy-query` as its
semantic trace oracle. Those are explicit component dependencies, not reasons to
mix their source trees. Root integration tests should exist only when ownership
cannot be assigned honestly to one consumer component.

## Design rules and constraints

### Build and dependency boundaries

- Keep the root CMake file small: project/version, common options, shared Tracy
  dependency setup, and `add_subdirectory` calls. Put target/test declarations
  with their owning component.
- Keep one exact Tracy source declaration and one shared native configuration
  helper. Do not compile package-incompatible Tracy versions into one process.
- Preserve target names and compatibility aliases where consumers/build scripts
  depend on them. Add temporary path/option compatibility only when tested and
  documented.
- Update Cargo path dependencies, `[patch.crates-io]`, `build.rs` rerun paths,
  CMake build-directory injection, Python harness paths, lockfiles, and generated
  binding provenance together.
- A component move is incomplete if tests merely disappear from `ctest -N`.
  Record before/after test inventories and explain every intentional rename.

### Documentation boundaries

- Root documentation answers “what is available and where do I start?” Each
  component README answers “what does this component deliver and how do I use
  it?” Detailed contracts belong in that component's `docs/` directory.
- Preserve safety limitations prominently: unwind-only recovery, required
  quiescence, one lifecycle per process, and no capture claim for hard process
  termination.
- Move the query agent skill with the query component and update release and root
  links accordingly.
- Validate every relative Markdown link and every repository URL after moves.

### Version and release boundaries

- Use repository/release version 0.4.0 consistently in CMake project metadata,
  `tracy-query --version`, release title/notes, and repository-owned crate
  metadata chosen for release. Do not alter upstream/package-substitution
  versions merely for cosmetic consistency.
- `v0.4.0` is created only in the successor repository after migration CI passes.
  Do not reuse or move earlier tags.
- Release automation downloads only artifacts from the same tagged workflow and
  verifies exactly the six expected query binaries before publishing.

### Repository migration safety

- Authenticate as `SanderVocke` with admin access and verify the destination name
  is absent immediately before creation.
- Use GitHub's documented repository-duplication procedure (bare clone plus
  mirror push, or an evidence-equivalent API/CLI sequence) to preserve history
  and tags. Record that this is not a fork-network relationship because GitHub
  rejects same-owner forks.
- Keep source and successor remotes distinctly named; print and verify URLs
  before every push/tag operation.
- Recreate relevant metadata/settings deliberately: public visibility, default
  branch, Actions permissions, description, homepage, topics, and release
  workflow permissions. Do not assume GitHub settings copy with Git objects.
- Treat archiving as irreversible during normal execution. If successor creation,
  CI, tagging, release publication, asset audit, or settings verification is
  blocked, stop with the source unarchived and report the exact blocker.

## Staged implementation

### Stage 1 — Freeze inventory, layout, compatibility, and migration runbook

Dependencies: current clean `master` and this plan branch.

- [ ] Record the current tracked-file ownership map, CMake targets, CTests, Cargo
  manifests/trees, release asset contract, repository settings, branches/tags,
  and baseline CI commit.
- [ ] Freeze exact target layout, component dependency graph, test ownership,
  component README/docs outlines, and shared-file allowlist.
- [ ] Identify every old relative path and repository URL in CMake, Cargo,
  Python, workflows, Markdown, generated/provenance files, and release notes.
- [ ] Decide version ownership: umbrella/query/runtime/macro versions versus the
  immutable sys patch version, and record lockfile consequences.
- [ ] Write the exact successor creation/settings/tag/release/archive command
  runbook with source/destination guards and rollback/blocked stop points.

Verification:

- [ ] Review the map so every tracked non-generated file has one destination or
  an explicit deletion rationale.
- [ ] Save `cmake --build ... --target help`, `ctest -N`, Cargo metadata/tree,
  Git refs, release list, and repository API output as before-state evidence.
- [ ] Confirm with GitHub API/docs that a literal same-owner fork is impossible
  and the approved full-history duplicate is the only unblocked interpretation.

### Stage 2 — Separate the `tracy-query` component

Dependencies: Stage 1 ownership map.

- [ ] Move query sources, public headers, tests, trace fixtures/provenance, and
  query agent skill into `tracy-query/` without content loss.
- [ ] Add `tracy-query/CMakeLists.txt` owning query libraries, executable,
  install/static checks, fixtures, and CLI/query tests.
- [ ] Add `tracy-query/README.md` and query-owned docs for installation, complete
  CLI use, data model, architecture, testing, limitations, and skill use.
- [ ] Update include paths, source metadata assertions, fixture checksum scripts,
  installed paths, and release skill path for the relocation.

Verification:

- [ ] Build/install `tracy-query`; run `--version`, help, check/range/info,
  reference/synthetic semantic suites, structural roundtrip, and multi-trace tests.
- [ ] Compare query target/test inventory and known semantic counts with baseline.
- [ ] Verify reference trace checksums/provenance and query skill links still pass.

### Stage 3 — Separate the embedded capture component

Dependencies: shared CMake policy and Stage 1 map; may proceed after Stage 2 paths
are stable.

- [ ] Move embedded C ABI headers, transport/coordinator sources, native tests,
  sys patch, Rust example, and detailed docs into `tracy-embedded-capture/`.
- [ ] Add component CMake ownership while reusing root Tracy dependency targets
  and preserving `TracyQuery::EmbeddedCapture`/required compatibility targets.
- [ ] Update the sys build script to locate the new root/component robustly and
  update all rerun paths, native target paths, licenses, provenance, bindings,
  manifests, lockfiles, and example commands.
- [ ] Add component README/docs covering C/C++ and Rust consumption, ABI v2,
  transport protocol, lifecycle, save/discard, diagnostics, safety, upgrades,
  and tests.

Verification:

- [ ] Run transport, native save/discard/error/failure-injection, loopback control,
  Rust normal/panic, ABI/version, and semantic capture tests from clean outputs.
- [ ] Prove discard still records zero writer/write/publish calls and no output.
- [ ] Run the patched sys ordinary-client and dependency-resolution checks.

### Stage 4 — Separate the cargo-nextest integration

Dependencies: Stage 3 component paths and ABI stable.

- [ ] Move runtime/macro crates, compile contracts, process fixture, Python
  harnesses, and nextest docs into `tracy-nextest-capture/`.
- [ ] Update path dependencies and patches to the embedded component, fixture
  manifests/lockfiles, CMake test registration, target-directory injection, and
  harness/query executable paths.
- [ ] Add component README/docs covering supported signatures, annotation,
  policies, environment, naming/retries/concurrency, CI artifacts, diagnostics,
  safety, unsupported hard failures, observer effects, and upgrade checks.
- [ ] Preserve strict no-op activation outside a complete nextest attempt and
  compile rejection for async, panic-abort, should-panic, incompatible harnesses,
  and unsupported return types.

Verification:

- [ ] Run helper unit/compile tests and pinned nextest 0.9.116 under `off`,
  `failure`, and `always`, including retries, concurrency, occupied ports,
  finalizer failures, exact trace semantics, and no partial files.
- [ ] Prove an ordinary cargo test body and cargo/nextest listing remain inert.
- [ ] Verify one patched sys package, exact higher-level crate versions, and an
  empty duplicate dependency tree.

### Stage 5 — Convert the root into the `tracy-extensions` inventory/superbuild

Dependencies: Stages 2–4.

- [ ] Reduce root CMake to shared project policy/dependencies and component
  delegation; add common helpers only where at least two components use them.
- [ ] Replace root README with the component inventory and concise common
  build/release navigation.
- [ ] Update repository-owned metadata, URLs, workflow paths, badges, generated
  provenance, and user-facing version output to the frozen 0.4.0 contract.
- [ ] Add a deterministic layout/link/reference audit rejecting stale old paths,
  stale operational repository URLs, missing component docs, and accidental
  duplicate shared definitions.
- [ ] Update `.gitignore` and clean generated artifacts so the new layout has no
  tracked or untracked build residue.

Verification:

- [ ] Configure from the repository root and enumerate expected targets/tests by
  component; no baseline test is silently absent.
- [ ] Validate all Markdown relative links and all CMake/Cargo/Python file paths.
- [ ] Search for old layout and `SanderVocke/tracy-query` references and review
  each allowed historical/migration occurrence.

### Stage 6 — Update CI, install, and 0.4.0 release automation

Dependencies: stable restructured paths and version contract.

- [ ] Update six-platform jobs, sanitizer jobs, Cargo caches/fetches, artifact
  diagnostics, installation, static checks, and smoke tests for relocated paths.
- [ ] Keep one workflow capable of validating branches and tagged releases;
  ensure release publication requires all matrix/sanitizer dependencies.
- [ ] Update release and existing-artifact workflows for repository-agnostic
  links, `v0.4.0`, relocated skill/docs, and exactly six query assets.
- [ ] Add release manifest verification for names, count, non-zero sizes,
  tag/SHA, and producing workflow run.

Verification:

- [ ] Locally validate workflow YAML and inspect release glob/count logic against
  synthetic expected asset lists.
- [ ] Install to a fresh prefix and prove only intended public executables/files
  are present and `tracy-query --version` reports 0.4.0.
- [ ] Verify Linux static and Windows runtime configuration remain attached to
  the relocated executable target.

### Stage 7 — Final pre-migration validation on the implementation branch

Dependencies: Stages 2–6 complete.

- [ ] From a clean checkout, run root Release configure/build/all CTests with
  pinned nextest, locked Cargo tests/trees, install/smoke checks, and docs/layout
  audits.
- [ ] Run Linux fully static and ASan/UBSan builds, including repeated embedded
  and nextest failure paths.
- [ ] Push the implementation branch and obtain one exact-commit successful
  Linux/macOS/Windows x86-64/ARM64 plus sanitizer CI run.
- [ ] Audit every immutable restructuring criterion and update this plan with
  commit/run evidence.

Verification:

- [ ] Working tree is clean, branch and remote SHA match, and all intended moves
  are represented as renames/deletions without lost source or tests.
- [ ] The source repository remains unarchived and no `v0.4.0` tag exists yet.

### Stage 8 — Create and verify `SanderVocke/tracy-extensions`

Dependencies: Stage 7 exact commit green; destination still absent.

- [ ] Recheck authenticated owner/admin access, destination absence, clean refs,
  and source unarchived state immediately before mutation.
- [ ] Create public `SanderVocke/tracy-extensions`, duplicate Git history/tags
  using the frozen runbook, set `master` default, and add a clearly named local
  successor remote.
- [ ] Configure description, homepage/topics, Actions permissions, and any
  required default-branch/release settings; do not claim a GitHub fork relation.
- [ ] Merge/fast-forward the validated restructuring commit to successor master
  according to the runbook and verify remote ancestry, tree hash, tags, and URLs.
- [ ] Run successor CI on the exact intended release commit and verify all jobs.

Verification:

- [ ] GitHub API reports public, unarchived, default `master`, expected HEAD SHA,
  Actions enabled, and `fork: false` with the migration explanation recorded.
- [ ] Fresh clone from the successor configures, builds, tests, installs, and has
  working root/component links without relying on the source remote.
- [ ] Source repository is still writable/unarchived at this gate.

### Stage 9 — Publish and audit release 0.4.0 in the successor

Dependencies: Stage 8 successor CI success.

- [ ] Create annotated tag `v0.4.0` at the audited successor master SHA and push
  only after printing/confirming destination remote and SHA.
- [ ] Let tagged CI build, native-smoke-test, statically verify, and publish the
  release; do not manually mix assets from another run or commit.
- [ ] Audit the release via API and download assets to a fresh directory.
- [ ] Record release URL, tag object/commit SHA, workflow run/job links, asset
  names/sizes, and checksums in this plan.

Verification:

- [ ] Release is non-draft/non-prerelease and contains exactly:
  `tracy-query-linux-x86_64`, `tracy-query-linux-arm64`,
  `tracy-query-macos-x86_64`, `tracy-query-macos-arm64`,
  `tracy-query-windows-x86_64.exe`, and
  `tracy-query-windows-arm64.exe`.
- [ ] Every asset is non-empty; producing native jobs ran installed `--version`;
  Linux static checks and Windows runtime guarantees passed on the tag commit.
- [ ] Release notes and all links identify `tracy-extensions` 0.4.0 and its three
  components accurately.

### Stage 10 — Cut over and archive `SanderVocke/tracy-query`

Dependencies: every Stage 9 release check passes.

- [ ] Reconfirm successor default branch, exact release, downloadable assets,
  documentation, and clone/build usability immediately before archival.
- [ ] Update the source repository description and homepage to point users to
  `https://github.com/SanderVocke/tracy-extensions`; preserve historical refs,
  releases, issues, and tags.
- [ ] Archive `SanderVocke/tracy-query` through the GitHub API/UI and record the
  response.
- [ ] Update local remotes/default working branch so future work targets the
  successor, without deleting the source remote needed for historical reference.

Verification:

- [ ] GitHub API reports source `archived: true` and successor
  `archived: false`; both URLs resolve and source metadata points to successor.
- [ ] Successor remains writable and its release/assets remain available after
  source archival.
- [ ] No post-archive source mutation is required for completion.

### Stage 11 — Final end-to-end audit

Dependencies: Stages 1–10 complete.

- [ ] Map every user request, immutable criterion, component, file move, command,
  test, platform gate, repository setting, tag, release asset, and archive action
  to concrete final evidence.
- [ ] Inspect actual successor files and GitHub API state rather than accepting
  green CI, the plan checklist, or release existence as proxies.
- [ ] Verify clean successor clone, component inventories/docs, root superbuild,
  all tests, install, exact version, tag ancestry, release assets, source archive,
  and local remote state one final time.
- [ ] Record any intentionally retained historical old-repository references and
  prove no stale operational reference remains.

Verification:

- [ ] All acceptance criteria are satisfied with no uncertainty or uncovered
  requirement.
- [ ] If any criterion is missing or a GitHub operation is blocked, leave the
  source unarchived when possible and stop with gathered evidence, attempted
  paths, blocker, and exact next input needed.

## Expected end-to-end command surface

Exact build directory names may be refined without weakening the gates:

```sh
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON \
  -DTRACY_QUERY_FULLY_STATIC=OFF \
  -DCARGO_NEXTEST_IN_PROCESS_EXECUTABLE=/path/to/cargo-nextest-0.9.116
cmake --build build --parallel
ctest --test-dir build --output-on-failure
cmake --install build --prefix stage
stage/bin/tracy-query --version

cargo test --locked \
  --manifest-path tracy-nextest-capture/crates/tracy-nextest-capture/Cargo.toml
cargo tree --locked \
  --manifest-path tracy-nextest-capture/tests/fixture/Cargo.toml \
  -i tracy-client-sys
cargo tree --locked \
  --manifest-path tracy-nextest-capture/tests/fixture/Cargo.toml -d

# GitHub state and release evidence (after guarded migration stages).
gh api repos/SanderVocke/tracy-extensions
gh run list --repo SanderVocke/tracy-extensions
gh release view v0.4.0 --repo SanderVocke/tracy-extensions \
  --json tagName,targetCommitish,isDraft,isPrerelease,assets,url
gh api repos/SanderVocke/tracy-query --jq '{archived,homepage,description}'
```
