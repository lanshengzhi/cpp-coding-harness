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
  "product session records must not live in the AI message model" becomes a machine-checked include
  rule once this move lands, so the removed reach-through cannot return.
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
- The Parity Architecture Gate gains a clause-4 rule, so `cch_ai` and the session module cannot
  re-cross this seam without failing validation.

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
