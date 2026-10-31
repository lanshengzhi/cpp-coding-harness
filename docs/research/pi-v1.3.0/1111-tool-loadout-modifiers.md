# #1111 recorded tool loadout and relative tool modifiers

## Scope

Batch B53 / F16, under #1044 and ADR 0075. Named Baseline: `pi-v1.3.0`, revision
`71023aec1546d6d677a65669c5665c0927ce0dd0`, Linux x86-64/glibc.

Frozen evidence: `fixtures/pi-v1.3.0/audit-1048/session-tool-restore.json`, scenarios
`resume without modifiers restores the recorded order and does not rewrite the transcript` and
`resume applies relative tool modifiers to the recorded loadout`. Rows:
`cch_coding_agent:Session:resume:tool-loadout` and
`cch_coding_agent:Session:resume:relative-tool-modifiers`.

## Delivered behavior

- **Resume restores the recorded order.** Replay applies the transcript's system-message tool deltas in
  order and keeps the resulting insertion order, instead of projecting the set through registry order. A
  recorded `["write","read","bash"]` declares exactly that order. Unavailable imported names are dropped
  without rewriting the transcript.
- **Relative modifiers follow pi precedence.** `ToolSelection` now carries `+name`/`-name` entries.
  `apply_tool_modifiers` removes a present name and appends an absent one in entry order, preserving every
  other position: `["-bash","+edit"]` over `["read","bash","write"]` yields `["read","write","edit"]`.
  `+name` on a present name does not move it; `-name` on an absent name is a no-op.
- **Validation matches pi `getToolListError`.** A list that mixes plain names/patterns with modifiers, or a
  modifier containing `*`, is a `Validation` error. `--tools`/SDK callers see the pi diagnostic text through
  the public assembly or CLI path.
- **The session keeps pi's restore decision.** An absent `--tools` or a modifier list restores the
  transcript loadout (`restore_tools_from_transcript`); an explicit plain allowlist replaces the initial
  selection instead. A fresh session applies modifiers to its default loadout, including activating a
  default-inactive name such as `codemode`.
- **Removed and unavailable tools cannot execute.** The restored declared set is filtered to registered
  tools, and the active set continues to govern execution (#1110). A `-bash` modifier removes the built-in
  from the declared set; a model call to it returns an error result and runs no command. Modifier-added
  names are replayed to the MCP surface so a not-yet-registered tool stays pending for late registration
  (#1104), while registered non-MCP tools are not re-added after Agent construction.
- **The transcript is not rewritten by restore.** Resume applies no declaration, model or thinking entry
  when the transcript already carries them; the session file is byte-identical after restore.

## Comparisons

| Frozen observation | C++ comparison |
| --- | --- |
| resume without modifiers restores the recorded order and does not rewrite the transcript | `tests/coding_agent/ToolSelectionTest.cpp` — `resume restores the recorded tool order without rewriting the transcript` |
| resume applies relative tool modifiers to the recorded loadout | `tests/coding_agent/ToolSelectionTest.cpp` — `resume applies ordered tool modifiers and a removed tool cannot execute`; `a fresh session applies tool modifiers to its default loadout in order` |
| validation rejects mixed names/modifiers and wildcard modifiers | `tests/coding_agent/ToolSelectionTest.cpp` — `tool list validation rejects mixed names and wildcard modifiers`; CLI case `CLI rejects mixed tool names and modifiers and wildcard modifiers` |

The order comparison is discriminating: the seed declares `["write","read","bash"]`, which differs from
both the registry order (`read`, `bash`, `edit`, `write`) and a sorted set. A set-equality or registry-order
restore passes a membership check but fails this exact list. The modifier case seeds `["read","bash","write"]`,
applies `["-bash","+edit","+not-registered"]`, and asserts the exact declared list, the filtered unavailable
name and byte-identical transcript before prompting.

The failure case separates execution from declaration: the model calls the removed built-in `bash` after
resume. The result must be an error with the bounded `unknown tool: bash` diagnostic, the marker file must not
exist, and the active list must remain `["read","write","edit"]`. A restore that only hides the name from a
snapshot but still resolves the registry would create the marker and fail. The CLI validation case rejects
both `read,+edit` and `-mcp__echo__*` before Session creation.

## Validation

- Focused Validation, `ctest --preset vcpkg -L issue1111`: 6 selected, 6 passed, 0 failed.
- Architecture, `ctest --preset vcpkg -L architecture -j2`: 43 selected, 43 passed, including the build-phase
  `PARITY-4011` include scan after reconfiguring for the changed include set.
- Revalidation of pinned receipts after touching shared inputs: the
  `^(issue1090|issue1101|issue1104)$` selection ran 36/36 for
  `deliveries/1101-revalidation-1111.xml` and 36/36 for `deliveries/1104-revalidation-1111.xml`.
- Full Validation on the branch with `TERM_PROGRAM=herdr`: `cmake --build --preset vcpkg` then the
  unfiltered `ctest --preset vcpkg` (serial) — 4,084 selected, 0 failed, 8 skipped, exit 0. The 8 skips are
  the six frozen-pi durable continuation cases, the realtime-clock wrapper and the Native TUI differential
  capture, none of which this slice touches. Earlier `-j2` runs on the same tree reported load-sensitive
  cases (`bounded parallel cancellation leaves queued tool capabilities unstarted`, the codemode interrupt
  budget case and the quarantined streaming benchmark); each passed on isolated rerun, and the serial
  unfiltered run above is the recorded result.
- Formatting: `scripts/format-check.sh origin/main` clean.
- Consolidation: `consolidate_audits.py build` wrote 1,464 rows; `check` passed (7 delivered rows, 562
  scenarios, 144 explicit orphans, 0 uncovered) and `tests/evidence/ConsolidationAuditTest.py` passed 42
  cases.

## Capability status

The two #1048 rows move from `partial`/`missing` to `implemented`; both remain `upstream-captured` because
whole-row verification is the #1177 receipt owner's scope. `1048-session-persistence.json` counts now read
13 implemented, 8 partial and 9 missing, and the report's resume section records the delivered paths and
comparisons. The two pinned receipts whose inputs this slice touched were revalidated without promoting
their rows.

## Remaining boundary

- This slice covers the ordinary application Session, not Durable Runtime or durable storage.
- #1044 stays open; this ticket does not authorize editing or closing the umbrella.
- An explicit plain `--tools` allowlist continues to select over the discovered set; this slice does not
  rework wildcard/allowlist ordering beyond preserving pi's restore decision.
- MCP tools that are not registered yet stay pending through the existing #1104 restore path; a terminal or
  emulator observation is not part of this slice.
