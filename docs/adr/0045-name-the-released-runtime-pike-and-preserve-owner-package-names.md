---
status: accepted
---

# Name the released Runtime Pike and preserve Owner package names

The only released product is the **Pike Runtime**: its final composition CMake target, executable, install path, CLI identity, and release artifact names are all `pike`. The internal Capability Owner Packages remain `cch_ai`, `cch_agent_core`, `cch_tui`, `cch_coding_agent`, and `cch_support` because those names describe architecture ownership rather than product branding; the rename is intentionally breaking and retains no `cpp_harness` compatibility entry.

## Considered options

- Rename every Owner Package to `pike_*`: rejected because branding would obscure the Parity Ownership Map and force a needless architecture-graph migration.
- Keep `cpp_harness` as a compatibility target or installed alias: rejected because the product is pre-release and a second identity would leave the Runtime surface ambiguous.

## Consequences

- User-visible Runtime names use `pike` consistently in build, install, help, tests, CI, and documentation.
- `cch_*` targets, `cch::...` namespaces, Owner Interface paths, and `CCH_*` architecture/build controls remain repository-internal architecture vocabulary.
