# Issue #1232 DaemonClient pipe ownership slice — 2026-11-08

## Scope

The test-only `DaemonClient` now gives each successful pipe endpoint a
`cch::support::UniqueFd` owner immediately, and returns on `fork` failure
before entering the parent path. Parent endpoints remain owned through the
client's lifetime. The OS `pipe` and `fork` call boundaries are injectable for
failure tests; the caller and controlled-subprocess seams were approved by the
user. No capability ledger status is claimed.

## Evidence

In isolated subprocesses with five-second alarms, injected second-pipe failure
checks that both first-pipe descriptors are closed while the client remains in
scope; injected fork failure checks all four descriptors under the same
condition. Both check `fcntl(F_GETFD)` fails with `EBADF` and the client reports
not alive. Each test failed against the preceding implementation and passed
after its corresponding fix. A third controlled subprocess sends a startup
marker, observes the two retained parent endpoints open and the two unused
endpoints closed while the client is alive, checks retained endpoints close at
scope exit, and reaps its own cooperating child. It failed when
endpoint ownership was deliberately released and passed when restored. The
three focused cases passed 3/3. These tests do not prove all OS error paths.

## Validation and review

The Debug build and unfiltered CTest suite passed: 4,572 passed, 11 skipped,
0 failed (4,583 selected; `TERM_PROGRAM=herdr`, `-j2`). The existing frozen
listing differential test passed. The build-phase architecture gate passed.
Consolidation build and check passed; receipt boundary validation passed for
32 receipts, 808 claims and 966 comparisons; frozen evidence verification
passed for 84 bundles, 180 artifacts and 1,012 scenarios. Format and diff
checks passed. The test source is not directly named by the receipt or
consolidation files searched; the boundary check passed with no receipt change.

Independent standards review found no issues. Spec review requested a
controlled successful startup and closure test; a subsequent review requested
checking the two unused endpoints as well. Both suggestions were incorporated.

## Remaining boundary

This slice does not implement child reaping, pidfd acquisition or fallback,
SIGPIPE handling, bounded startup or request I/O, `dup2` error handling, or
closed-standard-descriptor remapping. Numeric parsers and codemode worker
lifetime are also outstanding under #1232. The issue remains open.
