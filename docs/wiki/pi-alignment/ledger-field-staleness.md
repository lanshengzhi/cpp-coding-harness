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

## 2026-11-06 (later): the detectors were built, with recorded limits
Delivered at `lanshengzhi/cpp-coding-harness@79379d90b` (issue #1239, record `docs/research/pi-v1.3.0/1239-stale-field-gates.md`). The section above stays as the proposal it was; this records what landed and what it cannot do.

- `consolidate_audits.py` now fails closed on: `verified` + `implementation: missing`; `verified` + a `consumer`/`gaps` text describing the owner package as absent; `implementation: missing` + a built `src/<owner>/` + the owner named in the text; an `implementation_ticket` recorded closed in an offline snapshot; and an unbounded claim whose `discriminatingCase` reuses one of its own comparisons.
- Pre-existing contradictions sit in `fixtures/pi-v1.3.0/consolidation-1058/1239-stale-fields.json` — **777 entries** at that commit, identity `{id, field, contradiction}`, `value` retained as evidence, with a count ratchet that fails the build when a recorded contradiction is resolved but not removed. Reading 777 as 777 real defects is wrong: the record states the regex escapes absence phrasings (`"cch_durable has not landed in tree yet"`) and falsely flags true text (`"verified that when optional cch_durable package configuration is missing, fallback defaults are used."`).
- The issue-state snapshot is offline by design; a ticket closed after the snapshot date stays recorded open. The response is a 30-day age warning plus a recorded boundary, not a TTL failure. Refresh procedure: [delivery reconciliation](../../research/pi-v1.3.0/1058-delivery-reconciliation.md).
- The same commit bounded 44 claims whose discriminating case reused a comparison; verified rows fell 609 → 565. Row lists and follow-ups: #1240 (chord 15), #1241 (codemode 19), #1242 (images/durable 6), #1243 (agent/application 4).

### Correction, same day: most of that baseline was a false positive
At `lanshengzhi/cpp-coding-harness@5c93d5a` the closed-`implementation_ticket` check was found to fire on **595 rows, 549 of them delivery-backed verified** — a closed implementation ticket is the *normal* state of a delivered row, not a contradiction. Those 595 were 595 of the 777 baseline entries, so the baseline was three-quarters noise and indistinguishable from real defects. A promotion rewrites `implementation_ticket`, so its violation changed identity, was not in the baseline, and consolidation failed: the gate blocked the very work it was meant to protect. The genuine class is the row whose `implementation` is `missing`/`partial`, whose owning ticket is closed and which is not verified (~28 rows). Narrowing, baseline pruning and the missing killing tests are #1246; the lesson recorded in [ops-lanes](ops-lanes.md) is that a specified check must be measured against the artifacts before it is trusted, and this page's earlier "777 entries" line must not be read as a defect count.

Related: [evidence and delivery history](evidence-and-delivery-history.md), [v1.3.0 residual-gap audit](residual-gap-audit-2026-11-04.md), [index](index.md).
