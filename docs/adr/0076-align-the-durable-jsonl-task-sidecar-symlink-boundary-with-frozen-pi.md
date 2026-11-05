---
status: accepted
---

# Align the durable JSONL task-sidecar symlink boundary with frozen pi

The owner ruled on 2026-11-05 (decision B, answering the blocker reported by the
chief of staff on 2026-11-04 in
[decisions-2026-11-04](../wiki/pi-alignment/decisions-2026-11-04.md) §3): the durable
JSONL task-sidecar surface **aligns with frozen pi**, which follows final symlinks,
instead of rejecting them under the no-follow symlink policy. This ADR supersedes
that policy **for this surface only**. It implements the ruling as a formal change
with pinned evidence, a narrowed scope and compensating mitigations, not as a hack.

## Pinned frozen semantics

The ruling demanded exact pinned semantics before implementation. The probes run
against the frozen checkout at `71023aec` are retained under
[1180-sidecar-symlink-parity](../research/pi-v1.3.0/1180-sidecar-symlink-parity/NOTES.md)
with scripts and captured outputs. Frozen pi's public JSONL storage behavior on
Linux is:

- **Recovery enumeration**: `listDir` reports each entry's `lstat` kind, and
  recovery reads only `kind === "file"` sidecars. A symlinked `task-N.jsonl` (or
  `doc-N.jsonl`) is therefore treated as absent: an unreferenced one is skipped
  without its target being read or truncated; a live-marker-referenced one fails
  the open with `Missing confirmed sidecar record task-N.jsonl at sequence S`. The
  outside target is never modified at recovery, even when it holds garbage.
- **Commit append and flush**: `appendFile` and `flushFile` follow a final
  symlink, appending and fsyncing the symlink's target. A live commit through a
  placed symlink succeeds and writes into the outside file; the next open then
  fails closed with the missing-confirmed-sidecar corruption.
- **Reclamation**: `remove` unlinks the entry itself. A terminal batch through a
  symlink removes the link, leaves the outside target (with its orphan sidecar
  bytes) intact, and reopens on the inline terminal record.
- **`main.jsonl`**: a final symlink is followed for both reads and marker appends.
- **Intermediate components**: symlinks are followed everywhere, including the
  store directory's ancestors and a symlinked store directory itself. No component
  of frozen pi's JSONL storage path is no-follow.
- **No self-directed escape**: every followed link is one a store-directory writer
  or the caller's own store path placed; the store never resolves outside the
  caller-provided directory by itself. The ruling's safety condition — stop if pi's
  behavior is broader than final-component following — is not triggered.

The same evidence pins frozen pi's two-sidecar publication order: sidecars append
in commit write order (not sorted filename order) and flush in that same order, as
registered in
[fixtures/pi-v1.3.0/audit-1180/sidecar-order.json](../fixtures/pi-v1.3.0/audit-1180/sidecar-order.json).

## Decision

`cch_durable`'s JSONL storage adopts the pinned frozen behavior:

1. Task sidecar open, append and flush follow a final symlink (the `O_NOFOLLOW`
   rejection is removed from the task-sidecar open).
2. Recovery enumeration skips a symlinked task sidecar entry, treating it as
   absent, so an unreferenced link opens cleanly and a referenced one fails with
   the frozen missing-confirmed-sidecar corruption without touching the target.
3. Terminal reclamation of an adopted symlinked sidecar unlinks the entry itself
   (the followed-identity check still guards against substituted targets).
4. The encoder emits sidecars, flushes and reclamations in frozen write order so
   multi-sidecar commits match the frozen append/flush trace.

## Security tradeoff

**Protection lost.** A writer with access to the store directory can place a
symlink at `task-N.jsonl` and redirect a live task append (or an fsync flush)
outside the store; recovery still never reads or truncates through the link, but
commit-time bytes can land in an arbitrary file the process can reach. Under the
previous policy the store rejected the link instead.

**Why alignment wins here.** ADR 0075 makes frozen pi's documented behavior the
contract this runtime is verified against, and the no-follow rejection was itself
recorded as an intentional divergence that blocked the durable `Storage:jsonl`
sidecar and ordering rows from verification. The store already runs at host-user
permissions with a single-owner contract (frozen `packages/durable/README.md`:
"One process owns a storage at a time; there is no cross-process locking"); a
principal who can write the store directory can already replace sidecar contents
outright, so the symlink adds reach, not a new principal. The lost protection also
cannot deliver its promise unilaterally: a C++-only rejection makes this runtime
refuse stores frozen pi accepts and writes, which is the interoperability failure
mode the ruling funds.

**Compensating mitigations, retained.** The substitution defenses around the
follow stay in force: the observed device/inode identity of every task sidecar is
remembered per incarnation and a changed identity fails closed and poisons the
instance; a regular file that appears where a reclaimed sidecar belonged is still
rejected by exclusive creation rather than adopted; reclamation still compares the
followed entry against the observed identity before unlinking, so a substituted
target is left untouched; and the singly-linked-file check rejects hard-link
aliases before read or append, retaining the no-user-data-loss boundary for the
aliasing case frozen pi does not protect. All of these are covered by discriminating
tests tagged `[issue1180]`.

## Narrowed scope — what is not changed

- **Every other no-follow path** keeps the policy: the application Session journal
  and lifecycle appends (`SessionJournal`, `SessionLifecycle`), atomic writes
  (`AtomicWrite`), environment file operations (`WorkspaceFileOperations`,
  durable `Environment` EnvIO), and ADR 0064's symlink-refusing Session-store
  parent walk are untouched. Ordinary tool reads and writes stay no-follow; the
  Project Context File read-only exception in architecture.md is unaffected.
- **Atomic-write guarantees and fsync ordering discipline** are unchanged.
- **The hard-link (`st_nlink == 1`) rejection on task sidecars is retained** as a
  named, owner-accepted divergence: frozen pi follows hard links and truncates an
  outside target at recovery; this runtime refuses without modifying it.
- **Document sidecars and `main.jsonl` keep their existing behavior**, which
  already follows final symlinks through the path-based helpers. Frozen pi instead
  skips a symlinked document sidecar at recovery enumeration; that residual
  recovery-time divergence is recorded, not aligned, because the ruling narrows
  scope to the task sidecar.
- **Recovery-time reclamation order** stays the deterministic sorted enumeration
  rather than frozen pi's host directory order; no registered scenario observes it.
- **No worktree containment changes**: the durable store remains a host-path store
  with no containment concept (ADR 0057 world); this ADR changes link following on
  one surface, not path authority.
- Where the C++ substitution defenses and frozen behavior meet — frozen pi appends
  through a swapped link and fails at the next open, this runtime fails closed at
  the commit — the fail-closed point stays; both end poisoned.

## Consequences

- `docs/agents/architecture.md` §Security and containment no longer names the
  no-follow symlink policy as uniform: it records the retained policy and this
  named exception.
- The durable verification lane's `cch_durable:Storage:jsonl:sidecar` and
  `:ordering` rows complete their comparison through #1180's delivery receipt
  contract; #1180 itself remains open for the broader durable packet.
- Tests: the previous rejection expectations are replaced by frozen-parity
  expectations and new discriminating cases (skip-at-recovery, missing-confirmed
  corruption, append-through, reclaim-unlinks-entry, write-order trace), all
  tagged `[issue1180]`.

## References

- Owner ruling 2026-11-05 (decision B), disposition recorded in
  [decisions-2026-11-05](../wiki/pi-alignment/decisions-2026-11-05.md) and the
  [open questions](../wiki/pi-alignment/open-questions.md) §4 update.
- [1085-filesystem-boundary.md](../research/pi-v1.3.0/1085-filesystem-boundary.md)
  (the divergence record this ruling supersedes for this surface) and
  [1085-jsonl-continuation.md](../research/pi-v1.3.0/1085-jsonl-continuation.md).
- [1180-sidecar-symlink-parity](../research/pi-v1.3.0/1180-sidecar-symlink-parity/NOTES.md)
  pinned evidence; [1180-jsonl-sidecar-symlink-alignment](../research/pi-v1.3.0/1180-jsonl-sidecar-symlink-alignment.md)
  delivery record.
- Frozen pi: `packages/durable/src/storage/jsonl/storage.ts`,
  `packages/durable/src/env/node.ts`; frozen `packages/durable/README.md` §Storage.
- ADR [0057](0057-retire-workspace-containment-and-align-path-resolution-with-pi-resolvetocwd.md)
  (the no-follow policy survives everywhere except this named surface),
  ADR [0075](0075-align-selected-owner-packages-and-linux-capabilities-with-pi-v1-3-0.md)
  (frozen pi is the contract), ADR [0064](../adr/0064-defer-the-session-file-to-the-first-user-or-assistant-message.md)
  (the separate Session-store symlink walk, unaffected).
- Issues [#1180](https://github.com/lanshengzhi/cpp-coding-harness/issues/1180),
  [#1183](https://github.com/lanshengzhi/cpp-coding-harness/issues/1183),
  [#1085](https://github.com/lanshengzhi/cpp-coding-harness/issues/1085).
