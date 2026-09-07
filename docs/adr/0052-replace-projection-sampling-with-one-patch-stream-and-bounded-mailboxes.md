---
status: accepted
---

# Replace Projection Sampling with One Patch Stream and Bounded Subscriber Mailboxes

ADR 0051 established the projection seam as three operations — `state_version()`, `snapshot()`, and a single replaceable dirty edge — consumed by the Native TUI's "sample version, then sample snapshot" ticker protocol. Spec #597 story 10 requires alternative presentation layers to attach to the Headless Core without modifying it, but the sampling seam cannot serve a second projection: the dirty edge is stolen on re-attach, there is no patch vocabulary, every consumer pays a full-snapshot materialization per version, and push-only facts (tool partials and failures) reach the TUI outside the snapshot value entirely (#615).

We replace the three-method sampling contract with a single subscription seam: `attach(listener) -> Subscription`. The listener receives exactly two message kinds through a bounded, per-subscriber mailbox:

- **Base**: a complete immutable `AgentSessionSnapshot` captured at attach time — the subscriber's starting picture, including the full conversation history.
- **PatchMsg**: an ordered batch of value-bearing Patches. Each Patch names the changed slice of the session state and carries its new value, so applying patches in order to a Base reproduces the later snapshot exactly. Coarse mutations may carry the whole state value as a degenerate Patch.

The version counter becomes a private implementation detail; the dirty edge and the sample-version protocol are deleted. There is no shared patch history: the bounded mailbox is the only retention. When a subscriber falls behind and its mailbox overflows, the Core discards the backlog and the next message is a fresh Base — slow subscribers silently degrade to snapshot consumers, and resynchronization is indistinguishable from attaching. A new or replacement projection attaches mid-session and receives the state as it is *now*, never a replay from session start.

Push-only state (tool partials, tool failures, streaming lifecycle facts) becomes first-class Patch vocabulary: every fact a projection needs to render arrives through the one stream. The Native TUI's direct Agent Session event sinks survive only as a private fast path for the first projection, not as a second authoritative channel.

This supersedes the consumption protocol recorded in ADR 0051 (sampled version → sampled snapshot → composed frame). The ticker's authority as the counted frame, the sanctioned latency-first preview tier (#614), and the Block Frozen Protocol are unchanged; only the sampling mechanism is replaced by mailbox draining. The Native TUI binding (`SessionUiBinding`, the ticker's sampling logic) is rewritten accordingly as part of the tracer bullet this decision seeds.

**Convergence invariant** (the implementation's acceptance criterion): for every subscriber, `Base(version N)` followed by all Patches of versions N+1..M equals the session snapshot at version M.

**Worked example** — a second projection converging on a streaming tool partial:

1. A Web projection attaches at version 47 while the Native TUI is mid-run. It receives `Base(47)`: the full transcript and the active tool call in its pre-partial state. It renders immediately.
2. The Core publishes a tool partial as `PatchMsg(version 48, tool-partial record)`. Both subscribers' mailboxes receive it.
3. The Web projection applies the record on top of its Base and renders; its composed state equals the snapshot the Core would materialize for version 48. No Core modification, no stolen edge, no second channel.

**Test strategy** (at the existing projection unit seam):

1. Attach delivers a Base followed by ordered patches to multiple concurrent subscribers; no subscriber's edge affects another's.
2. Mailbox overflow resynchronizes with a fresh Base and never errors or blocks the Core.
3. Patch/snapshot convergence holds for a streaming tool partial and a message-chunk burst.

## Considered options

- Keep the three-method contract and add a subscribe entry point with a shared bounded patch history and per-subscriber version bookkeeping: rejected — three coordination mechanisms (version sampling, dirty edge, patches) plus hidden subscriber version state where one stream and a mailbox suffice; strictly more code and more concepts for the same behavior.
- Sequence-relative transcript operations (keyed per-entry add/update/remove over the messages vector): rejected — invents a stable-keying scheme that duplicates the session-event model the version semantics already enumerate.
- Copy the full snapshot on every version bump: rejected — a snapshot materialization is O(conversation size) while bumps arrive at streaming rate (50–100/s), recreating the O(N²) class ADR 0051 removed, multiplied once per subscriber; full copies remain only at attach and resync.
- Out-of-band secondary channel for push-only state: rejected — splits the vocabulary and leaves a second projection unable to render tool progress, which no viable frontend can accept.

## Consequences

- `SessionProjectionSource` is replaced by the subscription seam; `state_version()`, `snapshot()`, and `set_dirty_listener()` are deleted, and the version counter becomes private to the Core.
- Every projection-relevant fact flows through one channel; the TUI event sinks are demoted to a private fast path and migrate to the stream in the TUI's own follow-up refactor.
- A Web, GUI, or Spectator projection attaches by calling `attach` and requires no Headless Core modification (spec #597 story 10).
- Glossary terms recorded in CONTEXT.md: Projection Stream, Base, Patch, Subscription Mailbox.
