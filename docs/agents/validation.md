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
