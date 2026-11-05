# v1.3.0 residual-gap audit, 2026-11-04

Dated audit of how much of the owner's immediate goal is evidenced. Read at `lanshengzhi/cpp-coding-harness@6ca7b49` **plus the uncommitted working tree** (modified 1058 ledger, frontier and deliveries; untracked 1202 receipt) and live GitHub issue state queried 2026-11-04. It describes records, not a fresh behavioral test of the code. Conventions: [conventions](conventions.md).

## Scope and method
- **Scope (owner decision 2026-11-04):** exactly the selected scope of [ADR 0075](../../adr/0075-align-selected-owner-packages-and-linux-capabilities-with-pi-v1-3-0.md): selected non-AI capabilities, package migration, complete Linux durable support; stated exclusions and the Magpie/XDG boundaries stay out. See [goals](goals.md) and [ADR 0075 scope](adr-0075-scope.md). Anything outside that (for example `cch_ai` alignment, Boost removal, ChatGPT OAuth) is not audited here.
- **Starting points:** ADR 0075; the [1058 ledger](../../research/pi-v1.3.0/1058-consolidated-capability-ledger.json), [frontier](../../research/pi-v1.3.0/1058-consolidation-frontier.md) and [deliveries](../../research/pi-v1.3.0/1058-deliveries.json); the [#1202 note](../../research/pi-v1.3.0/1202-1104-evidence-integrity.md) and [receipt file](../../research/pi-v1.3.0/1202-withdrawn-1104-receipt.json); delivery reports under `docs/research/pi-v1.3.0/`; [#1044](https://github.com/lanshengzhi/cpp-coding-harness/issues/1044) and its children via `gh`.
- **Three separate records, as ADR 0075 requires:** membership, implementation, verification. Ledger fields are used as recorded; where later sources contradict them this page says so.

## What was and was not verified today
| Run (2026-11-04) | Result |
| --- | --- |
| `python3 scripts/evidence/audit_evidence.py verify --baseline pi-v1.3.0` | passed: 57 bundles, 118 artifacts, 661 scenarios at the full frozen revision |
| `python3 fixtures/pi-v1.3.0/consolidation-1058/consolidate_audits.py check` (against the **working-tree** ledger) | exit 0: 1464 rows, 447 rows anchored to registered artifacts, owner graph acyclic |
| `python3 tests/evidence/ConsolidationAuditTest.py` | 42/42 passed |
| `ctest --preset vcpkg -N -L architecture` | listed 43 tests; **not run** |
| Source-reading, scripted counts of the ledger and deliveries; `gh` queries of issues, sub-issues and blocked-by edges | done |

**Not done:** no C++ build, no ctest execution (so no receipt was re-executed and no JUnit re-checked), no frozen-pi capture or live-pi continuation test (`PI_CHECKOUT` was not used), no terminal-emulator or PTY run, no Full Validation. Passing integrity checks prove registered bytes, hashes and ledger consistency; they do not prove that a cited test compares the registered observation (the lesson of #1202). The `build/` tree was last touched 2026-10-21 and was not used.

## Headline numbers (working-tree ledger, 2026-11-04)
1435 supported rows (29 excluded, none "no decision"). Supported rows by recorded verification: 113 verified, 310 upstream-captured only, 1012 unverified (113 is 7.9%). Recorded implementation of supported rows: 1094 implemented, 172 partial, 169 missing. **These implementation counts lag the code** (see "Ledger lag" below) and must not be read as a percent-complete figure. The committed ledger at HEAD said 115 verified; the working tree lowers it to 113 (see below).

## A. Confirmed complete, with evidence
"Confirmed" here means: recorded and checkable evidence exists and today's integrity checks pass. It does **not** mean re-executed today.

- **Evidence foundation (ADR 0075 migration step 3).** Registry, provenance and digests verify at the frozen revision (run above). The consolidation checker and its 42 tests pass.
- **Membership.** No row is "no-decision"; the two former no-decision rows were excluded by the owner's #1059 ruling ([frontier](../../research/pi-v1.3.0/1058-consolidation-frontier.md) section 2). This is a ledger fact, not new behavior.
- **Owner package structure, as observed today.** `cmake/parity/manifest.json` lists the nine owners including `cch_durable`, `cch_mcp`, `cch_codemode` and `cch_chord`; `src/agent/` has no session, compaction, history or environment files (directory listing). The ADR warns that a filename move alone proves nothing; behavior is covered by the rows below, and the architecture gate was not run today.
- **At least 96 of the 113 verified rows are not named by #1202** (it names up to 17 other rows, counting #1090's two comparisons as two rows; see section B). Distribution of all 113 verified rows by owner: `cch_chord` 39, `cch_codemode` 39, `cch_mcp` 12, `cch_coding_agent` 8, `cch_tui` 7, `cch_agent_core` 4, `cch_durable` 4 (the #1202 rows were not mapped to row ids). Basis: each is a whole-row receipt in the [deliveries file](../../research/pi-v1.3.0/1058-deliveries.json) that passes the checker, whose cited test source names the registered artifact (a weak, mechanical heuristic I ran; it is neither necessary nor sufficient). #1202 explicitly did not audit these. Treat as "receipt-backed, unaudited by an independent reader".
- **Durable cross-runtime continuation, exercised slice only.** [JSONL (#1085)](../../research/pi-v1.3.0/1085-jsonl-continuation.md) and [SQLite (#1086)](../../research/pi-v1.3.0/1086-sqlite-continuation.md) reports record alternating pi/C++ runs against a live frozen checkout: 28/28 JSONL and 4/4 SQLite continuation selections, and Full Validation on the merged #1085 tree (3,894 of 3,895 passed, one skipped). This is the strongest independent evidence in the scope. Per the reports, it promotes **no ledger row**, and the corresponding `Interchange:*` rows still read missing/unverified (see C).

## B. Claimed but lacking independent evidence
- **#1202 receipts still claimed verified (3 + 4 + 4 + 4 rows, plus #1090's 2 comparisons; at most 17 rows).** [#1202](https://github.com/lanshengzhi/cpp-coding-harness/issues/1202) (open) names receipts whose cited tests do not read the registered artifact or do not compare all its fields: #1118 (3 rows, discriminators unanchored), #1179 (4 rows: `codemode-source-timeout-option` compares 1 of 3 literals; three worker rows), #1181 (4 of 7 rows), #1189 (4 rows), #1090 (2 comparisons, 2 rows). A crude check I ran (does the cited test source file contain the artifact's file name) flags comparisons in the #1090, #1179, #1181 and #1189 receipts and none in #1118, which broadly agrees with the list but is not a proof either way. All still count as verified in the ledger. None of its four acceptance criteria is met: no re-anchor, downgrade or boundary test exists yet.
- **#1104 (two rows) withdrawn only in the uncommitted tree.** The working tree removes the #1104 receipt (kept in the untracked 1202 receipt file), so `mcp-management-ui` is unverified and `mcp-external-signin-pickup` is unverified and now reads *missing*. That "missing" is the reverted source-audit status, not an observation: the [#1104 report](../../research/pi-v1.3.0/1104-mcp-management-during-connection.md) records the behavior as delivered and #1104 is closed. Committed HEAD still carries the receipt. The owner has not been recorded as accepting the downgrade; per the #1202 note it is the serial merger's decision.
- **Ledger rows with no committed owner decision on a divergence.** The JSONL task-sidecar symlink rejection "deliberately differs from frozen pi" ([#1085 report](../../research/pi-v1.3.0/1085-jsonl-continuation.md)). ADR 0075 removes historical exemptions except those it lists; this one is not listed. Undecided: see [open questions](open-questions.md).
- **Closed implementation tickets without row-level acceptance.** #1170 to #1175 (durable nested tasks, codemode continuation and output, event stream, JSONL fsync, `abandonOnRestart`) and #1176 to #1179, #1181, #1182 are closed, but the durable owner packet #1180 is open and ledger rows still say missing or unverified. The 2026-10-26 and 2026-10-30 reconciliations state a closed issue is not proof of acceptance.
- **Upstream-captured is not verified.** 310 supported rows have a frozen observation but no whole-row C++ comparison. For 858 rows (828 `cch_tui`, 30 `cch_coding_agent`) the only evidence route is the historical v1.0.4 bundle, which ADR 0075 says cannot back a v1.3.0 claim ([#1201](https://github.com/lanshengzhi/cpp-coding-harness/issues/1201) covers the 30 application rows; #1182 is closed after partitioning the 828 into batches, [route batches](../../research/pi-v1.3.0/1182-tui-route-batches.json); I did not check whether those batches were later executed).
- **Ledger lag (evidence the ledger is stale, not that work is missing).** The ledger records `cch_durable:Document:defineDoc` as "No document definition or validation exists in Pike" and 118 durable rows as missing, yet `src/durable/include/cch/durable/Session.hpp` (commit `4e9ad3fbf`, 2026-11-02) declares `define_document`, and [architecture.md](../../agents/architecture.md) describes #1170 to #1175 as delivered. 192 supported missing/partial rows carry no implementation ticket and 149 cite `#1058-draft`. #1183 (open) owns this reconciliation, so the current implementation percentage is unknown, not low.

## C. Open or blocked
Live state 2026-11-04 (GitHub is authoritative; these are a snapshot). #1044 is open with 92 direct sub-issues, 87 complete.

| Area | Open items (issue) | Notes |
| --- | --- | --- |
| Final integration | #1126 (acceptance, Full Validation), #1183 (delivery map, ledger and frontier reconciliation), #1044 | #1126 is blocked by #1167, #1183, #1035 and #831 |
| Terminal evidence | #831 dock footer, #1035 image placement repaint | Real emulator criteria unchecked; both block #1182 and #1126. Dated PTY evidence does not establish emulator rendering |
| Terminal comparisons | #1201 (30 application rows recaptured at v1.3.0) | No native parent or blocker recorded |
| Durable | #1180 (owner packet); #1148 (reservation atomicity, abort races); #1158 (environment listing and read bounds) | #1180 is blocked by #1148 and #1158 |
| Environment IO split from #1158 | #1197, #1198, #1199, #1200 | No native parent when queried; contribution to #1180 gating not shown |
| JSONL ordering | no ticket | #1180's body requires an independent two-sidecar frozen capture before claiming parity for `jsonl:ordering`, `:sidecar`, `:main-marker`; not assigned to an issue (checked by title search) |
| Interchange rows | `Interchange:alternating-writers`, `:cpp-create`, `Storage:jsonl:reclaim-task`, `Storage:sqlite:options` still declared-comparison or related-captured | continuation evidence exists in reports but is not registered as a receipt |
| Codemode | #1192, #1193, #1194, #1195, #1196 (children of #1183) | tool table and close surface, source-only rows, portable no-Node entry, discovery/timezone, lifetime hook and actual-exit |
| Evidence integrity | #1202 | see section B |
| Test reliability | #1167 (durable failure-path cleanup, blocks #1126) | not a capability but a gate |
| Possibly in scope, unassigned | #725 (DEL in a session makes Native TUI exit), #746 (use-after-free binding `Agent::state()` temporaries) | neither is a child of #1044; whether they belong to the selected scope is not determined here |

Not audited (outside scope or unrelated): #1130 to #1139 (Boost removal), #862 (ChatGPT OAuth, conflicts with the retained Magpie path), #860 (XDG docs home), #661, #682, #688, #692, #696, #712, #742, #743, #749, #830, #1141, #1154, #1162.

## Conclusion
As of 2026-11-04 the immediate goal is **not demonstrably complete** and cannot be quantified from the ledger. Receipt-backed verification covers 113 of 1435 supported rows (7.9%), of which up to 17 are in dispute. The largest uncertainty is the durable lane (207 supported rows, most recorded as missing but evidently partly delivered) and the 828 TUI rows still routed to v1.0.4 evidence. Cheapest next steps are decisions, not coding: accept or revert the #1104 downgrade and commit or discard the four uncommitted files; resolve #1202; finish #1183.

## Environment improvements that would cheapen the next audit
1. **Commit or park the uncommitted 1058/1202 files** so the working tree equals a named commit; this audit had to read two states.
2. **One script that joins ledger, deliveries and live issue graph** (per-owner implementation, verification, open tickets, blockers). I rebuilt this with ad hoc Python and about ten `gh` calls; native sub-issue and blocked-by data needed GraphQL.
3. **Automate the #1202 boundary check** (receipt test never reads its artifact), then run it in the evidence suite; it would replace my heuristic.
4. **Make ledger implementation status derive from, or be checked against, code and delivery reports**, so rows such as `defineDoc` cannot stay "missing" after delivery; give null or draft ticket references a checker warning.
5. **Keep a warm build and a `PI_CHECKOUT` with a working esbuild binary** (the 2026-10-30 reassessment found the sibling checkout unusable for real captures) so a focused `ctest -L issueNNNN` run takes minutes.
6. **A wiki link and tracker-reference lint script** in `scripts/`, so documentation checks are one command.

Related: [goals](goals.md), [open questions](open-questions.md), [evidence and delivery history](evidence-and-delivery-history.md), [index](index.md).
