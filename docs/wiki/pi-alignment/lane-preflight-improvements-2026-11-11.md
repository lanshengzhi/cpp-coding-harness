# Lane preflight receipt drift and the lane wrapper (2026-11-11)

## Scope

Environment improvement B, delivered on branch `lane-preflight-receipt-drift` at
`536160742`: (1) delivery receipt drift in the read-only preflight, (2) a wrapper that removes
the two hand-rolled lane-lock steps that produced incidents on 2026-11-11, (3) CI and
documentation wiring. This record makes no capability claim: no audit row, ledger, frontier or
delivery receipt changes, and no receipt is rebound.

The two incidents it answers, measured on this checkout:

- `71ad3e83a` changed four files pinned by delivery receipts. Seventeen digests went stale and
  the checkout still looked clean; the debt surfaced only after a full build and test run, where
  three tests failed and `consolidate_audits.py build` could not produce the ledger.
- A lane acquired `.lane-lock.json` without exporting `CCH_LANE_NAME`, so its own consolidation
  cases were refused as a foreign caller, and a lane finished without releasing the lock,
  blocking the next lane for about 1.5 hours.

## Delivered behavior

**Receipt drift in the preflight.** `scripts/dev/lane-preflight.py` reads
`docs/research/pi-v1.3.0/*-deliveries.json` and compares every pinned input's `sha256` with the
raw committed blob at `HEAD` and with the working-tree file, reporting three separate classes:
committed drift (`HEAD` already contradicts the pin, so a rebind is owed), uncommitted drift
(`HEAD` matches, only the working tree differs) and missing (present in neither). Each
non-empty class names the affected tickets and pinned paths, capped at 20 paths. The check is
read-only: it writes no file, rebinds no digest, runs no consolidation command and repairs
nothing. Unreadable manifests (foreign schema, duplicate key, invalid pin, excessive depth, a
pinned path that is not checkout-relative) and failed probes are reported as **unknown**, and an
unknown anywhere makes the summary unknown rather than clean. The `HEAD` side reads raw blobs
(`git ls-tree` plus `git cat-file blob`), so no smudge filter or text conversion runs; the
working-tree side opens with `O_NOFOLLOW`, accepts only regular files and refuses a parent
directory that resolves outside the checkout. See the
[design](lane-preflight-design-2026-11-09.md#delivery-receipt-drift-check-2026-11-11) and
[usage](lane-preflight-usage-2026-11-09.md#delivery-receipt-drift-2026-11-11) pages.

**Lane environment contradiction.** When the lock is readable and names the requested lane, the
preflight reports whether `CCH_LANE_NAME` matches, is unset or names another lane; the
contradiction is `attention`, and the value is never echoed.

**Wrapper.** `scripts/dev/with-lane.sh <lane> [--timeout N] -- <command...>` acquires the lock,
exports `CCH_LANE_NAME`, runs the command in the foreground and releases from a trap on exit,
HUP, INT and TERM. It propagates the command's own status, refuses a shell already inside a
different lane, leaves a foreign lock untouched, and needs only POSIX `sh` plus what
`lane-lock.sh` already requires. Documented limits: a signal sent to the wrapper alone is
handled when the running command finishes (POSIX trap deferral), and a `SIGKILL` cannot run the
trap, so the 4-hour staleness takeover remains the only recovery.

**CI.** The existing `Lane preflight tests` job gained the wrapper suite and a read-only drift
step over the checked-out tree, so a stale receipt fails the job before any build lane starts.
It sets `core.symlinks true` explicitly: a fresh clone leaves the value unset, and the indexed
`CLAUDE.md` symlink would otherwise make the checkout status unknown by design.

## Validation

Measured on 2026-11-11, lane `lane-preflight-receipts`.

- `python3 tests/dev/LanePreflightTest.py`: **23 cases, all pass** (16 before this change, +7:
  six receipt cases and one lane-environment case; 3.9-4.1 s). The receipt cases build their
  own minimal repositories and assert: no drift over two manifests; committed/uncommitted/
  missing separation in one fixture; a pinned path absent from `HEAD` classifies as committed
  drift; every unsafe manifest is unknown without a traceback or a leaked path; the check adds
  and modifies no file; a directory symlink out of the checkout is unknown; absence of the
  baseline directory is reported, while an empty baseline directory is unknown.
- `python3 tests/dev/WithLaneTest.py`: **8 cases, all pass** (1.4 s). They drive the real
  `lane-lock.sh` in throwaway repositories: success, failure (including a missing command, 127),
  a command killed by TERM/INT (143/130), the wrapper interrupted by HUP, TERM and INT through its
  process group (129/143/130 with the lock released and the command stopped), stdin/stdout
  pass-through, a different ambient lane refused (2), a foreign lock left untouched (including
  `--timeout 1` timing out without releasing it), and usage errors (2).
- Incident reproduction: `python3 tests/evidence/ConsolidationAuditTest.py` with this checkout's
  lock held by the lane but `CCH_LANE_NAME` unset gave **7 failures + 1 error over 94 cases**
  (31 s); with `CCH_LANE_NAME` exported it was **94 pass** (35 s). The failing cases are the
  in-process `consolidate.main()` calls whose lane gate saw an unset caller.
- Real checkout, this branch: `1 manifests, 32 records, 835 pinned inputs; committed drift 0,
  uncommitted drift 0, missing 0` in 2.8 s. The checkout and upstream lines report **unknown**
  on this host because `core.symlinks` is unset while `CLAUDE.md` is an indexed symbolic link —
  the pre-existing conservative status rule, not a result of this change; the receipt check
  itself does not depend on it.
- CI-shaped evidence: a fresh `--depth 1` clone of `536160742` with `core.symlinks true` ran the
  three job steps green, and the drift step exited 0 in 2.5-2.6 s. Injected into that clone: an
  uncommitted edit to `vcpkg.json` gave `uncommitted drift 13` (13 pinned entries across tickets
  1101 1118 1176-1188, one path) with the checkout otherwise clean; committing it gave
  `committed drift 13` with the checkout and upstream both clean; deleting and committing
  `cmake/tests/CodemodeTests.cmake` gave `missing: 3 pinned inputs, tickets 1179 1192 1194`.
- `scripts/format-check.sh` and `git diff --check`: clean. `tests/dev/LaneLockTest.py`: 22
  cases, 16 pass and 6 skipped, unchanged.
- No full CTest run was performed for this change (explicitly out of scope for the task), so
  this record claims nothing about the whole suite.

## Capability status

No capability row advances and no delivery artifact changes. The change is lane tooling: the
preflight, the wrapper, the preflight CI job, and the two preflight wiki pages plus this record.
No receipt digest is edited; the 32 records stay bound to their current bytes.

## Remaining boundary

**Detection moved earlier; the debt still needs a reviewed rebind.** Before this change a stale
pin was found after a build and a full suite; now the preflight and the PR job name the tickets
and paths in about three seconds, before either starts. That is the whole improvement. The
preflight cannot rebind, cannot decide whether a changed input still supports the receipt, and
cannot replace the rerun of the selection or the `revalidation<ticket>` entry that
[the delivery evidence contract](../../research/pi-v1.3.0/1058-delivery-reconciliation.md)
requires. It also cannot see a receipt whose digests still agree but whose selected input set,
scenario mapping or capability claim no longer holds — digest agreement is a check, not
acceptance.

Further limits, deliberately not hidden:

- The classification is a snapshot of `HEAD` and the working tree at one instant; concurrent
  changes, ignored files and repository configuration that hides work bound it exactly as they
  bound the status check.
- An uncommitted new pin is reported as committed drift until it is committed, because `HEAD`
  cannot satisfy it. That direction is deliberate: it fails loudly rather than calling a receipt
  clean that a clean checkout cannot reproduce.
- A checkout whose `core.symlinks` is unset while the index holds links still reports the
  checkout status unknown; the receipt lines remain usable, the CI step sets the value
  explicitly.
- The wrapper does not forward a signal it receives only itself to a running foreground command
  (POSIX trap deferral), and a `SIGKILL` leaves the lock for the staleness takeover. A lane must
  still check `lane-lock.sh status` before assuming the checkout is free.
