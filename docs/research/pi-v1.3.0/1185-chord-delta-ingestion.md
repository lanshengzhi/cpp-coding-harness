# #1185 Chord delta ingestion and applier identity comparisons

## Scope

Close the two `cch_chord` delta comparison seams that #1178 left blocked, and register whole-row
comparison receipts for the five rows those seams name. Authority:
[#1178](https://github.com/lanshengzhi/cpp-coding-harness/issues/1178),
[#1044](https://github.com/lanshengzhi/cpp-coding-harness/issues/1044),
[#1183](https://github.com/lanshengzhi/cpp-coding-harness/issues/1183),
[ADR 0075](../../adr/0075-align-selected-owner-packages-and-linux-capabilities-with-pi-v1-3-0.md),
Named Baseline **pi-v1.3.0** at `71023aec1546d6d677a65669c5665c0927ce0dd0`, Linux x86-64 with glibc.

The blockers were representational, not observational: the frozen captures record rejections and
identity facts that the delivered C++ API had no spelling for.

| Row | Frozen scenario | Previous blocker |
| --- | --- | --- |
| `cch_chord:delta:op-validation` | `unsafe prototype segments and malformed operation tuples are rejected before mutation` | `unknown op verb: x` and `p index` are unrepresentable in the typed `Op` variant |
| `cch_chord:delta:path-validation` | same scenario | same missing rejections |
| `cch_chord:delta:root-replacement` | `mutable application mutates in place except for a root replacement, which returns a new value` | `sameTarget`/`replacementIsNew` had no C++ spelling with an out-parameter applier |
| `cch_chord:Chord:delta` (1054 composite) | `document watches and states deliver only committed frames` | composite capability with no delta-vocabulary observation |
| `cch_chord:Chord:draft` (1054 composite) | declared C++ comparison with no frozen pairing | no artifact |

Bounded scope: the Chord delta vocabulary, its decoded-input admission and the mutable applier.
Document transactions, storage, replicated state and the durable Session stay with their own
packets; `src/durable`, `src/coding_agent` and `src/mcp` are untouched. No new frozen capture was
needed: the registered `audit-1057`, `audit-1061` and `audit-1054` artifacts already record every
message, ordering and identity fact this slice compares.

## Delivered behavior

### Decoded-input ingestion

`decode_op(const Json&)` and `decode_ops(const Json&)` (`src/chord/include/cch/chord/Delta.hpp`,
`src/chord/Delta.cpp`) are the decoded-tuple admission boundary. They accept the frozen `Op` tuple
grammar — inline paths, no path ids, no shortened tuples — and produce the typed `Op`/`OpBatch` the
rest of the package uses. The wire grammar (`#` definitions, numeric path references, omitted paths)
stays a boundary concern; the durable storage adapter's `decode` is not this seam.

The seam owns the frozen rejection vocabulary and its order, so a shape the typed variant cannot
represent is still rejected with its recorded message:

| Tuple | Recorded message | Artifact |
| --- | --- | --- |
| `["s", ["__proto__", "polluted"], true]` | `unsafe path segment: __proto__` | `audit-1057/delta-revisions.json` |
| `["x", ["value"]]` | `unknown op verb: x` | `audit-1057/delta-revisions.json` |
| `["p", [], "0", 0, ["x"]]` | `p index` | `audit-1057/delta-revisions.json` |
| `["s", [], 1]` / `["d", []]` | `path is empty` | `audit-1061/validation-order.json` |
| `["s", ["a"]]` / `["s", ["a"], 1, 2]` | `s arity` | `audit-1061/validation-order.json` |
| `["t", ["s"], -1]` | `t shape` | `audit-1061/validation-order.json` |
| `["p", ["a"], 0, -1, []]` | `p remove` | `audit-1061/validation-order.json` |
| `["m", ["a"], [0, 0]]` | `m permutation is not a bijection` | `audit-1061/validation-order.json` |

Validation order follows frozen `assertValidOp`: verb and arity first, then the path argument
(`path is not an array`, `path is empty`, `unsafe path segment: <segment>`), then the payload
(`p index`, `p remove`, `p items`, `m permutation is not an array`, `m permutation is not a
bijection`). Payload values are not inspected beyond their shape, matching the frozen applier.
`decode_ops` rejects the whole tuple array: a rejected later tuple returns no prefix batch, so no
target is ever reached with a partially ingested batch.

Two representational notes, both recorded rather than normalized away:

- The frozen rejection classes (`TypeError`, `UnsafePathError`) have no C++ spelling. Every rejection
  is `support::ErrorCode::Validation`; the compared field is the exact message.
- A non-string verb or path segment is rendered with the frozen `String()` spelling (strings,
  booleans, null, nested arrays, `[object Object]`); numbers use the shortest round-trip decimal,
  which is why the frozen `1`/`-1` spellings match but JavaScript's exponent thresholds for numbers
  do not. Every recorded case uses a string verb or an integer segment.

`apply(Json&, const Json&)` is the sequential entry point over decoded tuples: it decodes and
validates each tuple exactly when the frozen applier reaches it, then applies it through the same
applier core as `apply(Json&, const OpBatch&)`. A later rejected tuple therefore keeps the writes the
earlier tuples applied — the frozen `audit-1061` order cases — while `decode_ops` remains the
whole-batch admission seam used by a storage boundary. No second applier exists: both `apply`
overloads drive one `Applier` core.

### Root-replacement identity

`apply` now returns `support::Expected<Json>`, the adopted root (frozen `apply` returns the value,
because a replacement cannot be done in place). The contract:

- without a root replacement, the returned root shares the caller value's container
  (`Json::shares_container_with`) and the caller value carries every successful write;
- a root replacement is adopted, never written into the caller: the returned root shares the
  replacement payload's container and the caller keeps the value it had when the replacement was
  reached, including writes applied before it;
- validation stays sequential, so a later failing operation keeps earlier writes, and a replacement
  followed by a failure leaves the caller object unchanged.

That is the C++ spelling of the frozen `sameTarget`/`replacementIsNew` object-identity observations:
container ownership replaces JavaScript reference identity, and `shares_container_with` separates
"the caller value is the adopted root" from "a detached copy was returned", and "a new root was
adopted" from "the replacement was written into the caller". `ReplaceOp` adoption shares the payload
container, matching frozen "adopted, not copied"; the payload itself stays immutable, which is the
delivered Chord ownership rule and the observable difference from JavaScript's in-place adoption of a
mutable payload.

Operation structs also gained defaulted `operator==`, so a producer's batch can be compared with a
batch an ingestion boundary decoded.

### Comparison mapping

One delivery record, `1185-chord-delta-ingestion`, in
[1058-deliveries.json](1058-deliveries.json) pins the production, test, build, fixture and result
inputs. Every claimed row has an empty remaining boundary.

| Row | Frozen artifact and scenario | Comparison case | Discriminating case |
| --- | --- | --- | --- |
| `cch_chord:delta:op-validation` | `audit-1057/delta-revisions.json`: `unsafe prototype segments and malformed operation tuples are rejected before mutation` | `decoded operation ingestion rejects the frozen malformed shapes before any target access` | `decoded application keeps the preceding mutable write when a later tuple is rejected` |
| `cch_chord:delta:path-validation` | same artifact and scenario | same comparison case | `decoded ingestion rejects every reserved segment and never yields a partial batch` |
| `cch_chord:delta:root-replacement` | `audit-1057/delta-revisions.json`: `mutable application mutates in place except for a root replacement, which returns a new value` | `mutable application mutates in place while a root replacement returns a new value` | `a root replacement keeps the preceding incremental write and adopts a new root` |
| `cch_chord:Chord:delta` | `audit-1054/session-transactions.json`: `document watches and states deliver only committed frames` | `a committed watch frame's frozen delta vocabulary matches the tracker draft and value` | `a whole-root replacement is not the delta a committed watch frame delivers` |
| `cch_chord:Chord:draft` | `audit-1057/delta-revisions.json`: `aborting a changed draft revokes escaped handles and leaves the committed revision unchanged` | `aborting a prepared change revokes escaped draft handles` | same case |

All comparison cases live in `tests/chord/DeltaTest.cpp` except the draft abort case, which is in
`tests/chord/TrackerTest.cpp`. Both files are inside the pinned `-L chord` selection.

`cch_chord:delta:op-validation` and `cch_chord:delta:path-validation` share one frozen scenario. The
comparison case asserts all three recorded rejections with their exact messages plus the eight
representable shapes of the `audit-1061` validator scenario; the frozen error-class names have no C++
spelling (see above). The discriminating case for op-validation requires the frozen sequential order
— a leading valid write stays applied when a later tuple is rejected — which a whole-batch
pre-validation or an ingestion seam that accepted an unknown verb would fail. The path-validation
discriminating case requires all three reserved segments (`__proto__`, `constructor`, `prototype`)
and whole-batch rejection, which a check that only special-cased `__proto__`, or one that returned
the accepted prefix, would fail.

`cch_chord:delta:root-replacement` compares every frozen field: `sameTarget` (the returned root
shares the caller value's container after in-place mutation), `targetAfter`, `replacementIsNew`,
`replacement`, `isBaseRoot` and `isReplaceRoot`. The discriminating case requires a replacement after
an incremental write to keep that write in the caller and return a distinct root, which both a
replacement-written-into-the-caller implementation and one that discarded the preceding write would
fail.

`cch_chord:Chord:delta` is the coarse 1054 view of the chord delta vocabulary. The comparison case
takes the committed watch frame's exact tuple array and delivered value, decodes the tuples through
the ingestion seam, requires the batch the delivered `Draft`/`Tracker` emits for the same change to
equal that decoded batch field by field, and applies the decoded batch to the tracker's
pre-commit revision to reach `watchValueAfterCommit`. The compared fields are `frames[0].ops`,
`frames[0].value` and `watchValueAfterCommit`; the remaining fields of the frozen scenario
(`stateValueAfterCommit`, `stateDisposed`, `stopped`, `closedWatch`) are the document-watch and
replicated-state lifecycle owned by the `state:*` and `document:*` rows, not by the delta vocabulary
this row describes. The discriminating case requires the committed frame to be a non-base batch and
the tracker's prepared replacement of the same value to differ from it, which a delta seam that
accepted a whole-root replacement as the frame shape would fail.

`cch_chord:Chord:draft` compares the frozen abort scenario's four fields through the public
`Change`/`Draft`/`Tracker` seam: the escaped write's `Cannot use a settled overlay`, the adoption's
`Prepared change has been aborted`, the unchanged committed value and revision `0`. The same case is
the discriminating case: an implementation that refuses `prepare` but leaves an escaped handle usable
would apply the write and fail the check.

### Reviewed source-audit correction

`cch_chord:Chord:draft` is a coarse 1054 overview row whose recorded scenario was the placeholder
`C++ transaction draft revocation after callback settlement`, with `artifact: null`. Its own recorded
gap says the draft contract is owned by the #1057 capture, and the `group-durable-chord-subset` plan
group records that the 1054 rows are the coarse view of the 1057 detail rows. The source row is
therefore re-paired, in
[1054-durable-session-documents-storage.json](1054-durable-session-documents-storage.json), to the
registered `audit-1057` escaped-handle revocation scenario, and its recorded result is the captured
observation instead of the “not captured” placeholder. The session-callback settlement behavior that
the placeholder named stays in the durable packet: `document:draft-lifecycle` and
`document:callback-rollback` own the durable test `revokes escaped drafts when the callback settles`
(#1180). This is a reviewed re-pairing of one coarse row, not a derived observation and not a
membership change.

The `cch_chord:delta:mutable-application` source row's `cppEvidence` pointer in
[1057-chord-durable-dependency-subset.json](1057-chord-durable-dependency-subset.json) is refreshed to
the renamed mutable-application test. Source-audit `cppSeam`/`cppEvidence` fields for the newly
verified rows stay empty, as they do for the rows #1178 verified: the delivery receipt is the
evidence authority and the source audits keep their captured status.

### Review

Two-axis review (Standards and Spec, independent sub-agents) ran on the implementation commits.

| Finding | Disposition |
| --- | --- |
| Standards: new rejection messages used `+` concatenation (`CODING_STANDARDS.md` §9.1) | Fixed: `std::format` |
| Standards: `spell`/`spell_number` did not say what they produce (Mysterious Name) | Fixed: renamed `frozen_string` |
| Standards: orphaned frozen-evidence comment | Fixed: removed |
| Standards: borrowed `Json&` member lacked a lifetime note (§7.5) | Fixed: member comment |
| Spec: `decode_ops` whole-batch rejection could not reproduce the frozen sequential order cases | Fixed: added the sequential `apply(Json&, const Json&)` entry point and its order-case test |
| Spec: source-ledger `cppSeam`/`cppEvidence` left empty for the new rows | Reviewed: matches the #1178 precedent — the receipt is the evidence authority |
| Spec: the 1054 re-pairing looks like scope creep | Reviewed and kept: the row's own gap and the plan group name #1057 as the contract owner; the session-callback behavior stays with #1180 |

## Validation

Measured on this ticket's commits `86afd3134…` (implementation) and `60b043a72…` (review fixes),
checkout `60b043a72`, with the pinned vcpkg toolchain:

| Command | Result |
| --- | --- |
| `cmake --preset vcpkg` | configured; include-scan evidence refreshed for the new `<array>`, `<charconv>`, `<cmath>`, `<format>` and `<limits>` includes |
| `cmake --build --preset vcpkg --target cch_tests_chord` | built; build-phase Parity Architecture Gate PASS |
| `ctest --preset vcpkg -N -L issue1185` | 7 selected |
| `ctest --preset vcpkg -L issue1185` | 7/7 passed |
| `ctest --preset vcpkg -N -L chord` | 76 selected |
| `ctest --preset vcpkg -L chord --output-junit .../1185-chord.xml` | 76/76 passed, 0 failed, 0 skipped |
| `ctest --preset vcpkg -L architecture` | 43/43 passed |
| `cmake --build --preset vcpkg` then `TERM_PROGRAM=herdr ctest --preset vcpkg -j1` | 4,223 selected, 4,212 passed, 0 failed, 11 skipped (704.05 s) |
| `python3 fixtures/pi-v1.3.0/consolidation-1058/consolidate_audits.py build` | ledger rebuilt: 50 delivery-backed rows (5 from this record) |
| `python3 fixtures/pi-v1.3.0/consolidation-1058/consolidate_audits.py check` | PASS — 645 scenarios, 362 named by a row, 56 covered by a referenced artifact, 227 explicit orphans, 0 uncovered |
| `python3 tests/evidence/ConsolidationAuditTest.py` | 42 tests, all passed |
| `python3 scripts/evidence/audit_evidence.py verify --baseline pi-v1.3.0` | 54 bundles, 114 artifacts and 648 scenarios verified at `71023aec…` |
| `scripts/format-check.sh ac250a0be` | clean |

The eleven skips are environment-gated cases, not code observations: nine frozen-checkout continuation
cases require `PI_CHECKOUT` for the private pi checkout, `tool duration ignores a changed realtime
clock` requires an injected realtime clock fixture, and `Native TUI differential capture` is the
opt-in dual-runtime capture. No assertion, timeout or label exclusion was changed, and the skipped
cases supply no acceptance evidence.

The receipt pins the exact JUnit bytes for both this record (`deliveries/1185-chord.xml`) and the
rerun of the #1178 selection (`deliveries/1178-revalidation-1185.xml`), so the selected count, case
names and scenario mapping cannot drift silently. The #1178 record keeps its original artifact and
records the rerun as `revalidation1185`.

## Capability status

Exactly five rows move from their audit status to `implemented` / `verified`, each through the
validated delivery record: `delta:op-validation`, `delta:path-validation`, `delta:root-replacement`,
`Chord:delta` and `Chord:draft`. All other chord rows keep their recorded implementation and
verification status, audit history and named remaining work. Membership is unchanged at 65
`cch_chord` rows (60 supported, 5 excluded).

Ledger counts before → after this record: `cch_chord` implemented 24 → 29, missing 28 → 23;
`cch_chord` verification `verified` 45 → 50 total verified rows, with `upstream-captured` 375 → 371
and `unverified` 1016 → 1015. This is a reconciliation count, not a completion percentage.

## Remaining boundary

Nothing remains for these five rows. The durable storage-format decoder in
`src/durable/StorageEncoding.cpp` keeps its own storage-corruption vocabulary and does not consume
`decode_ops`; converging it on the Chord seam is a durable storage-boundary decision for the durable
packet (#1180), not a Chord row obligation. `#1178`, `#1183`, `#1044` and `#1126` stay open; closing
this slice accepts no adjacent row and no umbrella obligation.

Magpie-only AI access, XDG state roots, strict no-exception behavior, the ordinary application
Session, all explicit AI/platform exclusions and the no-historical-Pike-durable-migration boundary
remain unchanged.
