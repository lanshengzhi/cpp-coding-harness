# Issue #1232 production-slice probe — 2026-11-08

## Scope and receipt cost

At checkout base `84d34227e`, issue #1232 remains open. The production
candidates are `src/durable/CodingTools.cpp` (`format_size`) and
`src/codemode/Remote.cpp` (`RemoteDeterministicServe`). No production changes
were made in this probe. The user-owned `AGENTS.md` modification was retained.

The `1090-durable-images` receipt pins `CodingTools.cpp`: 4 rows, 23 inputs,
36 selected tests. The `1179-codemode-comparisons` receipt pins `Remote.cpp`:
39 rows, 51 inputs, 52 selected tests. A source change requires reviewing the
relevant mapping and revalidating the affected receipt, not just updating a
hash. The latter touches more rows than the approximately 25-row lane-sizing
guideline; the former is the smaller candidate. Both also require the full lane
gates for code delivery.

## Formatting observation

The captured read-tool cases in `fixtures/pi-v1.3.0/audit-1197/read-tool.json`
contain `50.0KB`, `58.6KB` and `60.0KB`, but do not establish halfway rounding.
A standalone GCC 16.2.1 C++23 probe compared `snprintf("%.1f")` and
`std::format("{:.1f}")` in the default process locale and rounding environment.
For 1023, 1024, 1280, 51199, 51200, 51455, 51456, 51457, 1048575,
1048576 and 1310720 bytes the two matched, including the KB/MB transitions.
At 51456 bytes both yielded `50.2KB`; at 1310720 both yielded `1.2MB`.

Node v26.13.0 `(n / divisor).toFixed(1)` instead produced `50.3KB` and
`1.3MB` at those respective halfway values, and `1.3KB` at 1280 bytes
(the C++ probe returned `1.2KB`). This is a comparison with JavaScript
`toFixed`, not proof that the frozen pi revision used that expression: its
source was unavailable in the local upstream checkout. The existing capture
alone cannot adjudicate that boundary. Locale and floating-point rounding-mode
changes were not tested. A straightforward formatting substitution therefore
must not be described as proving all frozen boundary behavior.

## Thread lifetime observation

`RemoteDeterministicServe` declares `guest`, `queue`, `exchange`, `run`,
`stop`, then `worker`; members are destroyed in reverse order. Its destructor
explicitly joins the worker before any member is destroyed. The worker captures
`served` by `shared_ptr` and drives `run`; detached exchanges also capture it.
The `DeterministicStart` receives a token from the separate `served->stop`
source, which exchange failure or a stop reply requests. A `jthread` destructor
requests its own stop token before joining; that token is not the token passed
to `run`. Removing the explicit join without analyzing ownership and stop
ordering changes the destruction protocol and does not establish equivalent
cancellation. If the worker releases the final owner, destruction on the worker
can try to join itself; the existing manual join has the same ownership concern,
so a simple type substitution does not resolve it. No execution schedule was
constructed to prove that final-owner path reachable.

## Follow-up: frozen source and public diagnostic

On 2026-11-08 the user approved the public read-tool diagnostic as the test
seam. The adjacent `../pi-frozen-1049` checkout was subsequently found clean at
`71023aec1546d6d677a65669c5665c0927ce0dd0`. Its
`packages/durable/src/truncate.ts:60-67` confirms that `formatSize` uses
`toFixed(1)`, so the Node midpoint observation above is the frozen algorithm,
not merely a hypothetical comparison. A direct `tsx` module import failed
because the installed `@earendil-works/chord` does not export `./output`;
the source and the separate Node expression supply the observation.

A temporary public read-tool Catch2 test wrote one 51,456-byte ASCII line and
compared the entire warning diagnostic against the frozen midpoint result. The
owning `cch_tests_durable` target built, and the focused CTest selected one test
and failed: the actual warning contained `Line 1 is 50.2KB`; the frozen formula
expects `Line 1 is 50.3KB`. The temporary failing test was removed, so it is not
part of the checked-in suite. This is an existing externally visible difference,
not a difference introduced by a refactor.

A separate GCC 16.2.1 C++23 probe set each floating-point rounding mode and
formatted the exact double `50.25`. In `FE_TONEAREST`, both `snprintf` and
`std::format` returned `50.2`; in `FE_UPWARD`, `snprintf` returned `50.3` and
`std::format` returned `50.2`. `FE_DOWNWARD` and `FE_TOWARDZERO` returned
`50.2` in both. No repository code setting the mode was found. This observation
prevents a blanket claim that the suggested direct replacement preserves the
existing behavior in every calling environment.

## Format slice: owner decision and implementation

The user subsequently chose frozen pi as authoritative and explicitly approved
both the public read-tool diagnostic seam and correction of the 51,456-byte
midpoint from `50.2KB` to `50.3KB`. The test writes oversized ASCII lines and
checks the returned diagnostic for the two midpoint neighbors, the 1 MiB unit
transition, and the 1.25 MiB midpoint and its neighbors. Before the change,
this test failed at both 51,456 and 1,310,720 bytes; afterward it passed, as
did the existing frozen read-tool case (2/2 selected).

The implementation uses `std::format` and moves exact binary midpoint values
(.25 or .75 in the selected unit) by one representable step upward so that
its ties-to-even result matches JavaScript's larger-decimal `toFixed` tie.
A separate C++/Node differential probe compared 1,154,721 values: every byte
count from 0 through 1 MiB, 1,024 pairs of MB midpoint neighborhoods, and
100,000 deterministic 64-bit samples. The latter used seed
`0x18374652613241`, multiplier `6364136223846793005`, increment
`1442695040888963407`, and arithmetic modulo 2^64. GCC 16.2.1 compiled the
probe and Node v26.13.0 supplied the `toFixed` oracle. It found zero differences.
This probe copies the formatter expression and is supplementary to the public-seam test;
it does not claim all possible inputs were exhaustively exercised.

The #1090 receipt's four image rows and scenario routing remain unchanged: the
production edit affects read-text diagnostics, not image preparation. Its exact
36-case selection passed and its production digest and JUnit evidence were
rebound. Consolidation build/check, receipt boundaries (32 receipts), and frozen
evidence verification (84 bundles, 180 artifacts, 1,012 scenarios) passed.
Two independent reviewers examined standards and frozen behavior. The standards
review led to nonfatal content-count checking, earlier diagnostic context, stream
state checking and use of `contains`; the spec reviewer found no parity defect.
The differential probe is empirical evidence, not a proof about non-default
floating-point environments.

Full Debug validation first selected 4,580 tests: three unrelated ProcessTerminal,
PTY and tool-cancellation cases failed and 11 were skipped; all three failures
passed in an isolated rerun. After the review adjustments, a full unfiltered
`TERM_PROGRAM=herdr CCH_LANE_NAME=lane-1232-format ctest --preset vcpkg -j2`
selected 4,580, passed 4,569, skipped 11 and exited 0. The build, format check
and `git diff --check` passed. No assertion or timeout was relaxed.

## Remaining work

The worker ownership, cancellation and final-owner ordering still need analysis
and tests before changing its thread type. The file-descriptor/process lifetime
and number-parser parts of #1232 also remain open. The format slice does not
complete the ticket. The earlier rounding-mode observation concerned the old
`snprintf` behavior; this slice follows the user's frozen-pi decision.
