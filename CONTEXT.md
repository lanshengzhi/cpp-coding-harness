# Domain Glossary

This file records the project's domain terms and shared design vocabulary. Keep it free of implementation details; it is a glossary, not a spec.

## Architecture terms

### Seam

From Michael Feathers: a place where behaviour can be altered without editing in that place. In this codebase a seam is the location at which a module's interface lives. An **adapter** satisfies the interface at that seam. Distinct from "boundary" or "API".

Example: the `AgentSession` interface is a seam; the runtime implementation behind it and test fakes in front of it are adapters.

## Domain terms

### RootRouter

The root `AGENTS.md` document that starts every agent run by routing the task to the right reference material, guardrails, validation slice, and handoff expectations. Distinct from a `Seam`, which is reserved for module/interface boundaries.

### VerifySlice

The smallest validation scope that proves a change at its affected documentation, implementation, or public-boundary surface. It escalates only when the changed surface demands it.

### AgentSession

A move-only runtime handle representing one open agent conversation session. The public seam exposes success-or-error prompt completion, one persistent lifecycle-event subscription path, live-state accessors, and close; assembly complexity lives behind it in a factory. Prompt progress is never returned through a per-prompt event sink.

### SessionFactory

The adapter responsible for assembling an `AgentSession` from source-specific creation intent. CLI and SDK adapters only translate their inputs into that intent; `SessionFactory` owns the shared provider, resume, trust, resource, tool, default, and security-sensitive assembly policy. Returns a thin handle plus creation-time diagnostics.

### PromptProcessingPipeline

The internal deep module that deterministically interprets one user input before agent execution: skill expansion flows into prompt-template expansion, and unmatched slash input remains an agent prompt. It owns an immutable session resource snapshot; `AgentSession` remains the public prompt seam and `AgentSessionRuntime` owns the end-to-end lifecycle.

### PromptProcessingOutcome

A passive internal result of prompt interpretation containing the final agent prompt after skill and prompt-template expansion. There is no command-handling branch; command dispatch belongs to the CLI adapter and RPC mode.

### UserBash

An interactive frontend operation triggered by `!command` or `!!command` that executes through the session's execution environment and persists a `BashExecutionMessage`. `!` participates in later model context, while `!!` remains persisted but is excluded from model context; neither is prompt processing.

### SkillExpander

Expansion of `/skill:name` from the session's immutable, already-authorized skill snapshot. It never performs prompt-time resource discovery; future reload behavior replaces the snapshot explicitly.

### PromptTemplateExpander

Expansion of `/templateName args` from the session's immutable prompt-template snapshot using bash-style argument substitution.

### CommandRegistry

A generic name-to-handler dispatch seam for slash-commands. Domain-specific commands are registered into it; the registry itself has no agent-session semantics.

### AgentSessionRuntime

The internal implementation behind the `AgentSession` handle. Coordinates prompt processing and the agent loop. Its event-handling order is live-state update, future extension handling when that module exists, persistent subscriber delivery in registration order, then completed-message persistence; no inert extension placeholder exists today. Subscriber or persistence failure fails the active prompt without rolling back live history or closing the session.

### LiveSessionState

The in-process conversation history and state derived from it, observed through `AgentSession` accessors such as message count and last assistant text. A completed message enters live state before subscriber delivery and persistence, so live state may be ahead of durable storage after a failure. Later prompts continue from retained live history; reopening reconstructs only durable entries.

### RpcMode

The machine-readable interaction protocol for driving one `AgentSession` through sequential JSONL commands. It owns command validation and correlation, prompt preflight acknowledgement, direct response/event interleaving, recoverable command errors, and clean EOF or shutdown semantics; the CLI adapter only connects startup policy and streams. Accepted prompts receive one success response before their events, preflight rejection receives one error response, and later execution outcomes never create a second response or terminal record.

### SessionJournal

The adapter responsible for durable, append-only line storage of the JSONL session file. Operates on raw string lines; knows nothing about message schemas or redaction.

### EntrySerializer

The adapter that converts domain session entries (messages, v3 tree metadata) into redacted JSON lines and parses lines back into domain values. Uses Glaze DTOs internally.

### SessionTree

A read-only index over a loaded session's entries, supporting branch navigation, leaf tracking, and compaction-aware context reconstruction.

### SessionResume

The domain operation that reopens a persisted session into the agent-ready context at the current active leaf, including branch and compaction semantics. Distinct from `SessionTree`, which indexes and navigates entries.

### SessionTopology

A passive classification of a stored session's resume shape, such as linear, branched, or compacted. Used to decide which consumers can resume it; it is not an execution-completion state.

### ToolConcurrency

A passive agent-layer claim made by an `AsyncAgentTool`: `Exclusive` is the safe default, while `ParallelSafe` explicitly permits overlapping execution when the run policy also allows it. It never appears in the provider-visible `ai::Tool` schema.

### ToolExecutionPolicy

A closed agent-layer run policy: either sequential execution or bounded parallel execution with an explicit `max_in_flight`. Parallel execution requires every resolved tool in the batch to claim `ParallelSafe`; otherwise the complete batch runs sequentially.

### ToolCallExecutor

The private adapter responsible for executing one accepted batch of assistant tool calls. Hides tool-call extraction, sequential/bounded-parallel dispatch, before/after hooks, lifecycle events, terminate-batch logic, and error-to-tool-result mapping behind a narrow interface. `AgentLoop` rejects length-truncated tool intent before this seam.

### ToolCallExecutorOptions

Passive private configuration for `ToolCallExecutor`: optional before/after hooks plus one `ToolExecutionPolicy`. It has no independent mode/cap fields.

### ToolCallBatchResult

Passive result of executing a batch of tool calls: source-ordered tool-result messages plus a flag indicating whether every call completed successfully and requested early termination.

### TriageState

A readiness classification on a local PRD or issue saying who can pick it up next. It is not an execution-completion state; acceptance checkboxes or review comments carry completion evidence.

*Additional terms to be added as design sessions continue.*
