# 06 — Make capability ownership explicit and retire the second assembly seam

**What to build:** Every capability adopted during session creation must carry explicit lifecycle ownership. Session close must clean up only session-owned environments, and runtime services must become a passive internal bundle rather than a second way to assemble sessions.

**Blocked by:** 01 — Unify session creation behind a private assembly plan.

**Status:** ready-for-agent

- [ ] Factory-created execution environments are session-owned and cleaned up exactly once.
- [ ] Host-injected shared execution environments remain host-owned and are never cleaned up by session close.
- [ ] Chat clients and custom tools transferred by unique ownership remain session-owned, and the public SDK contract states that ownership transfer.
- [ ] Execution-environment ownership is represented explicitly rather than inferred from shared-reference counts.
- [ ] The independent runtime-services configuration/factory interface is removed, leaving only the passive internal runtime-services value.
- [ ] CLI and SDK adapters no longer construct runtime services or the session runtime directly.
