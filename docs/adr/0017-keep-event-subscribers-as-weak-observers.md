---
status: accepted
---

# Keep event subscribers as weak observers

Ordinary Agent Session subscribers are move-only weak observers and cannot veto agent progress or durable history. Subscriber exceptions or reported failures become bounded diagnostics and may deactivate the faulty subscription; fallible persistence remains an internal commitment capability, and any future extension allowed to veto commitment must use a separately named strong middleware contract.

## Considered options

- Let every subscriber return `std::expected` and abort commitment: rejected because UI, logging, and host observers would gain surprising control over agent and persistence state.
- Ignore subscriber failures completely: rejected because hosts still need actionable diagnostics and protection from repeatedly failing callbacks.
- Separate weak observation from strong middleware: accepted because capability strength is explicit at the type and module boundary.

## Consequences

- RAII subscriptions and `std::move_only_function` capture semantics remain supported.
- Subscriber failure does not stop the agent or prevent a Session Entry append.
- Callback exceptions are caught at the subscription boundary and never unwind through an active coroutine.
- Event commitment retains explicit failure handling for storage and other named strong participants.
- Tests cover observer failure, observer self-removal, close-from-observer, diagnostic bounds, and persistence continuity.
