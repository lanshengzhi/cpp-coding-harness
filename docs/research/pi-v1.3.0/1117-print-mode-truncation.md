# #1117 print-mode length truncation

## Scope

B59 / F21d under #1044 and ADR 0075. The named baseline is `pi-v1.3.0`,
`71023aec1546d6d677a65669c5665c0927ce0dd0`, Linux x86-64/glibc.
The selected behavior is the ordinary CLI's final text, warning and exit status
when an assistant response reaches its output token limit. Application
verification receipts belong to #1177.

## Delivered behavior

Print finalization preserves the final assistant's text blocks on stdout,
then reports `Response was truncated: the model reached its output token limit.`
on stderr and returns exit 1 for `AssistantStopReason::Length`. Normal completion
keeps exit 0 and empty stderr. Error/abort outcomes retain their diagnostic-only
presentation. The existing bounded, redacted text presentation applies to partial
output as well as complete output.

The independent expected values come from the registered
[audit-1049 print-mode capture](../../../fixtures/pi-v1.3.0/audit-1049/print-mode.json):

| Frozen scenario | stdout | stderr | exit |
| --- | --- | --- | --- |
| length-truncated text | `partial answer\n` | `Response was truncated: the model reached its output token limit.\n` | 1 |
| complete text | `complete answer\n` | empty | 0 |

The subprocess comparison calls the production `run_cli_entry`, the same entry
returned directly by `src/main.cpp`, with real process stdin/stdout/stderr and
maps its return value to the child's process exit. Parsing, frontend selection,
ordinary Session assembly, gateway response adaptation, print finalization and
runtime teardown all run. Only the Magpie stream transport and its model catalog
are scripted through existing test access. Each child has isolated HOME/XDG state
and workspace, no live credentials or network, and a kernel deadline. Parent
assertions run after the child has been reaped. This tests the production entry
with transport injection; it does not test installed executable relocation.

The discriminating case checks stdout, stderr and process status separately.
An implementation that prints the correct partial text but returns 0 fails.
Normal completion of identical text also distinguishes a stop-reason decision
from a content-based heuristic. Error/cancellation cases keep stdout empty, and
redaction checks compare the exact safe output rather than only the presence of
a redaction marker.

## Validation

- Red: `ctest --preset vcpkg -L issue1117` initially selected 1 case and
  failed exactly its warning and exit-status assertions: stdout was correct,
  stderr was empty and the subprocess exited 0. The same case passed after
  the finalizer change.
- Focused: incremental `cch_tests_cli_arch` build with `-j2`, then
  `ctest --preset vcpkg -L print --output-junit /tmp/1117-print.xml`:
  **18 selected, 18 passed**, including all **6** `issue1117` subprocess cases
  and the existing SIGTERM/SIGHUP, error/abort and bounded diagnostic cases.
  The actual [JUnit result](deliveries/1117-print.xml) is retained for #1177.
- The build-phase architecture gate passes with strict no-exception evidence.
  An initial build failed because an accidentally overlapping Ninja invocation
  corrupted `.ninja_deps`; the dependency database was replaced and all
  production objects rebuilt to restore the evidence. No gate was bypassed.
- `scripts/format-check.sh 0087b09fc` and `git diff HEAD --check`: clean.
- Registered evidence read:
  `python3 scripts/evidence/audit_evidence.py read --family audit-1049 --artifact print-mode.json`:
  passed.
- Independent `code-review` agents: **Standards 0 findings; Spec 0 findings**.
  Review covered the production finalizer, CLI subprocess boundary, cleanup before
  assertions, frozen observations, scope and receipt ownership.
- Full build: `cmake --build --preset vcpkg -j2`: passed. The C++ checkout
  base was `0087b09fc7ae8990d505ec4c5fe13498e09e0fc2`, plus this ticket's changes.
- Unfiltered full-suite history:

  | Test environment / parallelism | Selected | Passed | Failed | Skipped |
  | --- | ---: | ---: | ---: | ---: |
  | Inherited `TERM_PROGRAM=ghostty`, `-j2` | 4133 | 4123 | 2 | 8 |
  | Explicit `TERM_PROGRAM=herdr`, `-j2` | 4133 | 4124 | 1 | 8 |
  | Explicit `TERM_PROGRAM=herdr`, `-j1` | 4133 | 4123 | 2 | 8 |

  The initial failures were #1124's `TERM_PROGRAM=herdr` precondition and the
  warnings gate's 900-second timeout. The terminal case passed with its required
  test environment. On the second full run, **all 43 architecture checks passed**,
  including **620 compile commands checked with `-Werror`, zero failures**.
  All 18 print cases, including all 6 `issue1117` cases, passed again.

  The second run's sole failure was the unmodified #1019 test
  `Process Terminal start and enter_output_only reject unrestored alternate screen state`.
  It had passed in the first full run and subsequently passed 20 consecutive
  isolated repeats (`--repeat until-fail:20`); its root cause is not established.
  No terminal code, timeouts or exclusions were changed. The terminal environment
  override supplies no additional evidence about an actual terminal emulator.
- Final serial command:
  `TERM_PROGRAM=herdr ctest --preset vcpkg -j1 --output-junit /tmp/1117-full-serial.xml`.
  Again, **all 18 print cases, all 6 `issue1117` cases and all 43 architecture
  checks passed**. **Full Validation is not green**: two terminal cases failed:

  - `Process Terminal atomic stop and default stop detect postdrain EIO on closed master`
    (`tests/tui/ProcessTerminalTest.cpp:2588`): expected a stop error, received success.
  - `a timed-out query resolves partial results and later replies advance state`
    (`tests/tui/TerminalColorsTest.cpp:291`): expected one completion callback,
    observed zero.

  Both had passed in earlier full runs. These cases are in the unchanged
  `cch_tests_tui` target, which does not link the CLI print code. Their root causes
  are not established; no repair of these terminal behaviors is claimed.
  Full JUnit outputs are retained locally at `/tmp/1117-full.xml`,
  `/tmp/1117-full-herdr.xml` and `/tmp/1117-full-serial.xml`; the focused print
  receipt above is committed for #1177.

## Capability status

This bounded slice implements and compares the selected length-truncated
print-mode outcome. It does not promote a consolidated capability row or edit
shared delivery receipts; that reconciliation is owned by #1177. The original
source audits and registered upstream capture are preserved.

## Remaining boundary

JSON event print mode remains excluded. Ordinary CLI Sessions retain the
Magpie-only model path, XDG state root and strict no-exception runtime. This slice
makes no Durable Runtime, terminal emulator or umbrella #1044 completion claim.
