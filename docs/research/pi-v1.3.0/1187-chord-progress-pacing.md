# #1187 Chord progress pacing and rate comparisons

## Scope

Close batch B4 of the [#1178](1178-chord-reconciliation.md) chord reconciliation: the progress
byte-rate constant and the pacing behaviour that had no timing-stable frozen observation. Authority:
[#1178](https://github.com/lanshengzhi/cpp-coding-harness/issues/1178),
[#1044](https://github.com/lanshengzhi/cpp-coding-harness/issues/1044),
[#1183](https://github.com/lanshengzhi/cpp-coding-harness/issues/1183),
[ADR 0075](../../adr/0075-align-selected-owner-packages-and-linux-capabilities-with-pi-v1-3-0.md),
Named Baseline **pi-v1.3.0** at `71023aec1546d6d677a65669c5665c0927ce0dd0`, Linux x86-64 with glibc.

Bounded scope: the `cch_chord` output helpers next to `bound_output`/`OutputBuffer`
(`src/chord/Output.cpp`, its Owner header and `tests/chord/OutputTest.cpp`), plus one supplemental
frozen capture and the receipt/ledger inputs. `src/durable`, `src/coding_agent`, `src/mcp` and the
`#1186` typed-JSON surface are untouched. The ordinary application Session gains no timer, and this
ticket does not wire the pacer into the durable harness tool output path; that consumer slice belongs
to the durable packet (#1180).

## Delivered behavior

### Frozen constant and timer-free pacing seam

pi's `Progress` (`packages/chord/src/output.ts`) paces commits with real timers: the first change
after an idle period commits at once, and each successful commit then delays the next one by
`max(minIntervalMs, bytes * 1000 / PROGRESS_BYTES_PER_SECOND)` milliseconds. A wall-clock
observation of that behaviour would measure scheduler lateness together with the pacing, so this
slice separates the pacing arithmetic from the clock instead of normalizing either away.

| Frozen pi surface | C++ seam (`src/chord/include/cch/chord/Output.hpp`) |
| --- | --- |
| `PROGRESS_BYTES_PER_SECOND = 100 * 1024` | `inline constexpr std::size_t kProgressBytesPerSecond = 100 * 1024` |
| `Progress.#nextAt` pause arithmetic | `ProgressTime progress_pause(std::size_t bytes, std::chrono::milliseconds min_interval)` |
| `Progress.#schedule`/`#flush` pacing state | `class ProgressPacer` (`mark`, `pending`, `next_commit_at`, `due`, `begin`, `committed`) |

`ProgressTime` is `std::chrono::duration<double, std::milli>`: pi's `#nextAt` is a JavaScript number,
so the byte-rate pause is a rational number of milliseconds. `1000 / 102400` reduces to `5 / 512`,
so the C++ quotient is exact for every byte count that fits in a `size_t` and agrees with the frozen
double result bit for bit. The pacer reads no clock and starts no timer; the caller supplies the
current time and runs the commit the pacer reports due. A mark coalesces into the next commit, and a
mark that arrives while a commit is running stays pending.

### Captured observation

One supplemental family, `audit-1187`, is captured at the same `pi-v1.3.0` revision by
[capture-1187.mts](../../../fixtures/pi-v1.3.0/audit-1187/capture-1187.mts) from the clean frozen
checkout. It drives the frozen `Progress` class under deterministic virtual timers — the observation
method the frozen unit test `packages/chord/test/output.test.ts:Progress` itself uses — and records
the exact commit schedule. It is **not yet registered** in
[baselines.json](../../../fixtures/pi-v1.3.0/baselines.json): registering a new family changes a
pinned input of five existing delivery records, so that refresh is the serial-merger step described
under §Remaining boundary.

| Scenario (`fixtures/pi-v1.3.0/audit-1187/chord-progress.json`) | Frozen observation |
| --- | --- |
| `progress commits the first change at once then paces later commits by their written bytes` | A 51 200-byte first commit at 0 ms buys 500 ms; a coalesced 10-byte commit at 500 ms buys the 100 ms minimum; the last 10-byte commit runs at 600 ms. |
| `progress keeps the exact fractional byte-rate pause instead of rounding it` | A 1 025-byte commit buys exactly `10.009765625` ms, not a whole millisecond and not a whole KiB. |

Both scenarios carry the explicit normalization that the schedule is the frozen pacing arithmetic
under virtual timers, never a wall-clock latency measurement, and that timestamps and byte counts are
recorded exactly with no tolerance.

### Complete delivered row (1)

`cch_chord:output:progress-rate` already had a registered frozen scenario in
`audit-1057/json-output-support.json` (`the progress byte rate is the frozen 100 KiB/s constant`,
`bytesPerSecond: 102400`). One delivery record, `1187-chord-progress-pacing`, in
[1058-deliveries.json](1058-deliveries.json), pins the production source, the comparison and
discriminating test sources, the frozen artifacts and manifests, the evidence tooling and the CTest
JUnit result. The row's `remainingBoundary` is empty.

| Row | Frozen artifact and scenario | Comparison case | Discriminating case |
| --- | --- | --- | --- |
| `cch_chord:output:progress-rate` | `audit-1057/json-output-support.json`: `the progress byte rate is the frozen 100 KiB/s constant` | `the progress byte rate is the frozen 100 KiB/s constant` | `progress pacing separates a byte-rate pause from a constant interval and rounded bytes` |

### `cch_chord:output:progress` — delivered, captured, not promoted

The pacing seam and its two frozen comparison cases are delivered in `tests/chord/OutputTest.cpp`,
but the row stays `missing` / `unverified`. Its source audit records no comparison scenario
(`scenario: null`), so the consolidation contract has no registered obligation to bind a receipt to;
promoting it needs the serial-merger registration of `audit-1187` in
[baselines.json](../../../fixtures/pi-v1.3.0/baselines.json) plus two `scenarioLinks` entries in
[consolidation-plan.json](../../../fixtures/pi-v1.3.0/consolidation-1058/consolidation-plan.json)
for ticket `1057`, row `cch_chord:output:progress`, naming the two scenarios above. A receipt then
claims the row against `audit-1187/chord-progress.json`. Registering a family also refreshes the
pinned `baselines.json` digest of the #1090, #1104, #1118, #1181 and #1189 receipts, which is why this
ticket stops at the capture and names the follow-up instead of performing a cross-owner pin refresh.

### Discriminating cases

The comparison cases separate the frozen schedule structurally: the first fails a per-chunk pacer
(which commits at every mark instead of at 0, 500 and 600 ms) and a constant-interval pacer (which
commits at 0, 100 and 200 ms). The second fails a pacer that rounds the pause to whole milliseconds or
rounds the accumulated bytes to whole KiBs, both of which read 10 ms instead of `10.009765625` ms.

The discriminating case `progress pacing separates a byte-rate pause from a constant interval and
rounded bytes` asserts the three pause results directly plus the pacer's due gate. The three
mutations were exercised by hand on the delivered tree before the receipt was written, and each one
failed exactly the named cases:

| Mutation | Failing cases |
| --- | --- |
| `progress_pause` returns `min_interval` (a millisecond constant, no byte rate) | both pacing comparisons and the discriminating case; the constant case still passes |
| `progress_pause` returns zero (publish per chunk) | both pacing comparisons and the discriminating case |
| the pause is truncated to whole milliseconds | the fractional comparison and the discriminating case; the 500/600 ms comparison still passes |

## Validation

Measured on this ticket's implementation commits `e168504be` and `70489aa5d` (the two-axis review
fix) and, after the `origin/main` merge, on the tree
`70489aa5d05945b7485949b6581658775146a76e` that the receipt pins:

| Command | Result |
| --- | --- |
| `cmake --preset vcpkg` | configured; Parity Architecture Gate PASS (3918 includes checked) |
| `cmake --build --preset vcpkg` | built; build-phase Parity Architecture Gate PASS |
| `ctest --preset vcpkg -N -L issue1187` | 4 selected |
| `ctest --preset vcpkg -L issue1187 --output-on-failure --output-junit docs/research/pi-v1.3.0/deliveries/1187-chord.xml` | 4/4 passed, 0 failed, 0 skipped |
| `ctest --preset vcpkg -N -L chord` | 88 selected on the merged tree (84 delivered + 4 from this ticket) |
| `ctest --preset vcpkg -L chord --output-on-failure --output-junit docs/research/pi-v1.3.0/deliveries/1178-revalidation-1187.xml` | 88/88 passed, 0 failed, 0 skipped |
| `ctest --preset vcpkg -L architecture` | 43/43 passed |
| `PI_CHECKOUT=… TERM_PROGRAM=herdr ctest --preset vcpkg -j1` | 4,242 selected, 4,242 passed, 0 failed, 2 skipped. The two skips are the recorded environment prerequisites (`CCH_TEST_REALTIME_JUMP` on the directly registered case, whose LD_PRELOAD wrapper passes, and `CCH_DIFFERENTIAL_SCENARIO`). An earlier serial run on the pre-review tree failed two load-sensitive codemode cases that passed 2/2 in isolation; no assertion, timeout or label exclusion was changed. |
| `python3 fixtures/pi-v1.3.0/consolidation-1058/consolidate_audits.py build` | ledger rebuilt; 59 delivery-backed rows |
| `python3 fixtures/pi-v1.3.0/consolidation-1058/consolidate_audits.py check` | PASS — 649 scenarios, 366 named by a row, 56 covered by a referenced artifact, 227 explicit orphans, 0 uncovered |
| `python3 tests/evidence/ConsolidationAuditTest.py` | 42 tests, all passed |
| `python3 scripts/evidence/audit_evidence.py verify --baseline pi-v1.3.0` | 55 bundles, 115 artifacts and 652 scenarios verified at `71023aec…`; `audit-1187` is not registered, so it was validated as a candidate instead (2 scenarios) |
| `scripts/format-check.sh <merge-base>` | clean |

A two-axis review of the delivered diff ran before the receipt. The Standards axis found one hard
`CODING_STANDARDS.md` §11.3 violation (`REQUIRE` on a boolean pacer state that cannot make later
execution unsafe) and two judgement calls: the byte-rate formula hard-coded `5.0 / 512.0` instead of
the declared constant, and a constant assertion repeated in the pacing comparison. The first two were
fixed in `70489aa5d` (`CHECK` and a `kProgressBytesPerSecond`-based quotient); the repeated assertion
stays because `bytesPerSecond` is a recorded field of the pacing scenario's frozen `expected`
object. The Spec axis found no missing requirement, no scope creep and no unsupported receipt claim.

The unfiltered run includes the two consolidation gate cases; they fail on a tree where the #1178
receipt still pins the pre-#1187 output sources, which is why the full run is reported on the
receipt tree. Any runner-sensitive failure in an unrelated owner is listed here rather than
normalized, and no assertion, timeout or label exclusion was changed. The `audit-1187` family is not
registered, so `audit_evidence.py verify` does not include it; it was validated as a candidate with
`audit_evidence.py check --manifest fixtures/pi-v1.3.0/audit-1187/manifest.json`, which reported the
exact registry record the serial merger applies.

## Capability status

One row moves from `missing` / `upstream-captured` to `implemented` / `verified` through the
validated delivery record:

- `cch_chord:output:progress-rate`

`cch_chord:output:progress` keeps its recorded `missing` / `unverified` status: the pacing seam and
its frozen comparison cases are delivered, but the row cannot be promoted before `audit-1187` and
its scenario links are registered. Membership is unchanged: 60 supported and 5 excluded `cch_chord`
rows before and after, and no neighboring owner's row is promoted.

## Remaining boundary

1. Serial merger: register `audit-1187` in `baselines.json` (manifest SHA-256
   `409185e746586ce6fef786f3d78755a61ae71faeadb913303b8d16b35056f7c1`), add the two `scenarioLinks`
   for `cch_chord:output:progress`, refresh the five existing receipts that pin `baselines.json`, and
   promote `output:progress` with a receipt against `audit-1187/chord-progress.json`.
2. Durable packet (#1180): consume the pacer from the durable harness tool output path. This ticket
   deliberately adds no timer to the ordinary application Session and changes no durable source.

No other row is accepted by this packet. [#1178](https://github.com/lanshengzhi/cpp-coding-harness/issues/1178)
batches B1/B2/B3/B5/B6 and [#1044](https://github.com/lanshengzhi/cpp-coding-harness/issues/1044)
stay open. Magpie-only AI access, XDG state roots, strict no-exception behavior, the ordinary
application Session, all explicit AI/platform exclusions and the no-historical-Pike-durable-migration
boundary remain unchanged.
