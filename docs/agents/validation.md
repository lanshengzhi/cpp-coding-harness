# Validation and delivery

Read this for implementation, review, test selection, Git delivery, or issue close-out.

## Implementation

Before editing, read the related code and tests plus the relevant build declarations. Follow `CODING_STANDARDS.md`.

The validation tiers are defined in `CONTEXT.md`: Focused Validation, Full Validation, and Fresh Validation.

During implementation, run the smallest focused test that can fail: build the owning shard incrementally on the default Debug preset, then select with native CTest arguments (CTest names and labels are the sole selection authority, ADR 0039):

Configure presets require `VCPKG_ROOT` pointing at a vcpkg checkout pinned to `vcpkg.json`'s builtin-baseline; after Fresh Validation it is `.deps/vcpkg` in the repository root. If `build/` is not configured (no `cmake --build --preset vcpkg` target tree), configure once with `cmake --preset vcpkg` first. Any edit to a compiled source can make the build-phase Parity Gate reject with PARITY-4011 (include evidence older than the source): this is staleness, not an architecture violation — reconfigure once (`VCPKG_ROOT=<root> cmake --preset <preset>`) to rescan the direct-include evidence, then build.

```bash
cmake --build --preset vcpkg --target cch_tests_coding_agent   # owning shard
ctest --preset vcpkg -LE architecture -R 'session assembly'    # focused name
ctest --preset vcpkg -LE architecture -L coding_agent          # owning module label
cmake --build --preset vcpkg && ctest --preset vcpkg -LE architecture   # suite minus the architecture label
```

The `vcpkg` test preset already treats an empty selection as an error (`noTests: error`), so a mistyped name or label never passes silently.

Full Validation is mandatory once before delivery, as required by `/implement`: an incremental build followed by the complete unfiltered offline CTest suite on the default Debug preset, including every architecture gate test:

```bash
cmake --build --preset vcpkg
ctest --preset vcpkg
```

### Formatting gate

Added or modified lines must conform to `.clang-format`. `scripts/format-check.sh [base-ref]` checks them through `git clang-format` (no argument: working tree vs HEAD; a ref such as `origin/main`: the branch's merge-base). CI runs the same check as a blocking formatting job. To fix findings, run `git clang-format` with the same arguments and re-stage. Untouched lines stay outside the gate (`CODING_STANDARDS.md` §14).

Fresh Validation is the environment-level tier: `scripts/bootstrap.sh` (host precheck plus pinned vcpkg), then `export VCPKG_ROOT="$PWD/.deps/vcpkg"`, `cmake --preset vcpkg --fresh`, `cmake --build --preset vcpkg`, and `ctest --preset vcpkg`. Reserve it for clean checkouts, vcpkg-baseline or toolchain changes, configure-orchestration changes, or explicit user request. Do not run it for ordinary code edits. Its unconditional vcpkg pin and `--fresh` configure are the reproducibility contract (ADR 0038, ADR 0039), not the per-change default.

## Architecture-sensitive changes

Run architecture tests when Owner Interface headers, include surfaces, dependency directions, provider/tool/session contracts, or CMake public/private boundaries change:

```bash
cmake --build --preset vcpkg
ctest --preset vcpkg -L architecture
```

## Strict no-exception validation

The strict no-exception core is the default build (ADR 0042; issue #487):
`CCH_STRICT_NO_EXCEPTIONS=ON` is the default, adding `-fno-exceptions` to
every project-owned production and test target and passing strict mode to the
Parity Architecture Gate. Turning the option off is a deliberate deviation
from the supported policy, intended only for local debugging, and emits a
configure-time warning.

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

## Closing a completed implementation issue

Use this completion path after implementation; triage and `wontfix` outcomes follow the `/triage` workflow instead. Close-out is ordered:

1. Confirm and tick every acceptance criterion in the issue body (`- [x]`).
2. Close the issue: `gh issue close <n> --comment "..."` (see `docs/agents/issue-tracker.md`).
