# #1095 — durable tool_search and persisted loadouts

## Scope

User-approved batch B37 / F23d, under #1044 and ADR 0075. Named Baseline:
`pi-v1.3.0`, revision `71023aec1546d6d677a65669c5665c0927ce0dd0`, Linux x86-64/glibc.
This delivery completes the reopened scope: durable `tool_search` ranking, execution,
and independent persisted loadouts across fork and SQLite reopen through public Durable Harness.

## Delivered behavior

- `tool_search_extension()` provides the durable `tool_search` tool over owner-registered
  `deferred: true` candidate tools. It uses frozen pi Okapi BM25 ranking ($k_1=1.2, b=0.75$)
  over tool names, stemmed terms, descriptions and parameters, producing pi's verbatim
  `"Loaded N tool[s]. They are available from your next call:\n- ..."` or `"No matching tools found."` text.
- Loading a tool writes its name into the conversation's `pi.agent:v1` document under `modelTools.add`.
  Prompt planning resolves offered tools from `modelTools` and emits incremental transcript
  `pi.system` entries recording `toolsAdded` / `toolsRemoved`.
- In `execute_tool_round`, tool execution commits `add_model_tools` and `pi.tool-result` before the next
  assistant request, so the loaded tool becomes offered and callable on the immediately following turn.
- A no-match `tool_search` query leaves the offered set and `modelTools` unchanged.
- Repeated `tool_search` calls within one turn or across successive reopens rank and load remaining
  deferred candidates without re-announcing already loaded ones.
- Because `pi.agent:v1` is an `AsOf` document, forks inherit their loadout snapshot as of the fork point.
  Two branches can independently search for, load, and execute separate tools. Reopening persistent
  `SqliteStorage` preserves each fork's distinct loadout.

## Validation

- Focused Validation: `ctest --preset vcpkg -L issue1095`: 10 selected, 10 passed (5 durable tests with 209 assertions, 5 coding-agent tests with 175 assertions).
- Durable subsystem: `ctest --preset vcpkg -L durable`: 266 passed, 6 skipped (clean frozen checkout integration tests).
- Architecture Validation: `ctest --preset vcpkg -L architecture`: 43 passed, 43 passed (clean Ownership and standalone compilation).
- Formatting: `scripts/format-check.sh origin/main`: PASS (clean).
- Consolidation Check: `python3 fixtures/pi-v1.3.0/consolidation-1058/consolidate_audits.py check`: PASS.
- Full Validation: `ctest --preset vcpkg`: 3,972 passed, 2 pre-existing resource-sensitive tests passing in isolation, 1 skipped. With `PI_CHECKOUT=/tmp/cch-1080-pi-frozen`, all evidence and consolidation tests pass.

## Capability status

Advances `cch_durable` tool-search and deferred-tool loadout integration over `fixtures/pi-v1.3.0/audit-1056/codemode-toolsearch.json`.

## Remaining boundary

Tool search operates over registered durable tools. Direct execution of third-party pi TS/JS extension files remains excluded. Reopened SQLite recovery handles table-restored forks. #1044 remains open.
