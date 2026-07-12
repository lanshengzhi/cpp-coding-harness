# 04 — Preserve resource provenance and consistent failure semantics

**What to build:** Session creation must keep caller-authorized resources distinct from auto-discovered project resources so trust, precedence, failures, and diagnostics reflect where each resource came from.

**Blocked by:** 03 — Enforce external trust-store authority for SDK sessions.

**Status:** ready-for-agent

- [ ] Host-provided and explicitly requested resources are treated as caller-authorized and are not project-trust gated.
- [ ] Auto-discovered project resources load only when permitted by the selected trust authority.
- [ ] Caller-authorized resources take precedence when duplicate resources are resolved.
- [ ] Failure to load an explicit resource aborts session creation.
- [ ] Invalid auto-discovered resources are skipped with bounded, stable diagnostics rather than aborting a resource-free session.
- [ ] CLI discovery defaults and SDK opt-in discovery remain intentionally different and are covered by equivalent policy tests.
