# Build Performance Plan

Status: Historical measurement record. The proposed policy and staged recommendation are superseded by architecture spec [#439](https://github.com/lanshengzhi/cpp-coding-harness/issues/439) and accepted [ADR 0039](adr/0039-own-the-capability-owner-package-graph-and-parity-architecture-gate.md)/[ADR 0040](adr/0040-own-asynchronous-operations-and-the-serialized-runtime-lifecycle.md). This document preserves benchmark evidence; it does not authorize implementation or define a supported build path. Historical target names such as `cpp_harness` and `cpp_harness_tests` are intentionally retained in the measurements; the current product target is `pike` per ADR 0045.

## Current architecture-policy supersession

The measurements below remain historical evidence. Conflicting policy and recommendations are replaced as follows:

| Historical proposal | Current authority |
| --- | --- |
| Preserve a portable or best-effort platform range. | The Supported Platform is native Linux x86-64 with glibc only. Unsupported platforms and cross-compilation fail at configure time. |
| Use a CMake 4.0+ floor, loose GCC 16+ selection, and optional experimental Clang. | Require CMake 4.4+, Ninja 1.11+, GCC 16.x for builds/releases, and Clang 22.x as a blocking Linux conformance verifier. CI and releases pin exact versions without this document inventing them. |
| Retain the legacy package graph, including `cch_util` and `cch_coding_agent_runtime`. | Use the four-Owner graph (`cch_ai`, `cch_agent_core`, `cch_tui`, `cch_coding_agent`) plus pi-neutral `cch_support`; `cch::support::JsonValue` replaces `util::JsonValue`. |
| Remove Catch2 and retain the local Catch-compatible imitation. | Use formal Catch2 v3 from the pinned dependency graph and delete the imitation without a compatibility header. |
| Split and schedule tests through package shards and `scripts/run-tests.sh`. | Discover fast tests as individual CTest cases; use CTest names and labels as the normal selection and scheduling authority, with measured grouping only for scenarios that justify it. |
| Reconsider optional Unity Build. | Unity Build and Unix Makefiles are unsupported. |
| Treat the numeric targets and stages below as the recommended implementation tranche. | Structural bounds, unique compilation, legal fan-out, and dependency classification block immediately. Numeric gates require controlled repeated measurements with baseline, variance, selection rule, and update procedure. All stages and the old recommendation below are superseded. |

## Historical objective

The proposal sought to shorten these developer feedback paths:

1. rebuild after changing one production `.cpp` file;
2. build and run the relevant tests;
3. rebuild after changing a shared project header;
4. clean Debug and Release builds, including bootstrap workflows.

Its portable compatibility-baseline assumption is superseded by the Supported Platform in ADR 0039. The explicit fast-development path remains useful only where it conforms to that supported boundary.

## Superseded scope and constraints

The following bullets record the proposal's assumptions at measurement time; they are not current build policy:

- Keep the then-supported platform range and GCC 16+/CMake 4.0+ floor from ADR 0038, with vcpkg as the only dependency source.
- Keep GCC as the primary fast-development compiler and evaluate Clang through a separate experimental preset.
- Keep build parallelism at four jobs on the measured host. Higher-memory environments may override it explicitly.
- Permit Ninja, ccache, private target-specific precompiled headers, package-aligned test splitting, and private dependency-boundary improvements.
- Avoid global PCH and initial Unity Build use.
- Preserve the then-existing package boundaries and keep Beast/Asio behind the Transport seam.
- Use fake-provider tests; build-performance validation requires no live provider or network access.

These were build-engineering proposals, not product domain language. They do not change `CONTEXT.md`. ADR 0039 supersedes ADR 0038's CMake 4.0 floor, loose GCC 16+ rule, and best-effort platform range; ADR 0039/0040 also replace the proposal's package, test, and Runtime assumptions.

## Measurement environment

Measurements were taken in disposable directories under `/tmp`, without network access or modification of the repository's build configuration.

| Property | Value |
| --- | --- |
| CPU | Intel Core i5-8300H, 4 cores / 8 threads |
| RAM | 15 GiB |
| Compiler | GCC 16.1.1; Clang 22.1.8 used for one comparison |
| Build tools | CMake 4.4.0, Ninja 1.13.2, ccache 4.13.6 |
| Configuration | C++23, Ninja, four jobs, vcpkg dependencies |
| Resource caveat | Swap was nearly full and VS Code, Chrome, and the agent were active; absolute cold-build times include host contention |

Ratios, target shares, cache effects, and hotspot ordering are more portable than the absolute seconds.

## Baseline evidence

### Debug

| Scenario | Time |
| --- | ---: |
| Fresh configure with dependencies served from the local vcpkg binary cache | 4.0 s |
| Clean full build | 647.1 s |
| No-op rebuild | 0.04 s |
| Typical production-source incremental build | 22.1 s |
| Typical test-source incremental build | 18.2 s |
| Worst measured incremental build | 56.7 s |
| Re-link `cpp_harness_tests` | 12.2 s |
| Re-link `cpp_harness` | 6.6 s |

The clean build compiled 291 entries. Aggregate translation-unit time was 2,430 seconds, giving about 3.76-way effective parallelism with four jobs.

### Release

| Scenario | Time |
| --- | ---: |
| Cold ccache, clean full build | 967.2 s |
| Warm ccache, clean outputs and rebuild identical sources | 4.2 s |
| Slowest translation unit | 93.7 s |
| Peak compiler-process RSS at four jobs | 3.74 GiB |
| Peak single `cc1plus` RSS | 1.40 GiB |
| Minimum available memory during the build | 1.24 GiB |

The warm rebuild hit ccache for all 291 compilation entries and was approximately 230 times faster than the cold build.

Six jobs are not safe on the measured host under its observed workload. Four jobs already reduced available memory to 1.24 GiB while swap was effectively full. Increasing parallelism is therefore not the recommended remedy.

### Primary hotspots

The monolithic `cpp_harness_tests` target accounts for 55.5% of aggregate Debug compilation time. The next largest targets are `cch_ai` at 12.3%, `cch_coding_agent_runtime` at 8.3%, and `cch_coding_agent_interactive` at 8.1%.

Slow translation units include:

| Translation unit | Debug in-build | Release cold |
| --- | ---: | ---: |
| `src/ai/providers/BoostBeastWebSocketTransport.cpp` | 50.3 s | 93.7 s |
| `src/coding_agent/tui/InteractiveMode.cpp` | 35.0 s | 57.0 s |
| `src/ai/providers/BoostBeastStreamTransport.cpp` | 31.8 s | 46.7 s |
| `src/ai/auth/OAuthHttpClient.cpp` | 28.3 s | 48.6 s |
| `tests/coding_agent/tui/InteractiveModeTest.cpp` | 26.3 s | 68.0 s |

A standalone Debug compile of the leading hotspot took 32.7 seconds with GCC and 22.1 seconds with Clang. This supports a full Clang experiment, but not an immediate compiler-default change.

## Diagnosed causes

### 1. The test build is monolithic

`CMakeLists.txt` places roughly 143 test translation units in one `cpp_harness_tests` executable. A focused test edit still pays for a large final link, and there is no package-level test build target. Changes to broadly used test support or project headers fan out across many test files.

### 2. Production sources are compiled twice

The following sources are compiled into both `cpp_harness` and `cpp_harness_tests`:

- `src/cli/CliParse.cpp`
- `src/cli/FrontendSelection.cpp`
- `src/cli/ListModels.cpp`
- `src/cli/StartupTui.cpp`
- `src/coding_agent/runtime/AsyncCliRuntime.cpp`

This wastes cold-build CPU and signals missing CMake ownership for the shared CLI/runtime composition.

### 3. Heavy templates have broad fan-out

Boost.Asio/Beast headers reach about 116 translation units. `src/util/Json.hpp` brings Glaze into about 55 translation units even though direct Glaze use is limited to a few serialization implementation files. This conflicts with the repository rule that generic and serialization machinery stays local.

### 4. The documented build path does not guarantee caching

`CMakePresets.json` does not provide a checked-in Ninja-and-ccache fast path. Local `CMakeUserPresets.json` configuration is not a project contract. The observed global ccache hit rate before the isolated experiment was only about 4.3%, while the controlled warm rebuild demonstrated the dominant potential gain.

### 5. A small set of implementation files dominate cold compiles

Beast/Asio transport implementations, interactive-mode sources, serialization, and large integration tests dominate the critical path. They should be addressed only after cache, target topology, and dependency-locality improvements are measured.

### Not a cause: graph scanning

The Ninja no-op build took 0.04 seconds. CMake/Ninja dependency-graph scanning is not a meaningful bottleneck.

## Historical target outcomes (not current gates)

These one-host numeric goals belong to the superseded proposal. They are not blocking thresholds; current numeric policy requires the controlled repeated evidence defined by ADR 0039.

### Historical Debug goals

- cold clean build: at most 420 seconds;
- warm-ccache clean rebuild: at most 60 seconds;
- typical production-source build to `cpp_harness`: at most 10 seconds;
- typical test-source build to its package test executable: at most 10 seconds;
- slowest GCC Debug translation unit: at most 35 seconds;
- shared-header rebuild fan-out or aggregate time: at least 40% lower;
- no-op build: remain below one second.

### Historical Release goals

- cold clean build: at most 720 seconds;
- warm-ccache clean rebuild: at most 30 seconds;
- slowest Release translation unit: at most 70 seconds;
- typical Release incremental build: at most 30 seconds.

Absolute cold-build targets should be compared on a similarly loaded host. Each implementation ticket should also report relative change against a same-session control run.

## Superseded proposed stages

The stages below are retained to explain the measurement record. They are not a current sequence or authorization; where they conflict with the supersession table above, ADR 0039/0040 and #439 control.

The proposal gave each stage an independent Go/No-Go measurement and avoided batching stages before measurement so a regression or win could be attributed.

### Stage 1: establish the benchmark contract

Add an agent-runnable script, proposed as `scripts/benchmark-build.sh`, that records:

- configure time;
- clean cold build time;
- no-op build time;
- warm-cache clean rebuild time;
- one typical production-source incremental build;
- one typical test-source incremental build;
- the leading hotspot rebuild;
- per-target and slowest-translation-unit summaries;
- compiler, generator, build type, job count, cache state, CPU, memory, and background-load caveats.

The benchmark must use its own build and cache directories, reject concurrent use of the same build directory, and avoid network access when dependencies are already available.

**Go:** repeated control measurements are close enough to distinguish a 10% change.

**No-Go:** the harness cannot distinguish build time from dependency download, competing builds, or cache state.

Do not add a hard absolute-time CI gate yet. Accumulate results first; later consider a wide trend check.

### Stage 2: add an explicit fast-development preset

Add a checked-in `dev-fast` family that:

- explicitly uses Ninja;
- requires ccache and fails clearly when it is unavailable;
- uses four build jobs;
- leaves the existing baseline presets unchanged;
- permits explicit command-line parallelism override on high-memory hosts;
- provides Debug first and a corresponding Release form if naming remains clear.

**Go:** warm-cache rebuilds satisfy the target and cold builds do not regress materially.

**No-Go:** cache use is silent or nondeterministic, or the preset changes the compatibility baseline.

### Stage 3: remove known build waste

**Superseded test-framework clause:** Catch2 removal is rejected. The current destination uses formal Catch2 v3 and deletes the local imitation. Source ownership must follow the four authoritative Owner libraries and central CMake declarations rather than the legacy target graph.

The historical proposal was:

1. Remove the then-unused Catch2 dependency from `vcpkg.json` and retain the repository's lightweight Catch-compatible header.
2. Give the five duplicated production sources one authoritative CMake owner, then link that owner into the executable and relevant tests.

The ownership change must preserve the dependency directions in `CODING_STANDARDS.md` section 12 and the architecture tests.

**Go:** compile commands contain one entry per shared production source per configuration, behavior and tests remain unchanged, and bootstrap still resolves all required dependencies.

**No-Go:** the change creates a reverse dependency or turns private CLI implementation into a public contract.

### Stage 4: split tests by package boundary

**Superseded scheduling clause:** package shards, the `cpp_harness_tests` aggregate, and `scripts/run-tests.sh` do not define the destination. Formal Catch2 v3 case discovery and CTest names/labels are the normal scheduling surface; scenarios are grouped only when measured startup or shared setup justifies it.

The historical proposal would have replaced the single test executable with package-aligned executables, initially considering:

- util;
- TUI;
- AI;
- agent;
- harness and tools;
- coding-agent core/runtime;
- coding-agent interactive TUI;
- CLI and architecture.

Exact grouping should follow dependency and fixture ownership rather than equal file counts. Keep Debug developer builds compiling all tests by default, but allow building and running one package's tests.

Provide:

- `ctest` registration for every shard;
- `scripts/run-tests.sh` as the uniform tag/filter entry point;
- a `cpp_harness_tests` CMake aggregate target that builds all shards;
- updated README and agent validation commands.

The proposal intentionally does not preserve `cpp_harness_tests` as one executable. Do not compile all test objects twice merely to retain that path.

**Go:** focused test edits build and link only the relevant shard, complete `ctest` behavior remains equivalent, and the package test loop meets its target.

**No-Go:** fixture initialization semantics, global test isolation, tag filtering, or architecture coverage differs from the current suite.

### Stage 5: localize Glaze

**Terminology and ownership supersession:** Glaze DTOs and serialization machinery remain Owner-private, but `cch_util`, `src/util/Json.hpp`, and public `util::JsonValue` are not destination contracts. Cross-Owner unstructured JSON uses passive `cch::support::JsonValue` through repository-internal Owner Interfaces.

The historical proposal would have removed Glaze-dependent machinery from `src/util/Json.hpp`, moved conversions and serialization helpers into private implementation areas, and removed dead helpers after verifying that they had no callers.

**Go:** approximately 50 non-serialization translation units no longer parse Glaze, architecture tests pass, and shared-header rebuild cost decreases.

**No-Go:** Glaze appears in a public header, serialization machinery moves outward, or the value contract changes.

### Stage 6: introduce narrow private PCH experiments

Only after the prior stages are measured, experiment on remaining expensive targets such as `cch_ai` and selected runtime, interactive, or test shards.

Rules:

- PCH contents are stable third-party headers, not project headers;
- each PCH is private to a target or a demonstrably compatible small target family;
- the normal compatibility build remains able to compile with PCH disabled;
- CI or required validation includes a no-PCH build so missing direct includes cannot be hidden.

**Go:** the target's cold compile time improves materially, total memory remains safe at four jobs, and no-PCH validation remains green.

**No-Go:** PCH creation dominates focused builds, increases rebuild fan-out, obscures include correctness, or fails to deliver at least a meaningful double-digit improvement.

### Stage 7: evaluate Clang separately

**Superseded compiler clause:** Clang 22.x is a blocking Linux conformance verifier, not an optional developer-only experiment or release-artifact compiler. GCC 16.x remains the sole build/release compiler.

**Go:** the full edit-build-test loop improves materially without compatibility loss.

**No-Go:** the isolated hotspot gain does not translate to the whole project or splits cache usage without enough benefit.

### Stage 8: optimize private hotspots only if targets remain unmet

If the leading Beast/Asio translation units still exceed the target, use compiler tracing and include analysis to identify template-instantiation and code-generation cost. Prefer:

- narrower private headers;
- moving template-heavy details behind private non-template functions or Pimpl boundaries;
- explicit instantiation where ownership is clear;
- reducing unnecessary protocol/header inclusion.

Preserve the public Transport capability seam and observable provider behavior. Do not replace the network library as a build-time shortcut.

**Go:** hotspot and incremental targets are met with unchanged provider tests.

**No-Go:** the proposed split leaks provider DTOs, transport details, or new capability methods across public seams.

### Stage 9: reconsider Unity Build only if necessary

**Superseded:** Unity Build is unsupported by ADR 0039 and is not a conditional optimization path.

## Superseded recommended decision

Do not approve or implement the old staged tranche from this document. #439 and ADR 0039/0040 replace its package, test, platform, compiler, Unity, and Runtime assumptions; approved work follows `/to-spec` → `/to-tickets` → `/implement` from those authorities.

The measured Release rebuild improvement from 967.2 seconds cold to 4.2 seconds warm remains evidence for Ninja/ccache on that host. The former package-sharding recommendation is superseded by Catch2 v3 case discovery and CTest scheduling. The measured-host warning against increasing parallelism remains evidence, not a universal policy.

## Historical validation and rollout

The proposal required the following for each then-accepted stage:

1. record control measurements with compiler, configuration, cache state, and system load;
2. implement only that stage;
3. run the smallest focused build/test that can fail;
4. rerun the same performance scenarios;
5. run the full test suite and architecture tests when CMake boundaries or public/private dependency directions change;
6. record the observed delta and residual risks;
7. revert or redesign the stage when its Go criteria fail.

Documentation-only changes to this proposal require heading, link, tracker-reference, and agent-facing-English checks; they do not require a C++ build.

## Decision boundaries

This historical record does not authorize code changes, issue creation, branch changes, commits, or CI policy changes. New work must derive from #439, ADR 0039/0040, the parity map, and the repository's `/to-spec` → `/to-tickets` → `/implement` workflow.
