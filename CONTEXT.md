# C++ Coding Agent Harness

This context names the concepts used to describe the harness as a product. It is a glossary, not an implementation or interface specification.

## Language

**Supported Capability**:
A pi capability that this harness explicitly claims to provide and for which it accepts a Semantic Parity obligation.
_Avoid_: Implemented feature, partial placeholder

**Deferred Capability**:
A pi capability that this harness does not currently claim to provide; its absence is not contract drift and creates no placeholder contract.
_Avoid_: Missing contract, unsupported stub

**Native TUI**:
The harness-owned interactive terminal product surface, combining reusable terminal UI capabilities with coding-agent-specific interaction. A rich client connected only through JSON/RPC or the SDK is not the Native TUI.
_Avoid_: Text REPL, RPC frontend

**Semantic Parity**:
For a Supported Capability, preservation of pi's externally observable meanings and state transitions while allowing an idiomatic C++ API shape.
_Avoid_: API-shape parity, mechanical translation

**Parity Baseline**:
The recorded pi revision against which Supported Capabilities, intentional differences, fixtures, and drift are evaluated until an explicit baseline advance.
_Avoid_: Latest pi, upstream HEAD

**Intentional Divergence**:
A documented departure from Semantic Parity that demonstrably improves the C++ caller contract and cannot be hidden behind a private adapter without losing that benefit.
_Avoid_: Implementation shortcut, accidental drift

**Provider Message**:
A model-facing conversation value directly accepted or produced at a chat-provider boundary.
_Avoid_: Agent Message, Session Entry

**Agent Message**:
A conversation value retained and processed by the agent; product-specific forms are converted to Provider Messages before a model request.
_Avoid_: Provider Message, Session Entry

**Model**:
A passive, credential-free value that completely names one model — its provider, id, and API identities, capabilities, limits, and cost. Every model request carries a concrete Model; a missing selection means the pi-aligned "unknown" placeholder Model, never an absent one.
_Avoid_: Model name string, client configuration, optional model

**Provider**:
The long-lived runtime capability that owns one provider identity's model catalog, authentication, and stream execution, delegating wire protocol work to the API the requested Model names.
_Avoid_: Provider factory, per-request client, hard-coded provider list

**Models Runtime**:
The canonical model/authentication runtime for one Agent Config Directory: it composes built-in and configured providers, resolves live authentication, and delegates model requests to the owning Provider. It is shared across Agent Sessions and refreshed as a whole rather than reconstructed per session.
_Avoid_: Provider registry, session-scoped client, configuration snapshot

**Credential**:
A stored per-provider authentication value in api-key or OAuth shape. Secrets inside a Credential never enter passive values, session history, frontend status, diagnostics, or logs.
_Avoid_: Provider config field, session metadata

**Request Authentication**:
The resolution of a provider's effective credential immediately before each model request — explicit key, then stored Credential, then ambient environment only when nothing is stored — including OAuth refresh. Failures surface through the request's normal error outcome and never silently fall back to a lower-precedence source.
_Avoid_: Login-time snapshot, startup authentication

**Execution Environment**:
The complete workspace file-system and shell capability bundle made available to an Agent Session; unavailable operations are excluded by assembly policy rather than hidden behind placeholder methods.
_Avoid_: Tool adapter, partial capability

**Tool Argument Contract**:
The JSON Schema value that defines and validates one tool's accepted arguments before policy hooks and execution.
_Avoid_: Parameter hint, provider DTO

**Tool Call Outcome**:
The final per-call result produced after argument validation, policy hooks, and execution; its failure is isolated from unrelated calls and the Agent Session unless termination is explicitly requested.
_Avoid_: Agent failure, intermediate tool response

**Agent**:
The live stateful capability that owns model-driven conversation execution, including its messages, model, tools, queues, and active-run state.
_Avoid_: Agent Session, agent loop

**Agent Session**:
One ongoing coding-agent conversation, including its current interaction state and durable history.
_Avoid_: Conversation handle, runtime session

**Agent Turn**:
One ordered agent lifecycle step from a model request through its assistant and tool outcomes to the next-turn decision point.
_Avoid_: Provider request, prompt

**Agent Stream Flow**:
The Agent's consumption of one model stream through the provider's `streamSimple` surface: the per-turn request-option set (reasoning, sessionId, cacheRetention, timeoutMs, maxRetries, maxRetryDelayMs, headers, signal), the terminal-error-event contract (exactly one error or aborted terminal event plus a final assistant message carrying stopReason and errorMessage), and the six-category error channel carried through the single Expected outcome.
_Avoid_: Provider request, per-adapter option struct, second exception hierarchy

**Thinking Level**:
The model-facing reasoning preference with pi's seven levels (off, minimal, low, medium, high, xhigh, max), defaulting to medium and clamped to the active model's supported set at session creation and on model switch; per turn it becomes the stream's reasoning option, with off meaning no reasoning is requested.
_Avoid_: Free-form effort string, provider-specific knob

**Live Session State**:
The current in-process view of an Agent Session, which may be newer than its durable history.
_Avoid_: Persisted state, session file

**Session Entry**:
One durable record in an Agent Session history.
_Avoid_: Wire payload, JSON line

**pi v3 Session Format**:
The interoperable wire format for supported Session Entry alternatives, including pi's field, null, ordering, and active-path meanings.
_Avoid_: pi-style JSONL, C++ session schema

**Session Event Commitment**:
The ordered policy that commits one agent lifecycle event to an Agent Session by advancing Live Session State, notifying weak subscribers, and appending Session Entries. Subscriber failures are diagnostic observations and do not veto commitment.
_Avoid_: Event handler, callback chain

**Session Resume**:
Reopening an Agent Session from its durable history at the selected active point.
_Avoid_: Reload, replay

**Session Publication**:
The single mutation point that makes an assembled Agent Session's storage real — creating required directories under their privacy policy and writing the session header after all fallible prerequisites have succeeded.
_Avoid_: Save, file write, flush

**Session Close**:
An idempotent request that rejects new work and releases Agent Session resources only after active lifecycle callbacks and operations have safely quiesced.
_Avoid_: Immediate teardown, abort

**Session Abort**:
An idempotent request to cancel the active prompt and produce its normal aborted lifecycle while leaving the Agent Session available for later work.
_Avoid_: Session Close, process kill

**Session Topology**:
The shape of an Agent Session history, such as linear, branched, or compacted.
_Avoid_: Completion state

**Compaction**:
The context-summarization capability that replaces compacted session history with a summary entry while retaining a recent tail, triggered on context overflow (compact and retry once), on threshold, or manually; summarization requests are isolated with cache retention "none" and a fresh session id.
_Avoid_: Truncation, deletion, raw history replay

**Project Resource**:
A project-associated skill or prompt template that may be made available to an Agent Session after policy checks.
_Avoid_: Runtime service, project file

**Project Trust**:
The user-controlled authorization decision governing whether project-authored resources may be loaded.
_Avoid_: Workspace configuration, project self-approval

**Agent Config Directory**:
The user-level root for durable harness state shared across workspaces; it is pi's own directory, so this harness and a pi installation interoperably share one credential and model-configuration store.
_Avoid_: Config home, user profile directory, harness-private state root

**User Settings**:
User-level preferences following pi's two-scope `settings.json` contract: a global file in the Agent Config Directory deep-merged with a project file that loads only under Project Trust. Settings never carry secrets or secret references; model selection defaults use pi's `defaultProvider`/`defaultModel` vocabulary.
_Avoid_: User config, config file, credential storage

**Settings Scope**:
One of the two `settings.json` layers — global or project — with project winning on deep merge and Project Trust gating project-scope reads and writes.
_Avoid_: Profile, level

**Runtime API Key Override**:
A process-lifetime API key supplied through `ModelRuntime::set_runtime_api_key` or CLI `--api-key`, never persisted, taking the highest precedence in Request Authentication.
_Avoid_: Saved key, default key

**Configured API Key**:
A `models.json` provider `apiKey` value — literal, `$VAR`/`${VAR}` environment template, or `!command` shell execution — resolving as the lowest Request Authentication precedence for config-only providers.
_Avoid_: Stored credential, hardcoded key

**Resume Model Resolution**:
Re-resolving a resumed session's persisted `model_change {provider, modelId}` against the live Models Runtime catalog, with base URL and authentication drawn from current composition rather than any session snapshot.
_Avoid_: Session restore, auth snapshot

**Auth Interaction**:
The host-supplied, cancellable interaction object through which an OAuth login flow exchanges prompts and events. The ai layer owns the content — what is asked and what is shown — while the host owns the presentation and cancellation, so provider implementations never render product UI and hosts never hardcode provider interaction policy.
_Avoid_: Login callback, rendered dialog, provider UI

**OAuth Login**:
An explicit, user-invoked login flow that produces a Credential through provider-owned steps (browser callback or device code) and persists it via `CredentialStore::modify`. Never triggered by Session creation or ordinary requests.
_Avoid_: Auto-login, implicit authentication, login snapshot

**Login Cancellation**:
A user-initiated abort of an in-flight OAuth Login through the Auth Interaction's stop source, normalizing to a stable cancelled error (message "Login cancelled") so the frontend suppresses failure UI.
_Avoid_: Failed login, error dialog

**Credential Refresh**:
Request-time renewal of a stored OAuth Credential whose expiry is within five minutes, serialized under the store lock with the rotated credential persisted before use; not cancellable in the request path, matching pi. Failure preserves the stored credential for retry and never falls back to a lower-precedence source.
_Avoid_: Background refresh, proactive expiry notification

**Re-auth Guidance**:
The two user-facing guidance outcomes produced when a request has no usable credential: the no-key message directing to login for the provider, and the expired-OAuth message directing to re-run login. Both surface at prompt preflight and at request time and are never silently skipped.
_Avoid_: Generic auth failure, silent credential fallback

**Auto-Retry**:
The session policy that re-enters the agent loop with exponential backoff after a retryable terminal error (transient provider and network patterns), excluding quota/billing and context-overflow errors; the failed assistant message is removed from live state but retained in session history, and the backoff wait is cancellable.
_Avoid_: Infinite retry, silent retry, adapter-level retry

**OAuth Callback Server**:
The local loopback HTTP server used by the Codex browser login flow on `127.0.0.1:1455` (`PI_OAUTH_CALLBACK_HOST`) to receive the authorization-code redirect, raced against manual code entry.
_Avoid_: Webhook, remote endpoint

**Adapter**:
The private protocol executor that converts a Provider request into one wire API's format and consumes its stream back into the shared event model — one per supported API surface (`openai-codex-responses`, `openai-responses`, `anthropic-messages`). An adapter is selected by the Model's `api`, is never publicly registrable, and its existence alone never makes a Provider supported.
_Avoid_: API client, generic OpenAI client, public registry

**Compat Field**:
A per-API typed compatibility value on the Model that carries the behavior-bearing switches a built-in catalog model populates for the adapter (`forceAdaptiveThinking`, `allowEmptySignature`). Only the scoped APIs' typed shapes exist; a generic JSON compatibility bag is never carried, and models.json carries no compat surface.
_Avoid_: Capability flag bag, models.json compat override

**Transport**:
The channel over which a provider's wire API delivers streamed events — WebSocket-first with narrow SSE fallback for Codex, SSE for the other scoped paths. Transport choice is fixed per adapter at pi's frozen defaults; it is never a per-request caller option in the C++ surface.
_Avoid_: Request option, transport override

**Session Affinity**:
The protocol feature that lets a provider correlate requests from one session — Codex via `previous_response_id` continuation and session-keyed socket reuse, DeepSeek via silently-ignored affinity headers. Where the provider is stateless or the continuation key is absent, full-context stateless replay is used instead.
_Avoid_: Session metadata, resume state

**User Bash**:
A Native TUI operation that runs a user-entered shell command without treating it as an Agent Prompt. Its completed execution belongs to Agent Session history and is either included in or excluded from later model context according to the user's invocation.
_Avoid_: Bash tool, prompt processing

**Interrupt Admission**:
The Native TUI decision of how an interrupt request applies to the current activity: which channel it targets (the active Agent run, the running User Bash, or a pending User Bash submission) and whether the request is stale relative to the active prompt generation. The decision is made against activity facts supplied at request time; the owning frontend performs the routed effect. It is the C++ translation of pi's app.interrupt precedence at the parity baseline.
_Avoid_: Escape handling, keybinding dispatch

**TUI Toolkit**:
The reusable terminal UI capability module (`cch_tui`) that owns the terminal seam, input protocol decoding, keybindings, the editor stack, autocomplete, fuzzy matching, layout-free components, markdown rendering, and terminal image capability, depending on no coding-agent types. The Native TUI's interactive mode is assembled from it; interactive-mode application components (model selection, login presentation, footer/status, chat UX) are not toolkit capabilities.
_Avoid_: UI library, widget set, interactive frontend

**Decoded Input Event**:
The TUI Toolkit's single decode of raw terminal escape sequences into typed KeyEvent/PasteEvent values at the terminal edge, with action matching against pi's `modifier+key` identifier grammar and full keyboard-protocol coverage (legacy, modifyOtherKeys, Kitty CSI-u with event types and alternate keys). An Intentional Divergence from pi's raw-string matching that preserves the observable sequence-to-action contract; paste input remains size-bounded.
_Avoid_: Raw string matching, per-consumer escape parsing

**Resolved Keybinding Registry**:
The immutable, resolution-time keybinding table consumed by dispatch, help, and hints alike: the 30 assembled `tui.*` actions (21 `tui.editor.*`, 3 `tui.input.*`, 6 `tui.select.*`) with pi's default keys at the parity baseline, one startup `keybindings.json` read with replace-all-defaults and empty-array unbind, and bounded redacted diagnostics. `tui.input.copy` and the six `tui.altScreen.*` ids are recognized-but-unassembled: diagnosed as known-but-unbound, never no-op bindings, because their only active behavior is alt-screen selection copy.
_Avoid_: Mutable global manager, no-op bindings, per-consumer key tables

**Terminal-Owned Image Placement**:
The TUI Toolkit's inline-image model in which components emit protocol-neutral image regions alongside their rendered lines and the Terminal owns physical placement and removal in absolute cell regions, including protocol selection (Kitty/iTerm2), cell-size math, animation-id reuse, and fallback text. An Intentional Divergence from pi's escape-sequences-in-lines with identical placement, sizing, and fallback outcomes.
_Avoid_: Escape sequences in render lines, component-owned protocol bytes
