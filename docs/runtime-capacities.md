# Runtime capacities and fairness limits

Status: Current policy record. This document records the scenario measurements that select the concrete Runtime capacities, reserved lane, and mailbox batch limits enforced by `cch::harness::RuntimeRoot` (ADR 0040 §Admission, overload, and fairness; issue [#465](https://github.com/lanshengzhi/cpp-coding-harness/issues/465)). It follows ADR 0040's requirement that worker counts, capacities, byte charges, and batch sizes become policy only after representative same-environment workloads record repeated samples, variance, a selection rule, the chosen value, and the regression property it protects.

## Authority and scope

`RuntimeRoot` is a private, coding-agent-composed Runtime root. Ordinary bulk work (filesystem, Shell, model streaming) draws from the admitted budget; persistence, credential, terminal-completion, and Close control work draws from the reserved budget so ordinary traffic can never reject required progress. Mailbox drains process bounded batches and requeue themselves at the loop tail so one busy target cannot monopolize the loop. These limits are the policy knobs this document selects. They are not product interfaces: `harness::RuntimeLimits` is private to `cch_agent_core`.

The chosen values live in one place — the `harness::RuntimeLimits` defaults in `src/agent/harness/RuntimeRoot.hpp` — and the production CLI uses that default set unchanged (`kRuntimeLimits{}` in `src/coding_agent/cli/AsyncCliRuntime.cpp`), so replacement Sessions reuse identical admission and mailbox-batch behavior.

## Measurement environment

Measurements were taken on the repository development host, native Linux x86-64 with glibc, GCC 16.1.1, Debug build of the Runtime library, benchmark drivers compiled `-O2`. No provider network or live credentials were involved.

| Property | Value |
| --- | --- |
| CPU | Intel Core i5-8300H @ 2.30GHz, 4 cores / 8 threads |
| OS | Linux (glibc) |
| Compiler | GCC 16.1.1 |
| Runtime library build | Debug |
| Loop driving | Dedicated loop thread (`io.run()`), production shape |
| Samples | 5–8 repeated runs per point |

## Workload and scenario

The representative workload is sustained Agent, Models, Tool, and worker traffic into one Runtime mailbox: a producer keeps the mailbox saturated at the production admission budget by completing fresh operations as capacity frees, so the drain never catches a quiet gap. Two progress probes run on the same loop:

1. **Timer progress** — a repeating 5 ms `steady_timer` counts fires and records the maximum inter-fire gap while traffic is sustained. An unbounded drain would never return to the loop and would starve the timer; the bounded batch must keep it on schedule.
2. **Close progress** — `RuntimeRoot::close()` is called while a full 32-delivery backlog is draining; the measurement records how long `close()` takes to return and whether every admitted terminal still reaches its mailbox afterwards.

## Measured data

### Timer and drain throughput by mailbox batch

For each `mailbox_drain_batch` value, 5 repeated 500 ms runs. `timer_fires` is out of ~100 ideal for a 5 ms timer; `max_timer_gap` is the worst inter-fire gap observed (ms); `deliveries/s` is the mailbox drain throughput.

| batch | timer fires mean | max timer gap mean (range) | deliveries/s mean (range) |
| --- | --- | --- | --- |
| 4 | 99.0 | 5.23 ms (5.06–5.37) | 344 K (263 K–426 K) |
| 8 | 99.0 | 5.36 ms (5.00–5.66) | 349 K (334 K–360 K) |
| 16 | 99.0 | 5.27 ms (5.12–5.61) | 343 K (323 K–371 K) |
| 32 | 99.0 | 5.39 ms (4.98–6.27) | 368 K (338 K–396 K) |
| 64 | 99.0 | 5.24 ms (5.07–5.44) | 358 K (340 K–378 K) |

Repeated batch-16 samples (8 runs each): timer fires 99.0/99.0/99.0; max timer gap means 5.24/5.12/5.25 ms; deliveries/s means 345 K/359 K/347 K.

### Close progress under a full backlog

`close()` called while a 32-delivery backlog (200 µs per delivery) drains concurrently:

| sample | close() return | total drain after close | terminals delivered |
| --- | --- | --- | --- |
| 1 | 146 µs | 8 ms | 32 |
| 2 | 129 µs | 8 ms | 32 |
| 3 | 115 µs | 8 ms | 32 |
| 4 | 122 µs | 8 ms | 32 |
| 5 | 58 µs | 8 ms | 32 |

## Selection rules and chosen values

1. **`mailbox_drain_batch = 16`.** Across the measured range every batch value keeps the 5 ms timer at ~99 fires per 500 ms with a ~5.2 ms worst-case gap, so no value is needed for timer progress alone; the batch's job is to bound how many deliveries one busy target runs before the loop returns to the reactor (where timers, input, and Close control work are dispatched). 16 is the largest value in the flat-throughput, flat-timer region (32 and 64 gain nothing while letting a single target run twice as long before yielding; 4 and 8 add requeue overhead with no measured benefit). It bounds one drain to ≤ 16 bounded deliveries — microseconds of work — which is what keeps the loop fair under sustained traffic.
2. **`worker_count = 2`.** Retained from the original Runtime root (issue #459). The stress scenarios above use two workers and show no worker starvation; the value is unchanged by this issue.
3. **`max_admitted_operations = 32`, `max_admitted_bytes = 1 MiB`.** Retained ordinary admission budget. The sustained-traffic stress runs saturate this budget and still keep timers and Close progressing, so the value is validated by the scenario rather than re-selected.
4. **`max_reserved_operations = 8`, `max_reserved_bytes = 256 KiB`.** The reserved control lane is sized for the control work that must never be rejected behind ordinary bulk work: one session persistence append chain holds at most one in-flight append plus queued appends, each charged `estimate_message_bytes + 4096` (`kAdmittedOperationOverheadBytes`). 8 operations and 256 KiB cover several concurrent sessions' persistence, credential, terminal-completion, and Close control work while staying an order of magnitude below the ordinary budget, so ordinary traffic can still exhaust its own lane without touching the reserved one.

   **Control-work consumers today.** The reserved lane is the mechanism; `SessionPersistence::submit_message_append` is its one current consumer because it is the only control class that routes through the shared Runtime admission. Credential and terminal-completion work operate outside the shared Runtime — credentials are ready `AsyncResult` value work and Native TUI terminal I/O is readiness-driven on its own path — so they are protected by their own bounded paths (the criterion's "equivalent bounded protection") rather than by the reserved lane. Close work is covered in selection rule 5.
5. **Close capacity.** `RuntimeRoot::close()` and Session Close perform no Runtime admission themselves, so Close never depends on ordinary queue capacity. Close-required control work — persistence appends that drain during the final commitment flush — draws from the reserved lane (the pre-reserved capacity), and Close control tasks posted to the loop are serviced between bounded mailbox batches. Both are pinned by the Close-progress stress tests below.
6. **`mailbox_drain_batch` and the reserved lane are enforced as hard structural bounds.** They are not numeric performance gates; they are the fairness and overload invariants the regression tests below pin.

## Regression properties (tests)

The following tests pin the chosen values and the properties they protect. They run at the production default `RuntimeLimits{}` unless noted:

- `timers keep firing while sustained traffic fills a production-capacity mailbox` — `[harness][runtime][issue465]`: a repeating 5 ms timer keeps firing while a producer keeps the mailbox saturated at the production budget; an unbounded drain would starve it. Pins `mailbox_drain_batch = 16` and timer progress.
- `Close control work progresses while a production-capacity mailbox drains sustained traffic` — `[harness][runtime][issue465]`: a loop-posted Close control task runs at a batch boundary (delivered < backlog) and every admitted terminal still delivers after `close()`. Pins Close progress and the bounded batch.
- `a busy target's mailbox drain requeues in bounded batches so a second target is not starved` — `[harness][runtime][issue465]`: a peer target's terminal is serviced after at most one bounded batch of a busy target's backlog. Pins tail requeue.
- `a loop-posted Close signal is serviced between bounded mailbox batches` — `[harness][runtime][issue465]`: same property at a small explicit batch. Pins tail requeue determinism.
- `reserved control admission succeeds while the ordinary budget is exhausted` / `reserved admission enforces an independent byte budget` / `control and ordinary terminals deliver through one mailbox in admission order` — `[harness][runtime][issue465]`: the reserved lane is independent of the ordinary budget (task count and bytes), releases correctly, and shares one FIFO sequence with ordinary work.
- `persistence control work is admitted while the ordinary runtime budget is saturated` — `[coding_agent][runtime][commitment][issue465]`: a real Session Event Commitment append is admitted on the reserved lane while the ordinary budget is full. Pins persistence (control) admission end to end.
- `RuntimeRoot close drains admitted worker completions before teardown` — `[harness][runtime][issue459]`: `close()` stops admission and joins workers while every admitted terminal reaches its mailbox. This test also covers the lost-wakeup fix in `stop_admission_and_drain_workers` (the `stopping` flag is written under the same `worker_mutex` the workers wait on), which is the Close-progress regression that issue #465 surfaced.

## Projection Stream mailbox capacity (ADR 0052, issue #617)

Separate from the `RuntimeRoot` admission limits above, the Headless Core's Projection Stream retains one bounded Subscription Mailbox per subscriber (`kProjectionMailboxCapacity = 64` messages, `src/coding_agent/include/cch/coding_agent/ProjectionStream.hpp`). The selection reasoning:

- **Buffering need.** The measured streaming publication rate is ~50–100 Core publications/s (issue #600 cost contract). The one production subscriber — the Native TUI frame ticker — drains at ~30 frames/s, so steady state accumulates 2–4 messages between frames. 64 messages buffers well over half a second of saturated Core activity, an order of magnitude above the steady-state need, so a single delayed frame can never overflow.
- **Bounded retention.** A subscriber that stops draining keeps at most 64 messages; overflow discards the backlog and the Core enqueues a fresh Base (ADR 0052: a slow subscriber silently degrades to snapshot consumption), so no queue grows without bound and publication never blocks the Core's serialized domain.
- **Regression property.** `Projection mailbox overflow resynchronizes with a fresh Base and never blocks the Core` (`[coding_agent][projection][issue617]`) pins the bound: more than 64 publications with no drain still complete the prompt, leave a bounded mailbox, and resynchronize with a fresh Base that converges with the Core snapshot.

## MCP Host defensive limits (ADR 0064, spec #833 story 24, issue #836)

Separate from the Runtime admission limits above, `cch_mcp` bounds what one Upstream MCP Server can cost a session. The chosen values live in the one private constants point `src/mcp/Protocol.hpp` and are not caller-tunable: a cap the configuration could raise would not be a containment limit.

- **`kMaxToolsPerUpstream = 5000` tools.** A `tools/list` walk that would push the accumulated catalog past this cap fails with `ResourceLimit` instead of admitting it. 5,000 is roughly two orders of magnitude above any tool catalog a real Upstream advertises, so an ordinary server never notices the bound, while a hostile or buggy one cannot force the session to hold an unbounded descriptor set.
- **`kMaxListPagesPerUpstream = 1000` pages.** A pagination walk that has consumed 1,000 pages and still carries a `nextCursor` fails with `ResourceLimit`. Together with duplicate tool names and repeated-cursor rejection — both `Validation` failures that stop the walk immediately — this is what keeps a cursor loop or a cursor generator from running unbounded.
- **`kMaxDiagnosticBytes = 1024`.** Every Upstream-supplied diagnostic is redacted before it is truncated (CODING_STANDARDS.md §10.2), and an `Error::context` is never carried into a diagnostic because a JSON parse failure puts the whole untrusted response body there.

These are containment bounds, not throughput policy: the walk they bound is bounded in-memory value work over a scripted or TLS transport, so there is no queue to tune and no fairness property to measure. The regression properties below pin them.

### Regression properties (tests)

- `a catalog at the per-server tool cap is admitted whole` and `a catalog past the per-server tool cap is rejected` — `[mcp][catalog][limits][issue836]`: the 5,000-tool boundary is exact on both sides.
- `a pagination walk at the cursor cap is admitted and one page past it is rejected` — `[mcp][catalog][limits][issue836]`: a 1,000-page walk succeeds, and the exchange that asks for page 1,001 is refused without being sent.
- `a duplicate tool name across pages is rejected` and `a pagination cursor the Upstream already returned is rejected instead of followed` — `[mcp][catalog][limits][issue836]`: pins both loop rejections, and the second also pins the exchange count, so a rejection can never be "retried" into a loop.

## MCP Host connection bounds (ADR 0064, ADR 0011, spec #833 stories 9 and 10, issue #839)

The same private constants point bounds what one Upstream connection costs while it is trying, failing, and being torn down. These are not throughput policy either: the work they bound is one probe of one endpoint, so there is no queue to tune. They are containment plus liveness.

- **`kDefaultRequestTimeout = 30 s`, `kMaxRequestTimeout = 300 s`.** One `tools/call` exchange gets the 30 s product deadline. A caller asking for more is capped rather than honored, because a per-call deadline the caller could raise would not be a containment bound.
- **`kInitialReconnectBackoff = 250 ms`, `kMaxReconnectBackoff = 30 s`, `kMaxConnectAttempts = 10`.** The ladder waits 250 ms before the first reconnect and doubles per rung, never past 30 s, and after ten rungs it is spent: the connection reports `failed` and only an explicit request from its owner starts a new ladder. Ten rungs at the cap is ~5.5 minutes of a permanently dead endpoint before the host stops knocking, and the total cost is bounded in both attempts and wall time.
- **`kConnectionCleanupBound = 1 s`.** The whole of a connection's close — stopping the admitted operations, dropping the client, cancelling the armed reconnect — is bounded to one second. A conforming transport answers its cancellation immediately, so the bound is a backstop for one that does not: a connection that cannot be quiesced in time is released anyway, and `UpstreamCloseOutcome::abandoned_operations` reports what was still outstanding.

Storm protection is the property the ladder exists for, and it is a coalescing rule rather than a jitter value, so it is exact and testable: while one delay is armed, every additional failure and every transport-closure notification is absorbed instead of arming another, and a rung advances only when a delay actually elapses. Fifty failed tool calls plus a hundred closure notifications therefore cost one rung, not one hundred and fifty.

### Regression properties (tests)

- `a flapping Upstream is not reconnected once per failure` — `[mcp][connection][backoff][issue839]`: pins the whole ladder. Fifty failed calls and a hundred closure notifications arm exactly one delay, the delays are exactly `250, 500, 1000, 2000, 4000, 8000, 16000, 30000, 30000, 30000` ms, the endpoint is probed exactly eleven times in total, and a hundred further notifications afterwards probe nothing.
- `a spent ladder starts over only when the owner asks for a connection again` — `[mcp][connection][backoff][issue839]`: pins that a spent ladder is the owner's to restart, not the Upstream's to keep knocking with.
- `no reconnect fires after a close that happened during the backoff` — `[mcp][connection][close][issue839]`: pins that the close cancels the armed delay and that letting it elapse anyway reconnects nothing.
- `a close during an in-flight call reaches quiescence with nothing abandoned` — `[mcp][connection][close][issue839]`: cancellation reaches the request, the call reaches a terminal outcome, the cleanup bound is never needed, and the connection reports zero abandoned operations.
- `a call that ignores cancellation is released at the cleanup bound` — `[mcp][connection][close][issue839]`: the connection asked for exactly the 1 s bound and, when it expired, released the connection and reported the one operation it abandoned.
- `startup never waits for an Upstream that never answers` — `[mcp][connection][startup][issue839]`: the connection attempt is handed back while the Upstream is still silent, the connection reads `pending` with one operation in flight, and a tool call against it fails immediately instead of waiting.
- `a tool call is bounded by the per-call deadline and capped at the containment bound` — `[mcp][connection][limits][issue839]`: every request the connection framed carries the 30 s default, and a caller that asked for ten minutes got the 300 s cap on the wire.

## Update procedure

A limit changes only through the same evidence path: record the representative workload and environment, repeated samples and variance, the selection rule, the chosen value, and the regression property that protects it. When a limit changes, update this table, the `harness::RuntimeLimits` defaults, and — if the production value is no longer the default — the explicit set in `src/coding_agent/cli/AsyncCliRuntime.cpp`. The regression tests above must pass at the new values before the change is accepted.
