# #1177 application comparison reconciliation

## Scope

Reconcile all 152 supported `cch_coding_agent` capability rows with current delivery evidence and
name a bounded follow-up for every row that has no whole-row comparison receipt. Authority:
[#1044](https://github.com/lanshengzhi/cpp-coding-harness/issues/1044),
[#1183](https://github.com/lanshengzhi/cpp-coding-harness/issues/1183),
[ADR 0075](../../adr/0075-align-selected-owner-packages-and-linux-capabilities-with-pi-v1-3-0.md),
Named Baseline **pi-v1.3.0** at `71023aec1546d6d677a65669c5665c0927ce0dd0`, Linux x86-64 with glibc.

Bounded verification scope: session/history/resume/compaction, resources/configuration/native
extensions/print mode, MCP and codemode integrations, plus application-owned terminal consumers.
The packet reuses #1048/#1049/#1051, #1067-#1073, #1104-#1119 and the existing Native TUI
consumers. It implements no neighbouring feature and changes no capability membership. The 152-row
count is a reconciliation inventory, not a completion percentage.

Receipt registration is serial and owned by the operator. This packet produces the comparison
evidence and the routing only: no entry in [1058-deliveries.json](1058-deliveries.json),
[baselines.json](../../../fixtures/pi-v1.3.0/baselines.json),
[consolidation-plan.json](../../../fixtures/pi-v1.3.0/consolidation-1058/consolidation-plan.json) or
the [1058 consolidated ledger](1058-consolidated-capability-ledger.json) is written here, so no
existing receipt needs revalidation.

## Reconciliation method

1. Read every supported `cch_coding_agent` row in the
   [1058 consolidated ledger](1058-consolidated-capability-ledger.json): its recorded
   `comparisonScenarios`, the pinned `evidence.artifact`, the recorded `implementation` /
   `verification` status and its `remainingWork`. The 2026-10-19 source-audit facts in
   [1047](1047-capability-ledger.json)–[1053](1053-codemode-audit-ledger.json) are history and were
   not treated as current code observations.
2. Locate the delivered public C++ seam for each row and the cases that claim the row's scenario.
   A case counts only if it reaches the row through the highest appropriate public caller — the
   application `Session`, the resource/settings loaders, `McpSessionManager`, the codemode
   catalog/tool, the Native TUI engine or the CLI — not through an owner-internal helper.
3. Classify what the case actually observes:

   | Verdict | Meaning |
   | --- | --- |
   | **R** | A delivered case opens the row's pinned `pi-v1.3.0` artifact, selects the recorded scenario, mirrors its `inputs` and compares its `expected`; a discriminator rejects a plausible shallow implementation. |
   | **r** | A case compares behaviour, but against a different capture (another family or another scenario), not the row's pinned artifact. |
   | **L** | A case exercises the scenario shape, but every expectation is a literal in the test; the artifact is never opened. |
   | **N** | No case covers the recorded scenario. |
   | **H** | The only recorded observation is the historical `pi-v1.0.4` bundle, which is not an observation of the named baseline. |

4. A row is receipt-complete only when **all** of the following hold: every recorded comparison
   scenario is read from the row's pinned artifact by a passing case; a discriminating case exists;
   and `remainingBoundary` can be empty, i.e. no recorded implementation gap falls inside the row's
   own claim. Rows that fail any condition keep their recorded status and are routed to one of the
   explicit batches P1–P8 below.

Verdict counts over the 152 rows: **R 10**, **r 8**, **L 90**, **N 14**, **H 30**. Ten rows are
accounted for by existing receipts (6) or by this packet's matched cases (4); the remaining 142 are
the P1–P8 routing targets. The per-row verdict is in the appendix.

## Matched rows delivered in this packet (4)

Four rows pass all three conditions without touching a test: the delivered cases already read the
pinned artifact and already separate the shallow implementation. This packet adds no test code, no
production code and no receipt.

| Row | Frozen artifact | Scenario | Case | Discriminating case |
| --- | --- | --- | --- | --- |
| 62 `codemode-application-inline-budget` | `audit-1053/application-catalog.json` | namespaces group sections and the inline budget omits tools with an explicit marker | `namespaces group sections and the inline budget omits tools with an explicit marker` | `an inline budget omits a namespace's tools with pi's marker` |
| 71 `codemode-mcp-refresh` | `audit-1053/application-catalog.json` | the empty codemode catalog still declares globals, discovery and the options grammar; namespaces group sections and the inline budget omits tools with an explicit marker | `the empty codemode catalog matches the frozen pi-v1.3.0 description`; `namespaces group sections and the inline budget omits tools with an explicit marker` | `a real session's next request lists MCP tools registered late and executes them` |
| 73 `codemode-settings` | `audit-1053/application-catalog.json` | the on loadout lists only non-direct callable tools and adds script-call descriptions; namespaces group sections and the inline budget omits tools with an explicit marker | `the on loadout lists only non-direct callable tools and adds script-call descriptions`; `namespaces group sections and the inline budget omits tools with an explicit marker` | `a settings document the schema accepts reshapes the request and reload re-reads it` |
| 146 `theme-token-set` | `audit-1049/configuration-schema.json` | theme schema accepts the 56-token built-in shape | `the theme token set matches the frozen pi-v1.3.0 capture` | the same case: `kThemeTokenCount` / `kRequiredThemeTokenCount` / fallback set versus the frozen descriptors |

### 62 — application inline budget

The artifact scenario records `inputs.tools = [read, mcp__docs__search]`,
`inputs.namespace = docs`, `inputs.inlineBudget = 40`. The case
`namespaces group sections and the inline budget omits tools with an explicit marker`
(`tests/coding_agent/CodemodeCatalogTest.cpp`) rebuilds exactly those inputs, compares the whole
rendered description with `expected`, and then separates the shallow implementation: `read` must
stay listed, the namespace must appear as `## docs (tools not listed)` with its description, and
`mcp__docs__search` must not be listed. The session-level discriminator
`an inline budget omits a namespace's tools with pi's marker` reaches the same rule through a real
`Session` with `settings.json` `{"codemode":{"inlineBudget":1}}`, so a catalog that ignores the
setting fails even if the pure function is right.

The ledger records `implementation: missing` for this row. That is the 2026-10-19 audit fact, not a
current observation: `CodemodeCatalogSettings::inline_budget` is consumed by the delivered catalog
and by `prepare_codemode_loadout` (the default-3000 case and the session case above both pass). The
row's implementation is delivered; the stale `missing` status is left for the registration step
rather than rewritten here.

### 71 — MCP refresh

Both recorded scenarios are artifact reads: the empty-catalog case compares
`create_codemode_description({}, {})` and `codemode_tool_definition().description` with the frozen
empty text, and the namespace/inline-budget case is the row 62 comparison above. The
discriminating case is the late registration path: the fixture server offers a second tool only
after a `tools/call` and emits `notifications/tools/list_changed`; the next real request must list
`mcp__echo__late`, the earlier request must not have contained it, and the script must actually call
it through the nested-call seam. A catalog that refreshes only on reconnect, or that re-renders the
same text, fails. `disabling a server and changing its exposure reshape the next request's catalog`
adds the removal and the direct-exposure reshape.

### 73 — application settings

The `on` loadout scenario is compared field by field against the artifact's `expected` object —
`hiddenDeclarations` (empty), `readDescription`, `searchDescription` and `codemodeDescription` — with
the `read`/`mcp__docs__search` inputs mirrored from the recorded `tools` strings. The second
scenario is the namespace/inline-budget comparison. The discriminating case
`a settings document the schema accepts reshapes the request and reload re-reads it` starts from
`{"codemode":{"mode":"on"}}`, rewrites the same scope to `mode: only`, reloads through the public
`Session::reload()` and requires the direct `read` declaration to leave the request and the catalog
to list `read` instead. A loader that applies the document only at construction, or a catalog that
never re-renders, fails.

### 146 — theme token set, and the field boundary this row does not claim

The case `the theme token set matches the frozen pi-v1.3.0 capture`
(`tests/coding_agent/tui/ThemeTest.cpp`) opens `audit-1049/configuration-schema.json`, selects
`theme schema accepts the 56-token built-in shape` and compares the token inventory:
`kThemeTokenCount` (56), `kRequiredThemeTokenCount` (51) and the sorted fallback-token names from
`all_theme_tokens()` / `theme_token_fallback()` against the frozen `optionalTokens`
(`scrollbarTrack`, `scrollbarThumb`, `searchMatchBg`, `searchMatchText`, `thinkingMax`).

That scenario's `expected` object has seven fields. **Three are the token inventory** —
`tokenCount`, `requiredTokenCount`, `optionalTokens` — and all three are compared, which pins the
inventory exactly (a 56-token set, 51 required, that fallback set). **The other four —
`schemaAccepted`, `runtimeAccepted`, `runtimeName`, `runtimeColorCount` — are pi's own runtime
observations recorded in the same scenario. They belong to the theme-schema/loading claim
(row 144), not to this row, and the case does not compare or claim them.** The row's own claim is
therefore fully compared; reading this receipt as covering schema acceptance or runtime loading
would over-claim.

## Candidates rejected in this packet (3)

| Row | Why it does not meet the receipt condition | Batch |
| --- | --- | --- |
| 59 `codemode-application-direct-exposure` | 3 recorded scenarios, 2 read. `the callable tool set excludes codemode itself` compares against a literal `{"read","mcp__docs__search"}` instead of the artifact's `expected` array, so condition 1 fails. | P6 |
| 101 `mcp-tool-result-conversion` | Both recorded scenarios are read, but the row's implementation is partial: the resource-link and binary-placeholder conversions are recorded gaps. A receipt would need `remainingBoundary: []`, which would over-claim beyond the two scenarios. | P5 |
| 144 `theme-schema` | 3 recorded scenarios, 1 read — and that one is the token-inventory scenario compared by row 146. The reject-unknown-token and runtime-accepts cases are literals, so condition 1 fails. | P3 |

## Verified receipts that are not artifact reads

The scan below walks every receipt row whose `remainingBoundary` is empty and checks whether the
cited test opens the pinned artifact (directly or through a quoted include). It is a record, not a
reopening: the receipt contract itself requires `artifact` + `scenario` + `test`, and all of these
receipts satisfy it; what they do not do is observe the artifact's own fields.

### In this packet's scope

| Receipt | Rows | What the cited case actually reads | Class |
| --- | --- | --- | --- |
| `1104-management-during-connection` | 95 `mcp-external-signin-pickup`, 96 `mcp-management-ui` | nothing from `audit-1104/management-during-connection.json`. `McpSessionManagerTest.cpp` (`on_turn_start reconnects a server signed in through another process`) and `McpManagerFlowTest.cpp` (`/mcp panel stays live while a server connect is pending and subcommands await the settle`) assert literals. | comparison-level |
| `1118-codemode-catalog` | 63 `codemode-application-namespace`, 68 `codemode-description-catalog`, 69 `codemode-description-loadout` | `application-catalog.json` **is** read by the comparison cases (`CodemodeCatalogTest.cpp`). The discriminating cases (`CodemodeCatalogSessionTest.cpp`: late registration, only-loadout, discovery globals) assert literals. | discriminator-level only |
| `1118-codemode-catalog` | 72 `codemode-models-description-gate` | Both the comparison and the discriminating case are the artifact-reading case. | not affected |

So the two known receipts behave differently: #1104 has no artifact read at all in either row,
while #1118's comparison reads are sound and only the separation case is unanchored. A future
strengthening of #1118 would compare the session-visible description against the artifact's
`expected` instead of `contains()` checks; that is a bounded follow-up, not a status change.

### Adjacent receipts found by the same scan (outside this packet)

Recorded for the owners of those receipts; not reopened here.

| Receipt | Rows | Literal comparisons (no artifact read) |
| --- | --- | --- |
| `1179-codemode-comparisons` | `codemode-source-timeout-option` | 1 of 3: `an over-limit script is interrupted with a timeout error` hard-codes `Execution timed out after 150 ms` instead of reading `audit-1053/script-execution.json` |
| `1179-codemode-comparisons` | `codemode-worker-admission`, `codemode-worker-repeated-cancellation`, `codemode-worker-construction-failure` | all: the worker cases in `CodemodeOwnerTest.cpp` assert literals; `audit-1053/worker-lifetime.json` is never opened |
| `1181-mcp-comparisons` | `mcp-resources-list-read` | all 3: `McpSessionIntegrationTest.cpp` reads only the `fixtures/pi-mcp/**` runtime fixtures, not `audit-1052/application-initialization.json` |
| `1181-mcp-comparisons` | `mcp-oauth-discovery`, `mcp-oauth-flow-cancellation`, `mcp-oauth-registration-signin` | all: `McpOAuthSessionTest.cpp` reads only `fixtures/pi-mcp/oauth/replay.json`, not `audit-1052/oauth-lifecycle.json` |
| `1189-close-refresh` | `mcp-client-lifecycle`, `mcp-close-no-refresh`, `mcp-oauth-refresh`, `mcp-refresh-cancellation` | all: same two artifacts, same two test files |
| `1090-durable-images` | `image-processor`, `read-image` | 1 each: the comparison cites `audit-1056/environment.json` while the test reads only `audit-1090/images.json` through `tests/durable/ImageExpectation.hpp` |

## Remaining routing — P1–P8

Every remaining row is assigned to exactly one batch with an owner, a public caller, its
row/scenario IDs, a normal and a discriminating case, and a dependency. No row is left to
[#1126](https://github.com/lanshengzhi/cpp-coding-harness/issues/1126) and no row moves to Deferred.
Batch P8 is already owned by
[#1201](https://github.com/lanshengzhi/cpp-coding-harness/issues/1201); P1–P7 are explicit batches of
this packet.

| Batch | Owner | Public caller | Rows | Artifacts | Normal case | Discriminating case | Dependency |
| --- | --- | --- | ---: | --- | --- | --- | --- |
| P1 Session tree/history/resume | `cch_coding_agent` | `Session`, `SessionStore`, `SessionResume` on the live JSONL store | 10 | `audit-1048/session-tree-and-fork.json`, `audit-1048/session-tool-restore.json` | build a tree with fork/resume, then compare entry kinds, ids, parent chain and projected context | a resume that re-reads only the active leaf, or a fork that copies the file instead of re-chaining, must fail | #1048 family; 3 rows are still `missing` |
| P2 Session lifecycle/publication | `cch_coding_agent` | `AgentSession`, `SessionFactory`, `SessionLifecycle`, `TranscriptExport`, `SessionImport` | 21 | `audit-1047/queue-admission.json`, `audit-1047/tool-execution.json`, `audit-1048/session-compaction-boundary.json`; 16 rows have no artifact | drive prompt/abort/close/compaction/reload through a real `Session` and compare settlement order and the persisted wire document | a close that drops a pending commitment, or a compaction that leaves the pre-compaction tree visible, must fail | #1047/#1048; 2 `missing` rows need an implementation or a recorded divergence; 16 rows need an independent capture |
| P3 resources/configuration/prompts/extensions | `cch_coding_agent` | `ProjectResourceLoader`, `SettingsManager`, `KeybindingsManager`, `PromptTemplateExpander`, `ExtensionHost`, theme loading | 21 | `audit-1049/*` (`configuration-schema`, `resource-selection`, `prompt-expansion`, `extension-run`), `audit-1051/*` | load a resource set, settings document, prompt template or extension set and compare the frozen expectation | a loader that merges instead of shadowing a worktree file, or a reload that keeps the stale value, must fail | #1049/#1114/#1115; rows 144 and 146 share the theme artifact |
| P4 print mode | `cch_coding_agent` | `pike --print` one-shot CLI | 2 | `audit-1049/print-mode.json` | run the one-shot CLI with text and with over-limit output and compare stdout, stderr and exit code | a run that writes the truncation warning to stdout, or returns 0 on truncation, must fail | #1117 delivered; the current `CliSmokeTest` literals must become artifact reads |
| P5 MCP application surface | `cch_coding_agent` | `McpSessionManager`, `McpProductionAdapters`, the `/mcp` panel | 12 | `audit-1052/application-initialization.json`, `audit-1052/transports.json`, `audit-1052/structured-content.json`, `audit-1104/management-during-connection.json`, `audit-1056/codemode-toolsearch.json`; 4 rows have no artifact | connect, list tools/resources, manage a server and convert results through the real session | a management action that waits for the connection to settle, or a resource read that reconnects, must fail | #1104/#1105/#1181/#1189; row 101 stays blocked on the resource-link/binary-placeholder gap |
| P6 codemode application | `cch_coding_agent` | codemode catalog, `CodemodeTool`, codemode execution through `AgentSession` | 18 | `audit-1053/application-catalog.json`, `audit-1053/script-execution.json` | render the catalog/loadout and execute a script through a real session | late MCP registration, a settings reshape or an inline budget that omits only the tool that did not fit | #1118/#1119 plus this packet's rows 62/71/73; row 59 stays open |
| P7 Native TUI consumers | `cch_coding_agent` | Native TUI engine (`InteractiveMode`) and the tool/status renderers | 38 | `audit-1051/product-differential.json`, `audit-1051/extension-editor.json`, `audit-1051/program-status.json`, `audit-1047/tool-execution.json`; 14 rows have no artifact | drive the TUI seam and compare the frozen product differential frame by frame | a renderer that formats from C++ defaults instead of the resolved theme/settings must fail | #1051/#1182; the 14 rows without an artifact need a capture or an explicit divergence |
| P8 historical terminal primitives | `cch_coding_agent` | `EditorComponent`, `NativeClipboard`, `RgbColor`, `TerminalColors`, `parseTerminalColorSchemeReport`, `setImageTranscoder` | 30 | historical `pi-v1.0.4` bundle only | recapture each primitive from `71023aec` and compare it | the v1.0.4-versus-v1.3.0 delta must be recorded, not normalised away | [#1201](https://github.com/lanshengzhi/cpp-coding-harness/issues/1201) |

### P1 — Session tree, history and resume (10 rows)

Rows 29, 33, 34, 36, 37, 39, 40, 41, 45, 47. Artifacts:
`audit-1048/session-tree-and-fork.json` (7 rows) and `audit-1048/session-tool-restore.json` (3 rows).
The delivered tree/fork/resume slices exist, but the cases assert literals or compare against the
live store instead of the frozen entry documents. The normal case must compare the recorded entry
kinds, ids, parent links, labels and the projected context; the discriminating case must reject a
resume that restores only the leaf and a fork that copies instead of re-chaining. Rows 29
(`compaction:in-memory`), 33 (`context-edit`) and 45 (`wire:baseline-alignment`) are recorded
`missing` and need an implementation decision before a comparison.

### P2 — Session lifecycle, publication and storage (21 rows)

Rows 1–4, 27, 28, 30–32, 35, 38, 42–44, 46, 48–50, 79, 80, 87. Sixteen rows have no artifact at
all — their recorded observation is a C++ test name or a declared comparison — so they need an
independent capture from `71023aec` or an explicit divergence before any receipt. The three
compaction-boundary rows and the queue-admission/tool-telemetry rows have artifacts. The normal
case drives a real `Session` through prompt/abort/close/compaction/reload and compares the
settlement order and the persisted wire document; the discriminating case rejects a close that
drops a pending commitment, a compaction that leaves the pre-compaction tree visible, and an export
that re-reads the file instead of the live store.

### P3 — Resources, configuration, prompts, extensions and themes (21 rows)

Rows 76, 77, 82–85, 88, 89, 117, 119–122, 132, 133, 135, 136, 141, 144–146. Artifacts:
`audit-1049/configuration-schema.json` (6), `audit-1049/resource-selection.json` (5),
`audit-1049/prompt-expansion.json` (3), `audit-1051/product-differential.json` (2),
`audit-1049/extension-run.json`, `audit-1051/program-status.json`,
`audit-1051/extension-editor.json`; 2 rows have no artifact. Row 146 is matched here; row 144 fails
condition 1 because only the token-inventory scenario is read. The normal case loads a resource
set, settings document, prompt template or extension set and compares the frozen expectation; the
discriminating case rejects a loader that merges instead of shadowing a worktree file, a reload that
keeps the stale value, and a theme schema that accepts a token the runtime rejects.

### P4 — print mode (2 rows)

Rows 107, 108. Artifact `audit-1049/print-mode.json`. The delivered one-shot CLI is compared through
`CliSmokeTest`, which hard-codes stdout/stderr/exit instead of reading the artifact. The normal case
runs the CLI with in-limit and over-limit output; the discriminating case requires the truncation
warning on stderr, the exact exit code and the `--print` text form, so a run that only prints the
first line fails.

### P5 — MCP application surface (12 rows)

Rows 90–101. Artifacts: `audit-1052/application-initialization.json` (3),
`audit-1104/management-during-connection.json` (2), `audit-1052/transports.json`,
`audit-1052/structured-content.json`, `audit-1056/codemode-toolsearch.json`; 4 rows have no artifact.
Rows 95 and 96 are the #1104 receipt rows whose comparison never opens the artifact (see above);
row 101 is implemented partially — the resource-link and binary-placeholder conversions are outside
its two scenarios, so a whole-row receipt would over-claim. The normal case connects, lists
tools/resources, manages a server and converts a result through the real session; the
discriminating case rejects a management action that waits for the connection to settle and a
resource read that reconnects the server.

### P6 — codemode application (18 rows)

Rows 58–75. Artifacts: `audit-1053/application-catalog.json` (12) and
`audit-1053/script-execution.json` (6). Rows 62, 71 and 73 are matched here; 63, 68, 69 and 72 are
the existing #1118 receipt rows; row 59 stays open because its callable-set comparison is a literal.
The remaining rows are the execution-side ones (`error-summary`, `image-output`, `nested-calls`,
`result`, `store-entry`, `tool-binding`) whose cases read a related capture but not the recorded
scenario, plus `application-deferred`, `application-tool-guidelines`, `callable-tools` and
`extension-registration` on the catalog side. The normal case renders
the catalog/loadout and executes a script through a real session; the discriminating case is the
late MCP registration, the settings reshape and the inline-budget omission.

### P7 — Native TUI consumers (38 rows)

Rows 51, 57, 78, 81, 102–105, 109–116, 118, 123–131, 137–140, 142, 143, 147–152. Artifacts:
`audit-1051/product-differential.json` (15), `audit-1051/extension-editor.json` (4),
`audit-1051/program-status.json` (3), `audit-1047/tool-execution.json` (2); 14 rows have no artifact.
The normal case drives the Native TUI seam and compares the frozen product differential; the
discriminating case rejects a renderer that formats from C++ defaults instead of the resolved
theme/settings, and a viewport that keeps stale scrollback. The 14 rows with no artifact — the
seven `product.*` toggles, `system-theme`, `residual-1032-system-theme`, `clipboard-paste`,
`osc52-opt-in`, `startup-project-trust`, `tool-bash-presentation` and `tool-read-line-range` — need
an independent capture or an explicit product divergence.

### P8 — historical terminal primitives (30 rows)

Rows 5–26, 52–56, 86, 106, 134, all `implemented/unverified` against the historical **pi-v1.0.4**
bundle. That bundle is not an observation of the named baseline, so no v1.3.0 receipt can be built
from it. Owned by [#1201](https://github.com/lanshengzhi/cpp-coding-harness/issues/1201): recapture
each primitive from `71023aec` through the public frozen seam, compare it, and record the
v1.0.4-versus-v1.3.0 delta instead of normalising it away. This packet contributes the row list and
the caller map only.

## Validation

The four matched rows were validated on the working tree at `88b997760` plus the local build
described below. No pinned input changed, so no existing receipt was revalidated.

| Command | Result |
| --- | --- |
| `cmake --build --preset vcpkg` | built, 405/405 edges; build-phase Parity Architecture Gate PASS at configure |
| `ctest --preset vcpkg -N -R '<9 exact case names>'` | 9 selected |
| `ctest --preset vcpkg -R '<9 exact case names>' --output-on-failure --output-junit "$PWD/docs/research/pi-v1.3.0/deliveries/1177-app-subset.xml"` | 9 passed, 0 failed, 0 skipped |
| artifact-removal probe | with `audit-1053/application-catalog.json` moved aside, the four catalog comparison cases fail and the session inline-budget discriminator still passes; with `audit-1049/configuration-schema.json` moved aside, the theme case fails |

The selected set is exactly the four rows' comparison and discriminating cases, not a label-wide
run:

1. `namespaces group sections and the inline budget omits tools with an explicit marker`
2. `the empty codemode catalog matches the frozen pi-v1.3.0 description`
3. `the on loadout lists only non-direct callable tools and adds script-call descriptions`
4. `the only loadout lists every callable tool and hides direct declarations`
5. `an inline budget omits a namespace's tools with pi's marker`
6. `a real session's next request lists MCP tools registered late and executes them`
7. `disabling a server and changing its exposure reshape the next request's catalog`
8. `a settings document the schema accepts reshapes the request and reload re-reads it`
9. `the theme token set matches the frozen pi-v1.3.0 capture`

Pinned comparison inputs (the two artifacts and the three test sources the four rows read):

| Input | sha256 |
| --- | --- |
| `fixtures/pi-v1.3.0/audit-1053/application-catalog.json` | `68a7e9716755ef5ebba97948b3ad9343b1621c1bdf72517248554d0927f52fb6` |
| `fixtures/pi-v1.3.0/audit-1049/configuration-schema.json` | `a7191ccc7c8c9f5f45d90a951b6d6c661c3e1531a53d6feed9aa395a987adeb8` |
| `tests/coding_agent/CodemodeCatalogTest.cpp` | `e26dc3bd8a2502393ad8c3fecc6ae76c07fba1fffc6755cd3ffcd3541d6950f5` |
| `tests/coding_agent/CodemodeCatalogSessionTest.cpp` | `f923e46e26fda8ed4b4fd674751e98bb1e5ccc3f3e9329775612fe52de60746f` |
| `tests/coding_agent/tui/ThemeTest.cpp` | `8330296e93d9913be89899ff46c5af580ed81deb234d7bc5e6191a9df5321703` |
| `docs/research/pi-v1.3.0/deliveries/1177-app-subset.xml` | `0accacec69781e31740a07f982293a4072e3d51e056025adcd726bdfa4ae899a` |

## Capability status

Membership is unchanged: 152 supported `cch_coding_agent` rows before and after, plus the excluded
rows that keep their existing boundaries. Six rows are verified through the existing #1104 and
#1118 receipts. Four rows (62, 71, 73, 146) now have a passing artifact-read comparison and a
discriminating case; their receipts are registered serially by the operator, and until then their
ledger status is untouched. Three candidates were rejected with a named failing condition and keep
their recorded status. The remaining 142 rows are routed to P1–P8; none is left unnamed.

No blanket promotion occurred. Row 62's ledger `implementation: missing` and rows 71/73's `partial`
are the 2026-10-19 audit facts, preserved as history; the delivered catalog and settings paths are
current-code observations, not a rewrite of the registered capture payloads.

## Remaining boundary

P1–P8 are the complete remaining `cch_coding_agent` obligation in this packet. They must not
implement neighbouring behaviour, must capture any missing frozen observation independently from
`71023aec…`, and must not weaken a normalization. `#1044` and `#1126` stay open; closing this
verification packet accepts no adjacent row and no umbrella obligation.

Magpie-only AI access, XDG state roots, strict no-exception behaviour, the ordinary application
`Session`, all explicit AI/platform exclusions and the no-historical-Pike-durable-migration boundary
remain unchanged. SIGKILL evidence does not establish power-loss or multi-process guarantees.

## Appendix — every supported `cch_coding_agent` row

Row numbers are the position in the 152-row supported `cch_coding_agent` inventory of
[1058-consolidated-capability-ledger.json](1058-consolidated-capability-ledger.json), in ledger
order; the row ID is authoritative. Verdicts and batches are defined above.

| # | Row | Capability | Impl / Verif | Frozen artifact | Verdict | Batch |
|---:|---|---|---|---|---|---|
| 1 | `AgentSession:lifecycle:abort` | Abort the current Agent operation and wait for idle, including compaction/branch-summary/retry work | partial/unverified | — | **L** | P2 |
| 2 | `AgentSession:lifecycle:close` | Close a session idempotently: stop admission, cancel or await admitted work, drain persistence, release resources | implemented/unverified | — | **L** | P2 |
| 3 | `AgentSession:queue-admission` | AgentSession queue admission | partial/upstream-captured | audit-1047/queue-admission.json | **L** | P2 |
| 4 | `AgentSession:tool-telemetry` | AgentSession tool telemetry | partial/upstream-captured | audit-1047/tool-execution.json | **L** | P2 |
| 5 | `EditorComponent` | EditorComponent (interface) | implemented/unverified | pi-v1.0.4 bundle | **H** | P8 |
| 6 | `EditorComponent.addToHistory` | EditorComponent.addToHistory | implemented/unverified | pi-v1.0.4 bundle | **H** | P8 |
| 7 | `EditorComponent.borderColor` | EditorComponent.borderColor | implemented/unverified | pi-v1.0.4 bundle | **H** | P8 |
| 8 | `EditorComponent.getExpandedText` | EditorComponent.getExpandedText | implemented/unverified | pi-v1.0.4 bundle | **H** | P8 |
| 9 | `EditorComponent.getText` | EditorComponent.getText | implemented/unverified | pi-v1.0.4 bundle | **H** | P8 |
| 10 | `EditorComponent.handleInput` | EditorComponent.handleInput | implemented/unverified | pi-v1.0.4 bundle | **H** | P8 |
| 11 | `EditorComponent.insertTextAtCursor` | EditorComponent.insertTextAtCursor | implemented/unverified | pi-v1.0.4 bundle | **H** | P8 |
| 12 | `EditorComponent.onChange` | EditorComponent.onChange | implemented/unverified | pi-v1.0.4 bundle | **H** | P8 |
| 13 | `EditorComponent.onSubmit` | EditorComponent.onSubmit | implemented/unverified | pi-v1.0.4 bundle | **H** | P8 |
| 14 | `EditorComponent.setAutocompleteMaxVisible` | EditorComponent.setAutocompleteMaxVisible | implemented/unverified | pi-v1.0.4 bundle | **H** | P8 |
| 15 | `EditorComponent.setAutocompleteProvider` | EditorComponent.setAutocompleteProvider | implemented/unverified | pi-v1.0.4 bundle | **H** | P8 |
| 16 | `EditorComponent.setPaddingX` | EditorComponent.setPaddingX | implemented/unverified | pi-v1.0.4 bundle | **H** | P8 |
| 17 | `EditorComponent.setText` | EditorComponent.setText | implemented/unverified | pi-v1.0.4 bundle | **H** | P8 |
| 18 | `NativeClipboard` | NativeClipboard (interface) | implemented/unverified | pi-v1.0.4 bundle | **H** | P8 |
| 19 | `NativeClipboard.getFilePaths` | NativeClipboard.getFilePaths | implemented/unverified | pi-v1.0.4 bundle | **H** | P8 |
| 20 | `NativeClipboard.getImage` | NativeClipboard.getImage | implemented/unverified | pi-v1.0.4 bundle | **H** | P8 |
| 21 | `NativeClipboard.getText` | NativeClipboard.getText | implemented/unverified | pi-v1.0.4 bundle | **H** | P8 |
| 22 | `NativeClipboard.setText` | NativeClipboard.setText | implemented/unverified | pi-v1.0.4 bundle | **H** | P8 |
| 23 | `RgbColor` | RgbColor (interface) | implemented/unverified | pi-v1.0.4 bundle | **H** | P8 |
| 24 | `RgbColor.b` | RgbColor.b | implemented/unverified | pi-v1.0.4 bundle | **H** | P8 |
| 25 | `RgbColor.g` | RgbColor.g | implemented/unverified | pi-v1.0.4 bundle | **H** | P8 |
| 26 | `RgbColor.r` | RgbColor.r | implemented/unverified | pi-v1.0.4 bundle | **H** | P8 |
| 27 | `Session:assembly:createAgentSession` | Resolve and assemble the application Agent Session from a model, settings, resources, tool policy and SessionManager | implemented/unverified | — | **L** | P2 |
| 28 | `Session:compaction:auto-recovery` | Automatic threshold/overflow compaction with one retry, persistent omission of the failed attempt and stale-usage guards | partial/unverified | — | **L** | P2 |
| 29 | `Session:compaction:in-memory` | Compaction and context reconstruction for an in-memory session | missing/unverified | audit-1048/session-tree-and-fork.json | **L** | P1 |
| 30 | `Session:compaction:manual` | Manually compact the active branch, append a checkpoint and rebuild the live context | partial/upstream-captured | audit-1048/session-compaction-boundary.json | **L** | P2 |
| 31 | `Session:compaction:summary-serialization` | Serialize conversation text for summarization without thinking while retaining thinking in the stored transcript | implemented/upstream-captured | audit-1048/session-compaction-boundary.json | **L** | P2 |
| 32 | `Session:consumers:tui-live-session` | TUI session operations use the live Agent Session store rather than opening a second store | missing/unverified | — | **N** | P2 |
| 33 | `Session:context:context-edit` | Append-only context_edit entries that null out or replace one earlier entry's model content without rewriting storage | missing/upstream-captured | audit-1048/session-tree-and-fork.json | **N** | P1 |
| 34 | `Session:context:projection` | Build a provenance-preserving, compaction-aware model projection with per-entry source mapping | partial/upstream-captured | audit-1048/session-tree-and-fork.json | **L** | P1 |
| 35 | `Session:event-commitment` | Commit completed Session Entry messages off-loop in FIFO admission order with a sticky failure and a run veto | implemented/unverified | — | **L** | P2 |
| 36 | `Session:fork:branch-file` | Fork a session at a message position into a new file with re-chained parents, retained labels, parentSession header and unknown-line preservation | implemented/upstream-captured | audit-1048/session-tree-and-fork.json | **L** | P1 |
| 37 | `Session:history:tree-ops` | Branch, reset leaf, label and branch-summary operations over the live tree | implemented/upstream-captured | audit-1048/session-tree-and-fork.json | **L** | P1 |
| 38 | `Session:publication:deferred-jsonl` | Publish a new JSONL session whose header and pre-conversation entries are deferred until the first user or assistant message | implemented/unverified | — | **L** | P2 |
| 39 | `Session:resume:context` | Reopen a session into compaction-aware messages, model and thinking level on the active leaf path | partial/unverified | audit-1048/session-tool-restore.json | **L** | P1 |
| 40 | `Session:resume:relative-tool-modifiers` | Apply relative +name/-name tool-selection modifiers to the recorded loadout on resume with ordered precedence | implemented/upstream-captured | audit-1048/session-tool-restore.json | **L** | P1 |
| 41 | `Session:resume:tool-loadout` | Restore the active tool loadout recorded in the transcript on resume, preserving declaration order and filtering unavailable tools | implemented/upstream-captured | audit-1048/session-tool-restore.json | **L** | P1 |
| 42 | `Session:storage:transcript-vs-durable` | Keep the application Session transcript independent from the Durable runtime, with no old-Pike durable import or automatic migration | implemented/unverified | — | **N** | P2 |
| 43 | `Session:storage:xdg-root` | Retain the Pike XDG Agent Config Directory and Magpie-only model path while aligning Session behavior with pi | implemented/unverified | — | **L** | P2 |
| 44 | `Session:tree:live-store` | Maintain fileEntries/byId/leafId equivalent live state and answer topology/context queries without re-reading the file | implemented/unverified | — | **L** | P2 |
| 45 | `Session:wire:baseline-alignment` | Serialize application Session entries at the pi-v1.3.0 coding-agent SessionManager format rather than the retired pi-agent-core 0.83 harness format | missing/upstream-captured | audit-1048/session-tree-and-fork.json | **N** | P1 |
| 46 | `Session:wire:compaction-boundary` | Persist and reconstruct the prompt/tool boundary on a compaction entry | partial/upstream-captured | audit-1048/session-compaction-boundary.json | **L** | P2 |
| 47 | `Session:wire:entry-kinds` | Serialize and read the v3 Session entry kinds with pi field names, optional/null semantics and unknown-line preservation | partial/upstream-captured | audit-1048/session-tree-and-fork.json | **L** | P1 |
| 48 | `Session:wire:header-fields` | Write the pi v3 session header shape and tolerantly read older headers | partial/unverified | — | **L** | P2 |
| 49 | `Session:wire:migrations` | Migrate v1/v2 pi session entries to current v3 on open, including ids/parentIds and hookMessage conversion | missing/unverified | — | **L** | P2 |
| 50 | `Session:wire:redaction` | Retain Pike's mandatory secret redaction at the Session persistence seam while documenting the pi wire divergence | implemented/unverified | — | **L** | P2 |
| 51 | `TUI:tool-duration` | TUI tool duration | partial/upstream-captured | audit-1047/tool-execution.json | **L** | P7 |
| 52 | `TerminalColorScheme` | TerminalColorScheme (type) | implemented/unverified | pi-v1.0.4 bundle | **H** | P8 |
| 53 | `TerminalColors` | TerminalColors (interface) | implemented/unverified | pi-v1.0.4 bundle | **H** | P8 |
| 54 | `TerminalColors.background` | TerminalColors.background | implemented/unverified | pi-v1.0.4 bundle | **H** | P8 |
| 55 | `TerminalColors.foreground` | TerminalColors.foreground | implemented/unverified | pi-v1.0.4 bundle | **H** | P8 |
| 56 | `TerminalColors.palette` | TerminalColors.palette | implemented/unverified | pi-v1.0.4 bundle | **H** | P8 |
| 57 | `clipboard-paste` | Clipboard paste | implemented/unverified | — | **L** | P7 |
| 58 | `codemode-application-deferred` | Deferred tools in the codemode catalog | missing/upstream-captured | audit-1053/application-catalog.json | **L** | P6 |
| 59 | `codemode-application-direct-exposure` | Direct versus codemode exposure | missing/upstream-captured | audit-1053/application-catalog.json | **R** | P6 |
| 60 | `codemode-application-error-summary` | Application failure text and call summary | missing/upstream-captured | audit-1053/script-execution.json | **r** | P6 |
| 61 | `codemode-application-image-output` | Application image saving and model presentation | partial/unverified | audit-1053/script-execution.json | **r** | P6 |
| 62 | `codemode-application-inline-budget` | Application inlineBudget setting consumption | missing/upstream-captured | audit-1053/application-catalog.json | **R** | P6 |
| 63 | `codemode-application-namespace` | Tool namespaces in the catalog and discovery | implemented/verified | audit-1053/application-catalog.json | **R** | P6 |
| 64 | `codemode-application-nested-calls` | Application nested call details and cancellation | partial/unverified | audit-1053/script-execution.json | **r** | P6 |
| 65 | `codemode-application-result` | Application result formatting and details | partial/upstream-captured | audit-1053/script-execution.json | **r** | P6 |
| 66 | `codemode-application-tool-guidelines` | Tool prompt guidelines in declarations | missing/upstream-captured | audit-1053/application-catalog.json | **N** | P6 |
| 67 | `codemode-callable-tools` | Callable tool selection | implemented/unverified | audit-1053/application-catalog.json | **L** | P6 |
| 68 | `codemode-description-catalog` | Model-facing codemode catalog description | implemented/verified | audit-1053/application-catalog.json | **R** | P6 |
| 69 | `codemode-description-loadout` | Codemode loadout modes and per-tool script descriptions | implemented/verified | audit-1053/application-catalog.json | **R** | P6 |
| 70 | `codemode-extension-registration` | Application extension registration | implemented/unverified | audit-1053/application-catalog.json | **L** | P6 |
| 71 | `codemode-mcp-refresh` | Late tool registration and description refresh | partial/upstream-captured | audit-1053/application-catalog.json | **R** | P6 |
| 72 | `codemode-models-description-gate` | Codemode `models` description gate | implemented/verified | audit-1053/application-catalog.json | **L** | P6 |
| 73 | `codemode-settings` | Codemode settings resolution | partial/unverified | audit-1053/application-catalog.json | **R** | P6 |
| 74 | `codemode-store-entry` | Codemode session store entry | missing/upstream-captured | audit-1053/script-execution.json | **r** | P6 |
| 75 | `codemode-tool-binding` | Script `tools.*` bindings and call routing | implemented/unverified | audit-1053/script-execution.json | **r** | P6 |
| 76 | `context-file-candidate-order` | Project context-file candidate selection | implemented/upstream-captured | audit-1049/resource-selection.json | **L** | P3 |
| 77 | `context-worktree-shadowing` | Nested linked-worktree context-file shadowing | implemented/upstream-captured | audit-1049/resource-selection.json | **L** | P3 |
| 78 | `exit-restoration` | Exit output and terminal restoration | partial/unverified | audit-1051/product-differential.json | **L** | P7 |
| 79 | `export-html` | /export HTML: render the session as pi's self-contained HTML document | partial/unverified | — | **L** | P2 |
| 80 | `export-jsonl-branch` | /export JSONL: serialize the current session branch as a pi session file | implemented/unverified | — | **L** | P2 |
| 81 | `extension-editor-lifecycle` | Extension editor install/restore lifecycle | implemented/upstream-captured | audit-1051/extension-editor.json | **L** | P7 |
| 82 | `extension-lifecycle-hooks` | Extension-triggered runs and `before_agent_start` contributions | implemented/upstream-captured | audit-1049/extension-run.json | **L** | P3 |
| 83 | `extension-native-tool-source` | Native extension Tool Source foundation | implemented/unverified | — | **L** | P3 |
| 84 | `extension-prompt-status-ui` | Extension prompt, dialog and status UI | partial/upstream-captured | audit-1051/program-status.json | **L** | P3 |
| 85 | `extension-registration-tool-source` | Native extension registration and tool source | partial/upstream-captured | audit-1051/extension-editor.json | **L** | P3 |
| 86 | `getNativeClipboard` | getNativeClipboard (function) | implemented/unverified | pi-v1.0.4 bundle | **H** | P8 |
| 87 | `import-session` | /import: copy a session JSONL file into the session directory and switch to it | partial/unverified | — | **L** | P2 |
| 88 | `keybindings-config` | Keybindings configuration parsing, migration and diagnostics | partial/upstream-captured | audit-1049/configuration-schema.json | **L** | P3 |
| 89 | `keybindings-reload` | Keybindings reload consumer | implemented/upstream-captured | audit-1049/configuration-schema.json | **L** | P3 |
| 90 | `mcp-application-resource-init` | Application connection setup without eager resource listing | partial/upstream-captured | audit-1052/application-initialization.json | **L** | P5 |
| 91 | `mcp-application-startup` | Application startup wait and background connection | missing/upstream-captured | audit-1052/application-initialization.json | **L** | P5 |
| 92 | `mcp-config` | Application MCP configuration and validation | partial/upstream-captured | audit-1052/transports.json | **L** | P5 |
| 93 | `mcp-deferred-tool-search` | Deferred exposure and `tool_search` | implemented/upstream-captured | audit-1056/codemode-toolsearch.json | **r** | P5 |
| 94 | `mcp-exposure` | MCP exposure model and discovery activation | partial/upstream-captured | audit-1052/application-initialization.json | **L** | P5 |
| 95 | `mcp-external-signin-pickup` | Pick up sign-ins performed outside the session | implemented/verified | audit-1104/management-during-connection.json | **L** | P5 |
| 96 | `mcp-management-ui` | MCP management UI and `/mcp` actions | implemented/verified | audit-1104/management-during-connection.json | **L** | P5 |
| 97 | `mcp-oauth-persistence` | OAuth credential persistence in mcp-auth.json | implemented/unverified | — | **L** | P5 |
| 98 | `mcp-reconnect-tools-changed` | MCP reconnect and tools/list_changed refresh | implemented/unverified | — | **L** | P5 |
| 99 | `mcp-resource-tools` | Codex-compatible MCP resource tools | implemented/unverified | — | **L** | P5 |
| 100 | `mcp-session-tool-binding` | Session tool registration and codemode routing | implemented/unverified | — | **L** | P5 |
| 101 | `mcp-tool-result-conversion` | MCP result conversion to model and script channels | partial/upstream-captured | audit-1052/structured-content.json | **R** | P5 |
| 102 | `mode-switch-renderer` | Regular/fullscreen mode switch | implemented/unverified | audit-1051/extension-editor.json | **L** | P7 |
| 103 | `model-selector` | Model selector | partial/upstream-captured | audit-1051/product-differential.json | **L** | P7 |
| 104 | `osc52-opt-in` | OSC52 clipboard opt-in | partial/unverified | — | **L** | P7 |
| 105 | `output-pad` | Dynamic output padding | partial/unverified | audit-1051/product-differential.json | **r** | P7 |
| 106 | `parseTerminalColorSchemeReport` | parseTerminalColorSchemeReport (function) | implemented/unverified | pi-v1.0.4 bundle | **H** | P8 |
| 107 | `print-mode-length-truncated` | Print-mode length-truncated outcome | missing/upstream-captured | audit-1049/print-mode.json | **L** | P4 |
| 108 | `print-mode-text` | Print-mode text outcome | implemented/upstream-captured | audit-1049/print-mode.json | **L** | P4 |
| 109 | `product.--tui-mode` | product.--tui-mode (product-entry) | implemented/unverified | — | **N** | P7 |
| 110 | `product.fullscreenCopyOnSelect` | product.fullscreenCopyOnSelect (product-entry) | implemented/unverified | — | **N** | P7 |
| 111 | `product.fullscreenExitOutput` | product.fullscreenExitOutput (product-entry) | implemented/unverified | — | **N** | P7 |
| 112 | `product.fullscreenWheelScrollLines` | product.fullscreenWheelScrollLines (product-entry) | implemented/unverified | — | **N** | P7 |
| 113 | `product.runInteractiveMode` | product.runInteractiveMode (product-entry) | implemented/unverified | — | **N** | P7 |
| 114 | `product.showHardwareCursor` | product.showHardwareCursor (product-entry) | implemented/unverified | — | **N** | P7 |
| 115 | `product.tuiMode` | product.tuiMode (product-entry) | implemented/unverified | — | **N** | P7 |
| 116 | `program-status-reporting` | Program-status reporter | implemented/upstream-captured | audit-1051/program-status.json | **L** | P7 |
| 117 | `prompt-autocomplete` | Prompt autocomplete menu | implemented/unverified | — | **L** | P3 |
| 118 | `prompt-busy-lifecycle` | Prompt and busy-state lifecycle | partial/upstream-captured | audit-1051/program-status.json | **L** | P7 |
| 119 | `prompt-editor-submit` | Prompt editor and submission | implemented/upstream-captured | audit-1051/product-differential.json | **L** | P3 |
| 120 | `prompt-quoted-empty` | Prompt template argument quoting | implemented/upstream-captured | audit-1049/prompt-expansion.json | **L** | P3 |
| 121 | `prompt-substitution-forms` | Prompt template positional, collection, default and slice substitution | implemented/upstream-captured | audit-1049/prompt-expansion.json | **L** | P3 |
| 122 | `prompt-template-resources` | Prompt template discovery and explicit prompt paths | implemented/upstream-captured | audit-1049/prompt-expansion.json | **L** | P3 |
| 123 | `residual-1032-system-theme` | Generated system theme | missing/unverified | — | **N** | P7 |
| 124 | `residual-1033-extension-editor-capture` | Extension-editor dual-runtime capture | implemented/upstream-captured | audit-1051/extension-editor.json | **L** | P7 |
| 125 | `residual-1043-scrollback` | Scrollback retained-row delta | partial/upstream-captured | audit-1051/product-differential.json | **L** | P7 |
| 126 | `residual-1043-settings-viewport` | settings-selector viewport and omitted rows | partial/upstream-captured | audit-1051/product-differential.json | **L** | P7 |
| 127 | `residual-1043-startup-wrap` | 72-column startup-help wrap family | partial/upstream-captured | audit-1051/product-differential.json | **L** | P7 |
| 128 | `residual-1043-tool-result-timing` | tool-result mid-stream capture moment | partial/upstream-captured | audit-1051/product-differential.json | **L** | P7 |
| 129 | `residual-808-model-selector` | Model selector marker and row layout | partial/upstream-captured | audit-1051/product-differential.json | **L** | P7 |
| 130 | `residual-810-thinking-selector` | Thinking selector layout | partial/unverified | audit-1051/product-differential.json | **L** | P7 |
| 131 | `residual-812-tool-result-presentation` | Successful tool-result presentation | partial/upstream-captured | audit-1051/product-differential.json | **L** | P7 |
| 132 | `resource-explicit-sources` | Explicit skill, prompt-template and theme path sources | implemented/upstream-captured | audit-1049/resource-selection.json | **L** | P3 |
| 133 | `resource-path-modifiers` | Local resource path modifiers | implemented/upstream-captured | audit-1049/resource-selection.json | **L** | P3 |
| 134 | `setImageTranscoder` | setImageTranscoder (function) | implemented/unverified | pi-v1.0.4 bundle | **H** | P8 |
| 135 | `settings-runtime` | Settings runtime consumption and reload | partial/upstream-captured | audit-1049/configuration-schema.json | **L** | P3 |
| 136 | `settings-schema` | Published settings schema shape | partial/upstream-captured | audit-1049/configuration-schema.json | **L** | P3 |
| 137 | `settings-selector` | Settings selector | partial/upstream-captured | audit-1051/product-differential.json | **L** | P7 |
| 138 | `startup-header-help` | Startup header and command-help presentation | partial/upstream-captured | audit-1051/product-differential.json | **L** | P7 |
| 139 | `startup-loaded-resources` | Loaded-resource startup section | partial/upstream-captured | audit-1051/extension-editor.json | **L** | P7 |
| 140 | `startup-project-trust` | Project trust startup prompt | implemented/unverified | — | **L** | P7 |
| 141 | `system-prompt-command-claims` | System-prompt/docs/command claims about resources and extensions | implemented/unverified | audit-1049/resource-selection.json | **L** | P3 |
| 142 | `system-theme` | Generated system theme | missing/unverified | — | **N** | P7 |
| 143 | `terminal-progress` | OSC 9;4 terminal progress | implemented/unverified | audit-1051/program-status.json | **L** | P7 |
| 144 | `theme-schema` | Theme validation schema and runtime loading | partial/upstream-captured | audit-1049/configuration-schema.json | **R** | P3 |
| 145 | `theme-selection-tokens` | Theme selection and token resolution | partial/upstream-captured | audit-1051/product-differential.json | **L** | P3 |
| 146 | `theme-token-set` | Theme token inventory | partial/upstream-captured | audit-1049/configuration-schema.json | **R** | P3 |
| 147 | `thinking-selector` | Thinking-level selector | partial/unverified | audit-1051/product-differential.json | **L** | P7 |
| 148 | `tool-bash-presentation` | Bash tool presentation | implemented/unverified | — | **L** | P7 |
| 149 | `tool-duration` | Tool execution duration | partial/upstream-captured | audit-1047/tool-execution.json | **L** | P7 |
| 150 | `tool-read-line-range` | Read tool line-range labels | partial/unverified | — | **L** | P7 |
| 151 | `tool-result-presentation` | Tool call/result presentation | partial/upstream-captured | audit-1051/product-differential.json | **L** | P7 |
| 152 | `transcript-state-scrollback` | Transcript state and retained scrollback | partial/upstream-captured | audit-1051/product-differential.json | **L** | P7 |
