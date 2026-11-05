# #1180: durable JSONL task-sidecar symlink boundary and two-sidecar publication order

## Scope

Completes the symlink-parity delivery of the durable JSONL task sidecar under
the owner ruling of 2026-11-05 (decision B,
[ADR 0076](../../adr/0076-align-the-durable-jsonl-task-sidecar-symlink-boundary-with-frozen-pi.md)),
for [#1180](https://github.com/lanshengzhi/cpp-coding-harness/issues/1180)
under [#1183](https://github.com/lanshengzhi/cpp-coding-harness/issues/1183)
and [#1044](https://github.com/lanshengzhi/cpp-coding-harness/issues/1044).
Selected comparison baseline: `pi-v1.3.0` /
`71023aec1546d6d677a65669c5665c0927ce0dd0`, Linux x86-64 with glibc.

The implementation and the ADR landed in `942d178be` (code and tests) and
`75f602ad5` (ADR 0076, the pinned-semantics probes and the registered
`audit-1180` write-order capture); this record finishes the delivery half:
the frozen write-order capture is routed onto the sidecar and publication-order
rows and the two rows complete through #1180's whole-row receipt contract.
#1180 itself remains open for the broader durable packet.

## Frozen contract

Pinned by probe against the frozen checkout at `71023aec` and recorded in
[1180-sidecar-symlink-parity](1180-sidecar-symlink-parity/NOTES.md):

- Recovery enumeration skips a symlinked task sidecar: an unreferenced link
  opens cleanly and a live-referenced link fails with the frozen
  missing-confirmed-sidecar corruption without reading or truncating the
  target.
- Commit append and fsync flush follow the final symlink; reclamation unlinks
  the entry itself and preserves the outside target; intermediate components
  are followed everywhere; the store never resolves outside the
  caller-provided directory by itself.
- Sidecars append in commit write order and flush in that same order before
  the main marker — sorted filename order is discriminated by the registered
  two-sidecar capture
  [fixtures/pi-v1.3.0/audit-1180/sidecar-order.json](../../fixtures/pi-v1.3.0/audit-1180/sidecar-order.json)
  (`doc 9` create then `doc 3` create yields `appendFile doc-9.jsonl`,
  `appendFile doc-3.jsonl`, `appendFile main.jsonl`, then the two flushes in
  that order when fsync is on).

Retained named divergences per ADR 0076: the singly-linked-file
(`st_nlink == 1`) rejection stays (frozen pi follows hard links and truncates
an outside target at recovery; this runtime refuses without modifying it),
and the C++ fail-closed point at commit where frozen pi fails at the next
open. Every other no-follow surface is unchanged.

## Delivered behavior

`942d178be` aligned `cch_durable`'s JSONL storage with the pinned semantics:
the task-sidecar open no longer rejects a final symlink; recovery treats a
symlinked task sidecar entry as absent; reclamation of an adopted symlink
unlinks the link, not the target; the substitution defenses (observed
device/inode identity, exclusive-create adoption guard, reclaim-time identity
compare) stay in force; and the encoder emits sidecars, flushes and
reclamations in frozen write order. The discriminating cases tagged
`[issue1180]` (skip-at-recovery, missing-confirmed corruption, append-through,
reclaim-unlinks-entry, descending-ID two-sidecar trace, and the registered
artifact comparison) all run against the frozen checkout (`PI_CHECKOUT`).

## Frozen evidence

| Family | Artifact | Scenarios | Rows |
|---|---|---:|---|
| `audit-1180` | `sidecar-order.json` | 2 | `cch_durable:Storage:jsonl:sidecar`, `cch_durable:Storage:jsonl:ordering` |
| `audit-1054` | `jsonl-store.json` | 1 (per row) | `cch_durable:Storage:jsonl:sidecar`, `cch_durable:Storage:jsonl:ordering` |

The plan routes the two refreshed `audit-1180` scenarios onto the two rows
(scenarioLinks, ticket 1054); both rows keep their original audit-1054
observation (`a JSONL commit appends every sidecar before the main marker`),
which the registered one-sidecar capture pins and the public storage
comparison test compares.

## Validation

Receipt `1180-sidecar-symlink-parity` in
[1058-deliveries.json](1058-deliveries.json), testedRevision
`c656c871d764dd5001cb94d5555666f651d7d120`:

- Selection: 2 cases, 2 passed
  (`ctest --preset vcpkg -LE architecture -R` over
  `a JSONL store commits, closes and reopens the same records through the
  public storage contract` and
  `a JSONL commit appends two sidecars in frozen write order before the main
  marker`), JUnit pinned at
  [deliveries/1180-sidecar-order.xml](deliveries/1180-sidecar-order.xml).
- The refreshed-capture comparisons cite the `[issue1180]` test that opens the
  registered `audit-1180/sidecar-order.json` artifact and compares both
  scenarios' frozen append/flush traces and file bytes; the discriminating
  case creates documents 9 then 3, so sorted-filename order fails the frozen
  trace.
- The original audit-1054 obligation cites the public storage contract test
  that opens the registered `audit-1054/jsonl-store.json` artifact and
  compares the row's frozen scenario (marker carries the sidecar's sequence
  and ordinal; the sidecar carries the base revision the marker names).
- Input digests pin `src/durable/JsonlStorage.cpp` and
  `tests/durable/JsonlStorageTest.cpp`; `scripts/evidence/receipt_boundary.py
  --baseline pi-v1.3.0` passes the new receipt (exit 0).

## Capability status

Two rows advance from `implemented` / `upstream-captured` to `implemented` /
`verified`, delivery-backed by `1180-sidecar-symlink-parity` with an empty
remaining boundary:

- `cch_durable:Storage:jsonl:sidecar`
- `cch_durable:Storage:jsonl:ordering`

## Remaining boundary

- `cch_durable:Storage:jsonl:main-marker` keeps its audit-1054 obligation
  unclaimed here; the write-order alignment covers its marker shape but this
  receipt does not claim it.
- `cch_durable:Storage:jsonl:fsync` keeps its audit-1054 obligation; the
  two-sidecar flush capture is routed to the ordering row and the fsync row's
  own whole-row claim stays with #1180's durable packet.
- The symlink-skip recovery-time divergence for document sidecars and the
  recovery-time reclamation order stay recorded, not aligned (ADR 0076 scope).
- #1180 remains open for the broader durable packet; no membership changed.
