# #1201 terminal primitives recaptured at pi-v1.3.0

## Scope

Recapture the 30 historical pi-v1.0.4 terminal primitives at the frozen
pi-v1.3.0 baseline (`71023aec1546d6d677a65669c5665c0927ce0dd0`) and compare the
C++ observations against the new frozen family. The 30 rows
(`EditorComponent*`, `NativeClipboard*`, `RgbColor*`, `TerminalColor*`,
`getNativeClipboard`, `parseTerminalColorSchemeReport`, `setImageTranscoder`)
were recorded as `upstream-captured` against the historical
`fixtures/pi-tui/bundles/pi-v1.0.4` family, which is not an observation of the
named v1.3.0 baseline and could not back a v1.3.0 receipt.

## Delivered behavior

- The `audit-1201` family captures the v1.3.0 behavior of the four primitive
  groups by executing the frozen checkout's own modules through their public
  seams: the OSC color parser and the public `queryTerminalColors` assembly
  (`terminal-colors.json`), the concrete `Editor` driven over a virtual
  terminal (`editor-component.json`), the Linux clipboard availability and
  read outcomes (`native-clipboard.json`), and the Kitty non-PNG transcoder
  lifecycle (`image-transcoder.json`).
- Each artifact carries one cross-baseline record naming what the historical
  pi-v1.0.4 bundle recorded (the `index-export-accounting` export list and the
  v1.0.4 source digest) beside the frozen source digest, so a reviewer sees in
  one place that all four source files are byte-identical between the two
  baselines. No primitive changed; nothing was normalised.
- The 1050 toolkit ledger rows for the 30 primitives now name the audit-1201
  scenarios; the plan links the `setImageTranscoder` lifecycle scenarios and
  classifies the four cross-baseline provenance records as explicit orphans.
  Expectations came only from executing the frozen checkout; nothing is
  derived from C++.
- The C++ comparisons live in the owning suites as `[issue1201]` cases that
  read the registered artifacts through `read_pi_v130_audit_evidence`:
  `TerminalColorsTest.cpp` (parser replies, the sixteen-reply palette
  assembly, scheme reports), `EditorTest.cpp` (the capability surface, typing
  / history / paste, the autocomplete cap and the padding frame),
  `NativeClipboardTest.cpp` (availability and the unavailable outcomes), and
  `ImageTranscoderTest.cpp` (fallback / convert / retry / clear).

## Validation

- Capture: `PI_CHECKOUT=<frozen> PI_BASELINE=pi-v1.3.0 <frozen>/node_modules/.bin/tsx fixtures/pi-v1.3.0/audit-1201/capture-1201.mts`;
  `audit_evidence.py check` passes; the family is registered in
  `fixtures/pi-v1.3.0/baselines.json`.
- Focused comparison selection: `ctest --preset vcpkg -LE architecture -L issue1201`,
  8/8 passed (`deliveries/1201-terminal-primitives.xml`), run at
  `db59d4bc26582da844c089f84d2e1dcbf3da16a1`.
- The ten receipts that pin the plan or the registry were revalidated under
  `revalidation1201` with their exact selections rerun into fresh JUnit
  artifacts (36/36, 19/19, 89/89, 89/89, 13/13, 16/16, 52/52, 5/5, 9/9, 36/36).

## Capability status

All 30 rows advance from `unverified` / historical-v1.0.4 evidence to
delivery-backed `verified` against the registered audit-1201 family. No
membership, implementation scope or neighboring row changed.

## Remaining boundary

The historical pi-v1.0.4 family stays registered under its own baseline; no
relabelling occurred. The clipboard `null` (empty) and transfer-failure
(reject) outcomes need a live X server, which the headless capture host lacks;
the interface spells them and the unavailable outcome is captured dynamically.

## 2026-11-06 disposition (#1229)

Retro-review blocker #1229 reconciled this receipt's evidence integrity. The
25 claims whose `discriminatingCase.test` string-equals one of the same
row's `comparisons[].test` entries now carry a non-empty `remainingBoundary`
entry in `1058-deliveries.json`: each claim stays validated but bounded and no
longer promotes its row, superseding the intermediate #1236 per-claim
withdrawal record for this receipt (removed; preserved in git history). The
rows revert to their source-audit status until a genuinely discriminating
negative/control/boundary case is delivered. No cited test, pinned input,
scenario mapping or routing digest changed; see
[1229-withdrawn-1209-root-tui-lifecycle-claims.json](1229-withdrawn-1209-root-tui-lifecycle-claims.json)
for receipts that also carry a #1229 withdrawal.
