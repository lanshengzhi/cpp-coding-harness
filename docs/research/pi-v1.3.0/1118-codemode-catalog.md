# #1118 application codemode catalog and the live callable loadout

## Scope

Batch B60 / ACatalog, under #1044 and ADR 0075. Named Baseline: `pi-v1.3.0`,
revision `71023aec1546d6d677a65669c5665c0927ce0dd0`, Linux x86-64/glibc.

Frozen evidence: `fixtures/pi-v1.3.0/audit-1053/application-catalog.json`, scenarios
`the empty codemode catalog still declares globals, discovery and the options grammar`,
`a listed tool section carries its declaration, output type and guidelines`,
`namespaces group sections and the inline budget omits tools with an explicit marker`,
`deferred tools are never listed and do not change the description`,
`the models declaration line is present only when models are enabled`,
`the callable tool set excludes codemode itself`,
`the on loadout lists only non-direct callable tools and adds script-call descriptions` and
`the only loadout lists every callable tool and hides direct declarations`. Rows:
`cch_coding_agent:codemode-description-catalog`,
`cch_coding_agent:codemode-description-loadout`,
`cch_coding_agent:codemode-application-namespace` and
`cch_coding_agent:codemode-models-description-gate`.

## Delivered behavior

- **The catalog is the live loadout.** `CodemodeCatalog` renders pi v1.3.0's
  `createCodemodeDescription` exactly: the intro, the globals block, one `### \`id\`` section per
  callable tool, `## <namespace>` headings with the `(tools not listed)` / `(some tools not listed)`
  markers, and the OpenCode-style round-robin `inlineBudget` selection. `toCodemodeDeclaration`
  defaults a missing output schema to `{ "type": "string" }`, appends trimmed prompt-guideline
  bullets, and `renderToolOutputType` supplies the script-call sentence's output type.
- **Settings drive presentation.** `codemode.mode` (`on`/`only`) and `codemode.inlineBudget` are read
  from the merged settings at assembly and refreshed by `/reload`. `on` adds the
  `Codemode: \`tools.<id>(args)\` resolves to …` sentence to declared callable tools and lists only the
  non-direct callable tools; `only` lists every callable tool and hides the direct declarations from
  requests while the transcript keeps declaring them.
- **Exposure, namespace and guidelines travel with the tool.** `ExtensionTool` carries pi's
  `ToolDefinition.exposure` and `namespace`; the MCP surface and the programmatic MCP source both
  attach the server namespace (`mcp__<server>`, alias `<server>`, configured description, initialize
  instructions) and the per-tool exposure. `CodemodeDiscovery` implements `searchTools`,
  `describeTool` and `describeNamespace` over that metadata, including the namespace filter and alias
  lookup.
- **Deferred discovery is real.** The discovery globals the description advertises are the globals
  the sandbox binds, and deferred tools are callable and discoverable but never listed. The `models`
  line appears only when `models` is among the bound globals; Pike retains no codemode `models.*`
  provider execution, so production binds the discovery globals only and never advertises it.
- **The next request sees live changes.** The session keeps one `CodemodeCatalogState`; it is applied
  to the registry before the first request (after #1111's `--tools` modifiers) and to the restored
  loadout on resume. The MCP surface refreshes it on registration, withdrawal (re-register as
  `hidden`), re-exposure and deferred-tool activation, so a real session's next request carries the
  added, removed or re-exposed tools. Hidden declarations are projected out of `AiContext.tools` at
  the provider boundary, matching pi's `_installHiddenDeclarationsProjection`.
- **Scripts call exactly the listed set.** The codemode tool's `tools.*` surface and discovery globals
  resolve the exposure-filtered callable set (`codemode`/`deferred` plus active `direct`), so a
  `model-only` or `hidden` tool is neither advertised nor callable, and a listed tool is executable.

## Comparisons

| Frozen observation | C++ comparison |
| --- | --- |
| the empty codemode catalog still declares globals, discovery and the options grammar | `tests/coding_agent/CodemodeCatalogTest.cpp` — `the empty codemode catalog matches the frozen pi-v1.3.0 description`; `the codemode description is the frozen pi-v1.3.0 empty catalog` |
| a listed tool section carries its declaration, output type and guidelines | `tests/coding_agent/CodemodeCatalogTest.cpp` — `a listed tool section carries its declaration, output type and guidelines` |
| namespaces group sections and the inline budget omits tools with an explicit marker | `tests/coding_agent/CodemodeCatalogTest.cpp` — `namespaces group sections and the inline budget omits tools with an explicit marker` |
| deferred tools are never listed and do not change the description | `tests/coding_agent/CodemodeCatalogTest.cpp` — `deferred tools are never listed and do not change the description` |
| the models declaration line is present only when models are enabled | `tests/coding_agent/CodemodeCatalogTest.cpp` — `the models declaration line follows the bound capability` |
| the callable tool set excludes codemode itself | `tests/coding_agent/CodemodeCatalogTest.cpp` — `the callable tool set excludes codemode itself` |
| the on loadout lists only non-direct callable tools and adds script-call descriptions | `tests/coding_agent/CodemodeCatalogTest.cpp` — `the on loadout lists only non-direct callable tools and adds script-call descriptions` |
| the only loadout lists every callable tool and hides direct declarations | `tests/coding_agent/CodemodeCatalogTest.cpp` — `the only loadout lists every callable tool and hides direct declarations` |

The comparisons are exact string diffs against the registered artifact, not substrings. The
discriminating cases are the live-session ones: a check that only compares the empty-catalog text
passes while the live catalog is stale, so `a real session's next request lists MCP tools registered
late and executes them` asserts the added tool is listed *and* runs it through the nested-call seam;
`the only loadout hides direct declarations from the request and lists them in the catalog` asserts
the hidden set on a real request and the transcript's retained declaration;
`the advertised discovery globals are bound and answer over the callable loadout` calls
`searchTools`/`describeTool`/`describeNamespace` in a real script; and
`disabling a server and changing its exposure reshape the next request's catalog` proves removal and
re-exposure. A catalog that advertises a fake global or a non-executable tool fails those cases.

## Validation

- Focused Validation, `ctest --preset vcpkg -L issue1118`: 19 selected, 19 passed, 0 failed.
  `-L catalog` 13, `-L codemode` 49, `-L mcp` 297, each fully passed.
- Architecture, `ctest --preset vcpkg -L architecture`: 43 selected, 43 passed, including the
  build-phase `PARITY-4011` include scan after reconfiguring for the changed include sets.
- Receipt revalidation on the merged tree: the `^(issue1090|issue1101|issue1104)$` selection ran
  36/36 into `deliveries/1118-revalidation.xml`; the #1101/#1104 pinned inputs this branch changed are
  recorded in their `revalidation1118` entries with the previous validation preserved.
- Full Validation on the merged tree with `TERM_PROGRAM=herdr`: `cmake --build --preset vcpkg` then the
  unfiltered serial `ctest --preset vcpkg -j1` — 4,102 selected, 0 failed, 8 skipped, exit 0. The 8
  skips are the six frozen-pi durable continuation cases, the realtime-clock wrapper and the Native
  TUI differential capture, none of which this slice touches. A parallel run of the same tree failed
  only the load-sensitive `bounded parallel cancellation leaves queued tool capabilities unstarted`,
  which passed on isolated rerun; an environment-default run (`TERM_PROGRAM=ghostty`) fails only the
  real-PTY evidence case that asserts `TERM_PROGRAM=herdr`.
- Formatting: `scripts/format-check.sh origin/main` clean.
- Consolidation: `consolidate_audits.py build` wrote 1,464 rows; `check` passed and the
  `cch_pi_v130_consolidation_*` cases passed.

## Capability status

The four rows move from `partial`/`missing` + `upstream-captured` to `implemented` + `verified`
through the `1118-codemode-catalog` delivery receipt, whose JUnit artifact is
`deliveries/1118-catalog.xml` and whose scenario mappings are listed above. The programmatic MCP
source now attaches exposure and namespace too, so the namespace row's whole capability is covered
by the persisted manager and the programmatic seam alike.

## Remaining boundary

- The codemode `models.*` provider execution stays excluded by ADR 0075; only the absent-description
  gate is supported, and the Magpie gateway remains the sole AI capability.
- The discovery globals report an absent tool/namespace as JSON `null` through the string host bridge;
  pi's JS host reports `undefined`. The absence is observable either way; the JS spelling is an
  execution-protocol boundary (#1119).
- The inline-budget cost estimate counts bytes (pi counts UTF-16 code units) and namespace ordering is
  byte order (pi uses `localeCompare`); both differ only for non-ASCII names or descriptions.
- `tool_search` keeps its existing `defaultActive: false` registration; this slice only marks its
  exposure `model-only`, so scripts never list or call it.
- #1044 stays open; this ticket does not authorize editing or closing the umbrella. The ordinary
  application Session is in scope; Durable Runtime is not.
