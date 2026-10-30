# Durable recursive file watch (#1156)

## Scope

The durable local environment watches a directory subtree when the caller asks for a
recursive watch. inotify is not recursive, so the adapter maintains the watch set itself:
it covers every existing subdirectory before `watch()` resolves and adds watches for
subdirectories created later. `exclude.hidden` and `exclude.names` apply to the whole
subtree, symbolic links below the target are never followed, and the frozen
`overflow` / `error` outcomes keep their meaning: `overflow` tells the caller coverage
was uncertain and to rescan; `error` is terminal and no callback follows it. The watcher
reports its `native` mode. User-approved batch B31a / F23a under #1044 and ADR 0075;
Named Baseline pi-v1.3.0, revision `71023aec1546d6d677a65669c5665c0927ce0dd0`.

Frozen evidence: three identical captures from `/tmp/cch-1080-pi-frozen` (verified
`71023aec…`, `tsx`, `NODE_OPTIONS=--conditions=source`) each report
`{"mode":"native","nested":true,"created":true,"hidden":false}` — a nested write is
observed, a write in a subdirectory created after the watch starts is observed, and
`exclude.hidden` suppresses events at depth.

## Delivered behavior

- `Environment::watch(path, on_change, WatchOptions{recursive, exclude_hidden, excluded_names})`
  covers the target directory and, recursively, every descendant directory before the
  returned watcher exists; a file (or not-yet-existing) target is covered at its parent,
  so creating it is a change. The change sink receives `WatchChange{paths}` values.
- A directory created or moved in under a recursive watch is scanned and covered before
  its own changes can be reported; `{overflow:true}` requests a rescan for writes that
  could have preceded watch installation.
- Moving or deleting a child removes its subtree watches immediately. Moving a directory
  within the tree establishes fresh watches at the destination; moving it outside the root
  or into an excluded name cannot keep reporting through its old inode. Ordinary child
  deletion preserves the root watcher; loss or movement of the watched root is terminal.
- Hidden names (when `exclude_hidden`) and `excluded_names` are neither watched below
  nor reported, at any depth. Symbolic links below the target are not followed.
- The watch set is bounded (10,000 directories, the frozen default): a tree over the
  budget fails `watch()` with a `Validation` error, and a tree that grows past the
  budget afterwards delivers one terminal `{error}` and stops.
- Lost events surface as `{overflow: true}`; the watched directory becoming unavailable
  (or an unrecoverable read error) delivers a terminal `{error}`. After `close()` no
  callback starts; `FileWatch::mode()` reports `"native"`.
- A file target reports only its own name: sibling changes in the parent directory
  are filtered out (review fix for the watch-the-target-itself gap).

## Validation

- Frozen capture: probe scenario run three times with identical output (above).
- Focused CTest: `-R 'recursive watch'` selected 3, passed 3 on three consecutive runs
  (nested write; later-created subdirectory; subtree exclusions with a symlink sibling;
  budget refusal). `-R 'environment reports'` selected 1, passed 1 on three consecutive
  runs (non-recursive behavior from #1089 unchanged, now with mode, structured changes
  and a sibling-file no-event assertion).
- Discriminating case: with the recursive implementation replaced by the previous
  explicit `Validation` rejection, the nested-write and exclusion tests fail in 0.01 s
  at `REQUIRE(watch)` — they fail, never hang; every wait is bounded by
  `try_acquire_for(5s)`.
- Durable label: 275 selected; the two codemode cases that failed under parallel load
  (`resumes recorded nested answers`, `replays committed answers after SIGKILL`) passed
  in isolation.
- Architecture: `-L architecture` 43 selected, 43 passed (Owner Interface header gained
  `WatchOptions` / `WatchChange` / `WatchChangeSink`; no new cross-Owner edges).
- Consolidation: re-pinned the `Environment.hpp` delivery digest for #1090 in
  `1058-deliveries.json` and rebuilt the consolidated ledger; `consolidation_check`
  and `consolidation_boundaries` pass (2 each).
- Formatting: `scripts/format-check.sh` reports clean.
- Full CTest: all pass except `Projection publishes 100 message-update chunks inside
  the issue cost bound` (2 ms processor bound), which fails identically on the clean
  tree under the current host load; it is the known resource-sensitive case (owner
  #632). A fullscreen scroll-indicator case that failed once under load passed on rerun.

## Review correction

The review reproduced stale paths after an inotify-watched directory was renamed:
`visible` to `.hidden` still reported `visible/secret.txt`, and moving that inode
outside the root still reported outside writes below the old path. A public
`Environment::watch` regression for a renamed nested directory failed against
the reviewed implementation in 5.02 seconds (three failed checks, worker joined
before assertions), then passed with the correction.

The six `issue1156` cases now pass, including current descendant paths after a
rename, hidden/name exclusions after a move, move-out, child deletion and terminal
root deletion. The independent real-file probe now reports zero hidden or
outside writes. Test waits observe the requested path with a bounded condition
variable wait; every discriminating assertion follows `close()`, and the old
unlocked `changes.clear()` and stale semaphore-permit shortcut are gone.
Full Validation on the repaired tree with the clean named frozen checkout:
**4,015 selected / 4,014 passed / 0 failed / 1 opt-in TUI skip**, including
**43/43 architecture cases**. The final Standards and Spec reviews report
zero remaining findings.

## Capability status

Advances the durable `ExecutionEnv` watch contract: recursive coverage, subtree
exclusions, no-follow symlinks, `overflow` / terminal `error` outcomes and `native`
mode reporting now have C++ evidence against the frozen behavior. Membership,
implementation and verification stay distinct: this slice does not claim the polling
mode, multi-target arrays, or the remote environment.

## Remaining boundary

Polling mode is not implemented (Linux native inotify only; the frozen adapter selects
polling only for Windows and unreliable network/FUSE file systems). One `WatchTarget` is
watched per call; the frozen multi-target array form is not exposed. The directory
budget is fixed at the frozen 10,000 default (a caller override is not part of the
frozen `WatchTarget` contract). The budget counts active watches, so removing a
subtree releases its directory capacity.
