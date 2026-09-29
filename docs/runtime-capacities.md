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

## MCP Host Upstream-tool bounds (ADR 0064, ADR 0007, ADR 0061, spec #833 stories 19-21 and 25, issue #842)

The `cch_coding_agent` half of the MCP Host — the adaptation from a discovered Upstream tool descriptor into a callable `cch::agent::Tool` — is bounded here too. The value is a private constant next to the code that applies it, `src/coding_agent/runtime/McpToolBinding.cpp`, for the same reason the wire-layer values live in one point: a cap a configuration could raise would not be a containment limit.

- **`kMcpQualifiedToolNameMaxLength = 64` characters.** A Qualified Tool Name is `mcp__<Server Id>__<tool>`, sanitized to `[a-zA-Z0-9_-]`; a name that would be longer keeps its head and a deterministic 8-hex-digit FNV-1a suffix, so a long third-party tool name is contained without two names ever collapsing onto one registered tool. This is a bound on a name an Upstream chose, and it is a *name* bound: how many tools a session may hold stays the per-server catalog cap of 5,000 from `cch_mcp`, times the number of configured servers.
- **One upstream tool result is bounded by the product's ordinary tool-output limit** — 50 KiB and 2,000 lines, the same `OutputLimit` `read` and `bash` already use. A second, MCP-specific output limit would be a second thing to tune for the same containment. Redaction is applied to the complete serialized result *before* the limit, so a secret is erased rather than truncated in half.
- **`kMaxDiagnosticBytes = 1024`**, restating the connection machinery's own bound for one upstream-supplied explanation that becomes a failed tool call, so an Upstream cannot turn a refusal into a flood of model-visible text.

There is no fairness property to measure here: the work being bounded is in-memory value work over an already-admitted catalog, not a queue. The regression properties below pin the properties instead.

### Regression properties (tests)

- `a Qualified Tool Name namespaces the Server Id and the upstream tool name` and `an over-long Upstream tool name is truncated with a deterministic hash suffix` — `[mcp][issue842][spec]`: the sanitizing, the 64-character cap, the determinism a reconnect depends on, and the distinctness that keeps a truncation from retargeting a live tool.
- `a flooding tool result is bounded and redacted before it is truncated` — `[mcp][issue842][spec]`: a megabyte of upstream content with a secret-shaped key at its tail comes back inside the 50 KiB bound with no trace of the secret, which is the redaction-before-truncation order made observable.
- `invalid arguments make no upstream request` — `[mcp][issue842][spec]`: ADR 0007 validation of the Upstream's own schema rejects the call before the request exists, so a malformed call costs no round trip.
- `a closed tool binding refuses a late publication and drains nothing` and `late discovery after session close registers nothing and resurrects no connection` — `[mcp][issue842][spec]`: pins that a catalog completing after Close re-registers nothing and knocks no further.

## MCP Host call-approval bounds (ADR 0064, ADR 0018, spec #833 story 22, issue #843)

The call-time authorization of an `approval: "ask"` Upstream MCP Server is bounded where the policy applies it, in `src/coding_agent/runtime/McpToolApprovalPolicy.cpp`. These are containment bounds on third-party text a prompt renders, not a user-tunable policy: there is no per-call approval limit, only these.

- **`kMaxArgumentsBytes = 64 KiB`** on the argument rendering an approval prompt carries. The prompt is about the exact call, so the arguments are shown in full up to this bound; a call whose prepared arguments render larger is still answered, with the rendering cut and marked rather than refused. 64 KiB is well above any argument object a real tool call carries and well below the product's 50 KiB tool *result* limit's neighborhood, so a hostile server's payload cannot turn the prompt into a wall of text.
- **`kMaxReasonBytes = 1024`** on each refusal a non-consent outcome produces. One template, one bounded, redacted interpolation, and the interpolated name is the session's own registered Qualified Tool Name rather than Upstream-supplied text — so the bound is habit here, and it is what keeps it true if the sentence ever grows a detail.

A refused call is a *bounded* cost in another sense too: one refusal is one failed tool call (ADR 0008), and the model may retry, but every retry is a fresh question with the same bound — the hook never re-asks a call it already answered, so a refusal cannot become a prompt storm.

### Regression properties (tests)

- `an ask call is put to the prompt once, with the tool name and its arguments` — `[mcp][issue843][spec]`: the prompt carries the Qualified Tool Name, the Server Id, the Upstream's own tool name, and the call's arguments, and asking it once produces exactly one question.
- `a call that is not an ask call is never put to the prompt` — `[mcp][issue843][spec]`: a built-in tool name and an `allow` server both answer without the prompt being consulted at all.
- `a session with no prompt refuses the ask call rather than running it`, `a prompt that fails refuses the ask call rather than running it`, and `a headless session refuses the ask call and never reaches the Upstream` — `[mcp][issue843][spec]`: the three ways consent cannot be obtained, each one zero upstream requests and one failed tool call.

## MCP Host Lazy-activation bounds (ADR 0064, ADR 0066, spec #833 stories 12-16 and 18, issue #847)

Lazy Tool Activation keeps a discovered `lazy` Upstream tool's JSON Schema out of the model context until the model activates it, and puts a discovered server's own `instructions` into the System Prompt. Both surfaces are bounded here, in the same private constants point as the rest of the `cch_coding_agent` half of the host (`src/coding_agent/runtime/McpToolBinding.cpp` for the catalog, `src/coding_agent/AgentSessionExecution.cpp` for the prompt section).

- **`kMaxSearchRows = 100` catalog lines per `mcp_search` call.** A search result is a discovery aid, not a context dump: the model narrows with `query` and `server` and activates what it needs, and a result past the cap says how many rows it hid rather than truncating silently. The number of catalogued tools itself is unchanged at the per-server cap of 5,000 from `cch_mcp`.
- **`kMaxSearchLineBytes = 240` bytes per catalog line's description.** The compact line is a catalog entry, not the tool's documentation; whitespace runs collapse to single spaces and the text is redacted before it is bounded, so a third party's description cannot flood one result.
- **`kMaxInstructionsBytes = 2 KiB` per server's `instructions`.** The server's own usage guidance is prose for the model, not an interface; a server that writes at length gets the head of it and a truncation mark.
- **`kMaxInstructionsSectionBytes = 16 KiB` for the whole `mcp_upstreams` System Prompt section.** A session with many chatty Upstreams cannot push the System Prompt without limit, and a server whose guidance did not fit is counted in a trailing line rather than dropped in silence.

There is no fairness property to measure here either: a search reads an already-discovered catalog and an activation edits one map, so neither is a queue. The properties below pin the bounds and the stickiness instead.

### Regression properties (tests)

- `the lazy catalog lists tools without their schemas and activates one on request` — `[mcp][issue847][spec]`: pins that a lazy tool's own JSON Schema is absent from the search result (checked against a property name that exists only inside that schema), that a `lazy` catalog publishes nothing, that the two meta-tools are staged exactly once, and that a second activation of the same tool is a success that changed nothing — the stickiness that keeps provider prompt caches intact.
- `a lazy server's tools stay out of the tool surface until the model activates one` — `[mcp][issue847][spec]`: the context-economy property, end to end through the production session door: the search call costs no upstream request and no `lazy` schema appears in any request.
- `an activated tool's schema reaches the next real model request and it can be called` — `[mcp][issue847][spec]`: the activation's effect lands one turn later, with the Upstream's own schema, and the call reaches the Upstream under its own tool name.
- `a server's own instructions reach the model in a real request` — `[mcp][issue847][spec]`: the guidance is a System Prompt section, so it is in every request the session makes once the server has offered it.

## MCP Host Multi Round-Trip bounds (ADR 0064, ADR 0008, spec #833 stories 27-29 and 32, issue #845)

A `tools/call` an Upstream suspends for client input costs the session two things the connection bounds do not: the user's attention, and a second exchange. Both are bounded, in `src/mcp/Protocol.hpp`, and neither is caller-tunable — a bound the configuration could raise would not be a containment bound.

- **`kMaxElicitationRounds = 8` suspensions per call.** A server that keeps asking costs a bounded sequence of decisions rather than a dialog that never ends; past the bound the single tool call fails with a diagnostic and the connection is untouched.
- **`kDefaultElicitationTimeout = 5 min`, capped by `kMaxElicitationTimeout = 30 min`.** The wait is on a *human* completing an out-of-band approval, so the default is generous where the per-request deadline is not, and the cap is containment rather than tuning. The bound races the answer, not the other way round: whichever arrives first ends the wait, and a late reply is discarded rather than completing a call twice.
- **`kMaxRequestStateBytes = 64 KiB` of opaque continuation token.** The token is echoed **verbatim**, so the only safe bound is one that refuses: a truncated token is a *different* token, and the server would reject it as though the user had answered a different question. 64 KiB is far above any token a real server issues and bounds what one hostile response can pin.

Two bounds are not constants because they are not the host's to choose. The **stop token** is the caller's, and the two-phase close stops it too, so a session that ends ends the wait. The **undeclared input-request type** is a refusal: an input request whose `type` is neither `url` nor `form` — the two modes `client_capabilities()` advertises — fails exactly one tool call without asking the user anything, which is the story-32 defensive matrix and ADR 0008's isolation rule.

The fairness property worth stating is the one that is *not* throughput: a suspension that is never answered produces **no re-send**. The user never answered, so nothing is sent in their name, and the call ends as one failed tool call. The regression properties below pin that alongside the numeric bounds.

### Regression properties (tests)

- `a URL elicitation is asked, answered, and the original call continues` — `[mcp][issue845][spec]`: the question the user is shown, a fresh JSON-RPC id on the retry, the original name and arguments, and the answer keyed by the request the Upstream named.
- `the opaque requestState is echoed byte-for-byte on every retry` — `[mcp][issue845][spec]`: the fixture writes the result's source text by hand, with members out of the order a value tree sorts them into and numbers a double would reformat, so a retry built by re-serializing the token would carry different bytes. Two rounds, a fresh id each, arguments intact.
- `a server that keeps suspending is bounded in rounds` — `[mcp][issue845][spec]`: exactly the declared cap of suspensions, then one failed call.
- `an unanswered elicitation reaches its bound, fails one call, and re-sends nothing`, `stopping the call during an elicitation leaves no suspended call and no re-send`, and `a continuation token past the host's bound is refused, not truncated` — `[mcp][issue845][spec]`: the bound, the session's stop token, the close-during-a-wait race with a late answer discarded, and the token refusal.
- `an undeclared input-request type fails one call and never asks the user` — `[mcp][issue845][spec]`: the user is never asked, one call fails, and the next ordinary call on the same connection still succeeds with no re-probe and no re-list.
- `an unanswered elicitation that reaches its bound fails the one call and sends nothing` — `[mcp][coding_agent][issue845][spec]`: the same property through the production session door, on the session's own timer, with the question withdrawn when the wait ends.

## MCP form-dialog bounds (ADR 0064, ADR 0008, spec #833 story 26, issue #846)

A form-mode Pending Elicitation is a schema an Upstream MCP Server wrote, and this build renders whatever the schema says. The Multi Round-Trip bounds above say how long the user may be asked; these say how much of a server's document the layout can carry. They are in `mcp_form_bound`, `src/coding_agent/tui/McpElicitationForm.hpp`, and they are the whole of the containment: past any of them the form is not drawn, the dialog says so on screen, and the answer carries no fields. A partial form shown as though it were the whole question is the one failure a user cannot see coming, so the notice is not optional.

- **`kMaxSchemaBytes = 64 KiB` of schema text.** The wire already caps one response at `kMaxResponseBytes = 8 MiB`, which is far more text than a dialog can usefully draw. A larger schema is not parsed.
- **`kMaxNesting = 32` levels of object/array nesting.** This is the liveness bound, not a tidiness bound: the JSON reader is recursive descent, so a schema nested deeper than the stack can hold is a **crash**, and a crash is not a disposition of a question. The check is a textual pre-scan for unclosed brackets outside strings, run before the reader, and it is deliberately one-sided — it may report nesting that is not there, which renders no form, and never reports less nesting than is there.
- **`kMaxFields = 16` fields per form.** A user cannot meaningfully fill in an unbounded pile, and each field is a row of the dialog. The dialog names how many of how many the Upstream asked for.
- **`kMaxLabelColumns = 80` and `kMaxDescriptionColumns = 240`** of schema-declared text per label and per description, and **`kMaxEnumValues = 16`** choices per field. A field named with a megabyte of text is still one field; truncating the label keeps the row a row. The bound is in columns because columns are what the composed render path enforces.

Two more things are bounded by not being this build's to raise. The **field count the answer carries** is the field count the schema declared, minus the fields past `kMaxFields`, and the notice says so. The **value** the user types is not bounded: it is the user's own text, not a server's document, and the answer it becomes is bounded by the request-size machinery it already goes out through.

### Regression properties (tests)

- `a schema this build will not read is reported on screen and answers nothing` — `[coding_agent][tui][mcp][issue846][spec]`: a schema that is not JSON, is not an object, is nested past the depth bound, is larger than the size bound, and declares more fields than a dialog can carry. Each is reported on screen, and the form still has three answers.
- `the form dialog renders within the width it is handed` — `[coding_agent][tui][mcp][issue846][spec]`: a schema with a 200-column title, a 600-column description, a 300-column field label, a 400-column field description, sixteen 70-column enum choices, and a 300-character value, swept across every narrow overlay width.
- `invalid input is rejected in the dialog and never sent` and `every schema constraint is enforced before an answer is produced` — `[coding_agent][tui][mcp][issue846][spec]`: `minLength`, `maxLength`, `minimum`, `maximum`, `integer` wholeness, `boolean` spelling, `enum` membership, and `required`, each producing an inline error and no answer.
- `form values are typed on the way out` — `[coding_agent][tui][mcp][issue846][spec]`: a field the schema declared `integer` leaves as a JSON number and not as the quoted text the user typed, and an optional field left empty is absent from the answer rather than an empty string.

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

## MCP Host cancellation and progress bounds (ADR 0020, ADR 0052, ADR 0061, spec #833 stories 30 and 31, issue #844)

Cancelling a prompt and the progress an Upstream reports while a call runs are bounded by what already exists plus one new display bound. Neither introduces a second cancellation mechanism: the run's existing `std::stop_token` is what a `tools/call` carries, and the client stack adds no stop vocabulary of its own.

- **No new time bound.** `notifications/cancelled` is a notification, so it has no response to be late for; it is written with the same 30 s per-call deadline and its outcome is discarded. A cancelled call settles when the transport answers the stop, which for a conforming transport is immediate, and the 1 s `kConnectionCleanupBound` remains the only backstop for one that ignores cancellation.
- **No new bytes bound on the wire.** A progress message rides the response body of the exchange it belongs to, so it is bounded by `kMaxResponseBytes` (8 MiB) with the rest of it and by `kMaxDiagnosticBytes` (1 KiB) once it is extracted, redacted before it is truncated.
- **`kMaxProgressLines = 16` retained progress lines** in `src/coding_agent/runtime/McpToolBinding.cpp`, with **`kMaxProgressMessageBytes = 200`** per line. The tool-execution display re-issues a *cumulative* partial result, so what it publishes is the retained lines rather than only the newest; at the bound the **oldest** line is dropped, because the newest is the one the user is watching. Sixteen lines is far above what a human reads during one upstream operation, and the cap is what keeps a flooding Upstream from making one tool block grow without limit.
- **A call made without a progress sink declares no `progressToken`**, so an Upstream's progress for it is dropped with every other unrecognized notification. Progress costs nothing for a display that asked for none.

The progress is **display-only**: a partial tool result is a projection fact (ADR 0052) and reaches neither the model's context nor the transcript, so a long upstream operation shows movement at no context cost. There is no fairness property to measure: the work is bounded in-memory value work over an already-admitted call, not a queue.

### Regression properties (tests)

- `a cancelled call tells the Upstream to stop the work the closed stream abandoned` — `[mcp][cancellation][issue844][spec]`: the stop reaches the request, the call settles `Cancelled`, and exactly one `notifications/cancelled` naming that call's own request id reaches the wire.
- `a late answer to a cancelled call never finishes the call a second time` — `[mcp][cancellation][issue844][spec]`: the Upstream's answer arriving after the stop leaves the settled `Cancelled` outcome untouched, keeps the completion count at one, and still issues no second cancellation; the next call on the same client succeeds.
- `a connection close during an in-flight call leaves no suspended upstream call` — `[mcp][cancellation][issue844][spec]`: the close reaches quiescence with `within_bound` and zero abandoned operations without needing the 1 s bound, the call settles `Cancelled`, and the Upstream is told once.
- `progress an Upstream reports for a call reaches that call's own sink` — `[mcp][progress][issue844][spec]`: two notifications echoed against the call's declared token arrive in order with their counters, totals, and messages, while the result the model sees is still the Upstream's own content.
- `a call made without a progress sink declares no token` — `[mcp][progress][issue844][spec]`: the framed request carries no `progressToken`, so an Upstream's notifications have nothing to match against.
- `a progress notification naming another call is dropped rather than shown against the running one` — `[mcp][progress][issue844][spec]`: a notification for a finished call and one for a call that never existed both reach neither sink, and the call still succeeds.
- `a progress notification is bounded and redacted before it reaches a display` — `[mcp][progress][issue844][spec]`: a 4 KiB message with a secret-shaped key at its tail comes back inside 1 KiB with the value erased, which is the redaction-before-truncation order made observable.
- `a cancelled prompt stops the upstream call and settles as one failed tool call` — `[mcp][cancellation][issue844][spec]`: end to end through the production session door, a cancelled prompt leaves one `notifications/cancelled` and one `tools/call` on the wire, and the session is open and usable for the next prompt.
- `progress an upstream call reports reaches the tool execution display and not the model` — `[mcp][cancellation][issue844][spec]` sibling `[mcp][progress][issue844][spec]`: the projection stream carries both cumulative partials with the progress text, and no tool result the model read contains any of it.

## MCP Host catalog-cache bounds (ADR 0064, spec #833 story 17, issue #848)

The tool-catalog cache is the one MCP Host resource that outlives a session: an Upstream's own `ttlMs`/`cacheScope` hints let one connection's `tools/list` walk answer the next one. It is therefore bounded in entries and in how long an Upstream's own hint can keep an entry alive. The values live in the same private constants point `src/mcp/Protocol.hpp` and are not caller-tunable: a bound the Upstream or the configuration could raise would not be a bound.

- **`kMaxCachedCatalogs = 64` entries.** At the bound the oldest-held entry is dropped, not the newest one refused. 64 is roughly an order of magnitude above any `mcpServers` configuration a user writes, and the eviction direction matters: refusing the newest entry would silently disable caching for a server the user still has, which is a worse failure than forgetting an old one. Admitting a Server Id that already has an entry replaces it rather than growing the cache.
- **`kMaxCatalogFreshness = 24 h`.** The longest an Upstream's `ttlMs` hint can keep an entry alive; a longer hint is clamped to it. A server that declares an effectively infinite freshness would otherwise pin a descriptor set for the life of the process, which hands the Upstream the decision of when the host may forget something. The hint is clamped *before* the integral conversion, so a value large enough to overflow the type is bounded rather than undefined.

One more rule is a bound on work rather than on memory, and is exact rather than approximate: **at most one background catalog refresh runs per Upstream at a time.** A stale entry is served immediately and one refresh is admitted behind it; a second listing while that refresh is in flight serves the same entry and starts nothing. Without the rule, an Upstream declaring `ttlMs: 0` would turn every listing into a second `tools/list` walk.

The cache is a shortcut and never a source of failure, so a miss degrades to the same `tools/list` walk that ran before the cache existed, and a catalog the cache refuses to admit is still returned to the caller from the walk that produced it.

### Cross-session reuse: the production owner is still undecided (owed an ADR)

`McpSessionHostOptions::catalog_cache` is an injection point, and **production does not fill it**: `SessionFactory` builds no cache, so a production session hands every connection a null `catalog_cache` and every connection walks `tools/list` exactly as it did before the cache existed. The cross-session reuse the cache was built for is therefore *not* delivered in production yet. What is shipped is the value, the decode, the cache, the connection path, and the session wiring that accepts and shares one instance; only the composition-root choice is open, and it is open on purpose:

- A process-scope mutable owner (a `static` in `SessionFactory`, or any other process-global in `cch_coding_agent`) would be this repository's **first** process-global mutable state, which is a new architecture pattern and not a code-level inference (ADR 0053, CODING_STANDARDS §13.3).
- The existing host-owned pattern for a long-lived shared service is the right *shape* — one `ModelRuntime` is built by the interactive host and handed to the boot Session and every in-session replacement, so closing one Session never releases what the next one needs (ADR 0029/0030). A host-owned `UpstreamCatalogCache` threaded the same way would introduce no new pattern.
- What that shape does **not** settle is the cache's own `scope`. A `session`-scoped instance shared across Sessions would serve an Upstream's `session`-scoped catalog to a session that did not fetch it, which is exactly the contamination `cacheScope` exists to prevent; sharing across sessions therefore *requires* a `process`-scoped instance, i.e. the repository promising to an Upstream that the untrusted descriptors it advertises — names, descriptions, JSON Schemas, `x-mcp-header` annotations — stay live for a host-lifetime object and will be published into a later session's model context. That is a security-relevant lifetime policy, and choosing it inside a tool-publication change would be choosing it silently.

**So the decision is recorded here as owed an ADR** rather than answered in code: which long-lived object owns a process-scoped cache, with what scope, in which frontends, and whether a cached catalog may be published into a later session at all. Until it is answered, the safe behaviour is the one in the tree: every session walks its own catalog, and `catalog_cache` stays caller-injected. A caller that wants cross-session reuse hands the host the cache of the session that ran before.

### Regression properties (tests)

- `a warm cache answers a second listing without asking the Upstream again` — `[mcp][catalog][cache][issue848]`: the boundary of `kMaxCachedCatalogs` is nowhere near here, but the entry count is asserted alongside the request count so a cache that stopped holding anything could not pass.
- `the cache is bounded in entries and drops the oldest one at the bound` — `[mcp][catalog][cache][limits][issue848]`: 64 entries admitted, the 65th admitted, `server-0` dropped and `server-64` held — the eviction direction, not just the count.
- `a freshness hint longer than the host admits is clamped rather than refused` — `[mcp][catalog][cache][limits][issue848]`: a `ttlMs` of 1e12 arrives on the wire and comes back as exactly 24 h, with the catalog still usable.
- `an Upstream that expires its catalog on every hint costs one refresh at a time` — `[mcp][catalog][cache][refresh][issue848]`: the `ttlMs: 0` case above. Ten listings against a refresh that never lands cost exactly two walks.
- `a refresh re-applies the defensive catalog limits and leaves the cached entry alone` — `[mcp][catalog][cache][limits][issue848]`: a refresh is a real walk, so the 5,000-tool cap fails it exactly as it fails a cold fetch, and the entry the caller already holds survives the failure.
- `a second session sharing the catalog cache asks the Upstream for nothing` and `a session with no shared cache still asks the Upstream for its catalog` — `[mcp][catalog][cache][issue848]`: the cross-session sharing mechanism is proven at the injection point, with the null-cache control that stops the first case from passing for the wrong reason. Neither asserts a production owner, because production has none (see above).

## MCP Host authorization bounds (ADR 0032, ADR 0064, ADR 0065, spec #833 stories 34 and 35, issue #849)

A browser authorization is the one MCP Host operation the user drives, and the only one that opens a socket the Upstream did not ask for. Its costs are bounded the same way: the values are constants beside the code that applies them, and no configuration can raise them.

- **`kExchangeTimeout = 30 s` per OAuth exchange.** Discovery, dynamic client registration, and the code exchange each run under the transport's own per-request deadline, and the deadline is containment rather than tuning: an authorization server that accepts a connection and never answers fails the flow instead of holding it open. The bound is the same 30 s the Streamable HTTP transport already applies to every MCP exchange, so the authorization is not a slower path through the same transport.
- **One loopback listener per authorization, on an ephemeral port.** The listener binds `127.0.0.1:0` and exists only for the life of one flow: closing the listener joins its thread and closes its socket, and nothing about it outlives the flow. An ephemeral port rather than a fixed one is what keeps two authorizations — in one process or in two — from colliding, and the plain-HTTP listener on the loopback interface is the redirect target, not a client transport: the package's client transports remain TLS-only (ADR 0054).
- **One request head per callback, read to the first blank line.** The listener reads a browser's redirect GET and answers it; it is not a general HTTP server, and it serves exactly one route — anything else is answered `404` and never reaches the flow.
- **One credential per Server Id, keyed by issuer.** A Server Id's OAuth record lives under the same `mcp.<server-id>` key as its bearer, with the issuer as a member of the record, so a credential issued by one authorization server is never presented to another. A lookup that does not match the requested issuer reads as *no credential* rather than as a usable token.
- **A cancelled authorization persists nothing.** Dismissal, a failed exchange, a wrong `iss`, and a listener that could not bind all end the flow with nothing stored, and the connection stays `needs_auth`.

A non-interactive session cannot reach any of this: with no prompt port installed, the flow fails before discovery, so a headless run never contacts an authorization server and then stalls on a browser nobody is watching.

### Regression properties (tests)

- `the full chain discovers, registers, redirects, exchanges, and stores` and `a client this Server Id already registered with is reused rather than registered again` — `[mcp][oauth][issue849]`: the whole flow above the one transport seam, with the reuse case proving a second authorization does not register a second client.
- `an authorization response from another issuer is refused and nothing is persisted`, `an authorization response with no issuer is refused`, and `a response that does not echo the state is refused before the code is exchanged` — `[mcp][oauth][issue849]`: the three ways a response this client did not ask for is refused, each asserting the token endpoint was never reached and the store stayed empty.
- `a session with no prompt port fails closed and contacts nothing` — `[coding_agent][mcp][auth][issue849]`: the non-interactive case, asserted on the transport's own request count.
- `a store failure cannot echo the issued token into the flow's error` and `a credential issued by another issuer is not a credential for this one` — `[mcp][oauth][issue849]` and `[coding_agent][credentials][mcp][auth][issue849]`: the redaction and the issuer keying, at the store rather than only in the flow.
- `the loopback listener answers the redirect and hands the flow its query`, `the loopback listener refuses a route it does not serve`, and `a cancelled wait settles rather than blocking the flow` — `[mcp][oauth][issue849]`: the listener over a real loopback socket, including the route it does not serve and the cancellation that must not leave the flow waiting.

## Update procedure

A limit changes only through the same evidence path: record the representative workload and environment, repeated samples and variance, the selection rule, the chosen value, and the regression property that protects it. When a limit changes, update this table, the `harness::RuntimeLimits` defaults, and — if the production value is no longer the default — the explicit set in `src/coding_agent/cli/AsyncCliRuntime.cpp`. The regression tests above must pass at the new values before the change is accepted.
