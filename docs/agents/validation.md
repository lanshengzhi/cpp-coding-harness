# Validation and delivery

Read this for implementation, review, test selection, Git delivery, or issue close-out.

## Acceptance discipline

A change is accepted for the property, not for the check that stands in for it (historical adjudications: `ADR 0056` addendum #664 and issue #657). Issue acceptance criteria, Parity Architecture Gate rules, golden tests, and test doubles are all in scope.

1. **A text or existence check is not acceptance of a property.** A predicate that matches an include spelling, searches for a string, asserts that a file or symbol exists, or compares a projection of a value can be satisfied by a respelling, a moved file, or a field added on one side while the property it is named for is violated. Run such a check when it is cheap and catches the ordinary case, but report its pass as a check, never as acceptance of the property.
2. **Acceptance includes a case the property separates and the check does not.** For each acceptance criterion, name one case the check passes while the property is violated, and make it part of the delivery: a test where automation reaches it, manual evidence where it does not. One question finds it — *if this stand-in were broken, which case would it let through?* — and that case then gets built. A criterion with no such case is not yet accepted.
3. **An empirical fork is accepted on a recorded observation, not on a described option.** Where two or more candidate behaviors are observably different, a small throwaway prototype may settle the fork; record the question it answered and the observation it produced, and treat that record as the evidence. Where the direction is already settled, or only one candidate is on the table, there is no fork to settle and no prototype to run. A design option listed in an ADR is not itself such a record.

## Validation tiers

This document owns the engineering validation vocabulary and procedures. Dependencies use the pinned vcpkg manifest; system packages are unsupported.

| Tier | Scope |
| --- | --- |
| **Focused Validation** | During implementation: build the owning test shard and run the smallest targeted test selection that can fail. Architecture-sensitive changes also run the architecture selection. |
| **Full Validation** | Once before code delivery: an incremental build followed by the complete unfiltered offline test suite, including every architecture gate test. |
| **Fresh Validation** | Environment-level validation from host precheck through a fresh configure, build, and full test run; reserved for clean checkouts, vcpkg-baseline or toolchain changes, configure-orchestration changes, or explicit user request. |

A multi-step change whose slices depend on one another proceeds slice by slice: complete the current slice and run its Focused Validation before starting the slice that depends on it. This orders the work against the dependency, not against a schedule. A single-step change is not split to satisfy it, and this is a default practice rather than a per-PR gate.

For documentation-only changes, use the [documentation checks](#documentation-only-changes) instead of a C++ build.

## Implementation

Before editing, read the related code and tests plus the relevant build declarations. Follow `CODING_STANDARDS.md`.

During implementation, run the smallest focused test that can fail: build the owning shard incrementally on the default Debug preset, then select with native CTest arguments (CTest names and labels are the sole selection authority, ADR 0039):

Configure presets resolve a vcpkg checkout pinned to `vcpkg.json`'s builtin-baseline via `VCPKG_ROOT`, falling back to `.deps/vcpkg` in the current worktree or the primary Git worktree (`cmake/vcpkg-toolchain.cmake`). If `build/` is not configured, configure once with `cmake --preset vcpkg` first.

**Include evidence (`PARITY-4011`).** The build phase re-derives each declared source's include directives and compares them with the recorded scan, so a changed include set needs a reconfigure (`cmake --preset <preset>`) before the Gate passes. A comment or body edit does not.

The build-phase Parity Architecture Gate sources active transitive-conformance depfile evidence directly from `.ninja_deps` (schema version 2; issue #551, ADR 0039 addendum). CMake C++ module scanning is disabled globally (`CMAKE_CXX_SCAN_FOR_MODULES OFF`), keeping every translation unit cacheable under ccache across all presets.

```bash
cmake --build --preset vcpkg --target cch_tests_coding_agent   # owning shard
ctest --preset vcpkg -LE architecture -R 'session assembly'    # focused name
ctest --preset vcpkg -LE architecture -L coding_agent          # owning module label
cmake --build --preset vcpkg && ctest --preset vcpkg -LE architecture   # suite minus the architecture label
```

Parallel builds share one host budget: four concurrent jobs is the ceiling (`CMakePresets.json` `jobs: 4`; two concurrent lanes at `-j2`, or four at `-j1`; measurement record in [build-performance-baseline.md](../build-performance-baseline.md)).

The `vcpkg` test preset treats an empty selection as an error (`noTests: error`), while a broad regex can match unintended cases. Verify the match set before trusting it (`ctest --preset vcpkg -N -R '<regex>'`), prefer a label (`-L <label>`) or the owning shard, and report the selected count with the result.

`catch_discover_tests` registers a target's cases only when that target builds, so a label run in a shard-only build tree silently covers a subset. Build the full preset before any label-wide run (`cmake --build --preset vcpkg`), or compare the `ctest -N` count against the primary checkout before trusting a pass.

Run Full Validation once before code delivery on the default Debug preset, as required by `/implement`:

```bash
cmake --build --preset vcpkg
ctest --preset vcpkg
```

### Formatting gate

Added or modified lines must conform to `.clang-format`. `scripts/format-check.sh [base-ref]` checks them through `git clang-format` (no argument: working tree vs HEAD; a ref such as `origin/main`: the branch's merge-base). CI runs the same check as a blocking formatting job. To fix findings, run `git clang-format` with the same arguments and re-stage. Untouched lines stay outside the gate (`CODING_STANDARDS.md` §14).

Report the CI form — `scripts/format-check.sh <merge-base>` — before delivery. The no-argument form sees only uncommitted changes, so it reports clean on work that is already committed. A rename gates the file as if new: every line it carries becomes a modified line, including lines this branch never touched. Reformat a moved file in the same commit as the move.

For Fresh Validation, run `scripts/bootstrap.sh` (host precheck plus pinned vcpkg), then `export VCPKG_ROOT="$PWD/.deps/vcpkg"`, `cmake --preset vcpkg --fresh`, `cmake --build --preset vcpkg`, and `ctest --preset vcpkg`. Its unconditional vcpkg pin and `--fresh` configure are the reproducibility contract (ADR 0038, ADR 0039). Ordinary code edits use Focused and Full Validation instead.

### Sanitizer timing checks

When a test adds or changes a wall-clock assertion, classify it under [CODING_STANDARDS.md §11.9](../../CODING_STANDARDS.md#11-tests). For a performance assertion, use the sanitizer guard below; keep the measured operation and every functional assertion outside it:

```cpp
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
    // Sanitizer overhead invalidates this performance bound; functional assertions still run.
    (void)elapsed;
#else
    CHECK(elapsed < kBound);
#endif
```

The measurement history and original test examples are recorded in issue #632. For runner-sensitive failures, use §Test quarantine below.

### Test quarantine

Flaky or runner-sensitive tests have one standard home instead of ad-hoc per-PR exclusions (issues #632, #634, #734):

- At the test site, add the `[quarantine]` Catch2 tag — surfaced as the `quarantine` CTest label through `ADD_TAGS_AS_LABELS` — next to a comment naming the reason, the owner issue, and the re-enable condition.
- In `.github/workflows/linux-toolchain.yml`, the `build-and-test` matrix `ctest_label_exclude` field excludes that label per lane via `ctest -LE`. Only lanes that cannot run the test reliably exclude it; each non-empty entry cites the owner issue and the re-enable condition.
- Active quarantined cases:
  1. `tests/coding_agent/ProjectionStreamTest.cpp` (projection 100-chunk cost bound; owner #632);
  2. `tests/coding_agent/tui/ChatContainerTest.cpp` (ChatContainer 100-chunk streaming flat-time benchmark; owner #632);
  3. `tests/cli/InstallRelocationTest.cpp` (staged-install relocation gate, kept active on GCC 16 TSan as canary; owner #734).

Quarantine is for resource-caused failures; bounds are never loosened silently to fit the runner.

## Architecture-sensitive changes

Run architecture tests when Owner Interface headers, include surfaces, dependency directions, provider/tool/session contracts, or CMake public/private boundaries change:

```bash
cmake --build --preset vcpkg
ctest --preset vcpkg -L architecture
```

## Strict no-exception validation

The strict no-exception core is the only supported configuration (ADR 0042;
issues #487, #720): `CCH_STRICT_NO_EXCEPTIONS=ON` is the default and sole
supported mode, adding `-fno-exceptions` to every project-owned production and
test target and passing strict mode to the Parity Architecture Gate. The
exception-enabled fallback was retired in #720 — no
`#if !defined(BOOST_ASIO_NO_EXCEPTIONS)` guards remain in project sources.
Turning the option off is a deliberate deviation from the supported policy,
intended only for local debugging, and emits a configure-time warning.

Strict-mode evidence must include:

- every project compile command carrying the manifest's required `-fno-exceptions` flag;
- no forbidden exception-enable flag such as `-fexceptions`;
- no `std::exception_ptr` outside the manifest's private completion-bridge allowlist; and
- no rethrow operation in the project source tree.

Because strict mode is the default, the Fresh Validation loop and every supported configure already validate it. The supported strict
configurations are the normal GCC 16 Debug and Release builds, the Clang 22
Debug conformance build, and the ASan/UBSan and TSan Debug configurations,
each with the same pinned vcpkg toolchain: every configure runs the Parity
Architecture Gate fail-closed with strict evidence, every normal build runs
the build-phase Gate, and the full offline CTest suite runs the no-exception
target graph. Do not use live provider credentials or network access.

## Documentation-only changes

Check headings, relative links, tracker references, and clear agent-facing English. Documentation-only changes do not require a C++ build.

## Provider tests

Use fake Providers and deterministic local resources by default. Use live API keys or network validation only when the user explicitly requests it.

## Git authority

`/implement` includes review and a task-scoped commit on the current branch. Push, merge, branch switching, worktree creation/removal, and branch deletion require explicit user authorization.

Before deleting ticket branches or removing their worktrees, audit them with `scripts/branch-audit.sh <integration-branch>`: a branch reporting `UNAPPLIED` commits holds work that landed nowhere and must be resolved (or explicitly kept) first. Close-out comments reference the merged commit on the integration branch, never a branch tip that may never merge.

## Closing a completed implementation issue

Use this completion path after implementation; triage and `wontfix` outcomes follow the `/triage` workflow instead. Close-out is ordered:

1. Confirm and tick every acceptance criterion in the issue body (`- [x]`); a tick means the property holds, not that a check for it passed (§Acceptance discipline).
2. When the change advances a capability row in `1058-consolidated-capability-ledger.json` or `1058-implementation-frontier.json`, its delivery evidence lands under `docs/research/<baseline>/`, where `<baseline>` is the named baseline the work was verified against — either a new `<issue>-<slug>.md` or a section merged into that ticket's existing owning record (the chord slices live in `1057-chord-durable-dependency-subset.md`). A new record uses `## Scope`, `## Delivered behavior` (or the frozen-evidence section it uses), `## Validation`, `## Capability status`, `## Remaining boundary`; copy the shape from the most recent delivery record in that directory. The `## Validation` section carries measured counts, not estimates. A change that makes no capability claim — an internal refactor, a module decoupling, a wiring change — needs no record.
3. Remove the in-flight state labels (`needs-*`, `ready-for-*`) that admission applied; a closed issue that still reads `ready-for-agent` claims work is available. `docs/agents/triage-labels.md` maps the roles.
4. Close the issue: `gh issue close <n> --comment "..."` (see `docs/agents/issue-tracker.md`).
5. Run `scripts/verify-closed-issue.sh <n>` and require exit 0. It is the completion check for steps 1-4: it fails on a non-closed issue, an unticked criterion, or a remaining state label, and names the failing check.
