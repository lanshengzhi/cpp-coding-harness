# Goals

## Owner statements (2026-11-04)
- Immediate goal: align Pike with pi v1.3.0.
- Long-term goal: align Pike with the latest stable pi.
- The owner agreed that this wiki lives in `docs/wiki/pi-alignment/`.

## Owner statements (2026-11-05)
- **Process goal, coequal with the alignment goals:** the project itself must be implemented per the installed mattpocock skills — tickets are tracer-bullet slices published to the tracker; each ticket is one fresh session governed by the `implement` skill (state the title, tdd at seams, typecheck/single-test cadence, full suite once, code-review before done, commit to the current branch); no bespoke shadow skills — repo docs plus installed skills are the whole protocol; the chief of staff's coordination (serial lanes, lane-lock, push per milestone, wiki memory, owner checkpoints) follows the chief-of-staff skill. The only sanctioned adaptations are the repo's own ADR-approved contracts (serial-merger #1187, evidence pipeline), recorded in [ops-lanes](ops-lanes.md) procedure adaptations. Success shape: a new agent reading AGENTS.md plus the tracker can take the project over losslessly.

## Owner decisions (2026-11-05, same day as the statement above)
- The draft `.agents/skills/pi-alignment-lane/` skill was revoked before ever being committed; dispatch prompts reference installed skills and repo docs instead. Rationale and the lane contract in brief: [ops-lanes](ops-lanes.md).
- One ticket per lane/session, replacing the earlier 3–5 sizing, after two multi-ticket lanes were cut off mid-delivery.

No further scope, schedule, order of work, definition of "latest stable" or change to an ADR was stated with these goals. None is recorded here. The later decisions below add only what they state.

## Owner decisions (2026-11-04, later the same day)
These refine, and do not delete, the statements above. Recorded at `lanshengzhi/cpp-coding-harness@6ca7b49`; the owner gave them in conversation, so no repository source carries them yet.
1. **Long-term goal (refines the statement above).** The owner will request periodic follow-up of upstream pi releases. The current alignment target remains pi v1.3.0. This does not yet define "latest stable" or the mechanism that advances the target; both stay undetermined (see [open questions](open-questions.md)). The earlier sentence "align Pike with the latest stable pi" is **superseded as a stand-alone goal** by this wording: it is not an instruction to track upstream automatically.
2. **Immediate v1.3.0 scope (settles an earlier open question).** The scope is exactly the selected scope of [ADR 0075](../../adr/0075-align-selected-owner-packages-and-linux-capabilities-with-pi-v1-3-0.md): selected non-AI capabilities, package migration and complete Linux durable support, with that ADR's stated exclusions and the retained Magpie and XDG boundaries. It is not a wider reading of "everything in pi v1.3.0". See [ADR 0075 scope](adr-0075-scope.md); the audit is [v1.3.0 residual-gap audit 2026-11-04](residual-gap-audit-2026-11-04.md).

## Relationship to existing decisions
- As of `lanshengzhi/cpp-coding-harness@6ca7b49`, the immediate goal matches the Named Baseline `pi-v1.3.0` (revision `71023aec1546d6d677a65669c5665c0927ce0dd0`) selected by [ADR 0075](../../adr/0075-align-selected-owner-packages-and-linux-capabilities-with-pi-v1-3-0.md). ADR 0075 selects a bounded set of capabilities, not every pi v1.3.0 feature: see [ADR 0075 scope](adr-0075-scope.md).
- The long-term goal has no accepted ADR in this repository as of that commit. The decision of 2026-11-04 (periodic follow-up, target stays v1.3.0) reduces, but does not remove, the tension with ADR 0075's "future releases require another audit": see [open questions](open-questions.md).
- Timeline context: [baseline timeline](baseline-timeline.md). Progress record: [evidence and delivery history](evidence-and-delivery-history.md).
