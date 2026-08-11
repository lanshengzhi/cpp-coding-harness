# Build Performance Plan

Status: Proposed. This document records measured evidence and a staged decision package. It does not authorize implementation.

## Objective

Shorten the highest-frequency developer feedback paths in this order:

1. rebuild after changing one production `.cpp` file;
2. build and run the relevant tests;
3. rebuild after changing a shared project header;
4. clean Debug and Release builds, including bootstrap workflows.

Preserve the existing portable build as the compatibility baseline. Add an explicit fast-development path first; consider making it the default only after it is stable and measured.

## Scope and constraints

- Keep the existing supported compiler and platform range.
- Keep GCC as the primary fast-development compiler; evaluate Clang through a separate experimental preset.
- Keep build parallelism at four jobs on the measured host. Higher-memory environments may override it explicitly.
- Permit Ninja, ccache, private target-specific precompiled headers, package-aligned test splitting, and private dependency-boundary improvements.
- Do not use a global PCH or enable Unity Build in the initial work.
- Preserve the existing package and capability boundaries. In particular, any later work on the Beast/Asio implementation must remain behind the existing Transport seam.
- Use fake-provider tests; build-performance validation requires no live provider or network access.

This is build-engineering policy, not product domain language. It does not change `CONTEXT.md`. The proposal is reversible and does not currently justify an ADR.

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

## Target outcomes

### Debug

- cold clean build: at most 420 seconds;
- warm-ccache clean rebuild: at most 60 seconds;
- typical production-source build to `cpp_harness`: at most 10 seconds;
- typical test-source build to its package test executable: at most 10 seconds;
- slowest GCC Debug translation unit: at most 35 seconds;
- shared-header rebuild fan-out or aggregate time: at least 40% lower;
- no-op build: remain below one second.

### Release

- cold clean build: at most 720 seconds;
- warm-ccache clean rebuild: at most 30 seconds;
- slowest Release translation unit: at most 70 seconds;
- typical Release incremental build: at most 30 seconds.

Absolute cold-build targets should be compared on a similarly loaded host. Each implementation ticket should also report relative change against a same-session control run.

## Proposed stages

Each stage has an independent Go/No-Go measurement. Do not batch stages before measuring: otherwise a regression or win cannot be attributed.

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

1. Remove the unused Catch2 dependency from `vcpkg.json`; tests currently use the repository's lightweight Catch-compatible header and do not link Catch2.
2. Give the five duplicated production sources one authoritative CMake owner, then link that owner into the executable and relevant tests.

The ownership change must preserve the dependency directions in `CODING_STANDARDS.md` section 12 and the architecture tests.

**Go:** compile commands contain one entry per shared production source per configuration, behavior and tests remain unchanged, and bootstrap still resolves all required dependencies.

**No-Go:** the change creates a reverse dependency or turns private CLI implementation into a public contract.

### Stage 4: split tests by package boundary

Replace the single test executable with package-aligned executables, initially considering:

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

Remove Glaze-dependent machinery from `src/util/Json.hpp`. Move conversions and serialization helpers into existing private serialization/Glaze implementation areas, and remove dead helpers after verifying that they have no callers.

Do not change the passive public `util::JsonValue` contract.

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

Add or locally exercise an experimental Clang fast preset and measure the complete Debug and Release builds. Keep GCC as the primary recommendation until the full matrix, diagnostics, tests, and ccache behavior are verified.

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

Unity Build is not part of the initial recommendation. Reconsider targeted, opt-in use only if the prior stages leave cold-build targets unmet.

Any experiment must compare memory, diagnostics, include correctness, and incremental behavior—not only clean-build wall time. Global Unity Build remains disallowed without a separate decision.

## Recommended decision

Approve stages 1 through 5 as the low-risk implementation tranche after they pass the project's issue/spec/ticket process. Treat stage 6 as a measured experiment, stage 7 as an optional developer-path comparison, and stages 8 and 9 as conditional work triggered only by unmet targets.

The highest-confidence immediate win is the formal Ninja-and-ccache path: an isolated Release rebuild improved from 967.2 seconds cold to 4.2 seconds warm. The highest-confidence structural win for focused work is package-aligned test splitting. Increasing parallelism is specifically not recommended on the measured host.

## Validation and rollout

For each accepted stage:

1. record control measurements with compiler, configuration, cache state, and system load;
2. implement only that stage;
3. run the smallest focused build/test that can fail;
4. rerun the same performance scenarios;
5. run the full test suite and architecture tests when CMake boundaries or public/private dependency directions change;
6. record the observed delta and residual risks;
7. revert or redesign the stage when its Go criteria fail.

Documentation-only changes to this proposal require heading, link, tracker-reference, and agent-facing-English checks; they do not require a C++ build.

## Decision boundaries

This proposal does not authorize code changes, issue creation, branch changes, commits, or CI policy changes. Approved implementation should leave this document through the repository's `/to-spec` → `/to-tickets` → `/implement` workflow.
