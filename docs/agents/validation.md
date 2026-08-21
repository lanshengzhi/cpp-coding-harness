# Validation and delivery

Read this for implementation, review, test selection, Git delivery, or issue close-out.

## Implementation

Before editing, read the related code and tests plus the relevant build declarations. Follow `CODING_STANDARDS.md`.

The validation tiers are defined in `CONTEXT.md`: Focused Validation, Full Validation, and Fresh Validation.

During implementation, run the smallest focused test that can fail through `scripts/check.sh`, which builds incrementally on the default Debug preset and passes selection arguments straight through to CTest (CTest names and labels are the sole selection authority, ADR 0039):

```bash
scripts/check.sh --target cch_tests_coding_agent -R 'session assembly'  # owning shard, focused name
scripts/check.sh -L coding_agent                                        # owning module label
scripts/check.sh                                                        # suite minus the architecture label
```

Full Validation is mandatory once before delivery, as required by `/implement`: an incremental build followed by the complete unfiltered offline CTest suite on the default Debug preset, including every architecture gate test:

```bash
cmake --build --preset vcpkg
ctest --preset vcpkg
```

Fresh Validation (`scripts/bootstrap.sh --test`) is the environment-level tier: clean checkouts, vcpkg-baseline or toolchain changes, configure-orchestration changes, or explicit user request. Do not run it for ordinary code edits. Its unconditional vcpkg pin and `--fresh` configure are the reproducibility contract (ADR 0038, ADR 0039), not the per-change default.

## Architecture-sensitive changes

Run architecture tests when Owner Interface headers, include surfaces, dependency directions, provider/tool/session contracts, or CMake public/private boundaries change:

```bash
scripts/check.sh --architecture
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

Because strict mode is the default, the default `scripts/bootstrap.sh --test`
run and every supported configure already validate it. The supported strict
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

1. Confirm and tick every acceptance criterion.
2. Remove every in-flight state label (`needs-*`, `ready-for-*`).
3. Close the issue.
4. Run `scripts/verify-closed-issue.sh <n>`.
5. Resolve every reported failure; close-out is complete only when the verifier exits 0.
