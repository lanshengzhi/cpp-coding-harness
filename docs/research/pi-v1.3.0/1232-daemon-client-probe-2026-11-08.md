# Issue #1232 DaemonClient probe — 2026-11-08

## Scope

Read-only investigation at `e81e1cb99` of the test-only client in
`tests/durable/EnvIoListingDifferentialTest.cpp`. No implementation or test
was changed. The existing user-owned workspace modifications were preserved.
Issue #1232 remains open.

## Observed boundaries

- The first `pipe` failure returns immediately. If the second fails, the
  first pipe's two descriptors leak. A failed `fork` is treated as the parent:
  it closes two ends, stores the other two, reads EOF and reports not alive;
  there is no child to reap. Neither failure has a deterministic regression.
- On success, all four descriptors are raw, child `dup2` errors are ignored,
  inherited extra pipe ends survive `exec`, and no PID is stored or reaped.
  The child calls `_exit(127)` when `execl` fails; the parent sees EOF and
  reports not alive, but does not reap it. `alive()` only checks a prefix
  received at startup; it is not a child liveness check or exec handshake.
  Descriptor numbers 0 and 1 being initially free need particular care when
  closing/duplicating ends; naive close-all after `dup2` can close stdin or
  stdout. Close-on-exec and correct remapping also matter for EOF.
- A write to a pipe with no readers can terminate the entire test process via
  SIGPIPE before the existing error return runs. Partial writes and interrupted
  reads/writes are also possible. A process-wide SIGPIPE disposition change
  would affect unrelated tests and children; any chosen handling needs a
  scoped, tested policy, including pre-existing pending signals.
- Startup marker reading and request I/O have no deadline, and the marker and
  response lengths are not bounded. A blocking `waitpid` alone can therefore
  hang on an uncooperative child. Closing both parent endpoints before waiting
  gives the cooperating daemon EOF and prevents a writer blocked by a full
  output pipe from depending on the parent draining it, but does not prove
  bounded shutdown for every child or descendant holding descriptors. Retry
  `waitpid` on EINTR only for the specific successfully forked PID; do not
  reap unrelated children. Any timeout/termination policy must state its
  scope and cannot guarantee termination of an uninterruptible process.
- At frozen pi revision `71023aec1546d6d677a65669c5665c0927ce0dd0`,
  `packages/env/daemon/src/frame.rs` treats EOF while reading a frame length
  as end of input. `main.rs` then kills tracked groups, closes output and joins
  the writer before exiting. This is source evidence for the cooperative EOF
  path, not a proof of bounded completion in all daemon states.

The owning shard is `cch_tests_durable` (`cmake/tests/DurableTests.cmake`).
A literal search of `1058-deliveries.json` found no direct reference to this
source path; this alone does not establish the full transitive receipt impact.
No build, CTest, receipt, or frozen evidence check ran during this probe.

## Proposed seams requiring confirmation

1. The existing test-only `DaemonClient` caller interface (`alive()`,
   `request()`, and scope exit), with deterministic local helper executables
   in isolated, timeout-protected subprocess tests. Exercise cooperative EOF,
   reaping of that exact child, exec failure, broken-pipe survival and a
   deliberately stalled child. OS-level child/FD observations are necessary
   to distinguish leaked resources; they expose the safety property but do
   not prove every scheduling or platform case.
2. A narrowly scoped operating-system boundary for deterministic `pipe`,
   `fork`, and, if needed, `dup2` failures, without mocking other client
   internals. It can exercise second-pipe and fork failure cleanup that normal
   successful executions cannot reliably force; it does not establish that
   every kernel error or process state was covered. Real exec failure can use
   a nonexistent executable. Decide the least intrusive injection mechanism
   after the seam is agreed; resource limits alone need isolation and may not
   reliably force fork failure.

The existing differential test remains the normal daemon protocol regression;
when the frozen executable is unavailable it skips, so it cannot substitute
for the deterministic lifecycle cases. The read-tool seam approved for the
previous formatting slice does not cover either proposed seam.

## Remaining work

Obtain agreement on the lifecycle and failure-injection seams, then do one
red/green cycle at a time. Resolve the bounded shutdown and SIGPIPE policies
against the chosen scope before claiming those properties. Complete the
relevant receipt impact analysis, focused and full validation, independent
review and lane delivery. Numeric parsers and codemode worker lifetime are
separate outstanding parts of #1232.
