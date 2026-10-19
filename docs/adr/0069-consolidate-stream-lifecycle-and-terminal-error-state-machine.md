---
status: accepted
---

> **#1042 amendment (one terminal error normalization path).** All terminal
> errors enter `StreamEmitter::finalize_error_event(AssistantErrorEvent)`; the
> convenience paths for failures, cancellation, returned error messages, and
> error-marked done events adapt their inputs to that value. Any typed
> cancellation signal (`reason`, message stop reason, `ErrorCode`, or
> `InferenceFailureKind`) wins conflicting error signals. The normalized event
> keeps the message (inheriting the partial content and stream identity the
> terminal leaves unset) and structured failure details, applies the canonical
> diagnostic once, and agrees on `Aborted`/`Cancelled` across its message,
> event, and inference classification. TUI consumers use the message's typed
> default-abort query and own their presentation wording. A sink rejecting the
> terminal error remains an infrastructure failure: propagate it directly and
> never retry or emit another terminal event. Implements [#1042](https://github.com/lanshengzhi/cpp-coding-harness/issues/1042).

# Consolidate Stream Lifecycle and Terminal Error State Machine in cch_ai

Following [ADR 0068](0068-retire-cloud-provider-adapters-and-delegate-routing-to-magpie.md)'s
consolidation of model execution onto a single OpenAI Chat Completions SSE wire path via Magpie,
this decision unifies the stream lifecycle execution engine and terminal error handling inside
`cch_ai`. It eliminates the outer `forwarding_sink` interception shim, redundant event caching,
and intermediate hook closures, concentrating stream state transitions, retry eligibility,
error redaction, and terminal outcome delivery into a single private deep state machine.

## Context and motivation

Prior to ADR 0068, Pike maintained multi-provider routing across Anthropic, OpenAI, and Codex
transports. To support heterogeneous protocol adapters, stream event handling was split across
layers:
1. `ModelsStream.cpp` wrapped caller-provided `AssistantEventSink`s in an outer `forwarding_sink`
   that intercepted `AssistantDoneEvent` and `AssistantErrorEvent`, sanitized terminal error text,
   cached `terminal_message`, and conditionally re-emitted missing terminal events.
2. `StreamExecutionEngine.cpp` wrapped the sink in an inner `guarded_sink`, managed HTTP chunk
   ingestion, emitted `AssistantStartEvent`, tracked exponential backoff retries, and derived
   `InferenceFailure`s before emitting its own `AssistantDoneEvent` or `AssistantErrorEvent`.
3. Intermediate hook closures (`SseAttemptHook`, `SseFinalizeHook`, `AttemptState`) bridged
   `OpenAICompletionsAdapter` and `StreamExecutionEngine`.

This layered division caused architectural friction:
- **Double bookkeeping and state leakage**: Outer layers intercepted inner terminal events to
  prevent duplicate emission, while maintaining independent fallback emission paths.
- **Scattered error redaction and derivation**: Public diagnostic formatting, error length
  bounding (`kMaxPublicErrorBytes`), and `InferenceFailure` kind derivation were duplicated across
  `ModelsStream.cpp` and `StreamExecutionEngine.cpp`.
- **Fractured test surface**: Tests targeted intermediate hook lambdas rather than the authoritative
  `Models::stream` interface.

## Decisions

1. **Eliminate outer event interception (`forwarding_sink`)**:
   - `cch::ai::Models::stream` directly forwards the caller's `AssistantEventSink` into the private
     stream execution engine.
   - The outer shim, intermediate `terminal_message` cache, and post-stream deduplication checks are
     removed.

2. **Unify stream lifecycle into a cohesive execution session**:
   - Internal hook closures (`SseAttemptHook`, `SseFinalizeHook`, `AttemptState`) are folded into a
     single private `StreamExecutionSession`.
   - The session owns SSE decoding, OpenAI Chat Completions JSON delta parsing, and event emission
     without allocating intermediate callback objects per attempt.

3. **Establish a single terminal exit point (`finalize_outcome`)**:
   - All network failures, non-2xx HTTP responses, SSE protocol failures, and cancellation signals
     converge at a single terminal exit point.
   - Error categorization, `kMaxPublicErrorBytes` length bounding, sensitive detail redaction,
     `InferenceFailure` classification, and `suggested_backoff_ms` calculation occur strictly at this
     point.
   - Exactly one terminal event (`AssistantDoneEvent` or `AssistantErrorEvent`) is delivered to the
     caller sink, paired with exactly one terminal `AssistantMessage`.

4. **Enforce the retry state invariant**:
   - Retries with exponential backoff are permitted strictly when `output_started == false`.
   - The emission of any text delta, thinking delta, or tool call irreversibly transitions
     `output_started` to `true`, permanently barring retry to prevent token duplication downstream.

5. **Strict orthogonal isolation of caller sink failures**:
   - A sink error (`sink(event)` returning `unexpected`) immediately halts network transport streaming.
   - Sink failures are never classified as provider errors, never trigger retry, and never result in
     error emissions back to the failing sink. The sink error propagates directly as the coroutine's
     `unexpected` return value.

6. **Uniform cancellation normalization**:
   - Cancellation requests via `std::stop_token` at any lifecycle stage (pre-request, in-flight, or
     during backoff sleep) uniformly produce `AssistantStopReason::Aborted` with error message
     `"Request was aborted"`, emitting an `AssistantErrorEvent` and returning the aborted
     `AssistantMessage`.

7. **Consolidate test surface at `Models::stream`**:
   - Intermediate hook unit tests are retired. Lifecycle events, terminal outcomes, retries, and
     sink error isolation are verified directly through `Models::stream` with injected test transports.

## Consequences

- **Locality**: Stream lifecycle states, retry invariants, and terminal error transformations are
  concentrated in one place instead of spanning three internal source files.
- **Leverage**: Callers and tests exercise a single cohesive interface. Eliminates 3 layers of
  closure allocation per request.
- **Contract guarantees**: Prevents duplicate terminal events or missing error redactions by construction.
