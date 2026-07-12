# 08 — Return diagnostics as values and leave presentation to adapters

**What to build:** Factory and runtime logic must report recoverable creation and prompt issues as values. Each adapter then chooses presentation appropriate to its contract, keeping embedded and machine-readable modes silent and protocol-clean.

**Blocked by:** 01 — Unify session creation behind a private assembly plan.

**Status:** ready-for-agent

- [ ] Session factory and runtime logic do not write diagnostics directly to standard output or standard error.
- [ ] Recoverable creation, resource, skill, and template diagnostics are returned as values through every entry point that needs them.
- [ ] SDK session creation and prompting remain silent while exposing diagnostics to the caller.
- [ ] Text-mode CLI renders human-facing diagnostics through its adapter.
- [ ] JSON and RPC modes keep standard output protocol-clean and preserve their documented diagnostic behavior.
- [ ] Output-capture tests cover factory/runtime silence and adapter-owned presentation.
