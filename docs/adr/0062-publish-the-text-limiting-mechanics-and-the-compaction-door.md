---
status: accepted
---

# Publish the text-limiting mechanics and the compaction door as Owner Interfaces

`OutputLimiter.hpp` (pi `truncateHead`/`truncateTail`/`formatSize`) is pure line- and byte-budget
mathematics with no Agent, Harness, or model state, yet it lived at `src/agent/harness/`, under
`cch_agent_core`'s private root. Five files in `src/coding_agent/` — including the TUI tool
renderers `BashRenderer.cpp`, `ReadRenderer.cpp`, `RenderUtils.cpp` — reached across the Owner
boundary with `#include "agent/harness/OutputLimiter.hpp"`, so the terminal frontends compiled
against a headless core's private area. The same pattern put `Compaction.hpp` (the compaction
door's whole legal contract: `CompactionSettings`, `should_compact`, `is_context_overflow`,
`estimate_context_tokens`, `compact`) behind `#include "agent/harness/compaction/Compaction.hpp"`
in three `cch_coding_agent` files, and let `SessionFork.cpp` / `SessionLifecycle.cpp` serialize
session entries and drive the session journal directly instead of going through `SessionStore`.

Three accepted decisions bear on the answer. [ADR 0039](0039-own-the-capability-owner-package-graph-and-parity-architecture-gate.md)
makes the interface root the only seam and states that deleting a package's interface root removes
its headers from every dependent's include path.
[ADR 0046](0046-move-pi-neutral-mechanics-from-cch-ai-to-cch-support.md) moved the bounded/redacted
text mechanics to `cch_support` but explicitly declined to promote them, because
`AsyncResultBridge.hpp` — the fourth header in that move — carries ten Boost.Asio headers and
[ADR 0042](0042-adopt-a-strict-no-exception-core-with-private-async-bridges.md) keeps Asio out of
every interface. [ADR 0056](0056-keep-session-record-serialization-in-the-session-module.md) keeps
Session Entry serialization inside the session module; the fork flow was the remaining hole in that
closure.

`BoundedText.hpp`, `Redactor.hpp`, and `OutputLimiter.hpp` carry no Boost and no third-party
weight, so ADR 0046's stated reason does not apply to them. They move to the `cch_support`
interface root as `<cch/support/BoundedText.hpp>`, `<cch/support/Redactor.hpp>`, and
`<cch/support/OutputLimiter.hpp>`, and the limiter's namespace follows ownership to
`cch::support` with all call sites re-qualified. `AsyncResultBridge.hpp` stays private: it is the
one header of the four that ADR 0046 named, and it is the one that still carries Asio.

`Compaction.hpp` moves to `<cch/agent/harness/session/Compaction.hpp>`, the public spelling of
the `cch::harness::session` namespace that already owns it. Promoting it required replacing the
two `boost::asio::awaitable` shapes in its contract — `SummarizationStreamFn` and `compact()` — with
`support::AsyncResult`, matching how every other Owner Interface operation is typed
([ADR 0040](0040-own-asynchronous-operations-and-the-serialized-runtime-lifecycle.md)). The
implementation keeps its coroutine; `support::detail::make_async_result` publishes the terminal
outcome on the consuming serialized domain, the same bridge `Agent::prompt` and the built-in tools
already use.

`SessionStore::create_from_entries(path, metadata, entries)` closes the fork hole: the caller hands
over a re-chained `SessionEntry` list and the facade owns the header line, the per-entry wire
encoding, and the journal. An `Unknown` entry is re-emitted from its stored wire line, so foreign
entry kinds survive a fork byte-identically; a `Header` entry is rejected because the header is
written from `metadata` alone. The previous fork code re-appended `'\n'` to lines that
`EntrySerializer::serialize_entry` already terminates, so a forked file carried a blank line
between every entry; writing each entry exactly once removes that.

The Parity Architecture Manifest gains the contract rule `no-agent-private-reach-through`, which
forbids `src/coding_agent/` and `src/cli/` from including the four closed paths. The rule is
fail-closed, so the closed seams stay closed at `ctest -L architecture` rather than by review.

## Considered options

- Move `OutputLimiter` to `cch_support` and re-express the compaction contract through
  `cch_coding_agent`: rejected because `AgentSession::Impl::attempt_compaction` would then re-own
  cut-point selection, file-operation extraction, and the cache-isolation rules, undoing ADR 0034's
  assignment of compaction to `cch_agent_core`.
- Leave `Compaction.hpp` private and keep a private `src/agent/harness/compaction/` include in the
  application layer: rejected because it keeps the private area a de-facto second seam, which is
  the failure mode ADR 0046 already recorded for `cch_ai`.
- Promote `AsyncResultBridge.hpp` alongside the other three so the support set is uniform:
  rejected again, for ADR 0046's original reason: it is the header that would put Asio into an
  interface vocabulary.
- Forbid all of `agent/` from `src/coding_agent/` in the manifest: rejected because the
  remaining reach-throughs are deliberate, not defects. `RuntimeRoot.hpp`, `Process.hpp`,
  `ShellResolver.hpp`, and `ShellEnvironment.hpp` are seams the application layer composes
  without a public equivalent, and publishing them means converting the Runtime and process
  contracts off `boost::asio::awaitable` — a separate decision from the seams this one closes.
  `AgentMessageAccess.hpp` and `AgentPromptAccess.hpp` are friend shims that keep the
  prompt-scoped stop source and the session-internal continuation off the public `Agent`
  contract, which is the intent of
  [ADR 0049](0049-keep-agent-policy-convenience-out-of-the-owner-interface.md); widening `Agent`
  to reach them would reopen that decision. None of the six leaks a wire line, a serialization
  DTO, or a text-budget algorithm into the application layer, so the residual exposure is
  narrower than the reach-through this decision removes.

## Consequences

- The frontends and the session runtime compile against `<cch/support/...>` and
  `<cch/agent/...>` only; no `src/coding_agent/` or `src/cli/` source includes a
  `cch_agent_core` private header for text limiting, compaction, or session writing.
- `PARITY-8004` reports a reach-through include; the fixture mirror in
  `tests/architecture/parity_gate_test.py` names the same rule.
- `tests/harness/OutputLimiterTest.cpp` moves to `tests/support/OutputLimiterTest.cpp` and
  `cch_tests_support`, since the mechanic is support-owned and needs no Agent or Harness context.
- The compaction door's terminal outcome arrives through `support::AsyncResult`; a consumer that
  does not resume it through `support::detail::await_async_result` gets no initiating executor, the
  same requirement every other `AsyncResult` producer already carries.
