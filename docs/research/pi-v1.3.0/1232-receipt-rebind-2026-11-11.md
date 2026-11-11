# #1232 integration receipt rebind — 2026-11-11

## Scope

Issue [#1232](https://github.com/lanshengzhi/cpp-coding-harness/issues/1232), continuing
[1232-frame-join-stop-notify-integration-2026-11-11.md](1232-frame-join-stop-notify-integration-2026-11-11.md).
That record integrated `cf717581b` (frame-local deterministic worker ownership) and
`21fecff` (deterministic stop-notify handshake) into one tree, listed the receipt inputs
the integration and its newer base had invalidated, and rebound nothing. This record
rebinds exactly four of those receipts — **1179, 1118, 1192, 1194** — by the delivery
contract in [1058-delivery-reconciliation.md](1058-delivery-reconciliation.md) §Delivery
evidence contract. No other receipt, row, capability claim or build file is touched.

Work tree: `/home/lansy/.cache/cch-1232-integration`, branch `integrate-1232`, HEAD
`44d0fa5c1d52bb834a54b812265b250424ba6844`. The main checkout was read only; its
in-flight changes and its free lane lock were preserved. Lane `integrate-1232` was held
for every build and test command below (`scripts/dev/lane-lock.sh acquire`, holder
`pi-integrate`, task `1232`, acquired `2026-11-11T06:48:49Z`), and `CCH_LANE_NAME` was
exported to every child process, including the `ConsolidationAuditTest.py` children that
run `consolidate_audits.py build`/`check`.

## Delivered behavior

Nothing in the runtime changes. This is an evidence rebind: the four receipts now pin
the bytes actually tested on this tree, and each carries a new
`revalidation1232FrameJoin` entry recording why.

Selections were read from each receipt's own `validation.command`, not restated from
prose, and confirmed with `ctest --test-dir build -N` before any run. All four counts
match the recorded `selected` exactly:

| Receipt | Selection | `ctest -N` | Matches recorded |
| --- | --- | --- | --- |
| 1179 | the receipt's anchored regex | 52 | yes |
| 1118 | `-L issue1118` | 19 | yes |
| 1192 | `-LE architecture` + the anchored 5-case regex | 5 | yes |
| 1194 | `-LE architecture -L issue1194` | 1 | yes |

Every selection ran with `--no-tests=error`, `-j 1` and `TERM_PROGRAM=herdr` on the
Debug tree at HEAD, after an incremental `cmake --build --preset vcpkg` (rc 0, 23 s,
build-phase Parity Architecture Gate PASS). All four are green; no failing or skipped
case was used to rebind anything:

| Receipt | New JUnit | selected | passed | skipped | failed | wall |
| --- | --- | --- | --- | --- | --- | --- |
| 1179 | `deliveries/1179-revalidation-1232-frame-join.xml` | 52 | 52 | 0 | 0 | 106.64 s |
| 1118 | `deliveries/1118-revalidation-1232-frame-join.xml` | 19 | 19 | 0 | 0 | 6.58 s |
| 1192 | `deliveries/1192-revalidation-1232-frame-join.xml` | 5 | 5 | 0 | 0 | 4.75 s |
| 1194 | `deliveries/1194-revalidation-1232-frame-join.xml` | 1 | 1 | 0 | 0 | 0.14 s |

CTest resolves `--output-junit` against its own test directory, so the four files were
first written under `build/docs/...` and then moved into
`docs/research/pi-v1.3.0/deliveries/`; their bytes and digests are those of the runs
above. Pinned digests:

```
4fd7317c7fa575a3eff73723fe5956d06c4a62e24291bc64925bbff0e6736fa9  1179-revalidation-1232-frame-join.xml
d50e1cb0440e3c8b9e5001ce92bd69107cbb1922c986cd554423dd8127cce907  1118-revalidation-1232-frame-join.xml
63611617fce06d4e6c7ea8bc998dac091743db1139fd05f39ab648b3d96a4081  1192-revalidation-1232-frame-join.xml
39ec7e19bc288ef2816554aa0b818e13baa1dc7b75ff0202bd85667584cd94fc  1194-revalidation-1232-frame-join.xml
```

Stale inputs were recomputed from the tree and refreshed one by one — no bulk rehash.
`rows`, `routingSha256`, `implementationCommits` and `report` are untouched in all four
receipts, and each `previousValidation` snapshot preserves the superseded
`command`/`testedRevision`/`recordedAt`/`selected`/`junit` verbatim.

| Receipt | Refreshed input | Pin → actual | Cause |
| --- | --- | --- | --- |
| 1179 | `vcpkg.json` | `dc956d94…` → `f19c0e24…` | upstream `71ad3e83a` |
| 1179 | `cmake/targets/Codemode.cmake` | `d5c1c594…` → `a07926c5…` | self-join slice |
| 1179 | `cmake/tests/CodemodeTests.cmake` | `ee80abcd…` → `fbc4c60f…` | self-join + integration |
| 1179 | `src/codemode/Deterministic.cpp` | `6e89bab0…` → `6db75486…` | integration (gate seam calls) |
| 1179 | `src/codemode/Remote.cpp` | `612eb8fa…` → `2b3866b0…` | self-join + integration |
| 1179 | `tests/codemode/CodemodeRemoteTest.cpp` | `8ecfe916…` → `0e392f85…` | upstream `71ad3e83a` |
| 1118 | `vcpkg.json` | `dc956d94…` → `f19c0e24…` | upstream `71ad3e83a` |
| 1118 | `cmake/targets/Codemode.cmake` | `d5c1c594…` → `a07926c5…` | self-join slice |
| 1118 | `tests/support/ModelsFixture.hpp` | `e3a1015b…` → `5258f123…` | upstream `71ad3e83a` |
| 1192 | `cmake/targets/Codemode.cmake` | `d5c1c594…` → `a07926c5…` | self-join slice |
| 1192 | `cmake/tests/CodemodeTests.cmake` | `ee80abcd…` → `fbc4c60f…` | self-join + integration |
| 1194 | `cmake/targets/Codemode.cmake` | `d5c1c594…` → `a07926c5…` | self-join slice |
| 1194 | `cmake/tests/CodemodeTests.cmake` | `ee80abcd…` → `fbc4c60f…` | self-join + integration |

Each `revalidation1232FrameJoin.reason` names both cause classes: the integration's own
changes (`Deterministic.cpp`, `Remote.cpp`, `CodemodeTests.cmake`, and the self-join's
`Codemode.cmake`), and the newer upstream base `71ad3e83a`, which had already changed
files pinned or consumed by these receipts — `vcpkg.json`, `Durable.cmake`,
`tests/support/ModelsFixture.hpp` and `tests/codemode/CodemodeRemoteTest.cpp` among them.

Top-level `validation` for each receipt now records `testedRevision`
`44d0fa5c1d52bb834a54b812265b250424ba6844`, `recordedAt` `2026-11-11T06:53:11Z`, the new
JUnit path and digest, the unchanged `selected`, and the selection as run (same `-R`/`-L`
/`-LE` flags, `--no-tests=error`, `-j 1`).

## Validation

All commands ran in the slice with `CCH_LANE_NAME=integrate-1232` exported.

- Focused selections above: 52/52, 19/19, 5/5, 1/1 — 0 failed, 0 skipped.
- `consolidate_audits.py build`: **rc 1**, `1101-structured-content: delivery digest
  mismatch: vcpkg.json`. The build validates every receipt before writing, so the ledger
  was **not** regenerated.
- `consolidate_audits.py check`: **rc 1**, the same message and record.
- The four rebound receipts pass the same validator in isolation: a `build_ledger` run
  over a filtered copy of `1058-deliveries.json` holding only these four records returns
  1 464 rows and 27 promoting claims (1179: 23, 1118: 3, 1192: 1, 1194: 0 — 1194's single
  claim keeps its `remainingBoundary`), the identical per-receipt row ids to the
  committed ledger, and 31 verified rows including the 4 rows verified by source audit.
  No claim, mapping or discriminator changed; this is a rebind, not a promotion.
- `python3 tests/evidence/ConsolidationAuditTest.py`: 94 tests, 11 failures, 5 errors
  (rc 1). **The failing set is byte-identical at HEAD**: the same suite run in a copy of
  this tree with HEAD's `1058-deliveries.json` reports the same 16 ids, and every one
  cites `1101-structured-content: delivery digest mismatch: vcpkg.json`.
- `python3 scripts/evidence/audit_evidence.py verify --baseline pi-v1.3.0`: rc 0 —
  84 bundles, 180 artifacts, 1 012 scenarios, baseline `pi-v1.3.0` at
  `71023aec1546d6d677a65669c5665c0927ce0dd0`.
- `scripts/format-check.sh 71ad3e83a`: `format check: clean` (rc 0).
- `git diff --cached --check`: clean; the staged set contains the four JUnits, the
  receipt file and this record — no `.validation/` or other local artifact.

### Why the ledger is not rebuilt

`build` and `check` refuse the whole tree while any receipt's pinned bytes are stale, and
12 receipts outside this lane's scope are stale at base `71ad3e83a`:

| Receipt | Stale inputs |
| --- | --- |
| 1101, 1178, 1181, 1185, 1186, 1188, 1187, 1182, 1176, 1177 | `vcpkg.json` |
| 1090 | `cmake/targets/Durable.cmake` |
| 1184 | `vcpkg.json`, `cmake/targets/Durable.cmake` |

The first of these fails the gate, so no `1058-consolidated-capability-ledger.json` bytes
were written; the committed ledger still embeds the pre-rebind receipts for 1179, 1118,
1192 and 1194 as well as the 12 stale ones above. That artifact therefore lags its input
by design of this lane's scope, not by an unreported failure. Rebounding those 12 is a
separate, owner-authorized lane: until it lands, `build`/`check` stay red and the ledger
stays at its previous build. The four rebinds need no further edit when that happens —
`build_ledger` reads them as they are, and the isolated run above already produced the
promotion set they contribute.

### One-off failure, still unexplained

The integration record carries one non-reproducing failure of
`RemoteCodemodeSandbox runs tools in the caller and the script on the remote` in a
`ctest -L codemode -j4` run issued alongside a whole-tree `git clang-format` sweep. It
did not reproduce in the integration lane (1.16 s alone, 20/20 under four CPU burners,
five further `-L codemode -j4` runs 181/181) and did not appear in any of the four
selections here. The cause remains unestablished and is recorded, not smoothed over.

## Capability status

No capability row changes status. The same 27 rows the four receipts promoted before are
the rows they promote now; `rows`, `routingSha256` and `implementationCommits` are
unchanged, so this record makes no new parity claim and adds no discriminating case.

## Remaining boundary

- The consolidated ledger is **not rebuilt** in this commit; it lags `1058-deliveries.json`
  for these four receipts until the 12 out-of-scope receipts above are rebound and
  `build`/`check` pass. That is the residual mainline debt of this candidate.
- 1179, 1118, 1192 and 1194 are rebound to `44d0fa5c1` only. Any later commit that
  touches their pinned files invalidates them again.
- The `#1232` runtime limits in the integration record still stand: the parks force a
  schedule, the red is a timeout rather than an assertion, the ASan/UBSan tier covered
  only the codemode shard, and a `request_stop()` arriving from `dispatch` is not covered
  by the new case.
- Nothing was pushed; no receipt outside the four was edited; no `.validation/` artifact
  was staged.
