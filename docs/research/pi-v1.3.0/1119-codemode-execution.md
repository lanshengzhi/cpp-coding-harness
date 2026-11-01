# #1119 ordinary application codemode execution

## Scope

B61 / AExecution under #1044 and ADR 0075. The baseline is `pi-v1.3.0`,
`71023aec1546d6d677a65669c5665c0927ce0dd0`, Linux x86-64/glibc.
The caller is the ordinary `AgentSession` assembled by `SessionFactory`, with a
scripted model and the real local filesystem/shell tools. Durable task journals
and durable output files remain owned by #1170–#1172. Application verification
receipts belong to #1177; no umbrella or capability-row status is changed here.

## Frozen observations

The candidate bundle [audit-1119](../../../fixtures/pi-v1.3.0/audit-1119/manifest.json)
executes frozen pi's `executeCodemode` with its real read/write/bash tools and
`SessionManager`. Its scenarios record:

- successful `store()` writes through a real tool call, then reopen and `load()`;
- a failed script whose `store()` write must not replace the persisted value;
- `store(key, undefined)` deletion;
- numbered `text()` items, the returned value, and the `<console_output>` block;
- a failure that keeps prior calls and console output and appends the
  `Tool calls made before the failure (they are not undone)` summary;
- the `max_output_tokens` budget, including UTF-16 counting, zero budget over
  the error text, and the spilled full text;
- a nested-tool failure (a fixed `Error: boom` from a fixture tool) and a
  script timeout whose in-flight call is cancelled;
- missing discovery results (`undefined`) and `image()` output saved to a file.

The capture checks the exact revision and clean tracked source, and writes the
manifest only after every observation succeeded. Wall time and present
`durationMs` values, temporary output paths and the unique workspace are
normalized; duration presence, bytes, statuses and ordering are retained.
`scripts/evidence/audit_evidence.py check --manifest` passes. The bundle is a
candidate for the #1177 receipt reconciliation and is not a capability claim by
itself. The older `audit-1056` store observation exercises the durable wrapper,
so it could not substitute for this application-Session capture.

`scripts/evidence/audit_evidence.py check --manifest
fixtures/pi-v1.3.0/audit-1119/manifest.json` emitted the registry patch for the
serial merger (a lane does not edit the shared registry):

```json
{
  "audit-1119": {
    "manifest": "audit-1119/manifest.json",
    "manifestSha256": "705c46d6d386d50c65b4220fef58446a6a6a72b30aacbd24a2fa757fa6e8d7e5"
  }
}
```

## Delivered behavior

- **Store persistence and resume.** A successful run's write set is appended as
  a `codemode-store` Custom entry through `SessionPersistence`, the same ordered
  commitment line as the transcript, before the tool result. `load()` replays the
  active branch root-to-leaf, applying each entry's deletes then sets. A failed
  run reports no writes and leaves the stored values untouched. In-memory
  sessions persist into their live tree through the same entry path.
- **Result presentation.** Output follows pi's `executeCodemode` shape: the
  header, `==> text N/M <==` markers when more than one text item exists, one
  trailing `<console_output>` block, adjacent text joined, the returned value as
  the last text item, and a `Script error:` block whose head is the script
  stack or `Script timed out: …`/`Script aborted: …`/`Script sandbox failed: …`
  followed by the prior-call summary.
- **Output budget.** `max_output_tokens` (default 10,000, 4 UTF-16 units per
  token) keeps the head and tail, writes the full text to a fresh temporary
  file, and names it in both the content and `details.fullOutputPath`. A spill
  failure keeps the script result and states why.
- **Images.** Each `image()` block is written once to a temporary file next to
  `[Image saved to <path> (<mime>, <size>)]`; a failed write keeps the image and
  becomes part of the label.
- **Nested call records.** `details.calls` carries pi's `id`, `name`, 200-unit
  `args`, `status`, settled `durationMs`, and 500-unit `error` preview. A call
  the sandbox cancels keeps pi's `<caller>/?` placeholder and no duration.
- **Sandbox bounds.** Scripts run with pi's 256 MiB guest heap limit and
  `timeout_ms` deadline; the prompt's stored-inline description already
  advertises the store and image behavior this slice makes real.

The nested-call seam now reports the call id on its result
(`AsyncToolExecutionResult::call_id`) so codemode can preserve pi's per-call id
without guessing.

## Comparisons

| Frozen observation | C++ comparison |
| --- | --- |
| successful writes persist and reload | `a session resumes codemode store writes and discards failed script writes` (real session, real `write`/`read`, close, reopen) |
| a failed script's writes are discarded | same case: the resumed script loads `42`, writes `wrong`, throws, and a later run still loads `42` |
| `store(key, undefined)` deletes | `a codemode store entry replays from the root and a later delete wins` — compares the frozen `delete` envelope *and* checks the replayed branch is empty |
| a nested failure keeps the error preview | `the frozen pi-v1.3.0 codemode nested failure records the failed call and its error preview` (exact normalized envelope, including `fail (error)` and the 500-unit preview) |
| numbered text, console block, returned value | `the frozen pi-v1.3.0 codemode output layout numbers text and groups console output` (exact normalized envelope) and `a real session presents codemode output and applies the script budget` |
| prior calls + console survive a failure | `the frozen pi-v1.3.0 codemode failure keeps prior calls and console output` (exact normalized envelope) |
| output budget, UTF-16 counting, spilled bytes | `the frozen pi-v1.3.0 codemode output budget truncates and spills the full text` (three scenarios; each spilled file is read back) and the live-session budget case |
| timeout cancels the in-flight call | `the frozen pi-v1.3.0 codemode timeout reports the interrupted call as cancelled` (exact envelope) and `a real session reports a timed-out codemode call as cancelled` (real `write`/`bash`) |
| `image()` saves bytes and names the path | `a codemode image is saved to disk next to its path label` — compares the envelope and then decodes the saved file |
| tool presentation unchanged through the renderer | `a codemode tool's text and image output render through the fallback and the inline image slot`, updated for the v1.3.0 five-block layout |

The discriminating cases: the resume case fails if the entry is never appended
*or* if a failed run appends its write set (the persisted value is reloaded);
the budget cases check the real spill bytes, so a fabricated path or a label
without a file fails; the image case reads the named file, so content-only
passes are rejected; the timeout cases check `cancelled` plus the placeholder id
and absent duration, so a generic `error` status fails.

## Validation

- Focused Validation: `ctest --preset vcpkg -L issue1119` — 10 selected, 10
  passed. `-L codemode` — 142 selected, 142 passed. `-R 'session|Session'` —
  432 selected, 432 passed, covering the persistence-channel and resume paths.
- Architecture: `ctest --preset vcpkg -L architecture` — 43 selected, 43
  passed, including the build-phase include scan after reconfigure, the
  strict-no-exception gates, `cch_owner_interface_standalone` and
  `cch_warning_gate`.
- Full Validation: `cmake --build --preset vcpkg` then the unfiltered serial
  `ctest --preset vcpkg -j1` with `TERM_PROGRAM=herdr` — 4,112 selected, 4,112
  passed, 0 failed, 8 skipped, exit 0 (590.85 s). The skips are the six
  frozen-pi durable continuation cases, the realtime-clock wrapper and the
  Native TUI differential capture; this slice touches none of them. Runs made
  while other worktrees were building reproduced known runner-sensitive cases
  (the PTY drain-EIO race, bounded-parallel cancellation, and the quarantined
  #632 projection cost bound); each passed in isolation, and the quiet final
  run above is the delivery evidence.
- Formatting: `scripts/format-check.sh origin/main` clean.
- Evidence: `check --manifest` for `audit-1119` passes; the 36-case
  `^(issue1090|issue1101|issue1104)$` selection ran into
  `deliveries/1101-revalidation-1119.xml` and the 19-case `issue1118`
  selection into `deliveries/1118-revalidation-1119.xml`, both fully passing.
  `consolidate_audits.py build`/`check` pass, and the
  `cch_pi_v130_consolidation_check`/`_boundaries` cases pass.

## Capability status

No capability row is promoted. The capture, implementation and comparisons are
inputs to #1177's receipt reconciliation, separate from target membership. The
existing #1118 receipt pinned `CodemodeToolSource.cpp`, `AgentSessionExecution.cpp`,
`RuntimeServices.hpp`, `SessionFactory.cpp` and `CodemodeCatalogSessionTest.cpp`;
its selection was rerun and its changed inputs are recorded as
`revalidation1119` without changing its rows or routing digest.

## Remaining boundary

- The discovery globals still report an absent tool/namespace as JSON `null`
  through the string host bridge; pi's JS host reports `undefined`. The absence
  is observable either way; the JS spelling needs a host-protocol change and is
  not claimed here.
- The codemode worker-limit message is the sandbox's `worker capacity exhausted`
  rather than pi's longer `Codemode worker limit reached …` sentence; the limit
  (two process-wide workers) is the same.
- Magpie-only AI access and the XDG state root are retained; no `models.*`
  execution, third-party pi plugins, Durable Runtime switch, historical durable
  migration or terminal-emulator claim is introduced. #1044 remains open.
