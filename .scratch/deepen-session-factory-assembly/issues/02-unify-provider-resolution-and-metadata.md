# 02 — Unify provider resolution and session metadata precedence

Category: enhancement
**What to build:** New and resumed CLI/SDK sessions must resolve provider, model, and host-client metadata consistently through the shared factory policy, while retaining the distinction between the client that executes requests and the metadata recorded for the session.

**Blocked by:** 01 — Unify session creation behind a private assembly plan.

**Status:** implemented

- [x] Provider and model resolution consistently follows explicit request, stored resume metadata, user configuration, then provider defaults.
- [x] Explicit overrides of stored provider or model metadata succeed with recoverable diagnostics.
- [x] A transferred host client controls request execution without discarding explicit or stored provider/model metadata.
- [x] Metadata-less new sessions using a host client receive the documented host sentinel, while resumed metadata is retained when no explicit override is supplied.
- [x] Equivalent CLI and SDK create/resume scenarios demonstrate the shared precedence policy.

## Comments

Implemented and verified as part of the completed SessionFactory assembly slice. The focused and full-suite command ledger is recorded in the [validation ledger](../../pi-agent-session-event-prompt-parity/issues/16-validate-and-close-plans.md).
