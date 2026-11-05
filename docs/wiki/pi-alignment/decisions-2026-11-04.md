# Owner decisions via the chief of staff, 2026-11-04

Decisions recorded 2026-11-04, read at `lanshengzhi/cpp-coding-harness@6ca7b493c` **plus the uncommitted
working tree** (modified `1058-consolidated-capability-ledger.json`, `1058-consolidation-frontier.md`,
`1058-deliveries.json`; untracked `1202-withdrawn-1104-receipt.json`) and live GitHub issue state
queried 2026-11-04. They answer
[#1183](https://github.com/lanshengzhi/cpp-coding-harness/issues/1183) inside the selected scope of
[ADR 0075](adr-0075-scope.md). Conventions: [conventions](conventions.md).

## Provenance

The owner's rule of 2026-11-04 delegated these decisions to the chief of staff: follow pi alignment;
the owner delegated the #1183 reconciliation rulings recorded here. The four decisions below are the
record of that ruling. The [reconciliation proposal](reconciliation-1183-proposal-2026-11-04.md) stays
a proposal: no code, ledger, frontier, deliveries, plan file or git state was modified by this ruling,
and nothing was committed. Tracker state did change: the new tickets in §2 were filed 2026-11-04.

## 1. The #1104 receipt withdrawal (downgrade) is accepted; re-anchor later

The working-tree withdrawal of the `1104-management-during-connection` delivery record is accepted as
the recorded disposition of #1104's two rows
(`cch_coding_agent:mcp-external-signin-pickup` → missing/unverified, ticket null;
`cch_coding_agent:mcp-management-ui` → unverified, ticket null), with the receipt preserved in the
untracked `1202-withdrawn-1104-receipt.json`. This resolves proposal §6(a) in favor of **accept** (not
revert). Re-anchoring the two rows' verification happens later, through tickets, under the delivery
receipt contract; it is not part of this decision.

Consequence for proposal E0: the uncommitted tree now carries an accepted disposition, but the physical
commit or revert of the four working-tree files is still to be executed by the serial merger — the
decision accepts the downgrade, it does not perform the commit.

## 2. Every receipt row named by #1202 is downgraded to unverified (fail-closed); re-anchor later via tickets

Fail-closed: until each disputed receipt is re-anchored, its rows count as **unverified**, not
"receipt-backed but disputed". This applies to all rows named by
[#1202](https://github.com/lanshengzhi/cpp-coding-harness/issues/1202): #1118 (3 rows), #1179 (4 rows),
#1181 (4 rows), #1189 (4 rows) and #1090's 2 comparisons (counted as 2 rows) — up to 17 of the 113
verified rows. Re-anchoring proceeds later, per receipt, through #1202's own acceptance criteria and
its automated boundary test; this decision fixes the default, not the per-receipt outcome. Downgrading
is a ledger/deliveries change and therefore remains to be produced by `consolidate_audits.py build`
from changed inputs; no ledger edit was made today.

## 3. JSONL task-sidecar symlink rejection: not decided — a named security boundary blocks the ruling

The owner instruction was: align with frozen pi **unless** `docs/agents/architecture.md` or an ADR
names the rejection a security boundary; if one does, do not decide and report a blocker. One does.

- [architecture.md](../../agents/architecture.md) §Security and containment (read at `6ca7b493c`)
  names "the no-follow symlink policy with atomic writes" among the remaining security policies that
  tool/file/session changes preserve.
- [ADR 0057](../../adr/0057-retire-workspace-containment-and-align-path-resolution-with-pi-resolvetocwd.md)
  keeps "the no-follow symlink policy and atomic writes" among the policies that survive containment
  retirement and remain in force.
- The boundary record
  [1085-filesystem-boundary.md](../../research/pi-v1.3.0/1085-filesystem-boundary.md) itself anchors
  the durable JSONL rejection to that policy ("The architecture security section retains a no-follow
  policy") and calls the rejection "an intentional safety divergence, not frozen parity".

The durable JSONL task-sidecar symlink/hard-link rejection is an application of that named no-follow
symlink policy, so the exception condition is met. **No alignment decision is recorded.** The
divergence needs an explicit owner ruling (accept as a named exemption, or fund alignment), and until
then `cch_durable:Storage:jsonl:sidecar` and its sibling ordering rows cannot complete — this is a
**blocker** for the durable verification lane, reported as such. No symlink-alignment issue was filed;
the open question stays open (see [open questions](open-questions.md)).

## 4. The #1182 route batches B1–B17 are filed as issues

The 17 route batches in
[1182-tui-route-batches.json](../../research/pi-v1.3.0/1182-tui-route-batches.json) (913 rows, 17
batches, no issue field as of 2026-11-04) are filed as tracker issues, one per batch, carrying each
batch's caller, capture families, normal and discriminating cases, rows and implementation
obligation. This implements proposal ticket N5 by creating the batch set itself. Emulator batches B6
and B14 and the product batch B17 carry native blocked-by edges to #831/#1035 as the route file's
dependency field records.

## New tickets filed 2026-11-04

All new issues are labelled `ready-for-agent`, matching the sibling children of #1044. Native
sub-issue parentage: GitHub's 100-direct-sub-issue limit was reached on #1044 after 92 existing
children plus N1–N4, N6, the script ticket and batches B1–B2, so batches B3–B17 are nested under
#1183 instead — the same mechanism #1044's own index documents for its verification packets
(#1183 already parents #1176–#1182 and the chord/codemode children). B1 and B2 remain direct
children of #1044.

| Proposal slot | Issue | Title |
| --- | --- | --- |
| N1 | [#1203](https://github.com/lanshengzhi/cpp-coding-harness/issues/1203) | [pi-v1.3.0 durable] Capture the 21 unobserved durable rows at the frozen v1.3.0 baseline |
| N2 | [#1204](https://github.com/lanshengzhi/cpp-coding-harness/issues/1204) | [pi-v1.3.0 chord] Verify the four chord rows left over by the closed B1-B5 batches |
| N3 | [#1205](https://github.com/lanshengzhi/cpp-coding-harness/issues/1205) | [pi-v1.3.0 agent-core] Verify the 13 agent-core rows delivered by closed slices #1107-#1113 (successor to #1176) |
| N4 | [#1206](https://github.com/lanshengzhi/cpp-coding-harness/issues/1206) | [pi-v1.3.0 application] Verify the delivered application rows (successor to #1177) |
| N5 → B1 | [#1209](https://github.com/lanshengzhi/cpp-coding-harness/issues/1209) | [pi-v1.3.0 TUI B1] Root TUI lifecycle, root mutation and diagnostic callbacks |
| N5 → B2 | [#1210](https://github.com/lanshengzhi/cpp-coding-harness/issues/1210) | [pi-v1.3.0 TUI B2] Regular renderer, frame commit and render-state transfer |
| N5 → B3 | [#1211](https://github.com/lanshengzhi/cpp-coding-harness/issues/1211) | [pi-v1.3.0 TUI B3] Alternate-screen viewport: frames, scroll algebra, scrollbar and search |
| N5 → B4 | [#1212](https://github.com/lanshengzhi/cpp-coding-harness/issues/1212) | [pi-v1.3.0 TUI B4] Alternate-screen transcript selection, multi-click and copy |
| N5 → B5 | [#1213](https://github.com/lanshengzhi/cpp-coding-harness/issues/1213) | [pi-v1.3.0 TUI B5] Alternate-screen overlay, wheel and mouse routing |
| N5 → B6 | [#1214](https://github.com/lanshengzhi/cpp-coding-harness/issues/1214) | [pi-v1.3.0 TUI B6] Alternate-screen kitty image placement and eviction (blocked by #1035, #831) |
| N5 → B7 | [#1215](https://github.com/lanshengzhi/cpp-coding-harness/issues/1215) | [pi-v1.3.0 TUI B7] Editor: layout, history, undo and kill/yank |
| N5 → B8 | [#1216](https://github.com/lanshengzhi/cpp-coding-harness/issues/1216) | [pi-v1.3.0 TUI B8] Input: editing, paste and submit |
| N5 → B9 | [#1217](https://github.com/lanshengzhi/cpp-coding-harness/issues/1217) | [pi-v1.3.0 TUI B9] Markdown, tables and math rendering |
| N5 → B10 | [#1218](https://github.com/lanshengzhi/cpp-coding-harness/issues/1218) | [pi-v1.3.0 TUI B10] Color parsing, palettes and terminal color capability |
| N5 → B11 | [#1219](https://github.com/lanshengzhi/cpp-coding-harness/issues/1219) | [pi-v1.3.0 TUI B11] SelectList and SettingsList |
| N5 → B12 | [#1220](https://github.com/lanshengzhi/cpp-coding-harness/issues/1220) | [pi-v1.3.0 TUI B12] Layout algebra, scroll containers and the small text components |
| N5 → B13 | [#1221](https://github.com/lanshengzhi/cpp-coding-harness/issues/1221) | [pi-v1.3.0 TUI B13] Terminal IO: process terminal, stdin framing, keys and keybindings |
| N5 → B14 | [#1222](https://github.com/lanshengzhi/cpp-coding-harness/issues/1222) | [pi-v1.3.0 TUI B14] Image dimensions, protocol detection and the Image component (blocked by #1035) |
| N5 → B15 | [#1223](https://github.com/lanshengzhi/cpp-coding-harness/issues/1223) | [pi-v1.3.0 TUI B15] Autocomplete providers and fuzzy ranking |
| N5 → B16 | [#1224](https://github.com/lanshengzhi/cpp-coding-harness/issues/1224) | [pi-v1.3.0 TUI B16] Loader, mouse regions and residual small components |
| N5 → B17 | [#1225](https://github.com/lanshengzhi/cpp-coding-harness/issues/1225) | [pi-v1.3.0 TUI B17] Application product entries and the selected emulator scenarios (blocked by #831, #1035) |
| N6 | [#1207](https://github.com/lanshengzhi/cpp-coding-harness/issues/1207) | [pi-v1.3.0 mcp] Verify mcp-roots-list and mcp-stdio-stderr-tail (unobserved, no ticket) |
| script | [#1208](https://github.com/lanshengzhi/cpp-coding-harness/issues/1208) | [pi-v1.3.0 tooling] Read-only reconciliation script joining ledger, deliveries, frontier and the live issue graph |

No symlink-alignment issue was filed: decision 3 records a blocker needing an owner ruling, which
precedes any ticket.

## Still undetermined after these decisions

- The symlink-divergence ruling itself (decision 3) — owner input required; blocker for the durable
  JSONL ordering/sidecar rows.
- Proposal §6(e) (`cch_chord:json:json-representation`: accept the compile-time-only boundary or fund
  a carrier spelling) and §6(f) (`cch_chord:context:root-background`: #1184-successor vs #1180) — not
  part of this ruling; noted in #1204.
- Proposal §6(g) (whether closed slices without reports, e.g. #1081, get report backfill as a condition
  for E2 implementation catch-up edits) — not part of this ruling.
- Proposal §6(d) is settled procedurally: N3 and N4 were filed as successors to the closed #1176/#1177,
  which stay closed.
- Proposal E1/E2/E4 (ticket-mapping build channel, implementation catch-up, frontier prose) remain to
  be executed against the ledger build path; nothing in the ledger, frontier or deliveries was edited
  by these decisions.

## Execution note — decisions 1–2 applied 2026-11-05

Recorded by the implementation sub-agent after executing E0 and the reproducible half of E1.

**E0 commit.** `a4df1a1153b044f3e406cf543d36f3ad1180c864` (current branch, not pushed):
`docs(evidence): withdraw the #1104 management-during-connection receipt per #1202`. Exactly four
paths: `1058-consolidated-capability-ledger.json`, `1058-consolidation-frontier.md`,
`1058-deliveries.json`, `1202-withdrawn-1104-receipt.json` (verified with `git show --stat`).
Verified rows after E0: 113.

**Receipts withdrawn (decisions 1–2, via the #1104 pattern: whole-record removal from
`1058-deliveries.json`, record preserved in a `1202-withdrawn-<receipt>.json` file, ledger
regenerated by `consolidate_audits.py build`):**

- #1104 `1104-management-during-connection` — both rows (committed as `a4df1a1`);
  `cch_coding_agent:mcp-external-signin-pickup` → `missing`/`unverified`,
  `cch_coding_agent:mcp-management-ui` → `unverified`; tickets null.
- #1189 `1189-close-refresh` — all four rows (the receipt's entire claim; left uncommitted in the
  working tree with the rebuilt ledger/frontier); preserved in `1202-withdrawn-1189-receipt.json`.
  All four rows revert to their **source-audit** status `partial`/`upstream-captured` with tickets
  null — fail-closed (no longer receipt-verified), but note the mechanism preserves the source
  verification label rather than forcing the literal `unverified` word that decision 2 uses.

**Stopped — no clean mechanism (per the stop rule, nothing improvised).** Decision 2 also names
rows in four receipts whose dispute is *partial*, and `consolidate_audits.py` promotes and withdraws
only whole delivery records:

- #1118 — 3 of 4 claimed rows disputed (one row expressly unaffected per the #1202 note); withdrawing
  the record would over-downgrade 1 row.
- #1179 — 4 of 39 claimed rows disputed (timeout-option row + three worker rows); withdrawing the
  record would over-downgrade 35 rows.
- #1181 — 4 of 7 claimed rows disputed; withdrawing the record would over-downgrade 3 rows.
- #1090 — 2 of 14 claimed comparisons disputed; withdrawing the record would over-downgrade rows and
  comparisons #1202 did not name.

What is missing: a per-row or per-comparison withdrawal channel in `consolidate_audits.py`
(e.g. a checked-in withdrawn-claims section the build subtracts from a record's claims, with
`check` validating the subtraction), or owner acceptance of whole-record withdrawal for these four.
Adding it was out of scope here because it changes tool behavior. Until then, 13 of the 109 verified
rows (#1118's 3, #1179's 4, #1181's 4, #1090's 2) remain receipt-backed but disputed.

**Final counts (ledger rebuilt 2026-11-05).** 1,464 rows; supported 1,435 / excluded 29.
Verification: `verified` 113 → **109**, `upstream-captured` 311 → **315**, `unverified` 1012
(unchanged), `not-applicable` 28. Implementation: `implemented` 1094 → **1090**, `partial` 172 →
**176**, `missing` 180, `not-applicable` 18. Delivery records: 15 → 13 active, 2 withdrawn and
preserved. Rows with missing observations: 1012.

**Checks run (all after the 2026-11-05 build), all passing:**

1. `consolidate_audits.py build` + `check` — PASS; "deliveries: 109 rows backed by pinned C++
   comparison results"; 0 uncovered scenarios; owner graph acyclic; 36 relative links resolve.
2. `python3 tests/evidence/ConsolidationAuditTest.py` — 42/42 OK.
3. `python3 scripts/evidence/audit_evidence.py verify --baseline pi-v1.3.0` — 57 bundles / 118
   artifacts / 661 scenarios verified at `71023aec`; exit 0.
4. `git diff --check` — clean (exit 0).

No code, issues, or files outside `docs/research/pi-v1.3.0` and `docs/wiki` were touched; nothing
was pushed. The E1 working-tree changes (deliveries/ledger/frontier rebuild, both withdrawal
preservation files' successors, and the wiki updates) are uncommitted for the serial merger's review.

Related: [open questions](open-questions.md), [goals](goals.md),
[reconciliation proposal](reconciliation-1183-proposal-2026-11-04.md),
[residual-gap audit](residual-gap-audit-2026-11-04.md), [index](index.md).
