# #1180 frozen-pi symlink and ordering pinning (owner ruling 2026-11-05, decision B)

Probes against the frozen checkout `../pi` at `71023aec1546d6d677a65669c5665c0927ce0dd0`
(pi-v1.3.0), run with the checkout's own Node (`v26.13.0`, `--conditions=source`).
Each probe used disposable files under a fresh `mkdtemp` directory; no frozen
source, no repository file and no user file was touched. The scripts and their
captured outputs are retained verbatim:

- [pin-symlink-probe.mts](pin-symlink-probe.mts) /
  [pin-symlink-probe.txt](pin-symlink-probe.txt) — per-operation symlink matrix.
- [pin-two-sidecar-order.mts](pin-two-sidecar-order.mts) /
  [pin-two-sidecar-order.txt](pin-two-sidecar-order.txt) — two-sidecar append
  and flush order; the registered capture is
  [fixtures/pi-v1.3.0/audit-1180/sidecar-order.json](../../../fixtures/pi-v1.3.0/audit-1180/sidecar-order.json).

## Pinned frozen semantics (public `openNodeJsonlStorage` / `JsonlStorage` boundary)

| Probe | Frozen pi behavior |
| --- | --- |
| A. Unreferenced symlinked task sidecar at open | Opens successfully. The symlink is skipped by recovery directory enumeration (`listDir` reports `lstat` kind `symlink`; only `kind === "file"` sidecars are read). The outside target is not read and not truncated. |
| B. Live-marker-referenced symlinked task sidecar at open (target holds the original valid sidecar bytes, or garbage) | `JsonlCorruptionError: Missing confirmed sidecar record task-2.jsonl at sequence 1`. The symlinked entry is treated as absent; the outside target is never read or truncated, even when it holds garbage that a follower would have torn-tail truncated. |
| C. Live commit with a symlink at the task sidecar path | The commit succeeds (seq 1). `appendFile` follows the final symlink and appends the sidecar record into the outside target. On reopen, recovery skips the symlink and the same `Missing confirmed sidecar record` corruption fails the open; the outside target keeps the orphan sidecar bytes. |
| D. `fsync: true` commit through the same symlink | Succeeds. `flushFile` reopens the path read-write and follows the final symlink. |
| E. One batch `[live, terminal]` through the symlink | Succeeds. Reclamation `remove`s the entry: the symlink itself is unlinked and the outside target keeps the orphan sidecar bytes. Reopen reads the terminal state inline and succeeds. |
| F. `main.jsonl` replaced by a symlink to the real file | Reopen reads through the symlink. A later commit appends the marker through the symlink (the real file grows; the link stays in place). |
| G. Store reached through a symlinked parent directory component | Followed at open, commit and reopen (host path resolution; nothing in the store rejects intermediate links). |
| H. The store directory itself is a symlink | Followed at open, commit and reopen. |
| I. Live-marker-referenced symlinked document sidecar | `JsonlCorruptionError: Missing confirmed sidecar record doc-3.jsonl at sequence 1`; the outside target is untouched. Same enumeration-skip rule as task sidecars (observed for the record; the owner ruling narrows the alignment scope to the task sidecar). |
| J. Hard link at the task sidecar path (control) | The outside target's partial tail is truncated (`complete\npartial` becomes `complete\n`) and the open fails with `Malformed complete task-2.jsonl line 1`. Frozen pi follows hard links and mutates the outside target; this is the retained C++ divergence, not covered by the ruling. |

## Exact scope answer for the ruling

- **Final-component symlinks**: followed for append, flush and `main.jsonl`
  reads/appends; skipped (treated as absent) at recovery enumeration for
  sidecars; removed as entries (link unlinked, target preserved) at
  reclamation.
- **Intermediate components**: symlinks are followed everywhere, including the
  store directory's own ancestors and a symlinked store directory itself.
  There is no no-follow component in frozen pi's JSONL storage path.
- **No self-directed escape**: every followed link is one a store-directory
  writer (or the caller's own store path) placed; frozen pi never resolves a
  path outside the caller-provided store directory on its own. File names are
  numeric sidecar names joined to the store directory. The behavior is exactly
  final-component following plus enumeration-skip at recovery — the ruling's
  covered surface — so the safety stop condition is not triggered.

## Two-sidecar ordering

Frozen pi appends sidecars in commit write order (`doc 9` create then `doc 3`
create yields `appendFile doc-9.jsonl`, `appendFile doc-3.jsonl`,
`appendFile main.jsonl`) and, with `fsync: true`, flushes them in that same
append order before the marker. Sorted-filename order would have produced
`doc-3.jsonl` first, so the shape discriminates. This divergence and its fix
are recorded in [1180-jsonl-sidecar-symlink-alignment.md](../1180-jsonl-sidecar-symlink-alignment.md).
