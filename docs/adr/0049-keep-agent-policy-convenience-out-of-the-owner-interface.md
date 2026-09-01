---
status: accepted
---

# Keep Agent policy convenience out of the Owner Interface

The `cch_agent_core` Owner Interface retains only the asynchronous Agent policy Hook contracts. The ten synchronous policy aliases and seven named `adapt_sync_*` function families had no production caller and existed only to shorten tests, making the production interface wider for approximately 102 lines of pass-through implementation. They are removed in one replacement with `AgentPolicyAdapters.cpp`; tests construct the real Hook types directly and return ready or pending `AsyncResult` values.

This decision supersedes only ADR 0018's synchronous-adapter ergonomics and sync-adapter test obligations. Hook suspension, deterministic lifecycle ordering, cancellation, move-only ownership, failure isolation, and separation from weak observers remain authoritative.

## Considered options

- Keep the named adapters: rejected because test convenience does not justify a second production Hook vocabulary in a Runtime-only product with no installed C++ interface.
- Replace them with one generic production adapter: rejected because it preserves the same shallow pass-through module under a smaller name.
- Move the named adapters to test support: rejected because `AsyncResult` already represents ready outcomes and tests should cross the same Hook interface as production callers.

## Consequences

- All repository callers migrate in one pass; no deprecated overload, alias, or compatibility module remains.
- Tests that cover suspension, ordering, cancellation, move-only ownership, and failure isolation survive at the asynchronous Hook seam; tests that only protect synchronous adaptation disappear.
- Owner Interface and architecture evidence shrink without changing Agent, Agent Turn, or Tool behavior.
