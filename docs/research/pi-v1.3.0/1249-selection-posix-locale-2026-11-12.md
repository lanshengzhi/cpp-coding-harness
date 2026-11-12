# #1249 selection word segmentation under the POSIX locale (2026-11-12)

## Observation and mechanism

The GCC 16 Debug job of run 43176359218 fails the frozen
`multiclick-word-joiners-and-classes` case seven times at observations 2–8;
its count assertions pass. The log does not expose the actual copied text or
the job's locale. At base `7ac7fb9ffbeb6f0ded6daf6339f9b61b5313d957`, the
same case passes with `en_US.utf8` and `zh_CN.utf8`, and fails with identical
seven observations under both `LC_ALL=C` and `LC_ALL=C.utf8`.

A separate local ICU 78.3 probe on `a.b` reports default `en_US_POSIX` and
boundaries 0,1,2,3 with `LC_ALL=C`; explicitly selecting `en_US` or `root`
reports 0,3. The frozen scenario's observation 2 expects `a.b`. The local
Node `Intl.Segmenter` resolves its default to `en-US` and returns `a.b` under
both C locales. The product previously passed the raw ICU default to its word
breaker. The evidence establishes a matching local cause; it does not prove
which locale CI actually supplied.

## Change and focused evidence

The shared word breaker translates ICU's `en_US_POSIX` default to `en_US`,
matching the observed Node default, while retaining other ICU locale defaults.
An AltScreen frozen-scenario regression sets the ICU default to `en_US_POSIX`
and restores it afterward. Before the change it failed the same seven
observations (109/116 assertions passed); after the change it passes. The
original failing case and the aggregate v1.3.0 frozen selection replay also
pass under `LC_ALL=C`. The fix affects every consumer of the shared word
breaker, including editor navigation, so the complete suite is required.

## Delivery evidence

`tests/tui/SelectionEvidenceTest.cpp` was pinned by receipts 1182, 1211 and
1212. Their recorded CTest selections were rerun after rebuilding: 13/13,
34/34 and 29/29 passed respectively, with zero skips. Each receipt now binds
its own new JUnit artifact and the tested source digest, retaining its prior
validation record. Scenario mappings and frozen inputs are unchanged.
Consolidation build and check pass after the rebind. `testedRevision` is the
checkout base above; the receipt digests bind the modified files actually tested.
The receipt boundary checks 32 receipts, 808 claims and 966 comparisons;
`ReceiptBoundaryTest.py` passes 24 cases; `audit_evidence.py verify` verifies
84 bundles, 180 artifacts and 1,012 scenarios.

The unfiltered Debug suite at `-j4` selected 4,608 cases: 4,593 passed, 12
skipped, and three failed. The failures were the terminal-color timeout callback
(`complete_calls` 0 rather than 1), bounded parallel cancellation (which calls
acquired durations differed from the expected first two), and the quarantined
streaming performance test (3,879 µs against 2,000 µs). All three passed when
rerun in isolation at `-j1`; their mechanisms have not been diagnosed here.
A subsequent unfiltered `ctest --preset vcpkg -j2 --output-on-failure` selected
4,608 cases, passed 4,596, skipped 12 and failed none (414.95 s). The skipped
Herdr case requires `TERM_PROGRAM=herdr`, absent on this host; the other skips
are the continuation, clock and differential-capture preconditions.

The locale mapping is established for the tested Node and ICU versions and
these concrete locale values; this is not a claim of complete equivalence for
all locales or other Node implementations.
