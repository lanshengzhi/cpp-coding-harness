# Open questions

Status as of 2026-11-05 at `lanshengzhi/cpp-coding-harness@749ce768a`. Items 1 and 2 were partly settled by owner decisions on 2026-11-04 (see [goals](goals.md)); item 4's symlink question was settled by the owner ruling of 2026-11-05 (decision B, [decisions-2026-11-05](decisions-2026-11-05.md)) and its delivery completed the same day (the `1180-sidecar-symlink-parity` receipt; see [evidence history](evidence-and-delivery-history.md)). The original text is kept and marked.

## 1. Long-term "latest stable pi" versus ADR 0075
**Update 2026-11-04 (owner decision).** The owner will request periodic follow-up of upstream pi releases; the current target remains v1.3.0. Consistent with ADR 0075 and ADR 0024, a later release still needs its own explicit audit, but nothing here says who starts it or when. Still undetermined: the meaning of "latest stable" (tag, release channel, date), the trigger and cadence, the advancement mechanism (new ADR, per-release audit, or other), and whether exclusions and retained boundaries carry over. No repository source defines these. A local `../pi` checkout listed `v1.3.0` as its newest tag on 2026-11-04; that is an observation of a moving working tree, not a definition, and ADR 0075 says the revision decides comparisons.

*Original text (2026-11-04, earlier; partly superseded by the update above):*
- ADR 0075 states that it is "one explicit package/baseline advancement. Future pi releases require another audit; the Owner graph does not automatically track upstream HEAD." [ADR 0024](../../adr/0024-record-and-explicitly-advance-the-pi-parity-baseline.md) likewise requires an explicit audit to advance a baseline.
- The owner's long-term goal (see [goals](goals.md)) names the latest stable pi. This wiki does not say whether that supersedes, extends or merely follows those rules.
- Undetermined: what "latest stable" means and when it is evaluated; whether it keeps ADR 0075's exclusions (retained Magpie/XDG boundaries, `cch_ai` not aligned overall, third-party TS/JS extensions, Cloudflare adapters, SDK/ABI promises; see [ADR 0075 scope](adr-0075-scope.md)); whether it needs a new ADR or a per-release audit.

## 2. Immediate goal completeness
**Update 2026-11-04 (owner decision).** The "wider reading" question is **settled**: the immediate scope is exactly ADR 0075's selected scope. What remains open is completeness within that scope: see the [residual-gap audit](residual-gap-audit-2026-11-04.md).

*Original text (partly superseded):*
- The owner wants v1.3.0 alignment. ADR 0075 records target, membership and clause-level supersessions but says acceptance "does not claim implementation or verification is complete."
- On 2026-10-30 the reassessment recorded the verification state at its reviewed checkout (see [evidence history](evidence-and-delivery-history.md)). Whether the immediate goal means ADR 0075's selected scope or a wider reading is not stated.

## 3. Naming hazard
- Files `docs/research/cch-tui-pi-v1.4-semantic-alignment.md` and `docs/research/mcp-codemode-pi-v1.4-parity-audit.md` contain "v1.4" in their names but state a pi v1.0.4 baseline in their text. See [sources](sources.md). Not a pi v1.4 claim.

## 4. New questions raised by the 2026-11-04 audit
**Update 2026-11-04 (chief-of-staff decisions, same day; see [decisions](decisions-2026-11-04.md)).** The three questions below moved as follows, and the original text is kept beneath the update:
- **#1202 receipts: settled (fail-closed).** Every receipt row named by #1202 counts as unverified until re-anchored through #1202's own criteria; re-anchoring proceeds later, per receipt, via tickets.
- **JSONL task-sidecar symlink divergence: settled (owner ruling 2026-11-05, decision B — funded alignment).** The owner ruled to align the durable JSONL task-sidecar symlink behavior with frozen pi, superseding the no-follow policy for that surface only; the decision, security tradeoff, narrowed scope and mitigations are recorded in [ADR 0076](../../adr/0076-align-the-durable-jsonl-task-sidecar-symlink-boundary-with-frozen-pi.md), with the pinned frozen semantics in [1180-sidecar-symlink-parity](../../research/pi-v1.3.0/1180-sidecar-symlink-parity/NOTES.md). The durable `Storage:jsonl:sidecar`/`:ordering` rows completed their comparison through #1180's delivery receipt contract on 2026-11-05 (`1180-sidecar-symlink-parity` at testedRevision `c656871d`, delivery record [1180-jsonl-sidecar-symlink-alignment](../../research/pi-v1.3.0/1180-jsonl-sidecar-symlink-alignment.md)); #1180 stays open for the broader durable packet. See [decisions-2026-11-05](decisions-2026-11-05.md).
- **Uncommitted 1058/1202 files: disposition settled, action pending.** The #1104 withdrawal is accepted (decision 1), so the working tree's downgrade is the recorded disposition; the physical commit or revert of the four files is still to be executed by the serial merger. Nothing was committed as part of recording the decisions.

*Original text (2026-11-04, earlier; partly superseded by the update above):*
See the [residual-gap audit](residual-gap-audit-2026-11-04.md). Undecided, owner input needed: how to treat the #1202 receipts still claimed verified; whether a deliberate divergence from frozen pi (JSONL task-sidecar symlink rejection, recorded in `1085-jsonl-continuation.md`) is an accepted exemption, given ADR 0075 removes historical exemptions except those it names; and who commits or discards the uncommitted 1058/1202 working-tree files.

Related: [baseline timeline](baseline-timeline.md), [index](index.md).
