# Ledger field staleness

Recurring problem, first recorded 2026-11-06 at `lanshengzhi/cpp-coding-harness@77d41e135`. The consolidated ledger (`docs/research/pi-v1.3.0/1058-consolidated-capability-ledger.json`) copies each source audit row's fields verbatim and only advances `verification` (and `delivery`) through the delivery contract. Fields that describe implementation — `implementation`, `consumer`, `gaps`, `notes`, `remainingWork` — keep their audit-time values, so a row touched by later work can be half-refreshed.

## Observed shape (2026-11-06)
- 88 rows still read `consumer: "none (cch_durable owner not introduced)"`, including `cch_durable:Storage:jsonl:sidecar`, which is delivery-backed verified.
- The `1054` source row for `cch_durable:Document:address-identity` says `gaps: ["No cch_durable target or C++ implementation exists; this audit records upstream behavior only."]` and `implementation: missing`, from the 2026-10-20 audit — before `src/durable/` existed. At `77d41e135` the package implements that surface, and the row's own `remainingWork` says the C++ comparison is what remains.
- Consequence: counting `implementation` over durable rows sizes evidence debt as code work. The [2026-11-06 history entry](evidence-and-delivery-history.md) records the superseded reading.

## Why it recurs
- An audit is a snapshot of the upstream/frozen side plus a judgement about Pike at one date. Later lanes change Pike, and nothing re-derives the implementation judgement.
- Delivering a receipt therefore improves `verification` while leaving `implementation` stale — the two fields drift apart until a row claims `verified` next to a gap note saying the feature does not exist.

## Candidate detectors (not built as of that commit)
- Fail the consolidation build when a row's `gaps`/`consumer` text names an owner package that the repository now builds (`src/<owner>/`), or when its `implementation_ticket` is closed.
- Fail when a row is `verified` while `implementation` is `missing` or `consumer` says the owner is absent.
- Both are cheap, mechanical, and would have caught the 2026-11-06 case; see [validation](../../agents/validation.md) §Acceptance discipline for why a text check alone is a check and not acceptance.

Related: [evidence and delivery history](evidence-and-delivery-history.md), [v1.3.0 residual-gap audit](residual-gap-audit-2026-11-04.md), [index](index.md).
