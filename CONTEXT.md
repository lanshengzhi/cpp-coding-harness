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
The harness-owned interactive terminal product surface, combining reusable terminal UI capabilities with coding-agent-specific interaction. A non-interactive programmatic frontend is not the Native TUI.
_Avoid_: Text REPL, RPC frontend

**Supported Platform**:
Linux on x86-64 with glibc, the only platform on which this harness accepts build, runtime, and Semantic Parity obligations for Supported Capabilities. Ubuntu 24.04 is the reproducible baseline and blocking CI environment; Arch Linux is a formally supported development environment validated from a pinned snapshot. Windows, macOS, Linux on other architectures, and musl-based systems carry no best-effort or placeholder product surface.
_Avoid_: Development host, portable source, rolling latest, best-effort platform

**Semantic Parity**:
For a Supported Capability, preservation of pi's externally observable meanings and state transitions while allowing an idiomatic C++ API shape.
_Avoid_: API-shape parity, mechanical translation

**Parity Baseline**:
The recorded pi revision against which Supported Capabilities, intentional differences, fixtures, and drift are evaluated until an explicit baseline advance.
_Avoid_: Latest pi, upstream HEAD

**Parity Ownership Map**:
The recorded mapping from C++ packages to the pi capability owners they represent at the Parity Baseline, preserving pi ownership and dependency direction without requiring package-shape identity.
_Avoid_: Package isomorphism, live upstream graph

**Parity Package Graph**:
The directed dependency graph among the pi package owners of Supported Capabilities at the Parity Baseline. C++ targets may split within an owner but never introduce a reverse owner dependency.
_Avoid_: Current CMake graph, package-shape parity

**Configured Package Graph**:
The per-configuration direct target dependencies, interface visibility, project-header inclusion, and source ownership produced by CMake and classified by owner and role. It is checked against the Parity Package Graph rather than inferred from CMake source formatting or a transitive build graph.
_Avoid_: CMakeLists layout, build-order graph

**Capability Owner Package**:
The authoritative C++ package presenting the supported interface for one pi package owner; private implementation targets and headers may deepen it without becoming cross-owner dependencies.
_Avoid_: Facade target, named module, source directory

**C++ Support Package**:
The pi-neutral package for shared C++ mechanics that owns no Supported Capability and depends on no Capability Owner Package. Its existence never adds another product owner.
_Avoid_: Product package, capability owner, general-purpose runtime

**Owner Interface**:
The repository-internal header contract of a Capability Owner Package, living under the Owner-local header root with one canonical `<cch/...>` spelling and containing only standard-library, support, and legal downstream Owner types. It is never installed, exported, or ABI-promised.
_Avoid_: Public SDK header, installed consumer surface, umbrella header

**Parity Architecture Manifest**:
The baseline-pinned machine-readable authority for the Parity Package Graph, Capability Owner Packages, role classification rules, and capability-evidence references. It defines the policy grammar rather than duplicating the configured target inventory; build validation and Parity Drift analysis consume this one policy source.
_Avoid_: CMake comment, target allowlist, duplicated policy

**Parity Architecture Gate**:
The required fail-closed validation that every configured production target, source, dependency, interface exposure, and project-header inclusion conforms to the Parity Architecture Manifest in every supported configuration on the Supported Platform. It remains mandatory independently of optional test builds.
_Avoid_: Advisory architecture test, source-format style check, optional CI job

**Parity Drift**:
An observed difference between the Parity Baseline and another pi revision; it is advisory until an explicit baseline advance accepts or classifies it.
_Avoid_: Contract failure, latest-pi obligation

**AsyncResult**:
The one lazy, move-only, single-consumption asynchronous operation contract (`cch::support::AsyncResult<T, E>`) returned by fallible asynchronous Owner operations: consumed exactly once by callback start or move-only `co_await`, with ordinary failure carried as `std::expected<T, E>` and cancellation supplied explicitly as `std::stop_token`.
_Avoid_: Boost.Asio awaitable in an Owner Interface, exposed executor or scheduler, copyable operation

**Runtime Root**:
The one concrete coding-agent-owned Runtime object per CLI invocation. It owns the private event loop, the bounded worker pool, and the bounded FIFO mailboxes of the state-owning runtime objects, and it survives Agent Session replacement until final application Close.
_Avoid_: Singleton scheduler, executor hierarchy, event bus, one thread or loop per Owner Package

**Serialized Execution Domain**:
The rule that a state-owning runtime object (Agent, Models Runtime, Agent Session) changes its state only on its own serialized path: cross-Owner events and worker results enter through the object's bounded FIFO mailbox, and workers never mutate that state directly.
_Avoid_: Broad locking, direct cross-thread mutation, unbounded queue

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

**System Prompt**:
The model-facing instruction text constructed for an Agent Session in pi's shape: the default or custom opening, the available-tools list with one-line snippets, guidelines from the tool contract, the Project Context File section, the skills section, and the current working directory line. It is built at session construction and rebuilt on Resource Reload.
_Avoid_: Hard-coded prompt, provider request text

**Project Context File**:
An AGENTS.md or CLAUDE.md (with case variants) discovered in the Agent Config Directory or in the working directory's ancestor chain and rendered into the System Prompt's project-context section; discovery is not gated by Project Trust.
_Avoid_: Repo instructions file, project readme

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

**Session Assembly**:
The one authoritative act that turns an Agent Session creation request into an assembled Agent Session — request validation, Provider/Model resolution against the live Models Runtime, resource loading, and Session Publication — consumed by CLI boot, in-session session replacement, and tests through one boundary.
_Avoid_: session factory, creation wrapper

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

**Session Selection**:
The user-facing flow of choosing which Agent Session to open — at boot through the resume picker or a session-family flag, or in-session through the session selector.
_Avoid_: Session picker, resume chooser

**Session Fork**:
A new Agent Session created from an existing session's history at a chosen point, carrying a parent-session pointer and the target working directory.
_Avoid_: Session clone, history copy

**Session Tree Navigation**:
The in-session flow of moving the active point through a session's tree topology: the tree overlay (opened by pi's default double-Escape trigger) lists the session tree with filters and label editing, and switching the active path follows the leaf/active-path semantics of the pi v3 Session Format. Branch summarization generation is not part of it.
_Avoid_: Tree view, fork picker, history browser

**Compaction**:
The context-summarization capability that replaces compacted session history with a summary entry while retaining a recent tail, triggered on context overflow (compact and retry once), on threshold, or manually; summarization requests are isolated with cache retention "none" and a fresh session id.
_Avoid_: Truncation, deletion, raw history replay

**Project Resource**:
A project-associated skill or prompt template that may be made available to an Agent Session after policy checks.
_Avoid_: Runtime service, project file

**Skill**:
A user- or project-authored instruction set discovered as a SKILL.md file (frontmatter name, description, and disable-model-invocation), listed to the model in the System Prompt's skills section and invocable as a `/skill:name` command while the Skill Commands setting is enabled; the file body is read at invocation time.
_Avoid_: Plugin, add-on

**Prompt Template**:
A user- or project-authored markdown file whose content substitutes for a matching `/name` slash invocation, with bash-style argument parsing and positional, `$@`, and default-value substitution.
_Avoid_: Macro, canned prompt, system prompt

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

**Print Mode**:
The one-shot text frontend selected by `--print`/`-p`, a non-TTY stream, or `--mode text`: it subscribes to nothing and prints only the final assistant message's text content blocks, reports terminal error/aborted outcomes to stderr with exit 1, and handles SIGTERM/SIGHUP with pi's dispose-and-exit semantics. Piped stdin, `@file` text, and sequential positionals merge into the initial prompt in pi's order.
_Avoid_: Event stream, JSON event print, one-shot slash dispatch

**User Bash**:
A Native TUI operation that runs a user-entered shell command without treating it as an Agent Prompt. Its completed execution belongs to Agent Session history and is either included in or excluded from later model context according to the user's invocation.
_Avoid_: Bash tool, prompt processing

**Slash Command**:
A user input beginning with `/` that the Native TUI routes before the Agent Prompt path: a builtin command, a Prompt Template invocation, or a Skill invocation. Built-in names and arguments are validated; unknown slash text produces a routing error unless the host explicitly recognizes it as a dynamic resource or compatible absolute-path submission.
_Avoid_: Command registry, dispatch table

**Resource Reload**:
The `/reload` command's re-read of User Settings, keybindings, skills, Prompt Templates, themes, and Project Context Files, followed by System Prompt rebuild and a refreshed loaded-resources presentation; refused while the Agent is streaming or compacting.
_Avoid_: Restart, hot swap

**Loaded Resources**:
The startup presentation listing the Project Context Files, skills, and Prompt Templates (and themes) in effect, grouped by source scope and carrying load diagnostics.
_Avoid_: Startup banner, resource log

**Interrupt Admission**:
The Native TUI decision of how an interrupt request applies to the current activity: which channel it targets (the active Agent run, the running User Bash, or a pending User Bash submission) and whether the request is stale relative to the active prompt generation. The decision is made against activity facts supplied at request time; the owning frontend performs the routed effect. It is the C++ translation of pi's app.interrupt precedence at the parity baseline.
_Avoid_: Escape handling, keybinding dispatch

**Generic Selector**:
The reusable string-list selection overlay used by the login auth-type picker ("Sign in with an account" / "API key"), `select`-type AuthPrompts, and the boot Project Trust prompt. pi's `extension-selector` component is not extension-only; it carries this non-extension role in the C++ app layer.
_Avoid_: Extension selector, provider-specific dialog

**TUI Toolkit**:
The reusable terminal UI capability module (`cch_tui`) that owns the terminal seam, input protocol decoding, keybindings, the editor stack, autocomplete, fuzzy matching, layout-free components, markdown rendering, and terminal image capability, depending on no coding-agent types. The Native TUI's interactive mode is assembled from it; interactive-mode application components (model selection, login presentation, footer/status, chat UX) are not toolkit capabilities.
_Avoid_: UI library, widget set, interactive frontend

**Decoded Input Event**:
The TUI Toolkit's single decode of raw terminal escape sequences into typed KeyEvent/PasteEvent values at the terminal edge, with action matching against pi's `modifier+key` identifier grammar and full keyboard-protocol coverage (legacy, modifyOtherKeys, Kitty CSI-u with event types and alternate keys). An Intentional Divergence from pi's raw-string matching that preserves the observable sequence-to-action contract; paste input remains size-bounded.
_Avoid_: Raw string matching, per-consumer escape parsing

**Resolved Keybinding Registry**:
The immutable, resolution-time keybinding table consumed by dispatch, help, and hints alike: the 30 assembled `tui.*` actions (21 `tui.editor.*`, 3 `tui.input.*`, 6 `tui.select.*`) with pi's default keys at the parity baseline, one startup `keybindings.json` read with replace-all-defaults and empty-array unbind, and bounded redacted diagnostics. `tui.input.copy` and the six `tui.altScreen.*` ids are recognized-but-unassembled: diagnosed as known-but-unbound, never no-op bindings, because their only active behavior is alt-screen selection copy.
_Avoid_: Mutable global manager, no-op bindings, per-consumer key tables

**Theme**:
A pi-format theme asset: a JSON document with an optional `$schema`, a required `name`, optional `vars` (resolved recursively with pi's circular-reference error), and a `colors` object over pi's 52-token set — 50 required tokens plus optional `scrollbarThumb` and `thinkingMax` with their fallbacks. Validation and missing-token diagnostics use pi's verbatim wording; theme names containing `/` are rejected.
_Avoid_: Theme catalog, custom theme format, palette

**Theme Setting**:
The `theme` user-settings scalar naming the active theme. A slash-containing value — the automatic `light/dark` pair — resolves as unset and the environment default applies, matching pi's own read semantics; the automatic pair itself is not supported.
_Avoid_: Auto theme, light/dark mode, terminal sync

**Theme Submenu**:
The `/settings` submenu listing the available themes (builtins, custom directory, registered) with in-memory preview on selection, a global-scope settings commit on confirm, and no revert on cancel — pi's preview/commit/cancel-does-not-revert behavior.
_Avoid_: Standalone theme overlay, theme picker dialog

**Terminal-Owned Image Placement**:
The TUI Toolkit's inline-image model in which components emit protocol-neutral image regions alongside their rendered lines and the Terminal owns physical placement and removal in absolute cell regions, including protocol selection (Kitty/iTerm2), cell-size math, animation-id reuse, and fallback text. An Intentional Divergence from pi's escape-sequences-in-lines with identical placement, sizing, and fallback outcomes.
_Avoid_: Escape sequences in render lines, component-owned protocol bytes

**Main-Screen Scrollback Flow**:
The TUI Toolkit's main-screen rendering model (pi `TuiMainScreen` parity) in which the renderer writes the full composed buffer to the terminal's main screen, lets overflow advance into the terminal's native scrollback, and tracks a viewport top over the buffer instead of clipping to the visible height; the terminal's own scrollback is the history surface, and a resize full-redraw clears screen and scrollback together. Startup content stays visible until the buffer grows past one screen, then scrolls away with it.
_Avoid_: Viewport-clip redraw, in-place line rewrite, alt-screen scrolling
