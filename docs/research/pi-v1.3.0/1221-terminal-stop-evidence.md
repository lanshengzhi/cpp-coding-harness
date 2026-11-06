# Issue 1221: terminal stop evidence clarification

## Scope

This note clarifies the modifyOtherKeys finding in
[1182-tui-reconciliation.md](1182-tui-reconciliation.md) and the B13 ticket. It
makes no capability promotion or delivery-receipt claim.

## Frozen observation

The registered `audit-1050/program-status-detection.json` capture records a
modifyOtherKeys disable at stop. In every captured scenario,
`program-status-child.mts` sends a device-attributes reply before `stop()`;
there is no Kitty flags reply. The frozen pi-v1.3.0 source at revision
`71023aec1546d6d677a65669c5665c0927ce0dd0`,
`packages/tui/src/terminal.ts`, enables modifyOtherKeys on that reply and its
`disableModifyOtherKeys()` returns without writing if the mode is inactive.
Consequently the captured disable is conditional on the earlier negotiation;
the capture does not establish an unconditional restore or a Kitty-negotiated
stop sequence. The contrary Kitty inference and unconditional-restore remedy
in the #1182 finding do not follow from that capture.

The C++ fallback stop now pops Kitty before disabling modifyOtherKeys. The
program-status PTY comparison waits for the fallback, then compares the entire
concatenated recorded stop stream. A separate Kitty-negotiated PTY control
checks that no inactive fallback is disabled. The latter is supported by the
frozen source, not by a newly registered frozen runtime capture. PTY output is
not real terminal-emulator evidence.

## Validation

The owning TUI shard built and the fallback, captured program-status stop, and
Kitty-control selections passed (3 cases). The incremental full build passed.
The full CTest run selected 4,515 cases and failed four, with 11 skipped: two
consolidation checks lacked the lane identity in their environment, one
concurrent durable-edit comparison failed, and a Herdr PTY evidence case required
`TERM_PROGRAM=herdr`. A subsequent selection with the owning lane environment
passed both consolidation checks, the durable-edit case, and both program-status
cases (5 cases). This rerun does not turn the full suite into a pass; Herdr and
the skipped cases remain unverified in this environment. The independent
standards and specification reviews required by the implementation workflow
remain outstanding.

## Capture preflight

The local `../pi` checkout is clean at the required full revision and its
`node_modules/.bin/tsx` is available. A preflight invocation of
`fixtures/pi-tui/capture/capture-named-tui.mts` with `PI_BASELINE=pi-v1.3.0`
failed during transformation, before any output was written: the script has
an extra `] as const;` at line 915. This defect predates the #1221 commits.
Even after repairing that syntax, its separate TUI registry currently contains
only `pi-v1.0.4`, so the requested baseline is not selectable there. The
script also writes the entire selected bundle and updates its registry; it is
not a safe targeted recapture of the three B13 families. A dedicated v1.3.0
capture and registration, with review of affected receipt inputs, is still
needed. The existing `audit-1050` observations remain registered evidence,
but they do not cover the complete B13 comparison.

The local TUI test shard and the incremental full build passed, and the three
focused stop tests passed on this checkout. The subsequent unfiltered CTest run
selected 4,515 cases: two failed and 11 skipped (exit 8). The quarantined
projection cost test measured 2.279 ms of processor time against its 2 ms
bound; the Herdr PTY test required `TERM_PROGRAM=herdr`, which was unset.
These checks do not establish emulator behavior or whole-row verification.

## 2026-11-06 capture continuation

The duplicate closing `] as const;` in the historical named TUI generator was
removed. A v1.3.0 invocation now reaches its baseline guard and rejects the
unregistered baseline before writing output. The existing v1.0.4 manifest's
generator digest describes the original captured source bytes, not the repaired
working copy; the historical artifact verifier still verifies all 36 artifacts.

A separate candidate `fixtures/pi-v1.3.0/audit-1221` executes the clean frozen
checkout at the registered revision. It records two key identity scenarios and
three input framing scenarios. In that capture, complete recognized OSC and APC
reply headers remain buffered across a 45 ms wait with a 15 ms timeout, while
ambiguous Alt-bracket input is released. The candidate passes `audit_evidence.py
check`, but is **not registered**: the shared registry is pinned by existing
delivery receipts, whose exact selections and changed inputs must be individually
reviewed and revalidated before registration. No prior receipt was rehashed.
The capture does not cover `utils-ansi.json`, terminal startup or stop, or the
complete keybinding surface. Its timer observations depend on host scheduling.

The incremental build passed; eight existing focused shifted-symbol, fragmented
reply and terminal fallback tests passed. They do not read this candidate bundle
and are not new frozen comparisons or receipt evidence. The existing registered
v1.3.0 verifier passed (76 bundles, 168 artifacts, 981 scenarios). No complete
full-suite result was obtained in this continuation. Independent standards and
specification reviews remain outstanding.

## 2026-11-06 registration and comparison continuation

The original `audit-1221` candidate bytes from commit `09fed85c1` are now
registered without recapture. The verifier reports 77 bundles, 170 artifacts
and 986 scenarios. Its five scenarios are explicitly classified as supplemental
orphans; none promotes a row. `KeysTest.cpp` reads both key scenarios and
compares every captured identity, repeat/release flag and positive and negative
binding result. `StdinBufferTest.cpp` reads all three input scenarios, checks
the captured output before and after completion, and models the terminal's
deadline-driven flush. It does not claim a real timer or emulator observation.

Registration changed inputs pinned by 23 existing receipts. Each receipt's
recorded selection was rerun into a separate JUnit file, with the previous
artifact retained: 694 test executions passed in total, counting selections
that overlap. Only the changed registry and plan input digests were refreshed;
each receipt records its prior validation and the reason. The ledger retains
all 1,464 row memberships, implementation and verification states, comparison
scenarios, evidence routes and source rows. Consolidation build and check pass
with 321 explicit orphans and no uncovered scenarios. The 76 consolidation
boundary tests and the 30-receipt artifact-boundary check pass. The two new
focused comparisons pass after the full incremental build. The unfiltered CTest
run selected 4,517 cases: one failed and 11 skipped (exit 8). The failure was
`real PTY records Herdr markdown and OSC 52 terminal bytes`, which requires
`TERM_PROGRAM=herdr`; it was unset. An isolated rerun failed at the same host
precondition. The skipped cases include native TUI and cross-runtime evidence,
so this run does not establish those environments' behavior. Formatting and
`git diff --check` passed. Independent standards and specification reviews
remain outstanding; this implementation sub-agent performed a local review.

## 2026-11-06 startup, stop and ANSI candidate

A separate `audit-1221-terminal-ansi` candidate executes the clean frozen
checkout and pins its capture script and artifacts. It records startup, input
negotiation and stop writes for no reply, a device-attributes reply and a Kitty
flags reply. In the captured process-stream harness, all three starts write
bracketed-paste enable followed by the combined keyboard/status/device query.
Only device attributes cause modifyOtherKeys enable and a corresponding stop
reset; Kitty flags and no reply do not. These are process-stream observations,
not terminal-emulator evidence. A native PTY comparison checks the captured
startup writes, negotiation writes and exact concatenated stop bytes for all
three paths. It does not check the entire startup stream or all forwarded input;
a complete mapped comparison remains outstanding.

The candidate also records visible width, stripping, wrapping, truncation and
strict column slicing for plain text, an SGR boundary and a wide hyperlink.
`ANSI utilities compare the pi 1.3.0 candidate observations` reads the checked
candidate and compares all 15 results, including exact ANSI bytes; it passes.
Candidate manifest and artifact integrity pass `audit_evidence.py check`.
The registered baseline verifier still passes 77 bundles, 170 artifacts and 986
scenarios. The two new comparisons passed, as did the four-test focused
selection including the existing fallback and Kitty controls. The incremental
full build passed. The unfiltered CTest run selected 4,519 tests: 4,505 passed,
three failed and 11 skipped. Two consolidation tests failed because the run
omitted `CCH_LANE_NAME` while this lane held the lock; both passed on rerun with
`CCH_LANE_NAME=issue-1221` (including 76 boundary cases). The remaining Herdr
PTY case failed its `TERM_PROGRAM=herdr` host precondition, which was unset.
Formatting and `git diff --check` passed. A local standards and specification
review found the partial startup and input comparison described above; no
independent reviews were performed.

These observations are not registered and do not constitute a delivery receipt.
Registration still requires review and revalidation of the existing receipt
inputs affected by the registry and consolidation-plan changes, followed by
consolidation and boundary checks. No row is promoted on this evidence.

## 2026-11-06 terminal and ANSI registration continuation

The unchanged `audit-1221-terminal-ansi` candidate is registered as six
supplemental orphan scenarios: three process-stream startup/negotiation/stop
paths and three ANSI inputs. Both C++ comparisons now load the artifacts through
the registered evidence reader. The ANSI comparison checks all five captured
results for each input; the terminal comparison checks selected startup writes,
negotiation writes and the full concatenated stop stream, but not the complete
startup stream or forwarded input. Neither comparison is a whole-row receipt.
The evidence verifier reports 78 bundles, 172 artifacts and 992 scenarios.

The registry addition and six orphan entries do not change any existing
receipt's row mapping or frozen artifact identity. All 23 receipts pinning one
or both changed inputs were revalidated with their recorded selections: 694
passing executions including overlaps, with new JUnit files and prior results
retained. Only the two changed input digests were refreshed in those receipts.
Consolidation build and check passed: all 1,464 row objects remain identical,
with 327 explicit orphans and no uncovered scenarios. The 76 consolidation
boundary tests and the static artifact-boundary check of 30 receipts passed.
The TUI shard and full incremental build passed; the two registered comparison
cases and three candidate controls passed. The unfiltered CTest run selected
4,519 tests: 4,507 passed, one failed and 11 skipped (exit 8). The failing
Herdr PTY evidence test requires `TERM_PROGRAM=herdr`, which was unset. The
skipped native TUI and cross-runtime cases do not supply those environments'
evidence. The static boundary check does not prove semantic completeness.

For a future whole-row review, existing distinct controls worth evaluating are
`shifted-symbol reports consume the implicit shift and keep the physical spelling`
against the key-identity replay, `StdinBuffer recovers ambiguous Alt-bracket
input within the sequence timeout` against the fragmented-reply replay, and
`Process Terminal does not disable an inactive keyboard fallback after Kitty
negotiation` against the terminal negotiation replay. These are candidate
controls, not accepted #1229 receipt mappings: each still needs a review against
the exact row and its complete scenario set. The local keybinding conflict test
checks candidate-order behavior for two contexts; its built-in default-table
comparison names revision `83114817`, not the required v1.3.0 revision.

## 2026-11-06 keybinding candidate continuation

A separate, unregistered `audit-1221-keybindings` candidate records the frozen
v1.3.0 built-in definitions and four manager scenarios: deduplicated defaults,
user conflicts and disabled keys, an unknown user claim overlapping a default,
and replacing user bindings after a conflict. The candidate manifest and
artifact pass `audit_evidence.py check`. Two C++ comparisons exercise matching,
resolved keys, conflict count and key, and replacement through the public
registry/context seam. An independent test checks that an override overlapping
an untouched default creates no user conflict while both actions still match.
The C++ conflict diagnostic does not expose pi's claimant list; the comparisons
do not establish parity for that field, user-binding accessors, global manager
state, all inputs, or every detail of replacement. The frozen built-in table is
captured but not fully compared: for example, pi's editor page-up and page-down
defaults include control-modified keys, and pi includes `tui.input.copy` and
additional fullscreen actions. Pike's regular-mode table targets an older
revision and splits or leaves some actions unassembled. No whole-row claim or
receipt is made. Registering this candidate would change the shared registry
and requires revalidation of the affected receipts before any promotion.

The focused comparisons and independent control pass (three tests); the full
incremental build and registered evidence verifier pass. The full CTest run
selected 4,522 tests: 4,509 passed, two failed and 11 were skipped. The MCP
server-death test passed on an isolated rerun; the Herdr PTY case requires
`TERM_PROGRAM=herdr`, which was unset. This rerun does not make the full suite
a pass. Real terminal-emulator behavior remains unverified; the prior
process-stream and PTY evidence does not establish it. Terminal residuals
remain with #831 and #1035. Independent standards and specification sub-agent
reviews were not performed in this implementation sub-agent; a local review
found the scope limitations above.

## Remaining boundary

Ticket #1221 remains open: the captured observations do not compare all 110
rows with normal and distinct discriminating cases, provide whole-row delivery
receipts, or establish real-emulator behavior. Keybinding tests include local
resolution and conflict cases, but the available capture does not compare the
complete keybinding surface against the named frozen baseline. Each row still
needs its complete audit and the #1229 distinct discriminating case. PTY and
process-stream bytes do not prove emulator behavior; terminal residuals on #831
and #1035 remain with those owners. No capability status is promoted by this
continuation.
