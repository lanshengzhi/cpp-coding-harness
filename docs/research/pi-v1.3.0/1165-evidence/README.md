# #1165 evidence log index

Raw bytes are preserved as captured; this index records what each log was
produced from so a later reader can tell current evidence from history.
Filters were run with `build/cch_tests_durable "<filter>"`; a tag that no
longer exists means the log is **historical and not reproducible** from
the current tree.

## Current tree (re-runnable now)

| Log | Filter | Status |
|---|---|---|
| `reservation-visibility-red-seam.log` | `[reservation-visibility]` | RED at `2d9823ba1` (10/10); the case is GREEN now, so re-running produces a pass, not this output |
| `fifo-order-admission-evidence.log` | `[.][fifo-order-probe]` (hidden) | historical: records the pre-seam bimodal capture (4/6 defect, 2/6 lucky). The probe was removed once the seam landed: its world detector could not distinguish the worker's own scans from the inspection's capture, and the property is now covered deterministically by `[stale-scan-line]` |
| `fifo-order-red.log` | `[.][fifo-order-probe]` (hidden) | historical: pre-seam probe run (`early_return`); probe removed |
| `frozen-waiting/` | frozen pi-v1.3.0 probe `71023aec` | three runs against the frozen checkout; reproducible there, not from this tree |

## Historical (filters no longer exist; original bytes kept)

| Log | Original filter | Notes |
|---|---|---|
| `stale-scan-red.log` | `[stale-scan]` | direct-scan RED on the pre-seam tree; under the seam the old test deadlocks by construction and is hidden as `[.][stale-scan-legacy]` |
| `resume-race-red.log` | pre-rebuild candidate | waiting-resume race RED from the dropped `5057091d3`/`15724cd09` line |
| `reservation-red.log`, `reservation-red-merged-main.log` | `[issue1165]` | reservation RED on the merge-base and the first candidate |
| `reservation-green3.log` | `[issue1165]` | green run of the dropped candidate; superseded by the rebuilt slice |
| `durable-shard.log` | `[durable]` | shard run of the dropped candidate |
| `held-fifo-pins.log` | pre-rebuild pins | came from a run with a failure; not a clean receipt |
| `close-drain-probe.log` | `[close-drain]` probe | racy pass probe; the current `[close-drain]` case is a contract target, not verification |
| `lock-before.log`, `lock-after.log` | `[issue1165-lock]` | mutation-lock probe from the investigation phase |
| `reentry-after.log` | `[issue1165-reentry]` | line-reentry probe from the investigation phase |
| `close-gated.log` | `[issue1165-close]` | close-gated probe from the investigation phase |
| `failure-red.log`, `failure-main-red.log` | `[issue1165-failure]` | #1166 failure-signaling REDs from before the merge |
| `mutation-gate-off.log`, `mutation-gate-only.log` | mutation-gate probes | investigation-phase probes of the mutation-line gate |
