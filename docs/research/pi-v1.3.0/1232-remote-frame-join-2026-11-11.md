# #1232 deterministic Remote self-join: frame-owned worker handle — 2026-11-11

## Scope

Issue [#1232](https://github.com/lanshengzhi/cpp-coding-harness/issues/1232) item 4
(`CODING_STANDARDS.md` §9.13, raw `std::thread` in `RemoteDeterministicServe`). Items 1-3
(`DaemonClient` pipe FDs, throwing number parsers, `format_size`) and the other open #1232
slices are not in this change.

A clean slice, not a mixed tree: `git clone --no-hardlinks` of the main checkout into
`/home/lansy/.cache/cch-remote-selfjoin-slice`, `origin` reset to the GitHub remote, branch
`remote-selfjoin-1232` at `origin/main` `71ad3e83a` ("build: benchmark cold builds and validate
PCH evidence"), `git status --short` empty before the first edit. The main checkout was read
only, and only for its uncommitted candidate (`src/codemode/Remote.cpp`,
`tests/codemode/CodemodeRemoteTest.cpp`, `src/codemode/RemoteWorkerTestHook.hpp`,
`tests/codemode/RemoteWithTestHooks.cpp`); nothing from it was copied as a delivery tree. The
`t3-1232-retirement` and `t3-1ca063d0` trees were read only. No PID/PGID watchdog was used at
any point.

Build configuration: absolute `-B /home/lansy/.cache/cch-remote-selfjoin-slice/build` (preset
`vcpkg`, Debug) and `build/asan-ubsan` (preset `vcpkg-asan-ubsan`), isolated
`TMPDIR=/home/lansy/.cache/cch-remote-selfjoin-slice/.tmp`, `VCPKG_ROOT` pointing at the pinned
vcpkg checkout the main worktree already bootstrapped (ports resolved from
`~/.cache/vcpkg/archives`). Lane `remote-selfjoin-1232` was held in the slice for the build and
test work and released when it ended; the main checkout was verified lock-free and never built
here.

Delivery mechanics: the branch was unpublished when the Spec-axis review asked for the commit
subjects to stop using a closing keyword. Both subjects were rewritten from `Fix #1232: …` to
`Refs #1232: …` (`detach worker handle ownership from RemoteDeterministicServe`, `keep worker
test seam out of the production archive`) by cherry-picking the two trees onto `origin/main` and
re-committing. At the rewrite point the tree (`ce5f2217…`) and the net diff against `origin/main`
(sha256 `fa904eda…`) were byte-identical to the pre-rewrite branch, and only the two subject lines
differ in the log. `Fix #1232` would have closed the umbrella issue on merge, which this slice does
not do.

## Ownership chain that produces the abort

`serve_deterministic_remote` allocates one `shared_ptr<RemoteDeterministicServe>`. The serve
coroutine frame, the worker lambda's captured copy, and each detached `dispatch` coroutine hold
that pointer. The baseline stored the worker's `std::thread` **inside** the state and joined it in
`~RemoteDeterministicServe`. When the worker closure's release is the last use, the destructor runs
on the worker thread and joins that same thread: `std::system_error: Resource deadlock avoided`,
`std::terminate` under `-fno-exceptions` (exit 134). Terminal publication and `drive()` return are
not thread retirement, so any schedule that retires the frame and the exchange closures first
reaches this state.

## Delivered behavior

- `RemoteDeterministicServe` is pure shared state again (`guest`, `queue`, `exchange`, `run`,
  `stop`); no thread handle, no destructor.
- New frame-local `ScopedDeterministicWorker` in `src/codemode/Remote.cpp`. The serve coroutine
  declares it immediately after `served`, so it destroys first, while the frame still owns the
  state. Its destructor: no-op when the handle is not joinable; `state.stop.request_stop()` when
  the run is not finished, so a driver parked on a caller answer is woken instead of waited on;
  `std::terminate()` when `worker.get_id() == std::this_thread::get_id()` — an unreachable
  invariant, fail-closed rather than a detach; otherwise `worker.join()`. The member carries a
  lifetime comment (non-owning reference to the frame's `served`) and a `std::thread`-not-
  `std::jthread` comment: a jthread's `request_stop()` reaches only its own stop source, not
  `state.stop`, and the scope owner must control the request-stop-then-join ordering.
- The same destructor is what makes the abandoned-frame path quiescent: destroying the serve frame
  while its run is unfinished requests stop on that run and joins the worker from the destroying
  thread, before the frame (and the state its closure shares) is gone. No path detaches.
- Worker creation assigns the owner's `worker` member; the lambda still captures only `served`.
- Protocol messages, buffering, `dispatch`, the exchange reply handling and the terminal settle
  point (`co_await served->exchange(batch)` immediately before `co_return`) are unchanged.

## Test seam

The seam is `tests/support/DeterministicWorkerTestGate.hpp` (renamed from the production-tree
`*Hook` header so it cannot be mistaken for a §3.3 `move_only_function` alias). The test-only
`cch_tests_codemode_worker_variant` static library compiles configure-time copies of the same
`CCH_CODEMODE_SOURCES` with `CCH_REMOTE_WORKER_TEST_HOOKS` and force-includes the gate only into
the copied `Remote.cpp`; `cch_tests_codemode` links that variant instead of `cch_codemode`. The
copies are test build artifacts, not a second production compilation: compiling the production
source path twice would fail the Parity Gate's `RULE_DUPLICATE_SOURCE_COMPILATION`, and an
`#include` wrapper reintroduced GCC's `-Wsubobject-linkage` diagnostic. A copied source is its own
translation unit, so the production path compiles exactly once, the variant needs no warning
suppression, and the shard's binary contains one codemode implementation. Production source never
includes the tests/support header; with no gate installed the hooks are an atomic load and a no-op,
and the production archive is compiled without either the macro or the header.

The gate exposes one park point per schedule (`WorkerParkPoint::after_drive` /
`inside_drive`), the worker's thread id, a non-owning view of the shared state, and witnesses for
the frame's request-stop-and-join:

| Hook | Thread | Observation |
| --- | --- | --- |
| `record_deterministic_worker_started(state)` | worker, at thread start | worker thread id, `weak_ptr` to the state |
| `record_deterministic_worker_drive_returned()` | worker, after `drive()` | the run settled and the worker is about to release its closure |
| `park_deterministic_worker()` | worker, after `drive()` | `after_drive` park (`parked`) |
| `park_deterministic_worker_in_drive(stop)` | worker, inside `drive()`, in the caller's `call` callback | `inside_drive` park plus `stop_requested()` sampled before and after the park |
| `record_deterministic_worker_join_started()` | destroying thread, before `join()` | join thread id, `join_started` |
| `record_deterministic_worker_join_finished()` | destroying thread, after `join()` | `join_finished`, whether the state is still alive, remaining strong owners |

`park_deterministic_worker_in_drive` is reached from a production seam: `host.on_call` always
captures the run's `std::stop_token` and calls `after_deterministic_worker_call(stop)`, which is an
empty function in the production archive (`[[maybe_unused]]` parameter) and the park in the
variant. The unconditional call keeps the capture used in both configurations, so neither build
carries an unused capture or an unused parameter.

## Regression and red/green evidence

`tests/codemode/RemoteWorkerLifetimeTest.cpp` carries both cases, tagged
`[codemode][remote][deterministic][issue1232]` so `ctest -L issue1232` selects them; both are
discovered under one `catch_discover_tests` selection with `TIMEOUT 60` and `RUN_SERIAL TRUE`
(the gate is process-wide, and a recorded defect aborts the process).

### The join case: `Remote deterministic worker is joined by the serve frame, never by the worker`

The schedule is forced, not raced:

1. the gate parks the worker immediately after `drive()` returns and before its closure releases
   the state;
2. the serve coroutine has taken its terminal message and is returning;
3. the case records whether the serve frame joined, from which thread, what the state's ownership
   looked like at the join, and whether the serve's own `AsyncResult` completed before the release;
4. the gate is released, the join and the serve completion are read back, the driving context
   thread is joined, and only then does the first `CHECK` run (`CODING_STANDARDS.md` §11.10 — a
   failing assertion in this `-fno-exceptions` TU runs no destructor, so no gate or thread may be
   live when it can fail).

The ownership claim no longer rests on a bare `use_count() >= 2`. It is asserted from the frame's
own destructor after `join()` returned:

- `join_thread == serve_thread` and `join_thread != worker_thread`: the frame — not the worker —
  performed the join, from the thread that runs the serve coroutine.
- `join_finished` and `state_alive_after_join`: the state was still alive after the worker thread
  had ended, so the worker closure's release was not its last use.
- `owners_after_join == 1`: exactly one strong owner remained at that instant, and the only
  candidate is the frame's own `served` — the destructor recording this *is* the frame's scope
  owner, and the worker's thread has already ended. `lock()` is the gate's own temporary and is
  subtracted.
- `serve_completed_after_join` (and `!serve_completed_before_release`): the serve's internal
  result completes only after the worker exited.

Under the recorded defect (handle owned by the state) there is no join to observe at all: the
process aborts on the worker thread before any of this is recorded.

### The abandoned-frame case: `Remote deterministic worker is stopped and joined when an abandoned serve frame is destroyed`

The schedule covers the path ADR 0040 names — a coroutine Owner must establish quiescence before
destroying a frame:

1. the script awaits a caller answer (`const value = await tools.wait(1); return value;` with one
   declared tool);
2. the gate parks the worker **inside `drive()`**, in the caller's `call` callback, after the call
   message was queued; the caller receives the message and returns a pending reply whose completion
   is dropped, so the run cannot settle on its own;
3. the case records that no stop had been requested while the worker sat there
   (`stop_requested_before_park_release == false`), then abandons the frame on its own thread:
   `io_context::stop()`, join the pumping thread, and destroy the context. Destroying it destroys
   the suspended serve frame, whose scope owner requests stop and joins the worker. A throwaway
   probe confirmed the mechanism before the case was written (abandoning a suspended `co_spawn`
   coroutine by destroying its stopped `io_context` destroys the nested awaitable frames and their
   locals on the destroying thread);
4. a releaser thread waits for the frame's `join_started` and only then releases the in-drive park.
   The request therefore cannot have come from the caller: it is made between the park and the
   release, by the frame's destruction. A missed signal still releases the park, so the case reads
   a missing join from its observations instead of hanging;
5. after `context.reset()` the case reads the observations and asserts before anything else:

  - `join_started`, `join_finished`, `join_thread_is_destroying_thread`,
    `join_thread_differs_from_worker` — the frame destroyed its scope owner (no detach) and joined
    from the destroying thread, never from the worker;
  - `stop_requested_after_park_release` — the run's stop token was requested while the worker was
    inside `drive()`;
  - `drive_returned` — the worker left `drive()`, which the caller's unanswered call made
    impossible without that request;
  - `state_alive_after_join` and `state_released_after_destroy` — the state outlived the worker
    thread and was released only when the frame was destroyed, so the frame owned it across the
    whole join;
  - `!serve_completed` — an abandoned frame delivers no terminal outcome to its consumer;
  - `abandon_elapsed < 10 s` (measured 2.2-2.6 ms) — the whole abandonment is bounded far below the
    case's 60 s CTest timeout.

### Red evidence

**Red A — the abandon path without the stop request** (temporary edit, not committed: the
destructor's `state.stop.request_stop()` line removed, everything else identical, variant rebuilt
after a reconfigure so the copied translation unit matched): the case never returns. The frame's
`join()` waits for a worker parked on an answer that will never arrive — the process was still
alive with its two threads after 8 s, and CTest reports
`Remote deterministic worker is stopped and joined when an abandoned serve frame is destroyed (Timeout)`
at the case's 60 s hard timeout (`2300 - … (Timeout)`, 60.06 s). That is the discrimination this
case exists for: destroy-without-stop is an unbounded join, not a wrong outcome.

**Red B — the handle back inside the state** (temporary edit, not committed: the state regains the
`std::thread` member and the join destructor with the same hook calls, the coroutine assigns
`served->worker`): the join case aborts the process — `terminate called without an active
exception`, SIGABRT, exit 134 (with the fail-closed self-join invariant in place; the baseline
that lacked the invariant threw `Resource deadlock avoided` out of `join()`) — and the abandon case
fails in 8 of 14 assertions with no join, no request and a state that is never released, i.e. the
frame destroyed nothing and the worker/state leaked. Both shapes are the recorded defect.

### Green measurements

- the new case: **60/60** consecutive runs, 1.10 s each; the `[issue1232]` selection (the two
  lifetime cases plus the numeric-parser case): **60/60**, 1.09-1.10 s per lifetime case;
- `owners_after_join == 1`, `state_alive_after_join == true` on every one of those runs.

### Cross-repair dependency: the delivery loop's stop wake needs `21fecff`

The delivered schedule parks the worker **inside** `drive()` before the run's caller-answer wait.
When the park is released, the run observes the request at its next interrupt poll
(`Deterministic.cpp` checks `stop_token.stop_requested()` at the top of the delivery loop and in
the wait predicate), so this case's discrimination and its boundedness do not depend on a
`condition_variable` wake.

A worker that is **already parked** in the delivery loop's `cv.wait` for a caller answer is a
different shape: the only wake is the run's `stop_notify` stop callback. On this tree that callback
calls `shared.cv.notify_all()` without pairing with `shared.mutex`, so a request that lands between
the wait predicate's `stop_requested()` load and the wait's enqueue can lose the wake, and the
frame's join would then be unbounded. Commit `21fecff` ("Fix deterministic stop-notify lost wake
and rebind #1179", branch `t3/deterministic-stop-notify-handshake`) pairs that callback with
`shared.mutex` and closes the window.

**This slice does not contain that fix**, deliberately: it is #1179's delivery and carries that
receipt's input rebinding, so taking it here would duplicate another lane's change and rewrite
another ticket's receipt from this slice. The consequence is recorded as a boundary, not hidden:

- the destroy → request-stop → join control flow and its boundedness are verified here for a run
  still inside `drive()` (the state the case forces);
- the wake of a delivery-loop wait that is already parked depends on `21fecff`, and is **not**
  verified here;
- therefore **this slice's abandoned-frame boundedness depends on that stop-notify fix; if the fix
  is not present (before or with this slice), the abandonment path must be treated as not fully
  verified.** Re-running this case on a tree that carries both changes is the check that closes it.

## Behavior changes

Two observable changes are intentional parts of this contract, not side effects of moving a
thread handle:

1. **Destroying a serve frame with an unfinished run now cancels that run and waits for it.**
   Basis: ADR 0040 requires a coroutine Owner to establish quiescence before destroying a frame and
   forbids destroying a resource while an admitted operation can still use it; detaching is
   excluded for the same reason (a detached worker would outlive the state its closure keeps alive),
   and `std::jthread` cannot express it (its stop source is not the run's). The frame's destruction
   can therefore block until the run settles; the run's own stop token is the bound, and the
   regression case measures it. Before this change the same destruction released the state instead
   of waiting, which is what made the self-join reachable.
2. **The serve's internal `AsyncResult` now completes only after the worker exits.** The join runs
   inside the coroutine body, and Asio delivers the terminal completion from the `co_spawn`
   completion handler after the frame returns, i.e. after the worker thread ended. This is a
   contract change for any consumer that observes the serve result's completion time: it now
   implies worker retirement. Nothing in this tree depends on the old order — caller settlement for
   the deterministic path comes from the terminal exchange message
   (`make_deterministic_exchange` → `Run::State::settle`), and the serve's own result only feeds
   the caller's `!settled` fallback error (`Remote sandbox returned before the script settled`),
   which is a no-op once the run is settled. The frozen artifacts
   (`fixtures/pi-v1.3.0/audit-1053/remote-exchange.json`, the audit-1099 deterministic capture) pin
   message kinds, sizes, replies and run results, not retirement order, and the #1179 receipt's
   52-case selection still passes (below), so no frozen or registered observation changed.

## Premise correction for the mixed tree's "final run owner" case

The main checkout's uncommitted candidate has `Remote deterministic worker releases the final run
owner safely`, which asserts `gate->state.use_count() == 1` while the worker is parked and that the
state is destroyed afterwards: it requires the worker closure to be the state's last owner. That
premise is false in the delivered shape — the serve frame keeps `served` alive across the join
(measured: 2 owners before the join, 1 after the worker thread ended) — so the case only holds in
the shape this ticket removes. `origin/main` does not carry that case (only the mixed tree does),
so nothing in this slice was deleted; the delivered cases keep the discrimination with the
corrected premise and now assert the surviving owner instead of a count taken at the park.

## Validation

Measured on the delivered tree, absolute paths, Debug preset unless stated:

- Configure: `cmake -S . -B build` → Parity Architecture Gate **PASS** (13 targets, 69 dependencies,
  287 sources, 3 961 includes). Full preset build: 6 steps / 24.7 s, build-phase Parity Architecture
  Gate **PASS** (`build/parity-build-gate.json`). The only warning in the tree is pre-existing and
  unrelated (`tests/coding_agent/tui/SystemThemeTest.cpp`, unused function).
- Owned shard label: `ctest --test-dir build -L codemode -j4` → **180 selected, 0 failed, 3 skipped**
  (75.1 s). The three skips are the registered pi-continuation cases (586-588), not this change.
- New case repeats: `*abandoned serve frame*` **60/60**; `[issue1232]` **60/60**; 1.09-1.10 s per
  lifetime case, abandonment 2.2-2.6 ms.
- ASan/UBSan: `ctest --test-dir build/asan-ubsan -R "Remote|deterministic" -j2` → **21/21 passed**.
  The wider ASan label run (`-L codemode`) → **80/84**, the four failures being the pre-existing
  local-sandbox timer-service heap-use-after-free in `CodemodeOwnerTest.cpp` cases
  (`issue1096`/`issue1196`; that file never constructs a Remote), recorded separately in the main
  workspace's `1232-local-sandbox-timer-uaf-2026-11-09.md`.
- #1179 receipt selection, the record's own regex: **52 selected / 52 passed** (103.8 s).
- Full Debug suite: `ctest --test-dir build -j4` with `TERM_PROGRAM=herdr` so the Herdr PTY case
  keeps its host precondition → **4 590 selected, 3 failed, 12 skipped** (236.5 s). The three are
  pre-existing at `origin/main` and unchanged by this slice:
  - `cch_parity_gate_fixture`: `PARITY-5005`, `target 'cch_ai' declares PCH input … but has 0
    generated PCH compile commands`, from the tip commit's PCH-evidence rule;
  - `cch_pi_v130_consolidation_check`: `1101-structured-content: delivery digest mismatch:
    vcpkg.json`;
  - `cch_pi_v130_consolidation_boundaries`: the same stale digest escaping its expected-error
    assertions.
- Warning gate: `cch_warning_gate` **PASS** (651 project compile commands recompiled with `-Werror`,
  including the seven variant copies and the forced-include gate header).
- Formatting: `scripts/format-check.sh` clean on the working tree and, in CI form,
  `scripts/format-check.sh origin/main` clean on the branch (two formatting hunks in the new case
  were reflowed by `git clang-format` before the commit, and the shard selection was rebuilt
  afterwards).

`nm` evidence:

- `nm -C --defined-only build/libcch_codemode.a | grep -E
  'DeterministicWorkerTestGate|park_deterministic_worker|record_deterministic_worker|deterministic_worker_test_gate'`
  → no output: the production archive has no gate symbol.
- `nm -C --defined-only build/cch_tests_codemode | grep -c ' T cch::codemode::serve_codemode_remote'`
  → `1`: the test executable has one strong `serve_codemode_remote` definition.
- The executable also carries `deterministic_worker_test_gate` and
  `park_deterministic_worker_in_drive`, proving it linked the hook-enabled variant rather than the
  production archive; `codemode_remote_symbol_ownership` automates these three checks and passes.

## Delivery receipt impact

`check` fails at `1101-structured-content` on `vcpkg.json`, which is already stale on `origin/main`
(pinned `dc956d94…`, current `f19c0e24…`); the earlier round reproduced the identical message from a
pristine `git archive origin/main` export, and `cmake/targets/Durable.cmake` (1090, 1184),
`tests/support/ModelsFixture.hpp` (1118) and `tests/codemode/CodemodeRemoteTest.cpp` (1179) are
stale there too. Independent of that pre-existing state, this change makes these pinned inputs
stale:

| Receipt | Stale inputs (pinned → delivered) |
| --- | --- |
| 1179 | `src/codemode/Remote.cpp` `612eb8fa…` → `ec533098…`; `cmake/tests/CodemodeTests.cmake` `ee80abcd…` → `44a623d1…`; `cmake/targets/Codemode.cmake` `d5c1c594…` → `a07926c5…` |
| 1118 | `cmake/targets/Codemode.cmake` `d5c1c594…` → `a07926c5…` |
| 1192 | `cmake/tests/CodemodeTests.cmake` `ee80abcd…` → `44a623d1…`; `cmake/targets/Codemode.cmake` `d5c1c594…` → `a07926c5…` |
| 1194 | `cmake/tests/CodemodeTests.cmake` `ee80abcd…` → `44a623d1…`; `cmake/targets/Codemode.cmake` `d5c1c594…` → `a07926c5…` |

Correction to the earlier round of this record: it reported `src/codemode/Remote.cpp` as "current
`f40ede25…`" and `cmake/tests/CodemodeTests.cmake` as "current `53349ba2…`". Neither is the
delivered file; both values belonged to an intermediate tree that the standards-review follow-up
then changed. The delivered hashes are the ones in the table, recomputed from the branch tip with
`sha256sum`. 1179's recorded selection is still the 52-case regex above, whose JUnit artifact is
`docs/research/pi-v1.3.0/deliveries/1179-revalidation-1232-codemode-parser.xml` (`testedRevision`
`33107f7d…`); revalidation reruns exactly that selection on the merged tree and refreshes the input
and JUnit digests. The new test and hook files are not receipt inputs.

No receipt, JUnit artifact, digest or ledger entry was rebound or rewritten, per instruction.

## Capability status

Internal lifetime fix with no capability claim: no ledger row, frontier entry, scenario or routing
digest changes, and the ticket's other items remain open. No push, no external message.

## Remaining boundary

- The frame join blocks the destroying thread until the worker exits. For destruction that happens
  inside the coroutine body the worker has normally already returned from `drive()`; for an
  abandoned frame the run is woken by `request_stop()` (the interpreter polls the token, and the
  delivery-loop wait is woken through `stop_notify` — see the `21fecff` dependency above). An
  uncooperative producer or guest can still delay that join; the liveness contract is the same one
  the earlier records state, but the wait now happens in a join instead of a leaked state.
- Not measured here: the wake of a delivery loop that is already parked on a caller answer (it
  needs `21fecff`); the generic (non-deterministic) `serve_codemode_remote` worker thread and its
  `RemoteWorkerReaper` static shutdown/admission questions; the local sandbox timer UAF;
  `DaemonClient` FDs, number parsers and `format_size`.
- The test seam is kept out of the archive by the `CCH_REMOTE_WORKER_TEST_HOOKS` definition living
  only on the test variant target. `codemode_remote_symbol_ownership` proves with `nm` that the
  production archive has no gate symbol and the test executable has exactly one
  `serve_codemode_remote` definition plus the hook-enabled variant.
