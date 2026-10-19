# pi-v1.3.0 capability consolidation and implementation frontier (#1058)

Status: audit-acceptance deliverable for the #1044 umbrella, at frozen revision
`71023aec1546d6d677a65669c5665c0927ce0dd0`. This work consolidates the eleven lane audits
(#1047-#1057) and the #1046 evidence infrastructure into one stable capability ledger, and drafts
the demoable single-context implementation tickets with real acceptance and failure cases. It does
not implement, publish, migrate or close anything: umbrella #1044 stays open, no audit row becomes
verified, and no historical artifact or observation payload is rewritten.

Deliverables:

- [1058-consolidated-capability-ledger.json](./1058-consolidated-capability-ledger.json) - the
  stable ledger: 1460 consolidated rows, one alias per source row, evidence routes to real
  registered scenarios or an honest missing observation.
- [1058-implementation-frontier.json](./1058-implementation-frontier.json) - the owner graph, the
  expand/migrate/contract/integrate-verify migration sequence, 32 implementation drafts and the
  residual-tracker reconciliation.
- [consolidation-plan.json](../../../fixtures/pi-v1.3.0/consolidation-1058/consolidation-plan.json)
  and
  [consolidate_audits.py](../../../fixtures/pi-v1.3.0/consolidation-1058/consolidate_audits.py) -
  the curated mapping (schema mappings, owner/consumer rules, groups, overlaps, corrections index)
  and the deterministic build/check tool.

## 1. What was consolidated

| Ticket | Ledger | Report | Rows | Bundle |
| --- | --- | --- | --- | --- |
| 1047 | [1047-capability-ledger.json](./1047-capability-ledger.json) | [1047-agent-execution-audit.md](./1047-agent-execution-audit.md) | 32 | audit-1047 |
| 1048 | [1048-session-persistence.json](./1048-session-persistence.json) | [1048-session-persistence.md](./1048-session-persistence.md) | 30 | audit-1048 |
| 1049 | [1049-ledger.json](./1049-ledger.json) | [1049-resources-configuration-extensions-print-mode.md](./1049-resources-configuration-extensions-print-mode.md) | 20 | audit-1049 |
| 1050 | [1050-toolkit-capability-ledger.json](./1050-toolkit-capability-ledger.json) | [1050-toolkit-capability-inventory.md](./1050-toolkit-capability-inventory.md) | 954 | audit-1050 |
| 1051 | [1051-native-tui-consumers-and-residuals.json](./1051-native-tui-consumers-and-residuals.json) | [1051-native-tui-consumers-and-residuals.md](./1051-native-tui-consumers-and-residuals.md) | 46 | audit-1051 |
| 1052 | [1052-mcp-audit-ledger.json](./1052-mcp-audit-ledger.json) | [1052-mcp-protocol-oauth-application.md](./1052-mcp-protocol-oauth-application.md) | 31 | audit-1052 |
| 1053 | [1053-codemode-audit-ledger.json](./1053-codemode-audit-ledger.json) | [1053-codemode-audit.md](./1053-codemode-audit.md) | 68 | audit-1053 |
| 1054 | [1054-durable-session-documents-storage.json](./1054-durable-session-documents-storage.json) | [1054-durable-session-documents-storage.md](./1054-durable-session-documents-storage.md) | 111 | audit-1054 |
| 1055 | [1055-durable-tasks-submissions-inbox-recovery.json](./1055-durable-tasks-submissions-inbox-recovery.json) | [1055-durable-tasks-submissions-inbox-recovery.md](./1055-durable-tasks-submissions-inbox-recovery.md) | 40 | audit-1055 |
| 1056 | [1056-durable-harness-workflows.json](./1056-durable-harness-workflows.json) | [1056-durable-harness-workflows.md](./1056-durable-harness-workflows.md) | 70 | audit-1056 |
| 1057 | [1057-chord-durable-dependency-subset.json](./1057-chord-durable-dependency-subset.json) | [1057-chord-durable-dependency-subset.md](./1057-chord-durable-dependency-subset.md) | 58 | audit-1057 |

Every one of the 1460 source rows keeps its identity: `sourceAliases` maps each `ticket:rowId` to
exactly one consolidated, package-qualified id, and every consolidated row lists the source rows it
absorbs. There is no catch-all row, no deferred bucket and no silent skip. The tool refuses to build
if the alias map is not a bijection over all source rows. Every included row carries an owner, its
current consumers (`consumer` plus `consumerOwners`), an evidence route to a registered scenario or
an honest missing observation, and an explicit `remainingWork` statement; excluded rows carry their
boundary instead.

The divergent lane schemas are mapped explicitly in `schemaMappings` rather than assumed uniform:
1047's `piSymbols`/`piTests` become the source pointer and the frozen-symbol list; the
`status.membership/implementation/verification` objects in 1049, 1051, 1052, 1053, 1055 and 1056
become the three top-level status fields; 1056's `upstreamEvidence.sources` becomes the source
pointer; 1056's `upstreamEvidence.blockers`, where present, stay in the row's gaps rather than being
folded into membership.

## 2. Ledger counts

| Dimension | Counts |
| --- | --- |
| Rows | 1460 |
| By lane | 1047:32, 1048:30, 1049:20, 1050:954, 1051:46, 1052:31, 1053:68, 1054:111, 1055:40, 1056:70, 1057:58 |
| By owner | cch_tui:925, cch_durable:212, cch_coding_agent:155, cch_chord:65, cch_codemode:51, cch_agent_core:29, cch_mcp:19, cch_ai:4 |
| Membership | supported:1432, excluded:26, no-decision:2 |
| Implementation | implemented:970, partial:228, missing:245, not-applicable:17 |
| Verification | unverified:1027, upstream-captured:408, not-applicable:25, verified:0 |
| Evidence route | captured:408, related-captured:28, declared-comparison:29, cpp-regression-only:12, unobserved:58, historical-v1.0.4:858, not-audited:42, excluded:25 |

The three dimensions stay separate. A supported row can be implemented and still unverified, and a
row with an upstream capture is not promoted because a similar row exists elsewhere: a declared
comparison scenario that resolves to a registered artifact for a row that does not itself record
`upstream-captured` is labelled `related-captured` and keeps a `missingObservation` statement. The
checker fails the ledger if any row claims `verified`, if an `upstream-captured` row has no
registered artifact, if an excluded row has no boundary, a placeholder boundary or an
`upstream-captured` status without a registered artifact, if an included row has no consumer text,
if an included row has no `remainingWork`, or if a supported non-implemented row has no gap, missing
observation or note.

Two rows keep a `no-decision` membership and are visible in `counts.noDecision`:
`cch_coding_agent:Session:entry:leaf-marker` and `cch_durable:Session:storage:sqlite-alternative`.
The 18 rows that previously had no consumer string now name either the current application seam or
an absent current consumer plus the proposed public caller; `consumerOwners` is no longer a
self-owned default for those rows.
They need an owner decision before their code is moved or retired; the frontier's P7 and P8 carry
that decision rather than letting the rows disappear.

## 3. Evidence routes and bundled scenarios

The twelve registered bundles ([baselines.json](../../../fixtures/pi-v1.3.0/baselines.json)) hold 60
artifacts and 373 scenarios. The consolidation covers the 370 scenarios of audit-1047 through
audit-1057 (audit-1046 is the bootstrap evidence-infrastructure bundle):

- 320 scenarios are named directly by at least one consolidated row's comparison list;
- 47 more scenarios are covered because their artifact is referenced by a row (the row records the
  artifact-level capture without listing every scenario name);
- 3 scenarios of `audit-1050/input-standalone.json` are registered as explicit
  `orphanScenarios` with a reason, related rows and a disposition instead of being silently linked
  to a different semantics;
- 0 scenarios are uncovered; the checker fails if any registered audit scenario has no route.

Route semantics enforced by the checker:

- `captured` requires a direct scenario name in a registered artifact (a declared artifact or a
  unique registered scenario match); an excluded-membership row may use it only when its source
  status is `upstream-captured` and its boundary states the no-C++-obligation;
- `artifact-only` (unused at this revision) is an artifact route without a scenario name;
- `related-captured` is a cross-reference only and keeps its own recorded status;
- `declared-comparison`, `cpp-regression-only`, `unobserved`, `historical-v1.0.4` and `not-audited`
  each require a concrete `missingObservation` statement;
- `excluded` rows must carry a boundary and may not carry a support route.

## 4. Provenance normalization

The `correctionsIndex` records 37 pointer-spelling corrections (33 replacements, one
symbol-suffix normalisation and 3 record-only items) with their authority and rationale. They
change pointer spellings only: no frozen observation payload, artifact byte, scenario name or
bundle is rewritten or rehashed. Corrections that resolved to a no-op stay in the index as
recorded items; corrections that were already satisfied do not rewrite a longer, already-qualified
pointer.

One consolidation defect was found and fixed during this work: the earlier substring replacement
doubled package prefixes on 16 pointers (for example
`packages/durable/src/harness/packages/durable/src/harness/submissions.ts:...`). The builder now
replaces the whole recorded pointer with the authoritative spelling, the checker rejects any
double-qualified package path, and the affected rows carry the corrected pointer. This changed only
the consolidated ledger's source spellings; the lane ledgers and their artifacts are untouched.

Separately from pointer corrections, the plan carries six explicit excluded-row boundary overrides
(session share/upload, both third-party TS/JS plugin rows, the settings runtime-observation row, and
the two 1050 platform exclusions). The settings row retains its source `upstream-captured`
verification and is linked to `fixtures/pi-v1.3.0/audit-1049/configuration-schema.json` scenario
`settings manager consumes values the published schema rejects`; membership stays excluded because
C++ must not reproduce the runtime validation bug. No source row, artifact byte or scenario name
changes.

## 5. Overlap resolutions

Similar names were merged only where the contract identity is the same; adjacent but different
contracts stay as separate rows with an explicit resolution:

| Overlap | Resolution |
| --- | --- |
| `overlap-toolkit-product` | The 1050 toolkit export contract belongs to cch_tui; the seven product-entry rows (`tuiMode`, `--tui-mode`, `runInteractiveMode`, `fullscreenExitOutput`, `fullscreenCopyOnSelect`, `fullscreenWheelScrollLines`, `showHardwareCursor`) are cch_coding_agent product behavior and are not-audited against v1.3.0, routed to the 1051 product differential. |
| `overlap-session-storage` | The application Session transcript (JSONL session files, cch_coding_agent) and the durable Session (transactional document/entry storage, cch_durable) are different runtimes. The legacy in-core SQLite SessionStore is the third, no-decision row; it is not the durable contract. |
| `overlap-tasks-harness` | Both lanes drive the same `Harness.open` entrypoint with different scenario slices: 1055 exercises task/submission scheduling, versioning, ownership and crash recovery, while 1056 exercises the workflow, registry, compaction, hooks and inspect/observability slices. Same owner (cch_durable); rows stay separate and cross-referenced, with no duplicate merge and no hidden coverage. |
| `overlap-codemode-chord` | The codemode rows name codemode execution and the chord rows name the durable-required Chord subset; the durable Harness consumes both, but neither is the other's contract. |
| `overlap-extension-lifecycle` | The application extension lifecycle rows (1049/1051) and the durable Harness lifecycle rows (1056) are two owners of one selected native-hook contract; the historical ADR 0066 restriction on extension hooks is superseded by ADR 0075 and is not a retained exclusion. |
| `overlap-mcp-application` | The reusable MCP protocol/client/transport rows (1052) stay in cch_mcp; the application config, management, resource and session-tool rows belong to cch_coding_agent. |

The eight capability groups (`capabilityGroups`) are disjoint and each member is a real row; the
six overlap resolutions and the three orphan scenarios are validated for known ids, non-empty
resolutions and registered scenario names.

## 6. Retained limitations (honest missing observations)

`retainedLimitations` records the ten limits this consolidation must not hide:

1. The application extension seam is a native-hook target; the captured 1051/1049 extension
   scenarios exercised the public extension factory with a minimal host stub, not a full Session.
2. The codemode capture drives the public codemode catalogue/factory path; it is not a full
   Session/TUI execution proof.
3. No C++ parity comparison ran for any lane: every upstream-captured row still needs one and no row
   is `verified`.
4. 1047 records symbol-level sources without C++ behavior comparisons for many rows.
5. The 1051 thinking-selector comparison used a five-level frozen fixture against the seven-level
   C++ thinkingLevelMap; the config mismatch must be fixed before any availability claim.
6. The 1055 crash recovery observes `SIGKILL` and reopen; power loss, fsync failure and
   multi-process durability are not proven.
7. The 1054 interchange rows prove pi-side reopen; neither C++ direction is verified.
8. Real emulator/PTY evidence is missing for image placement, footer frames, hardware cursor and
   full restoration rows.
9. The C++ model-selector assertion fails (17 passed / 1 failed) and has no artifact; no
   model-selector parity may be claimed before it passes.
10. Registration integrity (owner dependency declarations) is not C++ behavioral verification.

The excluded `settings-runtime-validation` row keeps its source `upstream-captured` verification:
the registered `fixtures/pi-v1.3.0/audit-1049/configuration-schema.json` scenario `settings manager
consumes values the published schema rejects` is an actual upstream observation (bogus
`defaultThinkingLevel`, non-array `packages`, `schemaCheck: false`). Membership remains excluded
because C++ must not reproduce the runtime validation bug; its boundary states both the captured
provenance and the no-C++-obligation. Six excluded rows carry explicit boundary text: session
share/upload and third-party TS/JS plugin execution replace the literal `None` boundaries, the 1056
third-party row additionally records the package-management exclusion, the settings row records
its captured observation and exclusion, and the two 1050 platform rows replace lowercase `none`
with their recorded platform-exclusion reasons.

The 26 excluded rows are the explicit no-claim boundaries and each carries its recorded boundary
in the ledger; 25 route to `excluded`, while the settings row routes its registered captured
observation to `captured` without changing its excluded membership: AI model-response timing,
cross-model thinking
transformation, final/partial tool-argument parsing, extra AI retry matching, classifier/
image-generation provider execution; the whole upstream Chord product beyond the durable-required
subset (plugin host, links/RPC, cluster, cells/schema, Node host); the Cloudflare storage backend,
the Cloudflare codemode Durable Object and `models.*` provider execution; direct third-party
TS/JS extension execution, session share/upload services, the upstream settings-runtime validation
bug and JSON print mode; the durable interchange history migration, the standalone SSH daemon and
Cloudflare deployment; MCP prompts/sampling; and the unconsumable toolkit helpers
(Marked/Token/Tokens, internal native modifiers, `isAppleTerminalSession`).

Separately, `closeOut.retainedBoundaries` keeps the boundaries that are not rows: Magpie-only model
discovery/authentication/inference, the XDG Pike state root, the installer/updater/Nix/npm and
installed-SDK claims, the API stability promise, and the no-historical-data-migration policy.

## 7. Target owner graph

The target graph ([architecture](../../agents/architecture.md), [pi parity](../../agents/pi-parity.md),
[ADR 0075](../../adr/0075-align-selected-owner-packages-and-linux-capabilities-with-pi-v1-3-0.md))
has nine owners and sixteen edges. The four new owners are introduced with real behavior in their
first slice, never as empty shells:

- `cch_chord` -> `cch_support`
- `cch_durable` -> `cch_chord`, `cch_codemode`, `cch_ai`, `cch_support`
- `cch_codemode` -> `cch_support`
- `cch_mcp` -> `cch_support`
- `cch_coding_agent` -> `cch_agent_core`, `cch_ai`, `cch_tui`, `cch_mcp`, `cch_codemode`, `cch_support`
- `cch_agent_core` -> `cch_ai`, `cch_support`
- `cch_tui` -> `cch_support`

Forbidden by the graph and checked: `cch_durable` -> `cch_agent_core`, `cch_durable` ->
`cch_coding_agent`, `cch_coding_agent` -> `cch_durable`, reusable owners -> application, and
`cch_chord` -> `cch_durable`. Every consolidated row's owner/consumer pair must be adjacent in this
graph in one direction.

## 8. Migration sequence

| Step | Kind | Drafts | Forwarding form | Removal slice |
| --- | --- | --- | --- | --- |
| M0 | integrate-verify | - | - | - |
| M1 | expand | P1, P4, P5, P6 | forwarding headers for moved sources | M2 |
| M2 | contract | P4, P5, P6 | - | - |
| M3 | expand | P2, P3 | chord document/delta and durable memory Session added beside the legacy path | - |
| M4 | migrate | P7 | temporary aliases in cch_agent_core | M6 |
| M5 | migrate | F2, F7 | durable stores added beside the legacy in-core SQLite path | M6 |
| M6 | contract | P7, P8 | - | - |
| M7 | integrate-verify | all behavior drafts | - | - |

Every temporary forwarding form names the slice that removes it (the checker rejects a forwarding
form without a removal slice, a removal that is not later, or a removal step that is not
`contract`), and no historical Pike durable format is imported, migrated or
dual-read. Placement is checked transitively against `blockingEdges`: every scheduled draft's
prerequisites must be scheduled no later, so M4's P7 precedes M5's F7; the check also runs a
negative probe that rejects the pre-correction F7-before-P7 order and a positive control on the
committed sequence.

## 9. Implementation frontier

32 drafts, ordered prefactor-first, each a demoable single-context ticket with at least two
property/case/failure-case acceptance scenarios, registered capture references, coverage prefixes
over real rows and an acyclic blocking-edge graph:

| Id | Owner | Kind | Title | Blocked by |
| --- | --- | --- | --- | --- |
| P1 | cch_chord | prefactor | Create the cch_chord value surface durable needs | - |
| P2 | cch_chord | prefactor | Add cch_chord document, delta and replicated-state tracking | P1 |
| P3 | cch_durable | prefactor | Create cch_durable with Storage, memory backend and Session commit | P1 |
| P4 | cch_codemode | prefactor | Extract cch_codemode from coding-agent | - |
| P5 | cch_mcp | prefactor | Extract cch_mcp from coding-agent | - |
| P6 | cch_coding_agent | prefactor | Move application session, history and compaction into cch_coding_agent | - |
| P7 | cch_durable | prefactor | Split the legacy SQLite/TaskStore coupling out of cch_agent_core | P3, P6 |
| P8 | cch_coding_agent | prefactor | Remove the application's second SessionStore writers | P6 |
| F1 | cch_durable | behavior | Run a durable Harness workflow through the public caller | P2, P3 |
| F2 | cch_durable | behavior | Implement the transactional JSONL durable store | P3 |
| F3 | cch_durable | behavior | Run versioned durable tasks with checkpoints, hooks and outcomes | P3 |
| F4 | cch_durable | behavior | Implement durable submissions, inbox admission and deduplication | F3 |
| F5 | cch_durable | behavior | Implement durable ownership, joins, abort and wait cancellation | F3 |
| F6 | cch_durable | behavior | Prove durable crash recovery with a real killed process | F3, F4 |
| F7 | cch_durable | behavior | Implement the durable SQLite store | P3, P7 |
| F8 | cch_durable | behavior | Prove bidirectional JSONL/SQLite interchange with frozen pi | F2, F7 |
| F9 | cch_durable | behavior | Implement durable documents, forks, reset/handoff and compaction | F1, P2 |
| F10 | cch_codemode | behavior | Align the codemode worker with shutdown-request versus actual-exit semantics | P4 |
| F11 | cch_codemode | behavior | Implement codemode Inline execution and deterministic replay | F10 |
| F12 | cch_codemode | behavior | Implement the generic Remote exchange and a Linux host | P4, F10 |
| F13 | cch_mcp | behavior | Fix MCP structured results, cancellation, close and transports | P5 |
| F14 | cch_coding_agent | behavior | Integrate MCP management, on-demand resources and session tools | F13, P6 |
| F15 | cch_agent_core | behavior | Carry monotonic tool duration end to end | P6 |
| F16 | cch_coding_agent | behavior | Restore recorded tool loadout and relative tool modifiers | P6 |
| F17 | cch_coding_agent | behavior | Implement native extension lifecycle and run triggering in the application Session | P6 |
| F18 | cch_durable | behavior | Wire durable Harness extension hooks and run triggering | F1, F17 |
| F19 | cch_tui | behavior | Align the TUI toolkit with v1.3.0 behavior | P6 |
| F20 | cch_coding_agent | behavior | Align the Native TUI product behavior with v1.3.0 | F19 |
| F21 | cch_coding_agent | behavior | Fix application configuration, prompt and print-mode deltas | P6 |
| F22 | cch_agent_core | behavior | Remove the historical Agent queue capacity divergence | P6 |
| F23 | cch_durable | behavior | Wire durable environments, images, codemode and tool-search | F1, P4 |
| F24 | cch_coding_agent | behavior | Reconcile and close the residual TUI tracker defects on refreshed evidence | F19, F20 |

The frontier preserves the specific corrections and limits from the lane merges: the 1051
thinking-map configuration mismatch (F24 compares with a matched fixture), the failing #808
model-selector assertion (F24 requires the post-Enter assertion to pass in a real PTY run), the
missing real emulator/PTY evidence (F20/F24 require dated emulator captures; OSC 7501 terminal
contract calls are not raw protocol acceptance), the supported resource/`file:` schema and queue
rules (F21/F22), the Magpie/XDG boundary (retained, not migrated), the Cloudflare/SSH/AI-provider/
third-party-JS exclusions (unchanged), the absent models capability gate (retained), the codemode
catalogue factory stub (F14 must add the Session/Interactive comparison), the bidirectional
JSONL/SQLite legs (F8) and the SIGKILL-only crash model (F6).

## 10. Residual tracker reconciliation

Open trackers are re-scoped to pi-v1.3.0 and reused, never duplicated:

| Issue | State | Action | Draft |
| --- | --- | --- | --- |
| #808 model selector marker/row layout | open | keep open, pass the failing PTY assertion | F24 |
| #810 thinking selector layout | open | keep open, compare with matched thinkingLevelMap | F24 |
| #812 tool-result presentation | open | keep open, re-check with persisted duration | F24 |
| #831 dock-growth footer frames | open | keep open, require real PTY frame capture | F24 |
| #1035 image placement-span repaint | open | keep open, require dated Kitty/WezTerm evidence | F24 |
| #1033 extension-editor dual-runtime capture | open | keep open, close the capture gap | F24 |
| #1032 generated system theme | open | keep open, re-target to v1.3.0 tokens | F24 |
| #1043 residual v1.0.4 deltas | open | re-scope the 72-column, scrollback, tool-result-timing and settings-viewport families | F24 |
| #946, #882, #915, #920, #925, #932 | closed | historical reference only; their alignment claims do not transfer to v1.3.0 | - |

## 11. Close-out

- Implemented today: the eleven lane audits, their bundles/artifacts and the 970 C++
  application/toolkit rows the ledgers record as implemented. That total includes 15 rows targeted
  at the new owners whose behavior currently lives under existing application owners: cch_durable
  3, cch_mcp 5, cch_codemode 7, cch_chord 0. A further 128 target rows are partial under those
  existing owners (durable 87, mcp 11, codemode 12, chord 18) and are pending ownership migration,
  not specified-only.
- Specified only: the four new Owner packages and manifest targets, the 194 missing target rows
  (durable 120, mcp 2, codemode 30, chord 42), the durable runtime and storage contracts,
  bidirectional interchange, native extension lifecycle, tool duration end-to-end, codemode
  Inline/Remote and the MCP cancellation/transport changes. The 228 partial rows are partial with
  recorded remaining work, not blanket specified-only. 0 rows are verified.
- Retained boundaries: Magpie discovery/authentication/inference; XDG Pike state root; all 26
  excluded surfaces; no historical durable data migration, automatic upgrade, dual read or fallback.
- Open blockers: umbrella #1044; the two no-decision membership rows; the honest missing
  observations (999 declared/unobserved/historical/not-audited plus 28 related-captured rows); real
  emulator/PTY evidence; and the SIGKILL-only crash model.

## 12. Validation

```sh
python3 fixtures/pi-v1.3.0/consolidation-1058/consolidate_audits.py build
python3 fixtures/pi-v1.3.0/consolidation-1058/consolidate_audits.py check
python3 fixtures/pi-v1.3.0/consolidation-1058/consolidate_audits.py check --pi-root /tmp/cch-1056-pi-frozen
```

`build` writes the ledger deterministically; `check` rebuilds it in memory, compares the committed
bytes and then validates source-row coverage and the alias bijection, the status vocabularies,
owner/consumer adjacency against the target graph (including the forbidden durable/application
edges), evidence routes against the registered bundle manifests, scenario coverage with no
orphaned registered scenario, the groups/overlaps/orphan registrations, the corrections index, the
frontier's draft graph (acyclic, dependency-ordered, real coverage prefixes, registered capture
scenarios), migration-step placement against the transitive prerequisite graph (including a
negative probe reproducing the old F7-before-P7 order and a positive control), the residual
reconciliation and the report's relative links. `--pi-root` additionally
verifies that every `packages/...` pointer resolves in the frozen pi checkout at the frozen
revision.

This tier is the docs/evidence tier of [validation](../../agents/validation.md): no C++ build is
required or run by these commands, and passing them is not behavioral verification of any row. The
existing bundle verifier and the focused regression selections remain the evidence-side gates; the
frontier's M7 names the full build and ctest gate for the implementation work.
