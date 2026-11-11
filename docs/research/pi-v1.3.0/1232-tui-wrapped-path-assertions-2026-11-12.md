# TUI path assertions under a long temporary root (2026-11-12)

## Scope

Close the adjacent finding recorded in
[1232-root-disk-timing-diagnostics-2026-11-11.md](1232-root-disk-timing-diagnostics-2026-11-11.md)
§Adjacent finding, and the same observation in the record for commit
`747448831` (the reserved-lane sizing change): six TUI cases were reported to
fail when the Catch2 isolation root — which since #1232 follows `TMPDIR` — is
long enough. The work is confined to the test suite: no production source
changes, no capability claim. Measured on this tree, the deep root failed
**fifteen** cases, not six.

The two product rules the assertions now name instead of guessing are unchanged
and remain pi-parity:

- **A single-row consumer truncates.** The footer pwd row is pi
  `truncateToWidth(pwd, width, "...")` (`src/coding_agent/tui/Footer.cpp`): one
  row, the leading cells, the ellipsis inside the frame width. It is not a
  wrap and it does not abbreviate the middle.
- **A wrapping `Text` breaks the line.** A dialog title, a status message and
  the editor render through `cch::tui::Text` → `cch::tui::wrap_text`: an
  absolute path with no break opportunity breaks at the frame width onto
  consecutive rows, and every wrapped row repeats the component's one-column
  left indent.

So each failure below is a **test assumption**, not a product defect: the
assertion asserted a contiguous substring that only exists while the path fits
one row. No footer/export/clipboard product behavior changed.

## Reproduction

Deep root (btrfs root disk), the leaf name makes the isolation root ~180
characters:

```
TMPDIR=/var/tmp/cch-deep/<long-leaf>/cpp-coding-harness-tui-deep-path-…-width
ctest --preset vcpkg -L tui -j 4      # 1939 selected, 15 failed
```

Short root, same filesystem:

```
TMPDIR=/var/tmp/cb/run1               # 1939 selected, 1 failed (#4207 only)
TMPDIR=/var/tmp/cb/run9               # after the fix: 1940 selected, 0 failed
```

The single short-root failure is the point of the record: the boundary is not
one length. `/var/tmp/cb/run1` yields a 97-character workspace path, which fits
the 100-column footer but not the 105-character `…/vanished` path in the
missing-cwd prompt (#4207). Each component sets its own threshold, so a
"temporary root long enough" reproduction has to be per assertion, and the fix
must not rely on any threshold.

## Failures, original assertion, new assertion

`tests/support/FrameText.hpp` (new) holds the two oracles:
`truncated_row(text, width)` / `truncated_row_prefix(text, width)` for the
single-row form, `wrapped_rows(text, width)` for the row segments a raw PTY
stream carries, and `rows_show_text(rows, width, text)` for "the whole text a
user reads, allowing the renderer's row breaks". The oracles are spelled in the
test tree rather than called from `cch::tui`, so a product change to either rule
fails the screen case instead of travelling with it.

### Footer pwd row (truncation)

| CTest case | Before | After |
| --- | --- | --- |
| `Dock footer rows survive an over-budget fullscreen dock` (`tests/coding_agent/tui/DockFooterFrameTest.cpp`, `assert_footer_rows` / `assert_fullscreen_bottom`) | `CHECK(path_row.find("cpp-harness") != npos)` and the `penultimate` / `last` twins on the physical bottom rows | `CHECK(path_row == tests::truncated_row(workspace_path, terminal.dimensions().columns))`, plus the existing `(auto) == npos` guard that keeps the row identified as the pwd row |
| `Dock footer rows survive autocomplete, multi-line, submit, and resize frames` (same sites) | same | same |
| `Native TUI footer renders usage totals, cache hit rate, context, and the model` (`tests/coding_agent/tui/FooterStatusInteractiveTest.cpp:280`) | `CHECK(screen.find(workspace.path().string()) != npos)` | `CHECK(screen.find(tests::truncated_row(workspace_path, 100)) != npos)` |
| `Native TUI fullscreen transcript keeps its scroll window across a mode round trip` (`tests/coding_agent/tui/TuiModeSwitchTest.cpp:1631`) | `screen[19]` contains `fake-model` **or** `cpp-harness` | `screen[19] == tests::truncated_row(workspace_path, 48)` |
| `CLI --tui-mode launches the requested Native TUI root and the no-option default is fullscreen` (`tests/cli/CliSmokeTest.cpp:455,919`) | PTY drain needle `{workspace.path().string()}`; `enter_alternate < output.find(workspace_path)` | needle `{tests::truncated_row_prefix(workspace_path, 100)}`; same ordering check against that prefix |
| `CLI stored tuiMode selects the product root through startup settings on a real PTY` (same helper) | same | same |
| `CLI --tui-mode overrides an opposing stored mode through product startup on a real PTY` (same helper) | same | same |

### Wrapped `Text` (dialog title, status line, editor)

| CTest case | Before | After |
| --- | --- | --- |
| `resuming a session whose stored cwd is gone prompts and resumes in the current cwd` (`tests/coding_agent/tui/SessionSelectorInteractiveTest.cpp:634`) | `CHECK(screen.find(missing_cwd.string()) != npos)` | `CHECK(tests::rows_show_text(screen, columns, missing_cwd.string()))` |
| `startup TUI: the boot missing-cwd prompt shows Continue/Cancel with pi's verbatim text` (`tests/cli/StartupTuiTest.cpp:369,372`) | `CHECK(screen.find(vanished/fallback.path().string()) != npos)` | `CHECK(tests::rows_show_text(screen, columns, path))` |
| `/import replaces the current session with the imported one` (`tests/coding_agent/tui/ImportSlashCommandInteractiveTest.cpp:292` and the two `wait_for_status_path` callers at `:345`, `:374`) | `wait_for_screen(running, "Session imported from: " + path)` on a whitespace-collapsing view | `wait_for_status_path(running, "Session imported from:", path)`: the label on the collapsed view **and** `rows_show_text` for the whole path |
| `an import that collides with a stored session file lands beside it` (same) | same | same |
| `/import on a missing file reports pi's error and keeps the current session` (same, `"Failed to import session: File not found:"`) | same | same |
| `Native TUI fullscreen exit rejects a late valid clipboard image payload arriving after Close` (`tests/coding_agent/tui/FullscreenExitOutputTest.cpp:1950,2028`) | `visible_screen(...).find("pi-clipboard-") != npos`, and the rejected-payload twin `== npos` | `tests::rows_show_text(screen, columns, "pi-clipboard-")`, and `CHECK_FALSE` of the same |
| `CLI interactive boot Continue recovers a vanished session cwd` (`tests/cli/CliSmokeTest.cpp:883,901`) | PTY needles `{original.path(), storage.path()}` and `{storage.path(), "deepseek-v4-flash"}` | needles `tests::wrapped_rows(path, 100)` for the prompt title, and `tests::truncated_row_prefix(storage_path, 100)` for the footer after Continue |

### Search filter (a different assumption: path *content*, not length)

`session selector search, named filter, and scope toggle work through the input
seam` (`tests/coding_agent/tui/SessionSelectorInteractiveTest.cpp:331`) typed
`parser` and required the other session's row to disappear. pi's session search
matches `id + name + allMessagesText + cwd` with fuzzy token matching, so a deep
root whose components spell `p…a…r…s…e…r` (`…/reproduction-…`) legitimately
matches the row: the negative assertion depended on the caller's temp path
spelling, not on the message. The query is now the quoted phrase `"parser bug"`,
whose space cannot reappear inside a temp path, so the row set is again a
property of the message. The matcher itself stays pinned by
`SessionSelectorSearchTest`; the input seam and the "only the matching row
remains" property are unchanged.

## Discriminating power

- **Footer rows.** `== truncated_row(pwd, width)` is byte-exact over the whole
  row and is derived from the path the test owns, so it is strictly stronger
  than a substring: it fails on a wrong retained prefix, a middle ellipsis, a
  missing ellipsis, a wrap, a `~` shorthand, or a wrong frame width. The
  `find("cpp-harness")` form it replaces passed at `/var/tmp/cb/run1` only
  because the 47-character cut happened to reach `…/cpp-harness-catch2-…`, and
  failed at `/var/tmp/cch-deep/…` where the cut lands in `cpp-coding-harness`.
- **Wrapped paths.** `rows_show_text` keeps "the whole path is on screen": the
  needle must tile exactly, so a renderer that truncates the tail, drops a
  segment, or abbreviates the middle still fails. It is stronger than the
  basename alternative (the tail alone would not prove the head is intact) and
  it cannot be assembled from two unrelated rows, because a row break is only
  crossed from a row that filled the frame (that is the only place `wrap_text`
  breaks a space-free text). `tests/support/FrameTextTest.cpp` pins both
  directions — one rendering it must accept and three it must reject, including
  `abcdef` across two short rows.
- **The rejected-payload twin.** The negative clipboard check had the same
  weakness in the other direction: with the plain row join, a wrapped
  `pi-clipboard-…` path would also have made `== npos` pass. It now uses the
  same oracle, so a leaked payload is caught whatever its length.
- **Perturbations run as the acceptance case.** Changing `Footer::render` to keep
  the *tail* of the pwd (temporary, reverted) failed 8 of the updated cases
  (both dock-footer cases, `Footer truncates the pwd and right side to the
  width`, the footer usage case, the fullscreen scroll-window case, and the
  three CLI PTY cases). Rendering only the first wrapped row in
  `cch::tui::Text::render` (temporary, reverted) failed 6 of the updated cases
  (both `/import` plus the colliding import, the missing-cwd resume, the CLI
  continue-recovery, and the startup missing-cwd prompt). Both perturbations are
  the shape a future "path display" regression takes, and neither could have
  been caught by the substring form when the path fits one row.

## Rules pinned with literal expectations

- `Footer truncates the pwd and right side to the width`
  (`tests/coding_agent/tui/FooterTest.cpp`) now asserts the exact row
  `/very/long/working/directory/path/tha...` at width 40, the visible width, that
  the dropped tail is absent, and it gained
  `Footer renders a pwd that fits the width unchanged` (`/srv/app`, no
  ellipsis). TUI cases assert the same form through the independent oracle, so
  the rule and its users fail together when the rule moves.
- The wrap half of the rule was already pinned with literals by
  `wrap_text pushes a whitespace-only prefix before a long token`
  (`ABCDEFGH` at width 4 → `ABCD`, `EFGH`) and
  `wrap_text reproduces the frozen wrap boundary order`; neither was changed.

## Golden files

None changed, and none was re-recorded. The change touches no file under
`fixtures/` and no `*Golden*.cpp` expectation; the only new artifact is the
receipt JUnit described below. The basis is a measurement rather than an
assumption: the full `-L tui` selection is green under both roots with the
golden files untouched, which it could not be if any golden encoded the
temporary root (a golden carrying a deep path would fail on the short root and
vice versa). No TUI screen golden existed with a temp-root-dependent row to
update, so "fix the golden" was not on the table for any of the fifteen.

## Validation

All runs on this checkout with the lane held (`scripts/dev/with-lane.sh`).

| Run | Selection | Result |
| --- | --- | --- |
| before, deep root | `-L tui`, `-j 4` | selected 1939, passed 1923, skipped 1, **failed 15**, 178.65 s |
| before, short root (`/var/tmp/cb/run1`) | `-L tui`, `-j 4` | selected 1939, passed 1937, skipped 1, failed 1 (#4207), 178 s |
| focused, deep root, 3× | 42-case regex (the 15 cases + the new oracle/footer cases) | 100% each run |
| focused, short root (`/var/tmp/cb/run6-{1,2,3}`), 3× | same 42-case regex | 100% each run |
| `-L tui`, deep root (`/var/tmp/cch-deep/run10/<leaf>`) | 1940 cases, `-j 4` | selected 1940, passed 1939, skipped 1, **failed 0**, 63.41 s |
| `-L tui`, short root (`/var/tmp/cb/run11`) | 1940 cases, `-j 4` | selected 1940, passed 1939, skipped 1, **failed 0**, 60.56 s |
| Full Debug suite (`ctest --preset vcpkg -j 4`, `TERM_PROGRAM=herdr`) | 4604 cases | selected 4604, passed 4592, skipped 11, failed 1, 275.30 s |

The two `-L tui` rows are the post-`clang-format` reruns of the committed tree; the
same two labels were also green immediately before the formatting pass
(62.37 s deep, 60.06 s short).

The single full-suite failure is **not from this change**:
`DaemonClient's daemon observes stdin EOF and exits` (`durable`, `issue1232`)
lives in `tests/durable/EnvIoListingDifferentialTest.cpp`, which carries another
lane's uncommitted work in this checkout (lane `issue1232-closers`, lock taken at
`2026-11-11T16:20:14Z` while this record's lane was between commands). That
file is excluded from this commit; the case aborted in the shared binary the
other lane was rewriting. Every TUI, CLI and support case passed.

One adjacent flake was observed once and is not attributable to this change: in
the first post-fix deep-root `-L tui` run, `E2E: a focused-editor submission
streams a scripted-runtime turn in the pi shape` failed its byte-exact golden
comparison with `Done: the notes file now says beta.` painted twice — a repaint
duplication under `-j 4` load, in a test this change does not touch and at a
fixed `/tmp/cpp-harness-e2e-workspace` (no temp-root dependence). It passed
30/30 assertions in isolation on both roots, 3+3 runs, and in the next two
full-label and full-suite runs. Not fixed here; it needs its own diagnosis.

Consolidation: `consolidate_audits.py build` wrote 1464 rows; `check` PASS
(1464 rows, 1009 scenarios, 331 explicit orphans, 0 uncovered). The three
`cch_pi_v130_consolidation*` / `cch_pi_v130_reconciliation_cli` CTest cases pass.
`scripts/format-check.sh` is clean.

Delivery receipt: `1210-regular-renderer` pins
`tests/coding_agent/tui/FullscreenExitOutputTest.cpp` and
`tests/coding_agent/tui/TuiModeSwitchTest.cpp`, which this change edits. Its two
input digests were refreshed, the exact `-L issue1210` selection (10 cases) was
rerun into
[deliveries/1210-regular-renderer-revalidation-test-tui-frame.xml](deliveries/1210-regular-renderer-revalidation-test-tui-frame.xml)
(10 selected, 10 passed), and the previous validation block is retained under
`revalidationTestTuiFrame.previousValidation`. No row, scenario mapping or
discriminator changed. `1058-consolidated-capability-ledger.json` was rebuilt
from that receipt.

## Capability status

No capability row advances. This is a test-suite correction: the assertions now
name the rendering rule their component uses instead of an accidental property
of a short temporary path.

## Remaining boundary

- **The search filter still matches the absolute cwd.** The E2E case now avoids
  the collision, but a user query can still legitimately match a session through
  its cwd path letters (pi behavior, pinned by `SessionSelectorSearchTest`).
  Nothing to fix; recorded so the next negative screen assertion about a search
  result is not written as if only messages matched.
- **Other screen assertions could still be one-row-dependent.** The survey
  covered every `screen.find(<path>)` / PTY path needle in `tests/`
  (`StartupTuiTest`, `CliSmokeTest`, `FooterStatusInteractiveTest`,
  `SessionSelectorInteractiveTest`, `ImportSlashCommandInteractiveTest`,
  `FullscreenExitOutputTest`, `DockFooterFrameTest`, `TuiModeSwitchTest`). The
  remaining path-substring assertions elsewhere compare non-rendered bytes
  (system-prompt text, shell output, `InstallRelocationTest` log lines) and are
  unaffected by row layout. A future case that asserts a path on a screen should
  use `tests/support/FrameText.hpp`.
- **Concurrent lanes share one build tree.** A full-suite run reflects
  whatever a concurrent lane has in the working tree; the lane lock serializes
  the commands but not the edits. The one failure above is that shape.
- CI still does not export `TMPDIR`, so neither this reproduction nor its fix is
  exercised there (the #1232 boundary, unchanged).
