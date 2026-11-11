# #1232 integration: frame join × stop-notify handshake — 2026-11-11

## Scope

Issue [#1232](https://github.com/lanshengzhi/cpp-coding-harness/issues/1232). Two published
branches integrated into one tree, and the cross-repair dependency between them measured:

| Branch | Tip | Base | Carried |
| --- | --- | --- | --- |
| `remote-selfjoin-1232` | `cf717581b` | `origin/main` `71ad3e83a` | frame-local deterministic worker ownership, test gate, two lifetime cases |
| `t3/deterministic-stop-notify-handshake` | `21fecff` | `f2b027fc6` | `stop_notify` paired with `shared.mutex`, #1179 receipt rebind |

Clean slice, not a mixed worktree: `git clone --no-hardlinks` of the main checkout into
`/home/lansy/.cache/cch-1232-integration`, `origin` reset to the GitHub remote, branch
`integrate-1232` at `71ad3e83a`, `git status --short` empty before the first cherry-pick. The
main checkout was read only; its unrelated in-flight changes and its free lane lock were
preserved. Lane `integrate-1232` was held in the slice for every build and test command and
released at the end.

Cherry-picks, in order, all clean — no conflict, no content change, no manual resolution:

```
185e7e955  Refs #1232: detach worker handle ownership from RemoteDeterministicServe   (6d90f3a0e)
7d7411735  Refs #1232: keep worker test seam out of the production archive            (ee9e68766)
9c3b20dc5  Refs #1232: cover the abandoned serve frame and its stop dependency        (cf717581b)
fd9efbdf1  Fix deterministic stop-notify lost wake and rebind #1179                   (21fecff)
```

`f2b027fc6` is three commits behind `71ad3e83a`, so `21fecff` was replayed rather than merged;
`src/codemode/Deterministic.cpp`, `src/codemode/Remote.cpp`, `cmake/tests/CodemodeTests.cmake`
and `cmake/targets/Codemode.cmake` are byte-identical between the two bases, which is why the
replay applied without a conflict. No commit subject contains a closing keyword for #1232 or
#1179: the fourth subject's `Fix` is followed by prose, not `#`, so it closes nothing.

## Correction to the recorded dependency

The `1232-remote-frame-join-2026-11-11.md` record states that the frame's join "would then be
unbounded" without `21fecff`. Measured on this tree, that is not the shape of the window.

`ScopedDeterministicWorker`'s destructor calls `state.run->finished()` **before**
`state.stop.request_stop()`, and `DeterministicRun::Impl::finished()` takes `shared.mutex`:

```cpp
if (state.run.has_value() && !state.run->finished()) state.stop.request_stop();
```

`cv.wait(lock, ready)` holds `shared.mutex` from the predicate's first evaluation until the
waiter is enqueued. So the destructor's `request_stop()` is ordered after the enqueue whenever
`finished()` blocked on that hold, and a `request_stop()` from the destruction path therefore
cannot land in the lost-wake window. Confirmed empirically: with only the pairing reverted, the
**existing** abandoned-frame case (`Remote deterministic worker is stopped and joined when an
abandoned serve frame is destroyed`) still passes, because its run is parked in `drive()` and
is woken by the interpreter's token poll, not by the wait.

The window is real, but it needs the two acquisitions to be *separated*: `finished()` returns
false and releases the mutex, the run then takes it and enters the predicate, and only then does
`request_stop()` fire. That is an ordinary time-of-check/time-of-use gap in the destructor, and
it is what the new case forces.

## The discriminating case

`tests/codemode/RemoteWorkerLifetimeTest.cpp`, tagged
`[codemode][remote][deterministic][issue1232][issue1232wake]`:

> An abandoned serve frame stops and joins a deterministic run parked in the caller-answer wait

Schedule, forced rather than raced, using one test-only gate
(`tests/support/DeterministicWorkerTestGate.hpp`) and no PID/PGID watchdog:

1. the script is `const value = await tools.wait(1); return value;` with one declared tool, so the
   run reports a caller call and then has nothing to apply;
2. `park_deterministic_delivery_loop_top` stops the worker at the delivery loop's top, before it
   samples the stop token or takes `shared.mutex`;
3. the serve frame is destroyed on the test thread; its destructor reaches
   `state.run->finished()` while the worker is still at the loop top, so the mutex is free and the
   check returns false, then parks at `park_before_deterministic_worker_stop_request`, between the
   check and the request;
4. a sequencer thread releases the loop-top park, the worker proceeds into
   `cv.wait`'s predicate and parks at `park_deterministic_delivery_wait` — mutex held, waiter not
   yet enqueued — and only then is the destructor released to call `request_stop()`;
5. the sequencer releases the predicate park once the run's stop callback has been entered.

Assertions, all read after every test-owned thread is joined (`CODING_STANDARDS.md` §11.10), and
ordered by a monotonic gate ordinal rather than by "each step happened":

- **(a) the frame's destruction requests stop** — `stop_request_pending` and
  `frame_stop_requested`, with `frame_stop_requested_seq > stop_request_pending_seq`, and the run
  already parked (`delivery_wait_parked`) at that point;
- **(b) the wake goes through `Deterministic.cpp`'s `stop_notify`** — the callback is entered
  after the predicate park (`stop_notify_entered_seq > delivery_wait_parked_seq`), it acquires
  `shared.mutex` only after the predicate released it
  (`stop_notify_mutex_acquired_seq > delivery_wait_released_seq`), it notifies after acquiring
  (`stop_notify_notified_seq > stop_notify_mutex_acquired_seq`), and the worker leaves `drive()`
  after that notification (`drive_returned_seq > stop_notify_notified_seq`). Nothing else can
  have woken the run: the caller never answers;
- **(c) the worker exits and is joined** — `join_started`/`join_finished`, the joining thread is
  the destroying thread and differs from the worker thread, the state is still alive after the
  join and released only when the frame is destroyed, and no terminal outcome reaches the
  abandoned consumer;
- **(d) the budget** — the case's internal waits are 5 s each and the measured destruction is
  **1.875–2.023 ms** over five sampled runs, against a 30 s CTest `TIMEOUT`.

### Red

Temporary edit, not committed: `src/codemode/Deterministic.cpp` loses exactly the
`{ std::lock_guard lock(shared.mutex); }` scope and its `record_deterministic_stop_notify_mutex_acquired()`
call; everything else, including all three parks and both repairs' control flow, is unchanged. The
variant was rebuilt after a reconfigure so the copied translation unit matched.

- direct run, 5 s in: process alive with exactly **two** threads, both in `futex_wait`;
- `gdb` backtrace of those two threads (test binary launched under gdb so ptrace is a parent
  relationship; `kernel.yama.ptrace_scope=1` blocks attaching to a sibling):

  ```
  Thread 1 (test thread)
    std::thread::join()
    cch::codemode::(anonymous namespace)::ScopedDeterministicWorker::~ScopedDeterministicWorker  Remote.cpp:813
    serve_deterministic_remote() lambda frame
    boost::asio::detail::co_spawn_entry_point -> awaitable_frame_base::destroy
    boost::asio::io_context::~io_context
    std::unique_ptr<boost::asio::io_context>::reset
    CATCH2_INTERNAL_TEST_4                                        RemoteWorkerLifetimeTest.cpp:479

  Thread 3 (deterministic worker)
    pthread_cond_wait
    std::condition_variable::wait
    cch::codemode::detail::DeterministicRun::Impl::drive                    Deterministic.cpp:829
    cch::codemode::detail::DeterministicRun::drive                          Deterministic.cpp:533
    serve_deterministic_remote() worker lambda                              Remote.cpp:913
  ```

  i.e. the destroying thread is blocked in the scope owner's join and the worker is blocked in
  the delivery loop's `cv.wait` — an unbounded join, not a wrong outcome;
- `ctest`: `An abandoned serve frame stops and joins a deterministic run parked in the caller-answer
  wait (Timeout)`, **30.04 s** at the case's 30 s bound;
- in the same red tree, the other four codemode `[issue1232]` cases are **4/4 green**, so the red is
  the new case and not a broken build.

### Green

- **30/30** consecutive direct runs, no failure, 33.3 s total (≈1.11 s each);
- `ctest -L issue1232`: **12/12 passed** on the full build (the codemode shard contributes five of
  them; the other seven are other #1232 slices on `origin/main` that carry the same label), the new
  case 1.14 s;
- 27 assertions in the case.

## Integration changes on top of the two branches

- `tests/support/DeterministicWorkerTestGate.hpp`: a third `WorkerParkPoint::delivery_wait`, the
  two worker parks and the frame's pre-request park, a monotonic `phase` ordinal with one
  `*_seq` per observed step, and the stop-notification witnesses. Every park is a latched
  permit with the existing 2 s liveness backstop and is one-shot, so a loop revisit cannot park
  without a releaser.
- `src/codemode/Deterministic.cpp`: two `#ifdef`-gated seam calls (loop top, wait predicate) and
  three inside the `stop_notify` callback. The callback body's mutex pairing is unchanged.
- `src/codemode/Remote.cpp`: the pre-request park and its after-request witness, both
  `#ifdef`-gated; the `finished()`-then-`request_stop()` control flow is unchanged.
- `cmake/tests/CodemodeTests.cmake`: the gate is force-included into the variant's copied
  `Deterministic.cpp` as well as `Remote.cpp` (the copies are build-tree artifacts, so the
  production path still compiles once); the `[issue1232]` discovery spec became
  `[issue1232]~[issue1232wake]` and the new case gets its own spec with `TIMEOUT 30`, because its
  red is an unbounded join and 30 s is a cheaper backstop than 60 s.

The production archive is unaffected: `nm -C --defined-only build/libcch_codemode.a` has no
`DeterministicWorkerTestGate`/`park_deterministic_*`/`record_deterministic_*` symbol;
`build/cch_tests_codemode` has exactly one strong `cch::codemode::serve_codemode_remote` and four
gate symbols; `libcch_codemode.a` carries one `Deterministic.cpp.o`.
`codemode_remote_symbol_ownership` automates the first two and passes.

## Validation

Debug preset, absolute `-S`/`-B` under `/home/lansy/.cache/cch-1232-integration`, isolated
`TMPDIR`, `VCPKG_ROOT` at the pinned `2f1d6054…` checkout, `VCPKG_INSTALL_OPTIONS=--only-binarycaching`.

- Configure: Parity Architecture Gate **PASS** (13 targets, 69 dependencies, 287 sources,
  3 961 includes).
- Full Debug build: 368 steps, **11 m 05 s**, rc 0; build-phase Parity Architecture Gate **PASS**;
  `cch_warning_gate` **PASS** (515 s) inside the suite.
- `ctest -L codemode -j4`: **181 selected, 181 passed, 3 skipped** (77.7 s), and 181/181 on
  five further `-j4` runs after the commit (see the one-off note below). The three skips are
  the registered pi-continuation cases 586-588.
- `ctest -L issue1232`: **12 selected, 12 passed** (6.6 s).
- ASan/UBSan (`build/asan-ubsan`, preset `vcpkg-asan-ubsan`, absolute `-S`/`-B`, configured in
  the same slice): shard `cch_tests_codemode` built (314 steps), `ctest -R 'Remote|deterministic'`
  → **22 selected, 22 passed**, 0 failed (28.9 s), the new case 1.19 s. `detect_leaks=0`, since
  the suite is not leak-clean by policy.
- #1179's recorded selection, the receipt's own anchored regex: `ctest -N -R` confirms exactly
  **52** cases; **52 selected, 52 passed**, 0 failed, 0 skipped (104.5 s, `-j 1`).
- Full Debug suite, `TERM_PROGRAM=herdr` so the Herdr PTY case keeps its host precondition:
  **4 591 selected, 4 575 passed, 12 skipped, 4 failed** (12 m 42 s). 4 591 is the self-join
  slice's 4 590 plus this case.
- Formatting: `scripts/format-check.sh 71ad3e83a` clean (CI form, merge-base of the branch);
  `git diff --check` clean.

### Failures, and why none is this integration

| Case | Verdict |
| --- | --- |
| `cch_parity_gate_fixture` | pre-existing: `PARITY-5005`, `target 'cch_ai' declares PCH input … but has 0 generated PCH compile commands`, from `71ad3e83a`'s PCH-evidence rule |
| `cch_pi_v130_consolidation_check` | pre-existing: `1101-structured-content: delivery digest mismatch: vcpkg.json` |
| `cch_pi_v130_consolidation_boundaries` | pre-existing: the same stale digest escaping its expected-error assertions |
| `Native TUI fullscreen scroll-to-end indicator paints on a direct-fullscreen launch while the transcript is scrolled` | pre-existing unfiltered-run flake, not this change |

One further failure appeared **once** and did not reproduce. It is recorded here rather than
smoothed over:

| Case | Occurrence | Follow-up |
| --- | --- | --- |
| `RemoteCodemodeSandbox runs tools in the caller and the script on the remote` (`[codemode][remote]`, test #2231) | once, in a `ctest -L codemode -j4` run issued in the same tool call as a full-tree `git clang-format` sweep, so five jobs plus a whole-tree formatter ran concurrently; the run omitted `--output-on-failure`, so its assertion text was written only to `Testing/Temporary/LastTest.log` and was overwritten by the next run | not reproduced: the case passes alone (1.16 s), **20/20** under four CPU-burner processes, and five further `-L codemode -j4` runs were **181/181** — three of them deliberately re-run with a concurrent `git clang-format` sweep to recreate the contention |

No path from this integration reaches that case: it drives `RemoteCodemodeSandbox` against the
fake `RecordingRemote`, so it never constructs a deterministic run, never reaches
`ScopedDeterministicWorker`, and never enters `DeterministicRun::Impl::drive` where the new parks
live; it also carries no wall-clock assertion. The cause is unestablished, and the one-off is not
attributed to this tree, but neither is it explained.

The three named failures reproduce their documented messages verbatim (re-run individually with
`--output-on-failure`). The fourth is the TUI case #1039: it passes alone (1.17 s) and **20/20**
under four CPU-burner processes, and earlier records already carry it as a full-run-only failure
at other bases — `1178-chord-reconciliation.md:240` (fails identically at base `0b118e912`),
`1080-submissions-inbox-admission.md:153`, `1113-agent-queue-admission.md:192`,
`1173-durable-event-stream.md:136`, `1240-remaining-investigation-2026-11-07.md:35`. This
integration touches only `src/codemode/**`, `cmake/tests/CodemodeTests.cmake` and two codemode
test files; the TUI case constructs no codemode sandbox and the new parks are `#ifdef`-gated and
inert without an installed gate. No new failure is attributable to this tree.

## Receipt impact — list only, nothing rebound

No receipt, JUnit artifact, digest or ledger entry was changed by this integration. The fourth
cherry-pick carries `21fecff`'s own authorized #1179 rebind; it pins `testedRevision`
`f2b027fc6…`, which does not describe this tree, so #1179 is stale again. The self-join branch
moved `cmake/targets/Codemode.cmake` and `cmake/tests/CodemodeTests.cmake`; this integration adds
`src/codemode/Deterministic.cpp` and `src/codemode/Remote.cpp` to that.

Stale inputs, pinned → actual, split by cause:

| Receipt | Input | Pinned | Actual | Cause |
| --- | --- | --- | --- | --- |
| 1179 | `src/codemode/Deterministic.cpp` | `6e89bab0…` | `6db75486…` | integration (seam calls) |
| 1179 | `src/codemode/Remote.cpp` | `612eb8fa…` | `2b3866b0…` | self-join + integration |
| 1179 | `cmake/tests/CodemodeTests.cmake` | `ee80abcd…` | `fbc4c60f…` | self-join + integration |
| 1179 | `cmake/targets/Codemode.cmake` | `d5c1c594…` | `a07926c5…` | self-join |
| 1179 | `vcpkg.json` | `dc956d94…` | `f19c0e24…` | pre-existing at `71ad3e83a` |
| 1179 | `tests/codemode/CodemodeRemoteTest.cpp` | `8ecfe916…` | `0e392f85…` | pre-existing at `71ad3e83a` |
| 1118 | `cmake/targets/Codemode.cmake` | `d5c1c594…` | `a07926c5…` | self-join |
| 1118 | `vcpkg.json` | `dc956d94…` | `f19c0e24…` | pre-existing at `71ad3e83a` |
| 1118 | `tests/support/ModelsFixture.hpp` | `e3a1015b…` | `5258f123…` | pre-existing at `71ad3e83a` |
| 1192 | `cmake/targets/Codemode.cmake` | `d5c1c594…` | `a07926c5…` | self-join |
| 1192 | `cmake/tests/CodemodeTests.cmake` | `ee80abcd…` | `fbc4c60f…` | self-join + integration |
| 1194 | `cmake/targets/Codemode.cmake` | `d5c1c594…` | `a07926c5…` | self-join |
| 1194 | `cmake/tests/CodemodeTests.cmake` | `ee80abcd…` | `fbc4c60f…` | self-join + integration |

Full delivered digests for the four codemode inputs:

```
6db754869d4b442ec77f84414fb5d7bf4adcfc7fb6f06d631bd15e409d51732c  src/codemode/Deterministic.cpp
2b3866b02e108bf8468f5b3540b925603665dbc4dcac61d93cfd589588b9de3a  src/codemode/Remote.cpp
fbc4c60f65065558bed9a8039e1cc6a1e3667de1c113962cb4c1b249185b261f  cmake/tests/CodemodeTests.cmake
a07926c5cde51829b8532c50c0af5e71da2ba26ab35348c92ad707c82ede4b31  cmake/targets/Codemode.cmake
```

Exact selections to rerun when a receipt is authorized to be rebound — each was verified with
`ctest -N` on this tree, and every count still matches the record:

- **1179** — extract the pattern from the receipt and run it verbatim:
  ```bash
  python3 -c "
  import json
  r = next(x for x in json.load(open('docs/research/pi-v1.3.0/1058-deliveries.json'))['records'] if x['ticket']=='1179')
  c = r['validation']['command']; print(c[c.index('-R')+1])" > /tmp/p1179
  ctest --test-dir <build> -N -R "$(cat /tmp/p1179)"          # expect 52
  CCH_LANE_NAME=<lane> TERM_PROGRAM=herdr ctest --test-dir <build> --no-tests=error \
      -R "$(cat /tmp/p1179)" --output-on-failure -j 1 \
      --output-junit docs/research/pi-v1.3.0/deliveries/<new>.xml
  ```
- **1118** — `ctest --test-dir <build> --no-tests=error -L issue1118` (19 cases).
- **1192** — the record's anchored 5-case regex
  (`a public codemode caller executes a script against an injected nested tool` …
  `closing codemode settles the run before its worker exit releases admission`), `-LE architecture`.
- **1194** — `ctest --test-dir <build> --no-tests=error -LE architecture -L issue1194` (1 case).

Per the delivery reconciliation contract, a rebind also refreshes the JUnit digest, `selected`,
`testedRevision`, `recordedAt`, and regenerates
`1058-consolidated-capability-ledger.json`. None of that was done here.

## Limits

- The two parks that force the window are a schedule, not a capability: they remove a race the
  production code can reach on its own (`finished()` and `request_stop()` are separate
  acquisitions), but they do not measure how often it happens in the field. The red run needed
  the parks to be repeatable; without them the same interleaving is a real but rare race.
- The red's evidence is a hard timeout, not an assertion failure. That is the honest shape of an
  unbounded join — the test cannot report an outcome it never reaches — but it means a red run
  costs the case's full 30 s and leaves the process to CTest's kill.
- The ASan/UBSan tier covers the codemode shard only; the rest of the suite was not built or run
  under sanitizers here.
- The case proves boundedness of *this* destruction path when the caller never answers. It does
  not cover a `request_stop()` that arrives from `dispatch` on a failed exchange or a caller stop
  reply; that path does not take `shared.mutex` first, so it can reach the window without any
  park, and it is the one a future case should add.
- An uncooperative producer or guest can still delay the join; the liveness contract is the
  token, not a deadline.
- Not re-run here: the #1179 receipt's frozen scenarios against a rebind, the generic
  (non-deterministic) `serve_codemode_remote` worker and its `RemoteWorkerReaper` shutdown
  questions, `DaemonClient` FDs, the number parsers, `format_size`, and the local-sandbox timer
  use-after-free recorded in `1232-local-sandbox-timer-uaf-2026-11-09.md`.
- No push, no external write, no receipt change.
