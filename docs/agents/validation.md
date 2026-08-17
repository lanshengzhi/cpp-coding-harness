# Validation and delivery

Read this for implementation, review, test selection, Git delivery, or issue close-out.

## Implementation

Before editing, read the related code and tests plus the relevant build declarations. Follow `CODING_STANDARDS.md`. During implementation, run the smallest focused test that can fail; run the full required suite once at the end as required by `/implement`.

Use the build and test entry points in `README.md`. A fresh default validation is:

```bash
scripts/bootstrap.sh --test
```

## Architecture-sensitive changes

Run architecture tests when Owner Interface headers, include surfaces, dependency directions, provider/tool/session contracts, or CMake public/private boundaries change.

## Strict no-exception validation

The no-exception core is staged behind the `CCH_STRICT_NO_EXCEPTIONS=ON` configure option until the migration tickets have removed the existing exception paths. The option adds `-fno-exceptions` to project-owned production and test targets and passes strict mode to the Parity Architecture Gate. It is not part of the default build yet.

Strict-mode evidence must include:

- every project compile command carrying the manifest's required `-fno-exceptions` flag;
- no forbidden exception-enable flag such as `-fexceptions`;
- no `std::exception_ptr` outside the manifest's private completion-bridge allowlist; and
- no rethrow operation in the project source tree.

The supported strict configurations are the normal GCC 16 Debug and Release builds, the Clang 22 Debug conformance build, and the ASan/UBSan and TSan Debug configurations, each with the same pinned vcpkg toolchain. A strict validation run configures with `-DCCH_STRICT_NO_EXCEPTIONS=ON`, builds the complete target graph, runs the architecture tests, and runs the full offline CTest suite. Do not use live provider credentials or network access. The default `scripts/bootstrap.sh --test` remains required while this ticket is staged.

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
