# 01 — Unify session creation behind a private assembly plan

**What to build:** CLI/RPC and SDK session creation must translate their source-specific inputs into one private session-assembly flow. The shared flow must normalize a closed new-or-resume target, validate it, and coordinate assembly while preserving the intentional differences between the two product profiles.

**Blocked by:** None — can start immediately.

**Status:** ready-for-agent

- [ ] Both source-facing creation profiles pass through one private normalization, validation, and assembly coordinator.
- [ ] The normalized target cannot represent both new and resume intent, or neither intent.
- [ ] New sessions receive one factory-generated opaque identity and independent UTC creation time; resumed sessions preserve stored identity.
- [ ] Source-specific defaults and resume capabilities remain explicit without exposing the private resolved plan to callers.
- [ ] Existing CLI, SDK, session, and public-boundary behavior remains green after the prefactor.
