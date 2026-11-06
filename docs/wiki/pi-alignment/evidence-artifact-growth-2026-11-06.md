# Evidence artifact growth in `docs/research/pi-v1.3.0` (measured 2026-11-06)

Why every capability lane commits a large regenerated JSON, and what it costs. Measurements taken
at `lanshengzhi/cpp-coding-harness@e35f1a554` (2026-11-06, lane `issue-1221` held).

## The mechanism (why the JSON is committed at all)

On 2026-11-06, the #1058 consolidation contract requires that a row move from `unverified` to
`verified` only through a committed delivery receipt. `fixtures/pi-v1.3.0/consolidation-1058/consolidate_audits.py`
builds one canonical ledger from eleven source audit ledgers plus `1058-deliveries.json`; `check`
rebuilds it in memory and compares bytes, so the committed ledger cannot drift from its inputs.
A verified row therefore costs three committed artifacts per receipt: the JUnit XML, a
`deliveries.json` entry, and a rebuilt `1058-consolidated-capability-ledger.json`. The contract text
is in [1058-delivery-reconciliation.md](../../research/pi-v1.3.0/1058-delivery-reconciliation.md)
§Delivery evidence contract and `docs/agents/validation.md` §2.

So the file count is not incidental: the ledger is the gate, and a gate that is only computed in CI
could not be cited by a row.

## The cost, as measured

| Artifact | Size | Note |
| --- | --- | --- |
| `1058-consolidated-capability-ledger.json` | 5,490,135 B / 119,043 lines | 1,464 rows, pretty-printed |
| `1058-deliveries.json` | 1.4 MB / 27,196 lines | reviewed receipt inputs |
| `1050-toolkit-capability-ledger.json` | 1.6 MB | source audit lane |
| `docs/research/` (tracked) | 21 MB | 385 files under `deliveries/` alone |
| `.git` | 368 MB | every revision stores a full ledger copy |

- 191 revisions of the ledger exist as of that commit; 253 commits touch a `docs/research/**/*.json`.
- Growth: 2,890 KB at `0815c1b25` (2026-10-20) → 5,490 KB at `e35f1a554` (2026-11-06), ~1.9x in 17 days.
- A representative evidence commit (`90a27bef0`, 2026-11-06) touches 26–29 files, +9,555/−308 lines,
  of which the ledger alone is +790/−148. The delivery commits are larger: `585b5efbf` is
  +39,721/−24,940.
- The ledger diff is dominated by churn, not by rows: pinned `sha256`, `testedRevision` and
  `recordedAt` values change on every re-pin, so a reviewer cannot read the diff as "which
  capabilities changed".
- 291 `previousValidation` blocks are embedded; maximum nesting depth is 1, so history accumulates
  linearly, not quadratically.

## Assessment

The contract is sound and was deliberately chosen. The *representation* is what costs: an
append-only evidence log is stored as one monolithic regenerated document, which maximises git
churn, makes review of an evidence commit impractical, and guarantees that two lanes cannot rebuild
the same file concurrently — the reason the one-code-lane rule in
[ops-lanes](ops-lanes.md) has to exist for evidence work at all.

Options raised on 2026-11-06, none decided:

1. Split the generated ledger per owner package (`cch_tui`, `cch_durable`, …) behind a small index,
   so a receipt rewrites only its own slice and a reviewer reads one slice.
2. Commit the digest of the derived ledger plus a compact summary, and let `check` prove a rebuild
   matches, keeping the 5 MB bytes out of every commit.
3. Emit rows one-per-line so a diff shows changed rows rather than changed pins.

Whatever is chosen must preserve the property that a row's verification is provable from committed
bytes alone.
