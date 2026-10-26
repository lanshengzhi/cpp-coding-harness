# Live Session consumers (#1072 / B14 P8)

## Scope

This implements #1072's bounded prefactor under ADR 0075 against Named Baseline
**pi-v1.3.0**, revision **71023aec1546d6d677a65669c5665c0927ce0dd0**,
on Linux x86-64/glibc. The application Session remains separate from the
independent Durable Runtime. #1044 stays open and is not edited by this delivery.

## Delivered behavior

The session selector recognizes the active session by its normalized path or
filesystem identity and calls `AgentSession::set_session_name`. That operation
appends under the live leaf, advances the live tree, and publishes the existing
Projection Stream update. A foreign selector row has no live Agent Session in
this host; its existing standalone open/append behavior is preserved.

`/export` obtains its JSONL branch and HTML wire snapshot from the active Agent
Session through the private Native TUI bridge. The bridge returns owned values or
the completed transcript export result; it exposes no mutable store reference.
HTML assembly consumes a passive wire snapshot. JSONL still exports only the
active branch, while HTML retains all tracked entries and identifies the active
leaf. Destination parsing, theme/tool/system-prompt extras, default HTML naming,
and the existing in-memory/unpublished-session rejection boundaries remain.

The public caller regressions drive `run_interactive_mode` through
`VirtualTerminal`. The live rename test inspects `session_entries`,
`session_tree`, and an independent Projection Stream subscription. Its cancelled
second rename leaves the entry count, name and subscriber backlog unchanged.
The export regression records two exchanges, navigates back to the first
assistant, then invokes both slash-command export lanes. JSONL excludes the
later exchange; decoded HTML includes both exchanges but names the selected
live leaf. Neither export rewrites the source transcript or moves the live leaf.

These cases distinguish the properties from their cheap stand-ins: a final-file
rename check passes with the original second writer while the tree and
subscriber stay stale; an export-file existence check passes while JSONL exports
the wrong branch and HTML names the wrong leaf. Both regressions failed on those
properties before their corresponding fixes and passed after them. Additional
public-caller cases reject an unpublished session and an in-memory session
without creating an export destination.

## Frozen observations

`capture-1072-live-session.mts` refuses a dirty checkout, the wrong registered
revision, or a host outside Linux x64/glibc. It runs the frozen SessionManager,
JSONL serializer and actual HTML exporter against temporary local resources;
it uses no provider requests or third-party plugins. The checked candidate
manifest and observations are in
`fixtures/pi-v1.3.0/audit-1072/{manifest.json,live-session.json}`.

The two observed scenarios record:

- A branch selected before the final file entry produces JSONL containing only
  that branch; actual HTML retains all entries and the selected live leaf.
- Appending `"  Live\r\nSession  "` yields `"Live Session"`, parents the new
  Session Info entry under the selected leaf and advances the live leaf.

Generated identifiers are compared by relationship; wall-clock timestamps and
header identity are omitted. The C++ caller regressions exercise the same branch
and parent/leaf distinctions with scripted model exchanges.

The frozen interactive selector callback itself still calls
`SessionManager.open` (`interactive-mode.ts:5753-5757`). Its UI callback therefore
does not establish the desired live-tree behavior. The active-selector change
is authorized by #1072/P8; no claim of complete frozen selector equivalence is made.

The candidate passed `audit_evidence.py check` at delivery and was initially left
unregistered because the consolidation plan was pinned by the #1101 receipt. The
[2026-10-26 reconciliation](1044-delivery-reconciliation-2026-10-26.md) registers
the unchanged manifest and both observations as explicit orphans, after reviewing
the plan delta and refreshing the existing #1101 comparison result. They are now
registered observations, not verified capability rows.

## Validation

- Focused `[issue1072]` selection: **4 selected, 4 passed**.
- Selector, `/export`, HTML and wire-snapshot regression selection:
  **24 selected, 24 passed**.
- Architecture selection: **43 selected, 43 passed** in the full run.
- Full Validation: incremental default Debug build passed. The first unfiltered
  run selected **3,732** cases: **3,730 passed, 1 failed, 1 skipped** (260.67 s).
  The unchanged codemode case `closing codemode settles the run before its worker
  exit releases admission` failed `wait_for_tool_calls(1)`, then passed three
  consecutive isolated repetitions without code changes. The cause is not
  established. The complete unfiltered rerun (`cmake --build --preset vcpkg -j4`
  followed by `ctest --preset vcpkg -j2`) passed: **3,732 selected, 3,731 passed,
  0 failed, 1 skipped** (267.22 s). The existing optional Native TUI differential
  capture was skipped in both runs.
- `scripts/format-check.sh d10e5fcea052074af3fe0a99feff393fce0ea082` and
  `git diff --check` passed.
- Candidate evidence manifest check and existing consolidation check passed.
- Independent parallel Standards and Spec reviews completed. Spec reported no
  findings. Standards reported an allegedly unused `ImageFixture.hpp` include;
  this is rejected because the export regression uses its `decode_base64` and
  `bytes_as_string` helpers to inspect the actual HTML payload. No actionable
  findings remain.

## Capability status

P8's implementation-frontier evidence now points to these live-caller regressions
and this report. No generated capability status advances: the composite
`cch_coding_agent:Session:consumers:tui-live-session` row retains its historical
source-audit status until a complete delivery receipt accepts that row. This
prefactor's implemented behavior and its remaining verification boundary are
recorded separately. The existing registered active-tool restore observation is
retained; it is not treated as evidence for live rename or export.

## Remaining boundary

This delivery makes no new real-PTY/emulator, power-loss, fsync-failure,
multi-process writer, historical durable migration, or overall AI parity claim.
Foreign sessions still use their own application JSONL store. Ordinary CLI
Sessions stay on the application SessionStore; Magpie-only AI access, XDG state
roots and strict no-exception behavior remain in force. Full-row frozen UI
comparison remains outstanding after the evidence registration; other #1044 capabilities remain outside this slice.
