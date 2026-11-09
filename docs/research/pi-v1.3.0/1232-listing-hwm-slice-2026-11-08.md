# #1232 listing peak-memory parser slice — 2026-11-08

## Scope

The 20,000-entry listing test uses the same stream-based peak-memory parser as the read-tool test. The parser is extracted unchanged into a test-only shared header; both measurements must succeed and the second must be at least the first before the unchanged 8 MiB bound is checked. The listing limit remains 100 entries. This slice does not complete #1232.

## Validation

At the approved injected status-stream seam, the missing-field regression failed against the listing's extracted old zero fallback (1/1 failed). The existing shared parser tests cover a valid reading, missing and zero fields, malformed integers, units, duplicate fields, integer and byte-conversion overflow, and a read error after a valid field. Removing the `bad()` check made the partial-read assertion fail (1/1 failed); removing the byte-overflow and unit checks made that test fail (3 assertions). The five selected parser, listing and read-tool cases passed before the final initial-read-error assertion was added.

A further mutation removing the numeric error and zero checks produced failures for both `0 kB` and `18446744073709551616 kB`. Restoring the exact parser and rebuilding passed the five selected tests (5/5), including the added initial read-error assertion. The earlier full run used the same restored parser.

The shared test header introduces no direct receipt input; neither consumer source is named in the delivery file, and the pinned durable CMake declaration is unchanged. Configure and architecture build gate passed. A complete build passed with the existing unrelated workspace candidates present. Consolidation check initially failed on the pre-existing modified codemode CMake receipt input and passed after path-limited isolation of the Remote/SIGPIPE candidates. On that isolated snapshot the incremental build and unfiltered CTest passed (4,587 selected; 4,576 passed, 11 skipped; `TERM_PROGRAM=herdr`, `-j2`, 461.88 s). Consolidation build/check passed; receipt boundary passed 32 receipts, 808 claims and 966 comparisons; frozen evidence verification passed 84 bundles, 180 artifacts and 1,012 scenarios. The scoped clang-format and diff checks passed.

The isolated candidates were restored after validation. Their combined tracked binary diff matched byte for byte, and all four isolated untracked files matched their recorded SHA-256 digests. The full-tree formatting script was not used because unrelated C++ changes are present; the scoped `git clang-format --diff HEAD` check covered the three staged C++ paths.

## Review follow-up

Standards review identified that the original listing regression reached only the parser. The listing now takes both measurements through `listing_peak_memory(std::istream&)`, which propagates the shared parser result. The regression exercises this same measurement path with valid, missing and unreadable status streams. Mutating that path to return `read_peak_memory(status).value_or(0)` failed the missing and unreadable checks (1 selected case, 2 assertions failed); restoring propagation passed. The shared header moved to `tests/support/PeakMemory.hpp` in `cch::tests`, both fallible readers are `[[nodiscard]]`, and the listing high-water ordering uses `CHECK` with a short-circuited subtraction.

`tests/` is already a formal test include root, and the durable shard uses it; neither consumer nor the new header is a direct input in `1058-deliveries.json`. No CMake declaration changed. Configure, full incremental build, the 5-case focused selection and, after the final explicit standard includes, a 4-case focused selection passed. Scoped formatting, `scripts/format-check.sh HEAD`, and staged diff checks passed. Receipt boundary passed 32 receipts / 808 claims / 966 comparisons, and frozen evidence verification passed 84 bundles / 180 artifacts / 1,012 scenarios.

A new unfiltered full CTest run on the existing mixed working tree selected 4,628 cases: 4,615 passed, 11 skipped and 2 failed (462.06 s). Both failures, `cch_pi_v130_consolidation_check` and `cch_pi_v130_consolidation_boundaries`, reported the pre-existing modified `cmake/tests/CodemodeTests.cmake` digest mismatch for receipt 1179. Standalone consolidation build and check failed for the same reason. The unrelated candidate was preserved; this run does not supersede the earlier isolated full pass above, which preceded these review changes. The final source change after that run only added explicit standard-library includes; the full incremental build and focused cases were rerun after it.

## Final isolated validation

After the review fixes, the Remote, daemon and AsyncBridge code candidates were isolated with path-limited backups. Configure and full incremental Debug build passed, including the architecture gate. Unfiltered CTest passed: 4,587 selected, 4,576 passed, 11 skipped, zero failed (`TERM_PROGRAM=herdr`, `-j2`, 421.97 s). Consolidation build/check passed; receipt boundary passed 32 receipts / 808 claims / 966 comparisons, and frozen evidence verification passed 84 bundles / 180 artifacts / 1,012 scenarios. `scripts/format-check.sh HEAD` and staged and unstaged diff checks passed. The shared header and its two consumers are not direct receipt inputs; the receipt boundary check passed without receipt changes.

## Remaining boundary

Independent standards and specification reviews were performed by the coordinator; the four standards follow-ups are described above. Other #1232 slices remain outside this change.
