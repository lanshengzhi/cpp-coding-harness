# Mainline receipt debt: 12 receipts rebound after upstream `71ad3e83a` — 2026-11-11

## Scope

The consolidation gate validates all 32 delivery receipts before it writes a ledger
(`consolidate_audits.py build`), so any receipt whose pinned bytes moved blocks the
ledger for every lane. Upstream commit `71ad3e83a` ("build: benchmark cold builds and
validate PCH evidence") added `boost-uuid` to `vcpkg.json` and `Boost::uuid` to the
durable target's declared dependencies in `cmake/targets/Durable.cmake`; twelve
receipts pin one or both files, so `build` had been failing on
`1101-structured-content: delivery digest mismatch: vcpkg.json` and no ledger could be
written.

This record rebinds exactly those twelve — **1090, 1101, 1176, 1177, 1178, 1181, 1182,
1184, 1185, 1186, 1187, 1188** — under
[1058-delivery-reconciliation.md](1058-delivery-reconciliation.md) §Delivery evidence
contract. The debt's cause is the upstream commit, not a #1232 change: no file this
lane touched is a codemode file, and no receipt edited here is a codemode receipt. The
four codemode receipts the integration line owns (1179, 1118, 1192, 1194) are not
edited, rerun or re-described here.

Work tree: `/home/lansy/.cache/cch-1232-debt`, a `git clone --no-hardlinks` of the main
checkout, branch `clear-receipt-debt-1232`. `origin/main` is
`71ad3e83a75e9155a708951e33bd452ad94f88ed`, which is where the slice started. The
deliverable sits at
`c8abe71ba4e8cb988a987ca9337d252acf3c2fe5`, the owner's
`integrate-1232` receipt-rebind commit (`44d0fa5c1` plus `c8abe71ba`, fetched from
`/home/lansy/.cache/cch-1232-integration`): a slice at plain `71ad3e83a` cannot be green,
because 1179 and 1118 are stale there too and their rebinds exist only on that branch
(§Remaining boundary). The main checkout was read only; its in-flight changes and its
free lane lock were preserved.

Lane `clear-receipt-debt-1232` held the slice for every build and test command
(`scripts/dev/lane-lock.sh acquire`, agent `lansy@btc`, acquired
`2026-11-11T07:06:02Z`), and `CCH_LANE_NAME` was exported to every child process,
including the `ConsolidationAuditTest.py` children that run `consolidate_audits.py
build`/`check`.

## Delivered behavior

Nothing in the runtime changes. Each of the twelve receipts now pins the bytes actually
present, carries a `revalidation1232ReceiptDebt` entry naming the upstream commit as the
sole cause, and records a fresh JUnit artifact from a rerun of its own selection.

Selections were read from each receipt's `validation.command`, not restated from prose,
and confirmed with `ctest --preset vcpkg -N` before any run. Every count matches that
receipt's recorded `selected`:

| Receipt | Selection | `ctest -N` | Recorded `selected` |
| --- | --- | --- | --- |
| 1090, 1101 | `-L '^(issue1090\|issue1101\|issue1104)$'` | 36 | 36 |
| 1178, 1185, 1186, 1188 | `-L chord` | 104 | 104 |
| 1181 | the record's anchored 16-case `-R` regex | 16 | 16 |
| 1184 | `-L issue1184` | 10 | 10 |
| 1187 | `-L issue1187` | 4 | 4 |
| 1182 | `-L issue1182` | 13 | 13 |
| 1176 | the record's anchored 5-case `-R` regex | 5 | 5 |
| 1177 | the record's anchored 9-case `-R` regex | 9 | 9 |

Every selection ran with `--no-tests=error`, `-j 1` and `TERM_PROGRAM=herdr` on the
Debug tree at `c8abe71ba` (`cmake --preset vcpkg` with the pinned `VCPKG_ROOT` and
`--only-binarycaching`; Parity Architecture Gate PASS, 13 targets / 69 dependencies /
287 sources / 3 961 includes; full build 681 steps, 18 m 15 s, rc 0). All twelve are
green — no failing or skipped case was used to rebind anything:

| Receipt | JUnit | selected | passed | failed | skipped | wall |
| --- | --- | --- | --- | --- | --- | --- |
| 1090 | `1090-revalidation-upstream-71ad3e83a.xml` | 36 | 36 | 0 | 0 | 2.20 s |
| 1101 | `1101-revalidation-upstream-71ad3e83a.xml` | 36 | 36 | 0 | 0 | 2.18 s |
| 1176 | `1176-revalidation-upstream-71ad3e83a.xml` | 5 | 5 | 0 | 0 | 0.18 s |
| 1177 | `1177-revalidation-upstream-71ad3e83a.xml` | 9 | 9 | 0 | 0 | 2.79 s |
| 1178 | `1178-revalidation-upstream-71ad3e83a.xml` | 104 | 104 | 0 | 0 | 0.55 s |
| 1181 | `1181-revalidation-upstream-71ad3e83a.xml` | 16 | 16 | 0 | 0 | 3.56 s |
| 1182 | `1182-revalidation-upstream-71ad3e83a.xml` | 13 | 13 | 0 | 0 | 7.88 s |
| 1184 | `1184-revalidation-upstream-71ad3e83a.xml` | 10 | 10 | 0 | 0 | 0.20 s |
| 1185 | `1185-revalidation-upstream-71ad3e83a.xml` | 104 | 104 | 0 | 0 | 0.53 s |
| 1186 | `1186-revalidation-upstream-71ad3e83a.xml` | 104 | 104 | 0 | 0 | 0.54 s |
| 1187 | `1187-revalidation-upstream-71ad3e83a.xml` | 4 | 4 | 0 | 0 | 0.15 s |
| 1188 | `1188-revalidation-upstream-71ad3e83a.xml` | 104 | 104 | 0 | 0 | 0.54 s |

545 case runs in total. The chord group shares one selection but each receipt pins its
own artifact, so no receipt depends on another receipt's capture. Pinned digests:

```
d703ed6143e98fe6bc567c1d7fa3cb2b6e23a0b8c0abb7d7ad65c7aebadf9a10  1090-revalidation-upstream-71ad3e83a.xml
080b903d5c3ab80c18e2490c487b2ecd3743a656b8c2ba73a5db206ae7cbacc3  1101-revalidation-upstream-71ad3e83a.xml
39eaa2fbc3d3547705a0caff27aa4bffcd4b702422a85431f008481f845c7ab8  1176-revalidation-upstream-71ad3e83a.xml
487e840f14ae7747e424a932838a743583abf638e1d4418745f4945891a1d82e  1177-revalidation-upstream-71ad3e83a.xml
04faa93e89d7bec4aa1ed3df4685ff6220f89e2c9eb76566c2426999c45cd5b6  1178-revalidation-upstream-71ad3e83a.xml
7376bd1833a9bbfb745686c2db5595765838372fe21d9989deea19e2a3f48d4f  1181-revalidation-upstream-71ad3e83a.xml
ffcf17753c43ddf6a6692ee8472506120c1b4cbca6ef5028f56a2744ceff4330  1182-revalidation-upstream-71ad3e83a.xml
6c15d708b02d45559280c990890c3624e0322b9e8d6651eb0a45e36c607ce2ec  1184-revalidation-upstream-71ad3e83a.xml
d69518e01d5d261d851c92eb589fabc407c73daefa9e2c4cd80ed1f5993ed28b  1185-revalidation-upstream-71ad3e83a.xml
ad649941179e533ead6265d4002fd347ea01dfac9f6bcb29ac20fee3eca3d050  1186-revalidation-upstream-71ad3e83a.xml
99b42719d9294c7807f1ebfbaa6b6cf1bc7fabef579b89132d6aefecada3103a  1187-revalidation-upstream-71ad3e83a.xml
92bfe2775d69cdb1eabb8a4d36d6c82fb2b22d424a442bb6394843171f4f5aaa  1188-revalidation-upstream-71ad3e83a.xml
```

Stale inputs were recomputed from the tree and refreshed one by one — no bulk rehash.
`rows`, `routingSha256`, `implementationCommits` and `report` are untouched in all
twelve receipts, and each new `revalidation1232ReceiptDebt` entry keeps the superseded
`command`/`testedRevision`/`recordedAt`/`selected`/`junit` verbatim as
`previousValidation`:

| Receipt | Refreshed input | Pin → actual |
| --- | --- | --- |
| 1090 | `cmake/targets/Durable.cmake` | `9feff05d…` → `05a4970a…` |
| 1101 | `vcpkg.json` | `dc956d94…` → `f19c0e24…` |
| 1176 | `vcpkg.json` | `dc956d94…` → `f19c0e24…` |
| 1177 | `vcpkg.json` | `dc956d94…` → `f19c0e24…` |
| 1178 | `vcpkg.json` | `dc956d94…` → `f19c0e24…` |
| 1181 | `vcpkg.json` | `dc956d94…` → `f19c0e24…` |
| 1182 | `vcpkg.json` | `dc956d94…` → `f19c0e24…` |
| 1184 | `vcpkg.json`, `cmake/targets/Durable.cmake` | `dc956d94…` → `f19c0e24…`, `9feff05d…` → `05a4970a…` |
| 1185 | `vcpkg.json` | `dc956d94…` → `f19c0e24…` |
| 1186 | `vcpkg.json` | `dc956d94…` → `f19c0e24…` |
| 1187 | `vcpkg.json` | `dc956d94…` → `f19c0e24…` |
| 1188 | `vcpkg.json` | `dc956d94…` → `f19c0e24…` |

Top-level `validation` in each receipt now records `testedRevision`
`c8abe71ba4e8cb988a987ca9337d252acf3c2fe5`, the new JUnit path and digest, the unchanged
`selected`, and the selection as run (the receipt's own `-R`/`-L` flags plus
`--no-tests=error`, `--output-on-failure`, `-j 1`); `recordedAt` is the UTC capture time
of that artifact. Every one of the twelve selections is insensitive to the
`71ad3e83a` → `c8abe71ba` delta: the changed files there are codemode sources, codemode
tests and codemode CMake declarations, and their intersection with the twelve receipts'
pinned inputs is empty. The artifact is still bound to the tree it ran in, which is why
`testedRevision` names `c8abe71ba` rather than `71ad3e83a`.

## Validation

All commands ran in the slice with `CCH_LANE_NAME=clear-receipt-debt-1232` exported.

- Before: `consolidate_audits.py check` at plain `71ad3e83a` exited non-zero on
  `1101-structured-content: delivery digest mismatch: vcpkg.json`. A digest sweep of the
  committed receipts found 14 stale at `71ad3e83a` (the twelve here plus 1179 and 1118)
  and exactly the twelve here at `c8abe71ba`; after this commit is applied to a plain
  `71ad3e83a` tree the stale set is the integration line's four (1118, 1179, 1192, 1194),
  as recorded under §Remaining boundary.
- Focused selections above: 36, 36, 104, 16, 104, 104, 104, 4, 13, 5, 9, 10 selected —
  545/545 passed, 0 failed, 0 skipped.
- `consolidate_audits.py build`: **rc 0**, wrote
  `1058-consolidated-capability-ledger.json` (5 832 304 bytes, 1 464 rows).
- `consolidate_audits.py check`: **rc 0**. The rebuilt ledger differs from the committed
  one only inside `deliveries.records`: 0 rows change membership, implementation,
  verification or evidence, and every other top-level key is byte-identical.
- `python3 tests/evidence/ConsolidationAuditTest.py`: 94 tests, **rc 0**.
- `python3 scripts/evidence/audit_evidence.py verify --baseline pi-v1.3.0`: **rc 0** —
  84 bundles, 180 artifacts, 1 012 scenarios, baseline `pi-v1.3.0` at
  `71023aec1546d6d677a65669c5665c0927ce0dd0`.
- `scripts/format-check.sh c8abe71ba`: `format check: clean` (rc 0); `git diff --check`
  clean. No C++ line is added or modified by this lane.

## Capability status

No capability row changes status. The twelve receipts promote the same rows they
promoted before — the ledger diff is confined to the delivery metadata, so this is a
rebind, not a promotion, and it adds no discriminating case.

## Remaining boundary

- **Base dependency.** This commit's green `build`/`check` holds on a tree that also
  carries the owner's `integrate-1232` rebinds (`c8abe71ba`). Measured, not assumed: the
  commit's receipt file and its twelve artifacts were applied to a probe worktree at
  plain `71ad3e83a` and `check` fails first on `1118-codemode-catalog: delivery digest
  mismatch: cmake/targets/Codemode.cmake`. A digest sweep over that tree reports exactly
  four stale receipts — **1118** (`cmake/targets/Codemode.cmake`), **1179**
  (`cmake/targets/Codemode.cmake`, `cmake/tests/CodemodeTests.cmake`,
  `src/codemode/Deterministic.cpp`, `src/codemode/Remote.cpp`), **1192** and **1194**
  (both `cmake/targets/Codemode.cmake`, `cmake/tests/CodemodeTests.cmake`) — plus the
  four `*-revalidation-1232-frame-join.xml` artifacts that exist only in `c8abe71ba`.
  Those four are the integration line's rebinds, not this lane's; with them present the
  twelve are closed and the gate is green.
- 1179, 1118, 1192 and 1194 were not rerun, edited or re-described here; their
  `revalidation1232FrameJoin` entries are the integration line's.
- Any later commit that touches `vcpkg.json` or `cmake/targets/Durable.cmake`
  invalidates all twelve receipts again; `vcpkg.json` in particular is a shared pin, so
  every dependency change is a twelve-receipt event until a narrower pin is designed.
- The build here used the host ccache and the preinstalled pinned `VCPKG_ROOT`; it is a
  Full-tier incremental build, not a Fresh Validation of the slice.
- Nothing was pushed; the main checkout, its in-flight changes and its lane lock were
  not modified.
