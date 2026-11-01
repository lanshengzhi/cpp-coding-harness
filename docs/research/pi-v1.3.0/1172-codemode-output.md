# #1172 — durable codemode output budgets, spill files and saved images

## Scope

Delivers durable codemode output formatting, token-budget truncation, complete
output spill files and environment-backed script-image saving through the
public Harness and `Environment` contract, under
[#1180](https://github.com/lanshengzhi/cpp-coding-harness/issues/1180),
[#1183](https://github.com/lanshengzhi/cpp-coding-harness/issues/1183) and
[#1044](https://github.com/lanshengzhi/cpp-coding-harness/issues/1044), under
[ADR 0075](../../adr/0075-align-selected-owner-packages-and-linux-capabilities-with-pi-v1-3-0.md).
Named baseline: `pi-v1.3.0` / `71023aec1546d6d677a65669c5665c0927ce0dd0`,
Linux x86-64 with glibc.

#1094 returned a broad 4 MiB / 100,000-line text bound and the frozen explicit
no-execution-environment image label; token-budget spill files, environment
image files and the complete ordered formatter remained missing. Whole-row
verification, capability promotion and the ledger rebuild stay with #1180.

## Frozen contract

Source of truth at the pinned revision:

- `packages/durable/src/codemode/output.ts` — `formatResult`, `formatItems`,
  `joinAdjacentText`, `truncate`, `saveImages`, `DEFAULT_MAX_OUTPUT_TOKENS`,
  `CHARS_PER_TOKEN`, `LINE_MARGIN` and `IMAGE_EXTENSIONS`.
- `packages/durable/src/codemode/codemode.ts` — `OUTPUT_LIMITS` (4 MiB /
  100,000 lines), `outputDir`, `outputFiles`, and the `start.maxOutputTokens`
  replay input.
- The #1094 ordered text/console/image/value observation and the #1090
  execution-environment and image capability boundaries.

The independent capture in `fixtures/pi-v1.3.0/audit-1172/` records eleven
scenarios from frozen pi at the named revision: token truncation with a complete
spill, an exact-budget non-truncation, one-character-over truncation, a
saved image through a real `NodeExecutionEnv`, the no-environment labels,
ordered text/console/image/value output, a failed script whose partial output is
spilled, Unicode truncation, the line budget exactly at its limit, one line over
the line budget, and a token budget larger than the text that still clamps the
tail to the whole line window. The capture resolves the baseline from
`baselines.json`, refuses a checkout whose `HEAD` is not the registered revision
or whose tracked tree is dirty, re-executes under that checkout's `tsx`, and
writes the artifact and manifest itself. Expected values, file bytes and
normalized paths come from that capture, not from C++. Its three declared
normalizations are the wall time, the run's temporary output directory and every
run of 100 or more newlines (`<newlines:N>`); the C++ comparison applies the same
normalizations.

## Delivered behavior

`codemode_extension` optionally takes a shared `Environment`. The extension
passes it to the durable codemode execution, which:

- Creates the run's output directory once through
  `Environment::create_temp_dir("pi-codemode-")`, records it in the frozen run
  document's `outputDir`, and reuses it on a replay, so fixed output names are
  overwritten rather than accumulating.
- Applies the frozen token budget `max_output_tokens * 4` UTF-16 units, capped
  by `maxBytes / 4`, and the frozen half-budget line window with `LINE_MARGIN`.
  Over budget it emits the frozen warning, trims the head and tail on UTF-16
  boundaries, moves images after the text and writes the complete text as
  `output.txt` under the run directory. A budget larger than the text keeps the
  frozen `slice(-n)` clamp instead of underflowing the tail slice.
- Saves each script image as `image-N.png/.jpg/.gif/.webp/.bin`, keeps one
  save per distinct base64 payload, and puts the frozen
  `[Image saved to <path> (<mime>, <bytes> bytes)]` label before the image.
- On a failed or failed-after-partial-output script, formats and saves the same
  way while keeping `isError=true`; a failed file write becomes the frozen
  label text and never a path that does not exist, and `fullOutputPath` is
  recorded only when the spill write succeeded.
- Without an injected environment keeps the frozen explicit
  `[Could not save the full output: no execution environment]` and
  `[Image (...) could not be saved: no execution environment]` outcomes, with
  no file references. The environment is checked before the recorded
  `outputDir` is read, matching frozen `outputFiles()`: a resumed run that
  already recorded a directory never writes through a missing environment.
- A cancelled run does not call the formatter, so it publishes neither a spill
  path nor an image path.

`Environment` gains the pure-virtual `create_temp_dir(prefix, stop)` seam, so
an implementation states whether it has a temporary output area; the local
adapter creates `/tmp/pi-codemode-XXXXXX` and the durable test doubles delegate
or report explicitly. No environment silently writes beside the workspace.

The frozen `OUTPUT_LIMITS` remain the visible envelope: the character budget is
at most `maxBytes / 4` UTF-16 units and truncation keeps at most twice the
half-budget line window, so the formatted text stays inside 4 MiB / 100,000
lines. The session and memory boundaries of #1094/#1090 are unchanged. No
image-generation or classifier provider path is added.

## Tests

- `tests/durable/HarnessCodemodeTest.cpp` adds
  `Durable codemode output matches frozen budgets spill and saved images`
  (eleven frozen sections), `Failed codemode output writes keep truthful labels`
  and `Cancelled codemode output publishes no file references`. They use the
  public Harness, a real `local_environment` temporary directory, an output spy
  that can fail or record the environment writes, and the captured family's
  expected content, details, spill bytes and image file bytes. The spill and
  image checks read the files back through `Environment::read`, so the
  comparison covers the same seam that wrote them. The cancellation case
  compares its visible result with the registered #1170 observation
  "cancelling the parent codemode call aborts its nested work" and asserts the
  environment was never asked to create a directory or write a file.

## Validation

Measured on checkout base `ef2f15034f2c4724ea289f08f76944e4ec4b0c0a`
(the current `origin/main`) with the default strict Debug preset.

- Focused Validation: `ctest --preset vcpkg -L '^issue1172'` selected **3**
  cases and passed **3/3** with **306** assertions (16, 268 and 22 per case);
  JUnit receipt `deliveries/1172-focused.xml` (sha256 `f06a969c…`).
- Owning module: `ctest --preset vcpkg -L durable -j1` selected **317** cases
  and passed **317/317**.
- Architecture: `ctest --preset vcpkg -L architecture` selected **43** and
  passed **43/43**.
- Full Validation: `TERM_PROGRAM=herdr PI_CHECKOUT=<frozen> ctest --preset
  vcpkg -j1` selected **4,162**, passed **4,162**, failed **0**, skipped **2**
  (780.84 seconds). The 10 live frozen-pi continuation cases ran and passed.
  The two skips require absent capture environments (a changed realtime clock
  and the Native TUI differential capture). Earlier unfiltered runs on the same
  inputs exposed two runner-sensitive cases that are not part of this slice:
  the documented #632 `[quarantine]` projection cost bound (172/173 functional
  assertions passed) and the pre-existing `Process Terminal atomic stop and
  default stop detect postdrain EIO on closed master` PTY race (`[issue1019]`,
  whose binary links only `cch_support`/`cch_tui`). The PTY case passed 15/15
  isolated runs, including three runs of its neighboring 11-case segment with
  the same environment, and the final full run passed both.
- #1090 delivery revalidation: the exact `^(issue1090|issue1101|issue1104)$`
  selection reran **36** selected, **36** passed. The changed
  `docs/agents/architecture.md` and
  `src/durable/include/cch/durable/Environment.hpp` inputs and the
  `deliveries/1105-existing-deliveries-revalidation.xml` JUnit were refreshed
  in `1058-deliveries.json`; the #1090 scenario mapping and selected cases did
  not change.
- Consolidation: `consolidate_audits.py build` and `check --pi-root <frozen>`
  passed (1,464 rows); `tests/evidence/ConsolidationAuditTest.py` ran **42**
  tests and passed. `audit_evidence.py check` accepts the unregistered
  `audit-1172` manifest (`5fd6060d…`); registration stays with the serial
  merger.
- `scripts/format-check.sh origin/main`, `git diff --check` and the captured
  `audit_evidence.py check` are clean.

## Capability status

No capability status or membership changes. The output-budget, spill and image
rows remain whole-row verification work for #1180; this slice supplies the
implementation and focused evidence only.

## Remaining boundary

The durable tool does not execute a classifier or image-generation Provider.
Script-image saving writes the bytes the script supplied; it does not resize,
orient or convert images through #1090's `ImageProcessor`. The output directory
is process-local temporary storage and the slice makes no power-loss, fsync or
multi-process deletion guarantee. The 256 MiB application heap limit recorded
as absent by #1094 is unchanged. Remote exchange replay remains #1099, the
ordinary application codemode store remains #1119, and #1044 remains open.

Two frozen edges are recorded rather than reproduced where the C++ value type
cannot carry them: a cut that falls between the two UTF-16 units of an astral
character keeps a lone surrogate in pi's JS string but becomes U+FFFD in UTF-8,
and pi's `atob` rejects invalid base64 where the durable formatter emits a
failed-image label. The guest rejects invalid `image()` data and the capture's
Unicode scenario is BMP, so neither edge is reachable through the public Harness
this slice delivers.
