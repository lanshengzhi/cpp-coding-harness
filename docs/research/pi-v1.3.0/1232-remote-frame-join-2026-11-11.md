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
`~/.cache/vcpkg/archives`). Lane `remote-selfjoin-1232` was acquired in the main checkout and in
the slice, and released in both when the work ended.

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
- New frame-local `DeterministicWorkerJoin` in `src/codemode/Remote.cpp`. The serve coroutine
  declares it immediately after `served`, so it destroys first, while the frame still owns the
  state. Its destructor: no-op when the handle is not joinable; `state.stop.request_stop()` when
  the run is not finished, so a driver parked on a caller answer is woken instead of waited on;
  `std::terminate()` when `worker.get_id() == std::this_thread::get_id()` — an unreachable
  invariant, fail-closed rather than a detach; otherwise `worker.join()`.
- Worker creation assigns the guard's `worker` member; the lambda still captures only `served`.
- Protocol messages, buffering, `dispatch`, the exchange reply handling and the terminal settle
  point (`co_await served->exchange(batch)` immediately before `co_return`) are unchanged.

## Regression and red/green evidence

Test-only seam `src/codemode/RemoteWorkerTestHook.hpp`, behind `CCH_REMOTE_WORKER_TEST_HOOKS`,
which only the `cch_tests_codemode` target defines. That shard compiles a second copy of
`Remote.cpp` through `tests/codemode/RemoteWithTestHooks.cpp`, so the archive's `Remote.o` is not
linked there and the production archive cannot carry the seam. With no gate installed the hooks
are one atomic load and a no-op.

`tests/codemode/RemoteWorkerLifetimeTest.cpp` — `Remote deterministic worker is joined by the serve
frame, never by the worker` (`[codemode][remote][deterministic][issue1232remote]`). The schedule
is forced, not raced:

1. the hook parks the worker immediately after `drive()` returns and before its closure releases
   the state;
2. the serve coroutine has taken its terminal message and is returning;
3. the case records whether the frame joined, from which thread, whether the parked closure was
   the state's only owner, and whether the serve's own `AsyncResult` completed before the release;
4. the gate is released, the join and the serve completion are read back, the driving context
   thread is joined, and only then does the first `CHECK` run (`CODING_STANDARDS.md` §11.10 — a
   failing assertion in this `-fno-exceptions` TU runs no destructor, so no gate or thread may be
   live when it can fail).

Every wait carries a 20 s bound as a liveness backstop; correctness is read from the latched
observations, never from "the wait returned". The only process-level backstop is CTest
`TIMEOUT 30` with `RUN_SERIAL TRUE` on this case.

**Red** (hook present, baseline state-owned handle): CTest fails in 1.23 s with
`Subprocess aborted`, `terminate called after throwing an instance of 'std::system_error'` /
`what(): Resource deadlock avoided`, SIGABRT. gdb at `std::__throw_system_error(int)` stops on
the worker thread with `~RemoteDeterministicServe` (`Remote.cpp:757`) ←
`_Sp_counted_base::_M_release_last_use` — the closure's release *was* the last use — ←
`~<lambda>` (`Remote.cpp:845`, the worker closure) ← `std::thread::_Invoker`/`_State_impl`. That
is the self-join and the last-owner property in one stack.

**Green** (form A): measured at the release point in the green binary —
`worker_parked=true`, `join_started=true`, `join_thread_differs_from_worker=true`,
`owners_while_parked=2`, `join_finished=false`, `serve_completed=false`; after the release
`join_finished=true`, `serve_completed=true`, `serve_completed_after_join=true`,
`serve_completed_ok=true`. The case passed 10/10 consecutive runs.

### Does the serve's internal `AsyncResult` now complete after the worker exits?

Yes, and the test measures it rather than assuming it: at the release point the worker is still
parked and `serve_completed` is false, and the completion is observed only after
`join_finished`. The frame joins inside the coroutine body, so Asio's `co_spawn` completion
handler — which delivers the `AsyncResult` — runs after the frame returns, i.e. after the worker
thread ended. The guard also makes the frame-destroyed path (context shutdown, abandonment)
request stop and join instead of releasing a state whose handle the worker owns.

### Does anything depend on "serve completes before worker exit"?

Checked, and nothing does. Caller settlement for the deterministic path comes from the terminal
exchange message (`make_deterministic_exchange` → `Run::State::settle`), not from the serve's own
result; the serve's result only feeds the caller's `!settled` fallback error (`Remote sandbox
returned before the script settled`), which stays a no-op once the run is settled. The frozen
artifacts (`fixtures/pi-v1.3.0/audit-1053/remote-exchange.json`, the audit-1099 deterministic
capture) pin message kinds, sizes, replies and run results, not retirement order. The #1179
receipt's recorded 52-case selection passes 52/52 on this tree (see below), so no frozen or
registered observation changed.

### Premise correction for the mixed tree's "final run owner" case

The main checkout's uncommitted candidate has `Remote deterministic worker releases the final run
owner safely`, which asserts `gate->state.use_count() == 1` while the worker is parked and that the
state is destroyed afterwards: it requires the worker closure to be the state's last owner. That
premise is false in form A by construction — the serve frame keeps `served` alive across the join
(measured: 2 owners), and the state is destroyed on the serving thread after the join — so the case
moves to `use_count() == 1` only in the shape this ticket removes. `origin/main` does not carry that
case (only the mixed tree does), so nothing in this slice was deleted; the delivered case keeps the
discrimination (baseline: abort; fixed: frame join from a non-worker thread, the closure is not the
last owner, completion after the join) with the corrected premise.

## Validation

- Debug `cmake --build build -j4`: 371/371, build-phase Parity Architecture Gate **PASS**
  (`build/parity-build-gate.json`). The only warning in the tree is pre-existing and unrelated
  (`tests/coding_agent/tui/SystemThemeTest.cpp`, unused function).
- Focused, owning shard label after the full build: `ctest --test-dir build -L codemode -j4` →
  **178/178 passed**; with only the codemode shard registered the same label was **82/82 passed**
  (40.09 s), including reversed answers, the pending missing result, the abort/settle cases and the
  local worker-admission cases; the new case 1.16 s.
- ASan/UBSan focused: `ctest --test-dir build/asan-ubsan -R "Remote|deterministic" -j2` →
  **20/20 passed** (new case 1.21 s), rerun on the final tree. The wider ASan label run (shard-only
  ASan tree) was **78/82**: the four failures are the pre-existing local-sandbox timer-service
  heap-use-after-free in `CodemodeOwnerTest.cpp` cases (`issue1096`/`issue1196` worker cases; that
  file never constructs a Remote), recorded separately in the main workspace's
  `1232-local-sandbox-timer-uaf-2026-11-09.md`.
- #1179 receipt selection (`ctest --test-dir build -R '^(Remote deterministic execution ... )$'
  -j1`, the record's own 52-case regex): **52/52 passed**.
- Full Debug suite (`ctest --test-dir build -j4`, `TERM_PROGRAM=herdr` so the Herdr PTY case keeps
  its host precondition, `CCH_LANE_NAME=remote-selfjoin-1232`), run twice on the delivered tree:
  **4588 selected, 4572-4573 passed, 12 skipped, 3-4 failed** (743.62 s and 245.29 s). The three
  stable failures are pre-existing at `origin/main` and reproduced on a pristine `git archive
  origin/main` export:
  - `cch_parity_gate_fixture`: the PCH-evidence rule from the tip commit rejects the fixture's own
    legal tree (`target 'cch_ai' declares PCH input ... but has 0 generated PCH compile commands`),
    same failure in the pristine export with no build;
  - `cch_pi_v130_consolidation_check`: fails at `1101-structured-content` on a stale `vcpkg.json`
    digest, same message in the pristine export (details below);
  - `cch_pi_v130_consolidation_boundaries`: 11 failures + 5 errors, all of them the same
    pre-existing `vcpkg.json` digest error escaping the expected-error assertions; the pristine
    export produces the identical 11 + 5 with the lane name exported.
  The fourth failure in the second run only, `Native TUI InteractiveEngine preserves exact nested
  and clipped cursor coordinates and protocol across mode switch in both directions`, passed in the
  first run and passes alone (1/1, 1.25 s): a TUI case sensitive to `-j4` load, in a module this
  change does not touch.
- Formatting: `scripts/format-check.sh` clean on the committed branch (CI form,
  `scripts/format-check.sh origin/main`); one line of the new test was reflowed by `git clang-format`
  before the commit and the shard and ASan selections were rebuilt and rerun afterwards.
- `fixtures/pi-v1.3.0/consolidation-1058/consolidate_audits.py check`: **FAIL**, see below.

## Delivery receipt impact

`check` fails at `1101-structured-content` on `vcpkg.json`, which is already stale on `origin/main`
(pinned `dc956d94…`, current `f19c0e24…`); a pristine `git archive origin/main` export fails with
the identical message, and `cmake/targets/Durable.cmake` (1090, 1184),
`tests/support/ModelsFixture.hpp` (1118) and `tests/codemode/CodemodeRemoteTest.cpp` (1179) are
stale there too. Independent of that pre-existing state, this change makes these inputs stale:

- **1179** (`1179-codemode-comparisons`): `src/codemode/Remote.cpp` (pinned `612eb8fa…` = the
  pre-fix baseline hash, current `f40ede25…`) and `cmake/tests/CodemodeTests.cmake` (pinned
  `ee80abcd…`, current `53349ba2…`). Its recorded selection is the 52-case regex above with JUnit
  artifact `docs/research/pi-v1.3.0/deliveries/1179-revalidation-1232-codemode-parser.xml`
  (selected 52, `testedRevision` `33107f7d…`); revalidation would rerun exactly that selection on
  the merged tree and refresh both input digests and the JUnit digest.
- **1192** and **1194**: `cmake/tests/CodemodeTests.cmake` only (same pinned/current hashes);
  each needs its own recorded selection rerun.

No receipt, JUnit artifact, digest or ledger entry was rebound or rewritten, per instruction; the
new test and hook files are not receipt inputs.

## Capability status

Internal lifetime fix with no capability claim: no ledger row, frontier entry, scenario or routing
digest changes, and the ticket's other items remain open. No push, no external message.

## Remaining boundary

- The frame join blocks the serving executor thread until the worker exits. In the delivered path
  `drive()` has already returned, and an unfinished run is woken by `request_stop()` (the run's
  `stop_notify` wakes its condition variable and the interpreter polls the token). An
  uncooperative producer or guest can still delay that join; the liveness contract is the same one
  the earlier records state, but the wait now happens in a join instead of a leaked state.
- Not measured here: the abandoned-frame path (io_context destruction while a deterministic run is
  in flight) beyond the analysis above; the generic (non-deterministic) `serve_codemode_remote`
  worker thread; `RemoteWorkerReaper` static shutdown/admission questions; the local sandbox timer
  UAF; `DaemonClient` FDs, number parsers and `format_size`.
- The test seam is kept out of the archive by the `CCH_REMOTE_WORKER_TEST_HOOKS` definition living
  only on the test target; unlike the mixed candidate there is no `nm` gate proving it, so the next
  slice that touches this CMake file may want to add one.
