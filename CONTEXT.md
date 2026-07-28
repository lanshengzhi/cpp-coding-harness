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

**Project Resource**:
A project-associated skill or prompt template that may be made available to an Agent Session after policy checks.
_Avoid_: Runtime service, project file

**Project Trust**:
The user-controlled authorization decision governing whether project-authored resources may be loaded.
_Avoid_: Workspace configuration, project self-approval

**Agent Config Directory**:
The user-level root for durable harness state shared across workspaces.
_Avoid_: Config home, user profile directory

**User Settings**:
User-level preferences for providers, Project Resources, and Agent Session storage.
_Avoid_: User config, config file

**User Bash**:
A Native TUI operation that runs a user-entered shell command without treating it as an Agent Prompt. Its completed execution belongs to Agent Session history and is either included in or excluded from later model context according to the user's invocation.
_Avoid_: Bash tool, prompt processing
