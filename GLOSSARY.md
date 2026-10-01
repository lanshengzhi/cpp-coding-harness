# C++ Coding Agent Harness

Pike is a coding-agent runtime for model-driven work in a user's workspace. This context names its conversations, capabilities, resources, and presentation surfaces.

## Language

### Product and ownership

**Supported Capability**:
A capability that Pike explicitly claims to provide as an intentional product decision.
_Avoid_: Implemented feature, partial placeholder

**Deferred Capability**:
A pi capability that Pike does not currently claim to provide.
_Avoid_: Missing contract, unsupported stub

**Pike Runtime**:
The released coding-agent application, combining an Agent Session with its command-line and Native TUI surfaces.
_Avoid_: `cpp_harness`, Owner library, SDK

**Supported Platform**:
The host platform on which Pike accepts build, runtime, and Semantic Parity obligations for Supported Capabilities.
_Avoid_: Development host, portable source, rolling latest, best-effort platform

**Semantic Parity**:
Preservation of the externally observable meanings and state transitions of a capability aligned with pi, independent of its C++ interface shape.
_Avoid_: API-shape parity, mechanical translation

**Intentional Divergence**:
A documented departure from Semantic Parity with a deliberate benefit to the C++ caller contract.
_Avoid_: Implementation shortcut, accidental drift

**Product Architecture Contract**:
The product-owned set of boundaries between Capability Owner Packages and presentation surfaces.
_Avoid_: pi parity, ad-hoc dependency review

**Capability Owner Package**:
The authoritative package responsible for a defined part of Pike's Supported Capabilities.
_Avoid_: Facade target, named module, source directory

**C++ Support Package**:
The pi-neutral package of shared C++ mechanics, distinct from the packages that own Supported Capabilities.
_Avoid_: Product package, capability owner, general-purpose runtime

**Owner Interface**:
The repository-internal contract through which a Capability Owner Package exposes its capabilities to other packages.
_Avoid_: Public SDK header, installed consumer surface, umbrella header

**Configured Package Graph**:
The configuration-specific view of package ownership, dependencies, and interface visibility assessed against the Product Architecture Contract.
_Avoid_: CMakeLists layout, build-order graph

**Parity Architecture Manifest**:
The authoritative policy record defining package ownership and the architectural boundaries checked by the Parity Architecture Gate.
_Avoid_: CMake comment, target allowlist, duplicated policy

**Parity Architecture Gate**:
The mandatory validation of the configured product against the Product Architecture Contract.
_Avoid_: Advisory architecture test, source-format style check, optional CI job

**Compat Layer**:
The one-time import boundary for pi's session and configuration formats, separate from Pike's runtime domain model.
_Avoid_: Runtime dual-read, deprecation shims, shared default directories

**Runtime Root**:
The shared runtime for one Pike invocation, spanning Agent Session replacements until final application Close.
_Avoid_: Singleton scheduler, executor hierarchy, event bus, one thread or loop per Owner Package

### Models and authentication

**Model**:
The credential-free description of one model's identity, capabilities, limits, and cost.
_Avoid_: Model name string, client configuration, optional model

**Provider**:
The live capability responsible for one provider identity's model catalog, authentication, and model requests.
_Avoid_: Provider factory, per-request client, hard-coded provider list

**Provider Definition**:
The complete description from which a Provider is established, including its identity, display name, model catalog, and authentication behavior.
_Avoid_: Provider factory options, adapter config bag, transport config

**Provider Info**:
The displayable description of an installed Provider and its authentication methods, distinct from access to that Provider's capabilities.
_Avoid_: Provider handle, runtime provider pointer, auth snapshot

**Models Runtime**:
The shared model and authentication capability serving Agent Sessions associated with one Agent Config Directory.
_Avoid_: Provider registry, session-scoped client, configuration snapshot

**Provider Message**:
A model-facing conversation value accepted or produced at a Provider boundary.
_Avoid_: Agent Message, Session Entry

**Compat Field**:
A Model's API-specific compatibility setting that expresses a supported variation in provider behavior.
_Avoid_: Capability flag bag, models.json compat override

**Session Affinity**:
The association that lets a Provider correlate model requests belonging to the same Agent Session.
_Avoid_: Session metadata, resume state

**Credential**:
A stored authentication value for one Provider, distinct from its catalog or displayable metadata.
_Avoid_: Provider config field, session metadata

**Request Authentication**:
The resolution of effective authentication for a particular model request.
_Avoid_: Login-time snapshot, startup authentication

**Runtime API Key Override**:
An API key supplied for the current Pike invocation rather than saved as a Credential.
_Avoid_: Saved key, default key

**Configured API Key**:
An API-key source declared in provider configuration rather than in the credential store.
_Avoid_: Stored credential, hardcoded key

**Auth Interaction**:
The exchange of prompts and events between provider-owned authentication and host-owned presentation.
_Avoid_: Login callback, rendered dialog, provider UI

**OAuth Login**:
An explicit user authorization flow that obtains a Provider Credential.
_Avoid_: Auto-login, implicit authentication, login snapshot

**Login Cancellation**:
The user's withdrawal from an in-progress OAuth Login, distinct from an authentication failure.
_Avoid_: Failed login, error dialog

**Credential Refresh**:
Request-time renewal of an expiring stored OAuth Credential for a Provider that supports renewal.
_Avoid_: Background refresh, proactive expiry notification

**Re-auth Guidance**:
The user-facing instruction to authenticate when a model request has no usable Credential.
_Avoid_: Generic auth failure, silent credential fallback

**Resume Model Resolution**:
The matching of a resumed Agent Session's recorded model identity to the current Models Runtime.
_Avoid_: Session restore, auth snapshot

### Agent work

**Agent**:
The stateful capability responsible for model-driven conversation execution.
_Avoid_: Agent Session, agent loop

**Agent Message**:
A conversation value retained and processed by the Agent, distinct from its provider-facing representation or durable Session Entry.
_Avoid_: Provider Message, Session Entry

**Agent Prompt**:
A user input admitted to the Agent for model-driven work rather than handled as a Slash Command or User Bash.
_Avoid_: Raw user input, Slash Command, User Bash

**Prompt Run**:
The complete handling of one Agent Prompt from admission to its terminal outcome and the Agent Session's return to idle, including any recovery runs.
_Avoid_: Agent Run, Agent Turn, raw input, agent loop

**Agent Run**:
One contiguous execution of the Agent's turn lifecycle within a Prompt Run, distinct from a later retry or compaction continuation.
_Avoid_: Prompt Run, Agent Turn, agent loop

**Agent Turn**:
One Agent lifecycle step comprising a model request, its assistant outcome, any resulting tool work, and the decision whether another turn is needed.
_Avoid_: Provider request, Prompt Run, agent loop

**Agent Stream Flow**:
The consumption of one model stream and its terminal outcome within an Agent Turn.
_Avoid_: Provider request, per-adapter option struct, second exception hierarchy

**Thinking Level**:
The model-facing reasoning preference selected for an Agent Session.
_Avoid_: Free-form effort string, provider-specific knob

**Execution Environment**:
The workspace filesystem and shell capabilities available to an Agent Session.
_Avoid_: Tool adapter, partial capability

**Tool Argument Contract**:
The executable definition of the arguments a particular tool accepts.
_Avoid_: Parameter hint, provider DTO

**Tool Call Batch**:
The group of tool calls produced by one assistant outcome and handled as one turn-level tool decision.
_Avoid_: Tool Execution, multiple Agent Turns

**Tool Execution**:
The lifecycle of one tool call from admission through its Tool Call Outcome.
_Avoid_: Tool Call Batch, Tool Call Outcome

**Tool Call Outcome**:
The final result of one tool call, distinct from the outcome of the Agent Turn or Agent Session containing it.
_Avoid_: Agent failure, intermediate tool response

**Auto-Retry**:
The session-level continuation of a Prompt Run with another Agent Run after a retryable terminal error.
_Avoid_: Infinite retry, silent retry, adapter-level retry

**Compaction**:
The summarization of older session context while retaining a recent conversation tail.
_Avoid_: Truncation, deletion, raw history replay

### Sessions and history

**Agent Session**:
One ongoing coding-agent conversation, including its current interaction state and durable history.
_Avoid_: Conversation handle, runtime session

**Live Session State**:
The current in-process view of an Agent Session, which may be newer than its durable history.
_Avoid_: Persisted state, session file

**Session Entry**:
One record in an Agent Session's history.
_Avoid_: Wire payload, JSON line

**pi v3 Session Format**:
The pi session-history interchange format understood at Pike's compatibility boundary.
_Avoid_: pi-style JSONL, C++ session schema

**Session Event Commitment**:
The ordered application of an Agent lifecycle event to an Agent Session's live state and history.
_Avoid_: Event handler, callback chain

**Session Assembly**:
The preparation of an Agent Session from its creation request and the capabilities and resources it needs.
_Avoid_: session factory, creation wrapper

**Session Publication**:
The step that establishes storage for an assembled Agent Session after its creation prerequisites succeed.
_Avoid_: Save, file write, flush

**Session Resume**:
Reopening an Agent Session from its durable history at the selected active point.
_Avoid_: Reload, replay

**Session Close**:
The end of an Agent Session's availability for work, with resource release after its admitted work has settled.
_Avoid_: Immediate teardown, abort

**Session Abort**:
Cancellation of the active prompt while leaving the Agent Session available for later work.
_Avoid_: Session Close, process kill

**Session Topology**:
The shape of an Agent Session's history, including its branches and compacted regions.
_Avoid_: Completion state

**Session Selection**:
The user-facing choice of which Agent Session to open.
_Avoid_: Session picker, resume chooser

**Session Fork**:
A new Agent Session derived from an existing session's history at a chosen point, with a parent-session relationship and a target workspace.
_Avoid_: Session clone, history copy

**Session Tree Navigation**:
The in-session choice of an active point in a branched Agent Session history.
_Avoid_: Tree view, fork picker, history browser

### Configuration and resources

**Agent Config Directory**:
The user-level home of durable Pike state shared across workspaces, separate from project resources.
_Avoid_: Config home, user profile directory, shared pi user root

**User Settings**:
The harness preferences resolved from the global and trusted project Settings Scopes.
_Avoid_: User config, config file, credential storage

**Settings Scope**:
One of the global or project preference layers contributing to User Settings.
_Avoid_: Profile, level

**Project Trust**:
The user-controlled authorization decision governing whether project-authored resources may be loaded.
_Avoid_: Workspace configuration, project self-approval

**Project Resource**:
A project-associated Skill or Prompt Template that may be made available to an Agent Session after policy checks.
_Avoid_: Runtime service, project file

**Skill**:
A reusable instruction set available to an Agent Session for a particular kind of task.
_Avoid_: Plugin, add-on

**Prompt Template**:
A named, reusable prompt with argument substitution for user invocation.
_Avoid_: Macro, canned prompt, system prompt

**Project Context File**:
A user or workspace instruction file incorporated into an Agent Session's System Prompt.
_Avoid_: Repo instructions file, project readme

**System Prompt**:
The model-facing instruction text for an Agent Session, combining product instructions with its loaded context and resources.
_Avoid_: Hard-coded prompt, provider request text

**Resource Reload**:
The in-session refresh of settings and loaded resources, including the System Prompt built from them.
_Avoid_: Restart, hot swap

**Loaded Resources**:
The presentation of the context files, Skills, Prompt Templates, and themes in effect for an Agent Session, together with their sources and load diagnostics.
_Avoid_: Startup banner, resource log

### Interactive surfaces

**Native TUI**:
Pike's interactive terminal product surface, combining reusable terminal capabilities with coding-agent interaction.
_Avoid_: Text REPL, RPC frontend

**Interactive Session Run**:
One Native TUI execution from boot through exit, including its session intent and host capabilities.
_Avoid_: TUI config bag, boot options

**Print Mode**:
The one-shot, non-interactive text frontend for piped or scripted Agent Prompts.
_Avoid_: Event stream, JSON event print, one-shot slash dispatch

**User Bash**:
A user-entered shell operation in the Native TUI, separate from an Agent Prompt, whose result belongs to session history and may be included in later model context.
_Avoid_: Bash tool, prompt processing

**Slash Command**:
A slash-prefixed user input considered by the Native TUI's command routing before admission as an Agent Prompt.
_Avoid_: Command registry, dispatch table

**Interrupt Admission**:
The Native TUI's choice of the activity to which an interrupt applies, taking account of pending input and active work.
_Avoid_: Escape handling, keybinding dispatch

**Generic Selector**:
The shared string-list choice surface for interactive prompts such as authentication and project trust.
_Avoid_: Extension selector, provider-specific dialog

**Tool Renderer**:
The application-layer presentation of a tool's call and result in the Native TUI, distinct from the tool's execution capability.
_Avoid_: Tool Execution, tool-name if-else inside the tool-execution component

**TUI Toolkit**:
The reusable terminal interaction and presentation capabilities from which the coding-agent-specific Native TUI is assembled.
_Avoid_: UI library, widget set, interactive frontend

**Theme**:
A named visual style for the Native TUI's semantic elements.
_Avoid_: Theme catalog, custom theme format, palette

**Theme Setting**:
The User Settings preference naming the active Theme.
_Avoid_: Auto theme, light/dark mode, terminal sync

**Theme Submenu**:
The theme-selection surface within the Native TUI settings flow.
_Avoid_: Standalone theme overlay, theme picker dialog

**Terminal-Owned Image Placement**:
The TUI image model in which the terminal, rather than individual components, owns an image's placement alongside its text.
_Avoid_: Escape sequences in render lines, component-owned protocol bytes

**Main-Screen Scrollback Flow**:
The Native TUI presentation model in which growing content becomes terminal scrollback rather than a separate application-managed history view.
_Avoid_: Viewport-clip redraw, in-place line rewrite, alt-screen scrolling

### Core projections

**Headless Core**:
The agent execution and session-state capability independent of any presentation surface.
_Avoid_: UI-driven runtime, mixed controller-view engine

**Projection**:
A read-only presentation of Headless Core state on a particular surface, independent of the Core's progress.
_Avoid_: Synchronous UI hook, view controller, bidirectional display binding

**Projection Stream**:
The authoritative sequence of Headless Core state delivered to a Projection as a Base followed by Patches.
_Avoid_: Dirty edge, version sampling, push sink, event mirror

**Base**:
A complete picture of Agent Session state at a Projection's attachment or resynchronization point.
_Avoid_: Initial snapshot, full refresh, state dump

**Patch**:
A record of a published session-state change containing the changed slice and its new value.
_Avoid_: Delta, diff, change event, op

**Subscription Mailbox**:
The bounded delivery backlog belonging to one Projection subscription.
_Avoid_: Ring buffer, patch history, subscriber version tracking

**Counted Frame**:
A Native TUI frame that advances the Projection's pacing and convergence accounting.
_Avoid_: Immediate frame, per-event render

**Preview Frame**:
An interim Native TUI presentation of the newest view state between Counted Frames, outside their pacing accounting.
_Avoid_: Ticker-only pacing, throttled render, synchronous view hook

**Block Frozen Protocol**:
The transcript-block lifecycle distinguishing active, finalized, and permanently committed presentation.
_Avoid_: Whole-document reparsing, stateless full rerender
