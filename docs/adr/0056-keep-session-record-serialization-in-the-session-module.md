---
status: accepted
---

# Keep session-record serialization in the session module instead of growing an AI JSON seam

`cch_ai`'s private `src/ai/glaze/` directory (1413 lines: `AiJson.hpp` 977, `ModelJson.hpp` 400,
`ToolDtos.hpp` 36) has no `cch_ai` production consumer. `AiJson.hpp`'s only `src/` consumer is
`src/agent/harness/session/EntrySerializer.cpp`, which builds pi v3 Session Format DTOs on
`ai::glaze::MessageDto` / `ContentDto`; `ModelJson.hpp` has no `src/` consumer at all, and
`ToolDtos.hpp` is consumed only by `AiJson.hpp`. This is [#540](https://github.com/lanshengzhi/cpp-coding-harness/issues/540)'s
Seam 2: the session module's reuse of AI message DTOs crosses into a private header rather than an
Owner-owned seam.

#540 poses the choice as two remedies: `cch_ai`'s Owner Interface grows a narrow message-JSON
read/write seam, or the session module owns its own DTO shapes and accepts duplication against the
shared message variant. [ADR 0046](0046-move-pi-neutral-mechanics-from-cch-ai-to-cch-support.md)
deferred this adjudication and described the reuse as "domain-coupled". This decision supersedes
that judgment and takes the second remedy.

Three accepted decisions constrain the answer.
[ADR 0005](0005-keep-provider-and-product-messages-in-their-owning-modules.md) states that "The
harness session module owns Session Entry values and serialization", and its considered options
reject allowing product or Session Entry alternatives to accumulate in `cch::ai::MessageVariant`
"because it would expand the lowest public message surface".
[ADR 0009](0009-treat-pi-v3-sessions-as-an-interoperable-wire-contract.md) makes the pi v3 session
file an interoperable wire contract whose field requiredness, explicit nulls, topology, and entry
alternatives may not be silently redefined by a C++ persistence need. `docs/agents/architecture.md`
§Local generic machinery places "Glaze DTOs, schema conversion, visitors, parsing helpers, and
similar machinery ... in serialization or implementation layers rather than Owner Interfaces".

## Decisions

- **The session module owns the session-record DTO shapes and their serialization.**
  `src/agent/harness/session/` gains the pi v3 Session Format DTOs and the message mapping it
  needs, and `EntrySerializer.cpp` stops including `ai/glaze/AiJson.hpp`. The session module keeps
  consuming `cch::ai::Message` values through the Owner Interface, which the legal
  `cch_agent_core` → `cch_ai` dependency already permits.
- **`cch_ai`'s Owner Interface does not grow a message-JSON read/write seam.** Serialization
  machinery stays out of every Owner Interface, and a pi wire contract does not become an AI
  capability.
- **Duplication against `cch::ai::MessageVariant` is accepted deliberately.** The two DTO sets may
  evolve independently; the pi wire contract, not structural identity with the AI message model, is
  what must be preserved.
- **The pi v3 wire contract stays pinned by the existing byte-stable golden tests.**
  `tests/harness/session/SessionRoundTripGoldenTest.cpp` and
  `tests/coding_agent/runtime/SessionSuiteGoldenTest.cpp` are the guard for the accepted
  duplication, matching [ADR 0054](0054-make-client-transports-tls-only-with-concrete-executor-beast-streams.md)'s
  "session-record JSON stays byte-stable under golden tests". The moved DTOs reproduce the current
  bytes exactly; changing a golden is a separate, explicit decision.
- **Every remaining file in `src/ai/glaze/` is dispositioned by the same ownership rule.** No file
  there survives merely because a test or another Owner includes it. `ModelJson.hpp`'s confirmed
  absence of any `src/` consumer — and whether production models.json parsing duplicates it — is
  resolved when the move lands; models.json parsing itself stays with its current coding-agent
  owner.
- **The Product Architecture Contract gains the clause it currently only asserts.** ADR 0053's
  "product session records must not live in the AI message model" gains an include-level rule once
  this move lands. That rule asserts the `src/agent/` side only; it does not by itself prevent
  `src/ai/` from re-growing a session-record shape (see the addendum on #657 rows 1 and 2).
- **The other reach-throughs #540 covers are remedied independently and recorded there.**
  `ai/utils/RetryClassifier.hpp`, `ai/ModelThinkingLevel.hpp`, and `ai/providers/FakeProvider.hpp`
  each widen or relocate an existing Owner surface rather than choosing a serialization owner, so
  they need an adjudication record, not an ADR.

## Considered options

- **`cch_ai`'s Owner Interface grows a narrow message-JSON read/write seam**: rejected because it
  puts serialization machinery in an Owner Interface against `architecture.md`, it expands the
  lowest public message surface ADR 0005 deliberately bounded, and it would turn a pi wire contract
  into an AI capability.
- **Leave the reach-through and record a closing rationale**: rejected because `src/ai/glaze/` has
  no `cch_ai` production consumer, so the coupling is not paying for itself inside its own Owner,
  and because ADR 0005 already assigns session serialization to the session module.
- **Move `AiJson.hpp` into the session module unchanged**: rejected as the framing rather than the
  remedy. The session module should own shapes that serve the pi v3 contract, not inherit a file
  organized around AI message values.
- **Let the session serializer consume a serialized `cch::ai::MessageVariant` shape**: rejected
  because it would let AI message-model vocabulary define the session wire format, inverting ADR
  0009's contract direction.

## Consequences

- One cross-Owner private-header include disappears from
  `src/agent/harness/session/EntrySerializer.cpp`, and `cch_ai`'s private area stops serving as
  another Owner's serializer.
- The session module can evolve the pi v3 wire shape under its own ownership, and the AI message
  model can change without silently changing session files.
- Two DTO sets exist over the same message values. The accepted cost is drift, bounded by the
  byte-stable golden tests; an unexpected golden diff is the signal that drift has occurred.
- `src/ai/glaze/` shrinks to what `cch_ai` itself consumes. Any file whose only consumers are tests
  or another Owner is removed or relocated rather than retained.
- ADR 0046's characterization of this reuse as "domain-coupled" is superseded for Seam 2. Its Seam
  1 outcome — Provider assembly owned inside `cch_ai` — is unaffected, as are ADR 0047's `Models`
  deepening and ADR 0055's Models Runtime narrowing.
- The Parity Architecture Gate gains a clause-4 rule (`agent-no-ai-private-includes`, `PARITY-8003`)
  that keeps session-module sources under `src/agent/` out of `cch_ai` private headers. It is an
  include-level rule on one side of the seam, so it does not cover `cch_ai` re-growing a
  session-record shape (see the addendum on #657 rows 1 and 2).

## References

- Issue [#540](https://github.com/lanshengzhi/cpp-coding-harness/issues/540) (Seam 2 adjudication)
  and the architecture-scouting map [#533](https://github.com/lanshengzhi/cpp-coding-harness/issues/533)
  this follow-up came from.
- [ADR 0005](0005-keep-provider-and-product-messages-in-their-owning-modules.md), [ADR
  0009](0009-treat-pi-v3-sessions-as-an-interoperable-wire-contract.md), [ADR
  0046](0046-move-pi-neutral-mechanics-from-cch-ai-to-cch-support.md), [ADR
  0053](0053-replace-pi-parity-authority-with-the-product-architecture-contract.md), [ADR
  0054](0054-make-client-transports-tls-only-with-concrete-executor-beast-streams.md).
- `docs/agents/architecture.md` §Local generic machinery.

## Addendum: the clause-4 guard is include-level and one-sided (Issue #657, rows 1 and 2)

Two claims above asserted more than the gate does. Both were narrowed in place; this addendum records
why, and keeps the two adjudications behind them traceable.

**The clause-4 rule does not enforce "no session record in the AI message model".** The rule is
`agent-no-ai-private-includes` (`cmake/parity/manifest.json:25-35`): `source_prefixes: ["src/agent/"]`
against `forbidden_include_prefixes: ["ai/", "src/ai/"]`, enforced by `_architecture_include_diagnostic`
(`cmake/parity/parity_gate.py:1950-1986`, diagnostic `PARITY-8003`). Its predicate compares the
including file's path prefix and the raw `#include` spelling, and nothing else — no declared target,
Owner, role, or file content. So it asserts nothing at all about `src/ai/`, and the file #652 removed
can come back as a self-contained DTO header whose include set no forbidden prefix contains.

A reversed rule (`source_prefixes: ["src/ai/"]`) does match such a header, but the predicate has no
selectivity: it also matches any sibling header carrying the same raw spellings, legitimate ones
included, so that configuration is not usable. A new rule kind (an Owner-scoped content scan) was
rejected — it needs four schema changes and conflicts with #652's recorded scope of reusing the
existing forbidden-include-prefix shape. **Adjudicated: accept the gap and leave the rule as it is.**

**The guard for the accepted duplication is narrower than "the guard".** The two golden tests named
above do pin the pi v3 bytes, but their comparison projects away the fields where the two DTO sets can
drift: the `SessionSuiteGoldenTest.cpp` projection keeps role, content, api, provider, model,
stopReason, errorMessage, toolCallId, toolName, isError, summary, and customType, and drops
`timestamp`, `usage`, `diagnostics`, `responseId`, `responseModel`, `rawStopReason`, `details`, and
`display`. The pi v3 golden covers two message states byte-exactly, out of twenty fixture lines.

Measured drift over the module's history is low: of seven shape changes, six landed on both DTO sets,
and all seven field additions changed both in one commit. But a field added to one set alone triggers
no failure at all — the compiler catches renames and deletions, and no field-count or schema check
exists. **Adjudicated: keep the deliberate duplication**, since the evidence supports the value it was
accepted for, and handle the guard's blind spots as separate defects rather than by reopening the
duplication decision.
