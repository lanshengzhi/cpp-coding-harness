# 01 — Unify session creation behind a private assembly plan

Category: enhancement
**What to build:** CLI/RPC and SDK session creation must translate their source-specific inputs into one private session-assembly flow. The shared flow must normalize a closed new-or-resume target, validate it, and coordinate assembly while preserving the intentional differences between the two product profiles.

**Blocked by:** None — can start immediately.

**Status:** implemented

- [x] Both source-facing creation profiles pass through one private normalization, validation, and assembly coordinator.
- [x] The normalized target cannot represent both new and resume intent, or neither intent.
- [x] New sessions receive one factory-generated opaque identity and independent UTC creation time; resumed sessions preserve stored identity.
- [x] Source-specific defaults and resume capabilities remain explicit without exposing the private resolved plan to callers.
- [x] Existing CLI, SDK, session, and public-boundary behavior remains green after the prefactor.

## Comments

Implemented and verified as part of the completed SessionFactory assembly slice. The focused and full-suite command ledger is recorded in the [validation ledger](../../pi-agent-session-event-prompt-parity/issues/16-validate-and-close-plans.md).
