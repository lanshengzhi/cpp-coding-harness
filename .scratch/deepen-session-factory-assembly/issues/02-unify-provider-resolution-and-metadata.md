# 02 — Unify provider resolution and session metadata precedence

**What to build:** New and resumed CLI/SDK sessions must resolve provider, model, and host-client metadata consistently through the shared factory policy, while retaining the distinction between the client that executes requests and the metadata recorded for the session.

**Blocked by:** 01 — Unify session creation behind a private assembly plan.

**Status:** ready-for-agent

- [ ] Provider and model resolution consistently follows explicit request, stored resume metadata, user configuration, then provider defaults.
- [ ] Explicit overrides of stored provider or model metadata succeed with recoverable diagnostics.
- [ ] A transferred host client controls request execution without discarding explicit or stored provider/model metadata.
- [ ] Metadata-less new sessions using a host client receive the documented host sentinel, while resumed metadata is retained when no explicit override is supplied.
- [ ] Equivalent CLI and SDK create/resume scenarios demonstrate the shared precedence policy.
