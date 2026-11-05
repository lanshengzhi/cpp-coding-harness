# Owner decision via the chief of staff, 2026-11-05

Decision recorded 2026-11-05, read at `lanshengzhi/cpp-coding-harness@8d075de21` plus live
GitHub issue state queried 2026-11-05. It answers the blocker reported 2026-11-04 in
[decisions-2026-11-04](decisions-2026-11-04.md) §3, inside the selected scope of
[ADR 0075](adr-0075-scope.md). Conventions: [conventions](conventions.md).

## Provenance

The owner's ruling of 2026-11-05 (decision B) settles the JSONL task-sidecar symlink
divergence recorded as a blocker on 2026-11-04: **align the durable JSONL task-sidecar
symlink behavior with frozen pi**, superseding the no-follow symlink policy for this
surface only. The 2026-11-04 condition — an explicit owner ruling choosing between a
named exemption and funded alignment — is met by this ruling. Implementation is
executed as a formal change under [ADR 0076](../../adr/0076-align-the-durable-jsonl-task-sidecar-symlink-boundary-with-frozen-pi.md),
not as a local hack.

## 1. JSONL task-sidecar symlink rejection: funded alignment (decision B)

- Frozen pi follows final symlinks on this surface; the C++ rejection under the
  no-follow policy is the divergence. The ruling funds alignment.
- Before implementation, the exact frozen semantics were pinned by probe against the
  frozen checkout at `71023aec` and recorded in
  [1180-sidecar-symlink-parity](../../research/pi-v1.3.0/1180-sidecar-symlink-parity/NOTES.md):
  recovery enumeration skips a symlinked sidecar (unreferenced link opens cleanly; a
  live-referenced link fails with the frozen missing-confirmed-sidecar corruption
  without touching the target); commit append and fsync flush follow the final
  symlink; reclamation unlinks the entry itself, preserving the outside target;
  intermediate directory components are followed everywhere; `main.jsonl` follows
  final symlinks for reads and marker appends.
- The safety condition attached to the ruling — stop and report if frozen pi's
  behavior is broader than final-component following (for example a self-directed
  worktree escape) — was checked and is not triggered: every followed link is placed
  by a store-directory writer or the caller's own store path; the store never
  resolves outside the caller-provided directory by itself.
- Scope is narrowed in ADR 0076: every other no-follow path, atomic-write
  guarantees, the retained hard-link (`st_nlink == 1`) rejection, document-sidecar
  and `main.jsonl` path behavior, and recovery-time reclamation order are
  explicitly unchanged. The architecture security section is updated to name this
  exception.
- Consequence for verification: the durable `Storage:jsonl:sidecar` and `:ordering`
  rows proceed through #1180's delivery receipt contract at a new testedRevision;
  the two-sidecar frozen capture is registered as
  [fixtures/pi-v1.3.0/audit-1180/sidecar-order.json](../../fixtures/pi-v1.3.0/audit-1180/sidecar-order.json)
  and the C++ encoder is aligned to frozen write order. #1180 remains open for the
  broader durable packet.

## Related

- [open questions](open-questions.md) §4 (the settled item), [goals](goals.md).
- [ADR 0076](../../adr/0076-align-the-durable-jsonl-task-sidecar-symlink-boundary-with-frozen-pi.md);
  [1180-jsonl-sidecar-symlink-alignment](../../research/pi-v1.3.0/1180-jsonl-sidecar-symlink-alignment.md).
- Tracker: ruling disposition commented on
  [#1183](https://github.com/lanshengzhi/cpp-coding-harness/issues/1183); #1180 stays
  open and owns the broader durable packet.
