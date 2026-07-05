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

A move-only runtime handle representing one open agent conversation session. The public seam exposes prompt execution, lifecycle event subscription, and close; assembly complexity lives behind it in a factory.

### SessionFactory

The adapter responsible for assembling an `AgentSession` from resolved configuration, project resources, provider client, tools, session store, and other runtime parts. Returns a thin handle plus creation-time diagnostics.

### PromptProcessingPipeline

A thin orchestrator that runs raw user input through slash-command dispatch, skill expansion, and prompt-template expansion in the correct order. Owns references to the three lower-level adapters but contains no business logic itself.

### SkillExpander

The adapter that expands a `/skill:name` command into the full skill instructions block. Construction-time dependencies: the session's loaded skills and the workspace filesystem.

### PromptTemplateExpander

The adapter that expands a `/templateName args` command using bash-style argument substitution. Construction-time dependency: the session's loaded prompt templates.

### CommandRegistry

A generic name-to-handler dispatch seam for slash-commands. Domain-specific commands are registered into it; the registry itself has no agent-session semantics.

### AgentSessionRuntime

The internal implementation behind the `AgentSession` handle. Coordinates the prompt-processing pipeline, agent loop, session store, history, and event subscriber fanout.

### AgentSessionState

A passive value summarising the introspectable state of an open session: provider, model, session id, workspace, and message count.

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

### ToolCallExecutor

The adapter responsible for executing one batch of assistant tool calls. Hides sequential/parallel dispatch, before/after hooks, terminate-batch logic, and error-to-tool-result mapping behind a narrow interface.

### ToolCallExecutorOptions

Passive configuration for `ToolCallExecutor`: optional before/after hooks, execution mode (sequential or parallel), and maximum parallel tools.

### ToolCallBatchResult

Passive result of executing a batch of tool calls: the list of tool-result messages and a flag indicating whether the batch requested early termination.

### TriageState

A readiness classification on a local PRD or issue saying who can pick it up next. It is not an execution-completion state; acceptance checkboxes or review comments carry completion evidence.

*Additional terms to be added as design sessions continue.*
