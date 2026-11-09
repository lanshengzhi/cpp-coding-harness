# RemoteRetirement final independent review, 2026-11-10

## Scope and authority

Issue #1232. Candidate files `src/codemode/RemoteRetirement.hpp`,
`tests/codemode/RemoteRetirementTest.cpp`, `cmake/tests/TestAggregate.cmake` and
`tests/codemode/retirement-standalone/CMakeLists.txt` in the independent
worktree `/home/lansy/.t3/worktrees/cpp-coding-harness/t3-9a406641`, branch
`t3/remote-retirement-slice-validation`. The design authority is ADR 0077,
still `status: proposed`. This candidate is not integrated with the production
Remote teardown path and is not a complete drain witness.

## Delivered in this review

- Publication now detaches registered waiters under the state mutex and marks
  the state published there. `completion.set_value()`, the test hook and every
  `boost::asio::post` run after the lock is released. This removes user/executor
  calls under the lock and permits a test hook to reenter `delivery_failures()`.
- Late registration and every observer take the same mutex: a waiter registered
  after publication returns `true` immediately; otherwise it is appended before
  the publisher detaches the list, so it is notified. Multiple waiters are
  detached together and each is posted independently.
- Callback construction uses `std::make_unique` instead of an owning raw `new`.
- `CCH_REMOTE_RETIREMENT_TESTING` gates the `before_post` injection seam and its
  constructor overload; production builds have neither.
- Every case carries `[issue1232]`; the public API comment now says the aggregate
  `delivery_failures()` count cannot identify a failed waiter and offers no
  bounded recovery or per-waiter error.

## Red and focused green

Red: `Remote retirement notification may reenter observer` calls
`observer.delivery_failures()` from the injection hook. Against the original
lock-held `post`, the process hung and an external `timeout 5s` ended it with
SIGTERM (exit 124). With lock-outside publication the case passes.

Focused results, strict no-exception normal Debug: 15/15. Strict ASan/UBSan
Debug with `detect_leaks=1`: 15/15. Exception-enabled local debugging standalone
(unsupported production configuration): 19/19, including the reentrancy case.
These are the pre-closeout tree's counts; §Closeout carries the final numbers.
Both formal configurations ran the build-phase Parity Architecture Gate. The
normal and ASan caches name this worktree; the ASan cache carries the configured
sanitizer flags. Prebuilt vcpkg archive instrumentation was not established.

## Deliberately unresolved

**Post failure.** A thrown `post` or `before_post` is counted after reacquiring
the mutex, and remaining waiters are still attempted. No per-waiter failure is
reported and no waiter is completed with an error: `post` receives a handler,
not a failure return, and the channel waiter has no per-waiter error slot. A
bounded cancel-all retry was not implemented. Asio's `channel_service::cancel`
invokes `post_receive`, which posts a cancelled completion through the same
executor; if `post` itself fails, that recovery can fail identically. Claiming
a reliable recovery API would therefore overstate what this build can prove.
The supported `-fno-exceptions` configuration terminates on a throwing
allocation; it promises neither recovery nor a successful witness. This is a
recorded boundary, not a fix.

**Coverage gaps.** No constructible throwing `executor` test was added: the
injection hook exercises the catch path but not a real failing executor. There
is no deterministic multi-failure or late-waiter-under-failure case, and no
test for `post` failure during cancellation. The real failure schedule remains
unmeasured.

**Witness boundary retained.** A `RemoteRetirement::Observer` covers admitted
participants and registered stop-callback leases. It does not cover terminal
delivery, the posting tail, queued notification, completion handlers, captures,
executor lifetimes, or work transferred outside this accounting domain. A
candidate witness must not be read as a full Remote drain. The caller must keep
the executor and borrowed resources alive until those independent obligations
quiesce. Executor destruction concurrent with publication remains unproved.

## Validation state

The full unfiltered suite ran against the pre-closeout header and test files:
`TERM_PROGRAM=herdr CCH_LANE_NAME=retirement-review-1232 ctest --test-dir build
--output-on-failure -j2` selected 4,602, passed 4,590, skipped 12, failed 0,
exit 0, 402.51 s. The twelve skips are the existing frozen-pi, realtime-clock and
TUI-capture preconditions; they are not passes. The normal and ASan build-phase
architecture gates passed. No receipt was rebound, no production integration
was attempted, and no commit, push or external delivery was made. Unrelated
worktree changes were preserved.

## Closeout, 2026-11-10

This review's findings are resolved in the candidate; its scope, contract and
recorded gaps are otherwise unchanged. The numbers below replace the tree
counts quoted above.

- **P1-1, reentrancy out of the exception guard.** `Remote retirement
  notification may reenter observer` uses only the `CCH_REMOTE_RETIREMENT_TESTING`
  hook and the public `delivery_failures()`, so it is valid under
  `-fno-exceptions` and moved out of the `#if defined(__cpp_exceptions)` block.
  Against the pre-review lock-held post it hung; against the current
  lock-released publication it passes in strict builds. On its own the move
  takes the strict shard from 15 to 16; P2-4's added case makes it 17.
- **P1-2, final digests and rebuild.** The copy-time digests in the formal
  record (`e7ce1fc1…`, `415cd14a…`) named the pre-review files. Immediately
  before this closeout the header was
  `9a5f0f1689dc781933f9e8cfd36b3bf5b0631ac5a19f0d742a6dff630782ee04` and the
  test `af25615db10fbb658138733f783a2944583441dc121bfec021ff1795cddc31c4`; after
  the closeout edits they are
  `6bacf8745e6c997d6f8faf1169fb5db39acf54c26d459f572f2af184f3ccff21` and
  `246e6581fedd5c5a6e72e2e349539bea3acd2e5e1e9ffdbee02c903194218c33`. The
  aggregate and standalone CMake files are unchanged at `35089a70…` and
  `b3454199…`. Both formal targets were rebuilt from these inputs and each
  build-phase Parity Architecture Gate passed; no reconfigure was needed because
  no include directive changed. The formal record's "three exception-only"
  sentence was wrong for its own tree, which had nineteen cases with four
  exception-only; the final source has twenty, seventeen registered in strict
  mode and three exception-only.
- **P2-1, test allocation.** `Remote retirement waits for callback capture
  destruction` now builds its `Tail` with `std::make_unique<Tail>(Tail{…})`
  instead of an owning `new` (CODING_STANDARDS.md §7.7).
- **P2-4, late executor observer.** `Remote retirement completes an observer
  registered after publication` retires the participant first, then starts
  `async_wait_on_executor` on a fresh executor: one `poll()` must complete it
  and the injection counter must stay at zero, which separates the `published`
  early return from any posted notification. The branch had no test before.
- **P3, documented postconditions.** Comment-only header additions state the
  waiter channel capacity of one and its dropped duplicate, the publication
  latch (delivery at most once), that a wait registered after publication
  returns true without a post and reports only the state at registration, that
  `on_stop` on an already-stopped token invokes the callback synchronously, the
  hook's placement outside the mutex, and that `retire_self()` empties its
  handle so a repeat call is a no-op and `admit_child()` returns `nullopt`.

Final validation, same lanes and configurations as above: strict normal Debug
17/17 and strict ASan/UBSan Debug with `detect_leaks=1` 17/17 (selected 17,
skipped 0, failed 0); exception-enabled GCC standalone 20/20; full unfiltered
suite 4,604 selected, 4,592 passed, 12 skipped, 0 failed, exit 0, 402.52 s. The
twelve skips are the same frozen-pi, realtime-clock and TUI-capture
preconditions. JUnit files: `retirement-normal-final.xml` `97328ed9…`,
`retirement-asan-final.xml` `ae5e8e46…`, `full-final.xml` `b3e5f66d…`,
`retirement-exceptions-final.xml` `41b36d33…`. The earlier XMLs, including this
review's 4,602-test run, describe the pre-closeout tree and are not evidence for
the final files. No receipt was rebound, no production integration was
attempted, and no push or external delivery was made.
