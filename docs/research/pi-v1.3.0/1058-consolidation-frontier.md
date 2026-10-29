# pi-v1.3.0 capability consolidation and implementation frontier (#1058)

Status: audit consolidation with subsequent delivery reconciliation for the #1044 umbrella,
at frozen revision `71023aec1546d6d677a65669c5665c0927ce0dd0`. The eleven source audit
ledgers remain the historical audit record. The generated ledger now combines those records
with explicitly validated C++ delivery evidence; an upstream capture alone never verifies a row.
The 32 drafts describe the original plan, not the live GitHub frontier of 76 implementation
slices. #1044 remains open.

Current reconciliation and the evidence contract are recorded in
[1058-delivery-reconciliation.md](./1058-delivery-reconciliation.md). No frozen observation
payload is rewritten.

Deliverables:

- [1058-consolidated-capability-ledger.json](./1058-consolidated-capability-ledger.json) - the
  stable ledger: 1464 consolidated rows, one alias per source row, evidence routes to real
  registered scenarios or an honest missing observation.
- [1058-deliveries.json](./1058-deliveries.json) - reviewed whole-row C++ comparisons,
  selected input digests and actual focused CTest JUnit results, applied after the audit.
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
| 1051 | [1051-native-tui-consumers-and-residuals.json](./1051-native-tui-consumers-and-residuals.json) | [1051-native-tui-consumers-and-residuals.md](./1051-native-tui-consumers-and-residuals.md) | 50 | audit-1051 |
| 1052 | [1052-mcp-audit-ledger.json](./1052-mcp-audit-ledger.json) | [1052-mcp-protocol-oauth-application.md](./1052-mcp-protocol-oauth-application.md) | 31 | audit-1052 |
| 1053 | [1053-codemode-audit-ledger.json](./1053-codemode-audit-ledger.json) | [1053-codemode-audit.md](./1053-codemode-audit.md) | 68 | audit-1053 |
| 1054 | [1054-durable-session-documents-storage.json](./1054-durable-session-documents-storage.json) | [1054-durable-session-documents-storage.md](./1054-durable-session-documents-storage.md) | 111 | audit-1054 |
| 1055 | [1055-durable-tasks-submissions-inbox-recovery.json](./1055-durable-tasks-submissions-inbox-recovery.json) | [1055-durable-tasks-submissions-inbox-recovery.md](./1055-durable-tasks-submissions-inbox-recovery.md) | 40 | audit-1055 |
| 1056 | [1056-durable-harness-workflows.json](./1056-durable-harness-workflows.json) | [1056-durable-harness-workflows.md](./1056-durable-harness-workflows.md) | 70 | audit-1056 |
| 1057 | [1057-chord-durable-dependency-subset.json](./1057-chord-durable-dependency-subset.json) | [1057-chord-durable-dependency-subset.md](./1057-chord-durable-dependency-subset.md) | 58 | audit-1057 |

Every one of the 1464 source rows keeps its identity: `sourceAliases` maps each `ticket:rowId` to
exactly one consolidated, package-qualified id, and every consolidated row lists the source rows it
absorbs. There is no catch-all row, no deferred bucket and no silent skip. The tool refuses to build
if the alias map is not a bijection over all source rows. Every included row carries an owner, its
current consumers (`consumer` plus `consumerOwners`), an evidence route to a registered scenario or
an honest missing observation, and an explicit `remainingWork` statement until verified;
excluded rows carry their boundary instead.

The divergent lane schemas are mapped explicitly in `schemaMappings` rather than assumed uniform:
1047's `piSymbols`/`piTests` become the source pointer and the frozen-symbol list; the
`status.membership/implementation/verification` objects in 1049, 1051, 1052, 1053, 1055 and 1056
become the three top-level status fields; 1056's `upstreamEvidence.sources` becomes the source
pointer; 1056's `upstreamEvidence.blockers`, where present, stay in the row's gaps rather than being
folded into membership.

## 2. Ledger counts

| Dimension | Counts |
| --- | --- |
| Rows | 1464 |
| By lane | 1047:32, 1048:30, 1049:20, 1050:954, 1051:50, 1052:31, 1053:68, 1054:111, 1055:40, 1056:70, 1057:58 |
| By owner | cch_tui:925, cch_durable:212, cch_coding_agent:159, cch_chord:65, cch_codemode:51, cch_agent_core:29, cch_mcp:19, cch_ai:4 |
| Membership | supported:1435, excluded:29, no-decision:0 |
| Implementation | implemented:1023, missing:213, not-applicable:18, partial:210 |
| Verification | not-applicable:28, unverified:1016, upstream-captured:413, verified:7 |
| Evidence route | captured:420, related-captured:23, declared-comparison:31, cpp-regression-only:10, unobserved:52, historical-v1.0.4:858, not-audited:42, excluded:28 |

The three dimensions stay separate. A supported row can be implemented and still unverified, and a
row with an upstream capture is not promoted because a similar row exists elsewhere: a declared
comparison scenario that resolves to a registered artifact for a row that does not itself record
`upstream-captured` is labelled `related-captured` and keeps a `missingObservation` statement. The
checker rejects `verified` without a complete validated delivery, an `upstream-captured` row
without a registered artifact, an excluded row without a concrete boundary, an included row
without consumer text, an unverified included row without `remainingWork`, or a supported
non-implemented row without a gap, missing observation or note. Source audit statuses remain
visible in `sourceRows`; a delivered row also keeps its previous consolidated `auditStatus`.

No row keeps a `no-decision` membership: `counts.noDecision` is empty. The two rows that were
`no-decision` are now `excluded` by the owner's #1059 ruling —
`cch_coding_agent:Session:entry:leaf-marker` (a non-pi extension: pi v1.3.0 keeps the active leaf in
runtime state only) and `cch_durable:Session:storage:sqlite-alternative` (the agent's SQLite session
store had no production caller and no pi v1.3.0 equivalent; its code was deleted in commit
`d0844505d`).
The 18 rows that previously had no consumer string now name either the current application seam or
an absent current consumer plus the proposed public caller; `consumerOwners` is no longer a
self-owned default for those rows.
They need an owner decision before their code is moved or retired; the frontier's P7 and P8 carry
that decision rather than letting the rows disappear. The owner's #1059 ruling has now supplied it,
so both rows are excluded and the membership blocker is closed.

The four rows added for the `/export`, `/import` and `/share` capabilities live in the 1051 lane
(`cch_coding_agent:export-jsonl-branch`, `cch_coding_agent:export-html`,
`cch_coding_agent:import-session` and `cch_coding_agent:excluded-session-share-command`). No
registered artifact scenario names the export output, the HTML document or the import flow, so all
three included rows are `unverified` with a `declared-comparison` evidence route: the C++ tests prove
the C++ contract, not pi parity.

## 3. Evidence routes and bundled scenarios

The 27 registered bundles ([baselines.json](../../../fixtures/pi-v1.3.0/baselines.json)) hold 81
artifacts and 480 scenarios. The consolidation covers all 477 non-bootstrap scenarios
(audit-1046 is the bootstrap evidence-infrastructure bundle):

- 347 scenarios are named directly by at least one consolidated row's comparison list;
- 55 more scenarios are covered because their artifact is referenced by a row (the row records the
  artifact-level capture without listing every scenario name);
- 75 scenarios are registered as explicit `orphanScenarios` with a reason, related rows and a
  disposition instead of being silently linked to different semantics. Their exact identities
  are listed in the generated ledger and include the later #1076 task observations;
- 0 scenarios are unrouted: no registered audit scenario is missing a route; the checker fails if any
  registered audit scenario is neither named, artifact-covered nor registered as an orphan.

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

The `correctionsIndex` records 37 pointer-spelling corrections (25 replacements and 12 record-only
items) with their authority and rationale. They change pointer spellings only: no frozen observation
payload, artifact byte, scenario name or bundle is rewritten or rehashed. A replacement is applied
only to the rows its `appliesTo` scope names, and only to a pointer that is exactly the recorded
spelling (a full pointer, or the same symbol list on a path-tail shorthand). An already-qualified
pointer, a different symbol list on the same path, and a row outside the scope keep the lane's
recorded provenance. `appliedCorrections` names only the corrections that actually rewrote one of
that row's pointers; the checker fails a correction that rewrote nothing without being record-only,
and fails a rewrite outside the correction's scope.

The record-only items are the ones with nothing to rewrite in the consolidated source list: #1053's
remote-type triple and worker-fixture label live only in the pinned artifact envelope, and its
`src/tools/output-files.ts` spelling is absent from every lane ledger and artifact, where the
qualified `src/utils/output-files.ts:writeOutputFile` pointer already appears; #1055's two C++
`TaskStore` spellings and #1056's `TaskStore.` path are lane `cppSeam` text the source list never
carried; and #1056's file-only `submissions.ts`/`inbox.ts`/`tool-search.ts` spellings are already
qualified in the lane ledger, where the rows observe `Submissions.status` and
`createToolSearchTool` alone as well as the wider exports. Rewriting those to the wider spelling
would have replaced an observed member or added an export the lane never recorded. #1052's
`validateIssuer` correction now names the function that performs the inline issuer check
(`authorizeMcp,runFlow`) instead of the error type it throws.

One consolidation defect was found and fixed during this work: the earlier substring replacement
doubled package prefixes on 16 pointers (for example
`packages/durable/src/harness/packages/durable/src/harness/submissions.ts:...`). The builder now
replaces only an exactly matching recorded pointer, the checker rejects any double-qualified
package path, and the affected rows carry the corrected pointer. This changed only the consolidated
ledger's source spellings; the lane ledgers and their artifacts are untouched.

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
| `overlap-toolkit-product` | The 1050 toolkit export contract belongs to cch_tui; the seven product-entry rows (`tuiMode`, `--tui-mode`, `runInteractiveMode`, `fullscreenExitOutput`, `fullscreenCopyOnSelect`, `fullscreenWheelScrollLines`, `showHardwareCursor`) are cch_coding_agent product behavior and stay `not-audited`/`unverified`. #1051 holds three adjacent capability rows in the same product area: `mode-switch-renderer`, `exit-restoration` and `hardware-cursor`. Only two product rows name a counterpart, `fullscreenExitOutput` -> `exit-restoration` and `showHardwareCursor` -> `hardware-cursor`, and a counterpart is adjacent, not equal: neither 1051 row observes the 1050 setting, and neither is verified. The other five, including `runInteractiveMode` (the adjacent switch scenario observes a renderer switch, not that assembly), have no 1051 counterpart for their own operation and no v1.3.0 observation. Every row keeps its visible missing-observation statement. |
| `overlap-session-storage` | The application Session transcript (JSONL session files, cch_coding_agent) and the durable Session (transactional document/entry storage, cch_durable) are different runtimes. The legacy in-core SQLite SessionStore is the third row and is now excluded: it is not the durable contract. |
| `overlap-tasks-harness` | Both lanes drive the same `Harness.open` entrypoint with different scenario slices: 1055 exercises task/submission scheduling, versioning, ownership and crash recovery, while 1056 exercises the workflow, registry, compaction, hooks and inspect/observability slices. Same owner (cch_durable); rows stay separate and cross-referenced, with no duplicate merge and no hidden coverage. |
| `overlap-codemode-chord` | The codemode rows name codemode execution and the chord rows name the durable-required Chord subset; the durable Harness consumes both, but neither is the other's contract. |
| `overlap-extension-lifecycle` | The application extension lifecycle rows (1049/1051) and the durable Harness lifecycle rows (1056) are two owners of one selected native-hook contract; the historical ADR 0066 restriction on extension hooks is superseded by ADR 0075 and is not a retained exclusion. |
| `overlap-mcp-application` | The reusable MCP protocol/client/transport rows (1052) stay in cch_mcp; the application config, management, resource and session-tool rows belong to cch_coding_agent. |

The eight capability groups (`capabilityGroups`) are disjoint and each member is a real row; the
six overlap resolutions and the registered orphan scenarios are validated for known ids, non-empty
resolutions and registered scenario names. The toolkit/product resolution additionally requires
every named counterpart to be a real `1051` row that is not verified, every routed product row to
stay `not-audited`/`unverified` with a missing-observation statement, and at least one product row
to stay visibly unmatched. It also ties the routed row back to its inherited `1050` lane gap, which
must state the missing observation and name its adjacent `#1051` row when it has one, so the
consolidation cannot report a product behavior as audited by a lane that never observed it.

## 6. Retained limitations (honest missing observations)

`retainedLimitations` preserves the ten limitations recorded at audit time. Later deliveries
are tracked separately: #1101 has a whole-row comparison receipt, the TUI residual tickets
are closed, and #1123/#1125 have actual PTY comparisons. Real emulator rendering and broad
runtime obligations remain unverified where their evidence is missing. The original audit
observations were:

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

The 29 excluded rows are the explicit no-claim boundaries and each carries its recorded boundary
in the ledger; 28 route to `excluded`, while the settings row routes its registered captured
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

### 6.1 Corrected source seams after the #1123 merge

The source ledgers recorded the OSC 7501 program-status capabilities as absent from C++ while
`#1123` had already implemented them. Fourteen consolidated rows carried a factually false
`cppSeam`/`cpp` seam, a `missing` or `partial` implementation status and a `none` consumer:

| Row | Was | Now |
| --- | --- | --- |
| `cch_tui:ProgramStatus` | `missing`, "no C++ program-status type" | `implemented` |
| `cch_tui:formatProgramStatus` | `missing`, "no OSC 7501 formatter" | `implemented` |
| `cch_tui:Terminal.setProgramStatus` | `missing`, "no set_program_status capability" | `implemented` |
| `cch_tui:ProcessTerminal.setProgramStatus` | `missing`, "no program-status state or write" | `implemented` |
| `cch_tui:ProcessTerminal.writeProgramStatus` | `missing`, "no pending-status write" | `implemented` |
| `cch_tui:ProcessTerminal.queryAndEnableKittyProtocol` | `missing`, "query path is missing" | `implemented` |
| `cch_tui:ProcessTerminal.setupStdinBuffer` | `missing`, "no reply interception" | `implemented` |
| `cch_tui:ProcessTerminal.start` | `partial`, "no program-status query" | `implemented` |
| `cch_tui:ProcessTerminal.stop` | `partial`, "no program-status clear" | `implemented` |
| `cch_tui:ProcessTerminal.handleKeyboardProtocolNegotiationSequence` | `partial`, "no pending sentinel" | `implemented` |
| `cch_tui:program-status:detection-override` | `missing`, "no OSC 7501 path" | `implemented` |
| `cch_tui:Terminal` (export) | `partial` | `implemented` |
| `cch_tui:ProcessTerminal` (export) | `partial` | `implemented` |
| `cch_coding_agent:program-status-reporting` | `missing`, "No C++ reporter" | `implemented` |

The two export rows are aggregates of their members, so they follow automatically once every
`Terminal` and `ProcessTerminal` member is `implemented`. The source of the correction is the
`#1050` ledger generator, `fixtures/pi-v1.3.0/audit-1050/build-ledger.py`; the generator and its
`validate-ledger.py` expectations were updated together, and the ledger was regenerated rather
than hand-edited.

The corrected seams are `cch_tui::ProgramStatus` with `format_program_status`,
`is_program_status_reply` and `kProgramStatusQuery`; the virtual
`cch_tui::Terminal::set_program_status` with its `ProcessTerminal` and `VirtualTerminal`
implementations; and `src/coding_agent/tui/ProgramStatusReporter.{hpp,cpp}` as the product caller.
`ProcessTerminal` re-sends the cached status when OSC 7501 support is confirmed and after a
restart, and writes the clear status during `stop()`, which is pi's private
`ProcessTerminal.writeProgramStatus` (`packages/tui/src/terminal.ts:581`, called from `:243` and
`:299`). The reporter is a port of pi's `program-status-reporter.ts` state machine: the same event
transitions, the same blocked-map "most recent dialog wins" rule, the same `firstLine` error text
and the same dedup.

The comparison against the frozen captures is recorded in the C++ tests, not in this pipeline:
`tests/tui/ProgramStatusTest.cpp` asserts the captured `program-status-format.json` wire bytes
verbatim, `tests/tui/ProcessTerminalProgramStatusTest.cpp` replays the
`program-status-detection.json` startup order, DA sentinel, both `PI_PROGRAM_STATUS` overrides and
the `stop()` clear on a real pseudo-terminal, and
`tests/coding_agent/tui/ProgramStatusReporterTest.cpp` and
`ProgramStatusEvidenceTest.cpp` replay the `audit-1051/program-status.json` state sequence.

The fourteen program-status rows retain `upstream-captured`: their accepted comparisons are
recorded in tests and the ticket, but no complete per-row delivery receipt has been added here.
#1101 is the first whole-row receipt in `1058-deliveries.json`. Verification now requires a
validated receipt rather than being prohibited for every row.

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
| P1 | cch_chord | prefactor | Create the cch_chord value surface durable needs (#1060, delivered) | - |
| P2 | cch_chord | prefactor | Add cch_chord document, delta and replicated-state tracking | P1 |
| P3 | cch_durable | prefactor | Create cch_durable with Storage, memory backend and Session commit | P1 |
| P4 | cch_codemode | prefactor | Extract cch_codemode from coding-agent (#1064, delivered) | - |
| P5 | cch_mcp | prefactor | Extract cch_mcp from coding-agent (#1065, delivered) | - |
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
missing real emulator/PTY evidence (F20/F24 require dated emulator captures; OSC 7501 now has its own
pseudo-terminal evidence from #1123 — the frozen startup order, the DA sentinel, both `PI_PROGRAM_STATUS`
overrides and the `stop()` clear — while real emulator evidence is still missing for
image/footer/cursor/restoration), the supported resource/`file:` schema and queue
rules (F21/F22), the Magpie/XDG boundary (retained, not migrated), the Cloudflare/SSH/AI-provider/
third-party-JS exclusions (unchanged), the absent models capability gate (retained), the codemode
catalogue factory stub (F14 must add the Session/Interactive comparison), the bidirectional
JSONL/SQLite legs (F8) and the SIGKILL-only crash model (F6).

## 10. Residual tracker reconciliation

Six refreshed TUI residual tickets (#808, #810, #812, #1033, #1032 and #1043)
remain closed. #831 and #1035 are reopened for the dated terminal evidence their
acceptance criteria require; their delivered byte/frame comparisons remain valid.
The reviewed dispositions are in `residualReconciliation` and
[the 2026-10-26 reconciliation](1044-delivery-reconciliation-2026-10-26.md).
Ticket closure does not promote capability rows or waive remaining comparison obligations.

#946, #882, #915, #920, #925 and #932 remain historical references; their alignment
claims do not transfer to v1.3.0.

## 11. Close-out

The configured graph contains real Chord, codemode, MCP and durable owners.
Durable has memory/JSONL/SQLite storage and basic task/checkpoint execution;
codemode has Inline, deterministic local execution and a Remote Linux host.
These packages are no longer specified-only shells. The current frontier's
`closeOut` records delivered slices and remaining work separately.

[#1071](1071-legacy-retirement.md) reconciles the retired legacy SQLite/task
subsystem. [#1101](1101-structured-content.md) supplies the first whole-row
comparison receipt. Other delivered slices retain their reports/tests until
complete per-row receipts accept their broader capability obligations.

The source audit ledgers are not live completion estimates. The generated ledger
above and its `counts` are authoritative for recorded status; 1019 rows still have
missing observations, including 858 historical-only TUI routes. Session ownership
#1068–#1070, live consumers #1072 and application environment/tools #1073 are delivered.
Full durable workflows beyond #1079, bidirectional continuation, remaining
Agent/application/MCP behavior and actual emulator evidence keep #1044 open.
#1095 is reopened because its application tool-search evidence does not accept the
Durable caller; #1093 retains its unaccepted outcome/settlement ordering clause.
Magpie/XDG, the 29 explicit exclusions and the no-old-Pike-migration boundary
remain unchanged.

## 12. Validation

```sh
python3 fixtures/pi-v1.3.0/consolidation-1058/consolidate_audits.py build
python3 fixtures/pi-v1.3.0/consolidation-1058/consolidate_audits.py check
python3 fixtures/pi-v1.3.0/consolidation-1058/consolidate_audits.py check --pi-root /tmp/cch-1056-pi-frozen
```

`build` writes the ledger deterministically; `check` rebuilds it in memory, compares the committed
bytes and then validates source-row coverage and the alias bijection, the status vocabularies,
owner/consumer adjacency against the target graph (including the forbidden durable/application
edges), evidence routes against the registered bundle manifests, scenario coverage with no unrouted
registered scenario and the registered orphans kept explicit, the groups/overlaps/orphan
registrations including the toolkit/product routing (only real 1051 counterparts, no promotion, and
the inherited 1050 lane gap kept truthful), the corrections index (every replacement rewrote a
scoped pointer, no rewrite outside its scope, no unused non-record-only item), the
frontier's draft graph (acyclic, dependency-ordered, real coverage prefixes, registered capture
scenarios), migration-step placement against the transitive prerequisite graph (including a
negative probe reproducing the old F7-before-P7 order and a positive control), the residual
reconciliation and the report's relative links. `--pi-root` additionally
verifies that every `packages/...` pointer resolves in the frozen pi checkout at the frozen
revision.

Both the consolidation check and the boundary regressions in
`tests/evidence/ConsolidationAuditTest.py` are CTest entries beside the named-evidence gate; the
regressions select the consolidation labels through the normal `ctest -L` surface.

This tier is the docs/evidence tier of [validation](../../agents/validation.md): no C++ build is
required or run by these commands, and passing them is not behavioral verification of any row. The
existing bundle verifier and the focused regression selections remain the evidence-side gates; the
frontier's M7 names the full build and ctest gate for the implementation work.
