# Read-only lane preflight

Run from a Git checkout, with a full expected upstream commit and its checkout root:

```sh
python3 scripts/dev/lane-preflight.py --lane lane-example \
  --revision <full-commit-id> --checkout <upstream-checkout-root>
```

`PI_CHECKOUT` supplies the upstream root if `--checkout` is omitted. Python 3 and Git on the system default executable path are required. The report suppresses paths, branch names, lock metadata, Git errors and file contents. It counts staged, unstaged and untracked entries, checks the lock identity without claiming it, and checks upstream revision and status. Dirty work, a foreign lock, or a mismatch need attention; unreadable or missing required inputs are unknown. Both produce a nonzero exit. A matching lock identity is not proof of ownership; the snapshot does not authorize a lane or prove a capture is correct.

Git runs with a restricted environment, global and system configuration disabled, optional locks off, and fsmonitor and untracked cache disabled. Repository configuration is still read. Status is unknown if `core.filemode` is not explicitly true, if an indexed symbolic link exists and `core.symlinks` is not explicitly true, if configuration contains filters, includes or trace2 settings, if the index contains assume-unchanged or skip-worktree entries, or if it contains submodules or an unrecognized index mode. With no indexed symbolic links, a missing or false `core.symlinks` does not alone prevent a status check. Git can otherwise hide executable-bit changes or a symbolic link replaced by a regular file containing its target. These conservative checks also cover missing or invalid settings; they do not establish filesystem capabilities.

The Git index represents regular files (including executable files), symbolic links and gitlinks; the latter are rejected. The index-flag check covers `core.ignoreStat` and sparse-checkout skip-worktree entries; explicit status options override untracked-file and submodule-ignore settings, and fsmonitor and untracked cache are disabled. Other Git behavior still limits the snapshot: ignored files are omitted, `core.ignoreCase` must match the filesystem, and `core.checkStat` and `core.trustctime` affect Git's stat-cache comparisons. A deliberate change preserving the relevant metadata can evade status; concurrent changes can invalidate any result. These measures are not a sandbox for hostile repositories, Git executables or filesystem state; use only trusted local inputs.

An optional CTest regex and expected count must be supplied together with `--ctest-regex` and `--expected-count`. **Selection is unavailable**: the command reports unknown and exits nonzero without invoking CTest, including when the expected count is zero. A disposable CTest 4.4.5 probe showed that `ctest -N --show-only=json-v1` creates `Testing/Temporary/LastTest.log` and executes write commands in test definitions. A read-only sandbox blocked the writes but also prevented CTest from producing a listing. See the [design adjustment](lane-preflight-design-2026-11-09.md#selection-design-adjustment-2026-11-09). A separately invoked listing is outside this script's read-only contract; its count is subject to the subset-registration caveat in [validation](../../agents/validation.md#implementation).

The symlink regression used disposable current and upstream repositories: with `core.symlinks=false`, replacing an indexed link by a regular file containing its target produced an empty Git status and a false preflight pass in both roles before the fix. The corrected check reports unknown without modifying the index or configuration. The tests also cover missing and invalid settings with indexed links, missing and false settings without links, and a detected replacement when the setting is true.

Test the implemented CLI behavior with `python3 tests/dev/LanePreflightTest.py`; the Linux toolchain workflow runs this suite as a separate job. After the file-mode fix, 14 CLI tests passed in the isolated checkout on 2026-11-09. Before the fix, the regression reproduced false clean results for both the current and upstream checkouts with `core.filemode=false` and a tracked script's executable bit changed. Tests create disposable repositories and check that inherited Git trace destinations are not modified. The script does not intentionally write files, but cannot guarantee no side effects from arbitrary Git configuration, filesystem behavior or concurrent changes.

Historical validation on 2026-11-09 under lane `lane-preflight`: the seven then-existing CLI tests and `cmake --build --preset vcpkg` passed. The full `ctest --preset vcpkg` run selected 4,624 tests, with 11 skipped and two failures: `cch_pi_v130_consolidation_check` and `cch_pi_v130_consolidation_boundaries`. Both reported that the existing modified `cmake/tests/CodemodeTests.cmake` did not match the `1179-codemode-comparisons` delivery digest. This run did not modify or rebind that delivery evidence; the remaining checks in its failing boundary suite were not independently passed.

Subsequent validation on 2026-11-09 used an isolated checkout based on `64d9503bc0279179a8716543419958e74a4eebcd`, containing these four preflight files and a separate CI test job, without the unrelated main-checkout modifications. The 12 CLI tests, full build and unfiltered CTest passed: 4,587 tests selected, 12 skipped, no failures. Both consolidation tests passed in this tree. This run predates the file-mode fix and does not validate the unrelated modified main-checkout files. A subsequent independent review found the file-mode false-clean issue described above.

After the file-mode fix, the 14 CLI tests and incremental full build passed in the same isolated checkout. The unfiltered CTest run selected 4,587 tests, with 12 skipped and no failures; both consolidation tests passed. This run used `CCH_LANE_NAME=lane-preflight-filemode` and `TERM_PROGRAM=herdr`. The CI job configuration was checked locally; no remote CI run was triggered.

After the symlink fix, all 16 CLI tests and the incremental full build passed in the isolated checkout. Unfiltered CTest selected 4,587 tests, with 12 skipped and no failures; both consolidation tests passed. This run used `CCH_LANE_NAME=preflight-symlinks` and `TERM_PROGRAM=herdr`.

## Delivery receipt drift (2026-11-11)

The same command now also reads `docs/research/pi-v1.3.0/*-deliveries.json` and compares every pinned input's digest with the committed blob and the working-tree file. No flag is needed; the check is part of the snapshot.

```sh
python3 scripts/dev/lane-preflight.py --lane lane-example \
  --revision <full-commit-id> --checkout <upstream-checkout-root>
```

A clean receipt set reads:

```
pass: delivery receipts: 1 manifests, 32 records, 835 pinned inputs; committed drift 0, uncommitted drift 0, missing 0
```

and any drift adds one line per class, for example:

```
attention: delivery receipts: committed drift: 2 pinned inputs, tickets 1101 1130; paths: cmake/tests/EvidenceTests.cmake, scripts/evidence/audit_evidence.py
attention: delivery receipts: uncommitted drift: 1 pinned input, tickets 1110; paths: src/tui/Renderer.cpp
attention: delivery receipts: missing: 1 pinned input, tickets 1120; paths: fixtures/pi-v1.3.0/example.json
```

Read the classes as different work:

- **committed drift** is debt. `HEAD` no longer matches the receipt, so a clean checkout cannot reproduce what the receipt claims. Rebind per [the delivery evidence contract](../../research/pi-v1.3.0/1058-delivery-reconciliation.md#delivery-evidence-contract) — rerun the selection or record a `revalidation<ticket>` entry — and never bulk-rehash. The preflight deliberately cannot and does not do this.
- **uncommitted drift** is temporary. `HEAD` matches and only the working tree differs; committing the intended change (and rebinding if the change is real) clears it, and a full run is not evidence either way while it stands.
- **missing** is a pinned path present in neither `HEAD` nor the working tree; restore it or rebind, and treat a path that was never meant to be pinned as a receipt defect.

`unknown` is the conservative answer: a manifest that cannot be read safely (foreign schema, duplicate key, invalid pin, excessive depth), a pin whose path is not checkout-relative, or a probe that failed. The summary is unknown even when other classes were established, so an unreadable manifest can never look clean. A baseline directory without a manifest is unknown; a repository with no `docs/research/pi-v1.3.0` directory reports the check as absent instead.

The check writes nothing: it does not edit a manifest, does not rebind a digest, and does not run `consolidate_audits.py`. Digest agreement never proves a receipt is still applicable — the selected input set, the scenario mapping and the capability claim remain review obligations (see [the delivery evidence contract](../../research/pi-v1.3.0/1058-delivery-reconciliation.md)). The check costs two Git reads and one file read per pinned input (about 2.6 s for 32 records and 835 inputs on 2026-11-11).

A locked lane also needs `CCH_LANE_NAME` for its own subprocesses; when the lock names the requested lane, the report adds `pass: lane environment: CCH_LANE_NAME matches the held lane`, or an `attention` line when the name is unset or names another lane. Run lane work through the wrapper below so this cannot drift.

## Running under the lane lock (2026-11-11)

[`scripts/dev/with-lane.sh`](../../../scripts/dev/with-lane.sh) is the one step that replaces `lane-lock.sh acquire`, `export CCH_LANE_NAME=<lane>` and `lane-lock.sh release`:

```sh
scripts/dev/with-lane.sh lane-example -- ctest --preset vcpkg
scripts/dev/with-lane.sh lane-example --timeout 600 -- scripts/dev/lane-preflight.py --lane lane-example --revision <full-commit-id>
```

It acquires the lock (waiting up to `--timeout` seconds, default 0), exports `CCH_LANE_NAME=<lane>` for the command and its descendants, runs the command in the foreground and releases the lock from a trap on exit, HUP, INT and TERM. The exit status is the command's own, `128+signal` when the command died from a signal, `129`/`130`/`143` when the wrapper was signalled. Usage and environment errors exit 2; an acquire refusal exits with `lane-lock.sh`'s status. Nothing is written outside the lock file.

Behaviour worth knowing:

- The wrapper owns the lock for the command's lifetime. Re-running it under the same lane is fine, but its exit releases the hold, including one the lane already had; start one wrapper per lane session rather than nesting it inside a manual hold.
- The command stays in the wrapper's process group with the wrapper's stdin, stdout and stderr. That is deliberate: a POSIX shell starts an asynchronous command with `SIGINT` ignored, so a test run started that way cannot be interrupted. The cost is the POSIX trap rule — a signal sent to the wrapper process alone is handled when the running command finishes. A terminal interrupt reaches the whole process group and releases at once.
- A failed release prints `with-lane: WARNING: release failed` and keeps the command's status; a `SIGKILL` cannot run the trap at all, and `lane-lock.sh`'s 4-hour staleness takeover is the only recovery. Check `scripts/dev/lane-lock.sh status` before assuming a lane is gone.
- An environment already inside a *different* lane is refused instead of silently retargeted. `CCH_LANE_TASK_ID` and `CCH_LANE_AGENT` are still recorded in the lock exactly as `lane-lock.sh` reads them.
- Runtime: POSIX `sh` plus what `lane-lock.sh` already needs (Git, `mkdir`, `sed`, `uname`, and GNU `date`/`stat`, so Linux with GNU coreutils). It assumes no other shell: no arrays, no `local`, no `[[`. The wrapper and `lane-lock.sh` are driven through `sh` by their tests, which is the POSIX shell on both this host and the CI runner.

Test the wrapper with `python3 tests/dev/WithLaneTest.py` (8 cases: success, failing command, command killed by TERM/INT, wrapper interrupted by HUP/TERM/INT through its process group, stdin/stdout pass-through, different ambient lane, foreign lock untouched, usage errors). It drives the real scripts inside a throwaway repository, so it cannot leave a lock in this checkout.
