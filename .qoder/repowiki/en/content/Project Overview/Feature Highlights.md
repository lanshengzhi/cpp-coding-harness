# Feature Highlights

<cite>
**Referenced Files in This Document**
- [README.md](file://README.md)
- [main.cpp](file://src/main.cpp)
- [Sdk.hpp](file://include/cch/coding_agent/Sdk.hpp)
- [SessionTree.hpp](file://include/cch/harness/session/SessionTree.hpp)
- [ExecutionEnv.hpp](file://include/cch/harness/ExecutionEnv.hpp)
- [ProviderRegistry.hpp](file://include/cch/ai/ProviderRegistry.hpp)
- [ToolRegistry.hpp](file://include/cch/agent/ToolRegistry.hpp)
- [ToolFactories.hpp](file://include/cch/tools/ToolFactories.hpp)
- [Skill.hpp](file://include/cch/coding_agent/Skill.hpp)
- [ProjectTrust.hpp](file://include/cch/coding_agent/ProjectTrust.hpp)
- [RpcMode.hpp](file://src/coding_agent/runtime/RpcMode.hpp)
- [JsonEventPrinter.hpp](file://src/coding_agent/runtime/JsonEventPrinter.hpp)
- [JsonlSessionStore.hpp](file://include/cch/harness/session/JsonlSessionStore.hpp)
- [StreamEvent.hpp](file://include/cch/ai/StreamEvent.hpp)
</cite>

## Table of Contents
1. [Introduction](#introduction)
2. [Project Structure](#project-structure)
3. [Core Components](#core-components)
4. [Architecture Overview](#architecture-overview)
5. [Detailed Component Analysis](#detailed-component-analysis)
6. [Dependency Analysis](#dependency-analysis)
7. [Performance Considerations](#performance-considerations)
8. [Troubleshooting Guide](#troubleshooting-guide)
9. [Conclusion](#conclusion)
10. [Appendices](#appendices)

## Introduction
This document presents the feature highlights of the C++ Coding Harness, emphasizing its multi-modal interaction modes (CLI text, JSON, RPC), persistent session management with JSONL storage and tree navigation, secure execution environment with workspace containment, extensible tool system with built-in capabilities, provider-agnostic AI integration with streaming support, and an embeddable C++ SDK for host applications. It also covers the skill system for project-local knowledge injection, project trust and resource controls, and clarifies the experimental nature versus production readiness status. Practical examples illustrate how the harness operates as both a CLI tool and an embeddable library, with explicit limitations and deferred features to set appropriate expectations.

## Project Structure
The project is organized into layered packages that separate public contracts, capability seams, and implementation adapters. The CLI entry point delegates to a runtime orchestrator that wires provider clients, execution environments, tools, and session persistence. The SDK exposes a source-level surface for embedding the agent loop without shelling out to the CLI.

```mermaid
graph TB
A["CLI Entry<br/>src/main.cpp"] --> B["CLI Runtime Orchestration<br/>src/coding_agent/runtime/*"]
B --> C["Agent Loop & Events<br/>include/cch/agent/*.hpp"]
B --> D["AI Providers & Streaming<br/>include/cch/ai/*.hpp"]
B --> E["Execution Environment<br/>include/cch/harness/ExecutionEnv.hpp"]
B --> F["Session Storage & Tree<br/>include/cch/harness/session/*.hpp"]
B --> G["Tools Registry & Factories<br/>include/cch/agent/ToolRegistry.hpp<br/>include/cch/tools/ToolFactories.hpp"]
B --> H["Skills & Trust Controls<br/>include/cch/coding_agent/Skill.hpp<br/>include/cch/coding_agent/ProjectTrust.hpp"]
subgraph "Public Contracts (include/cch)"
C
D
E
F
G
H
end
subgraph "Implementation (src)"
B
end
```

**Diagram sources**
- [main.cpp:1-33](file://src/main.cpp#L1-L33)
- [Sdk.hpp:1-347](file://include/cch/coding_agent/Sdk.hpp#L1-L347)
- [ExecutionEnv.hpp:1-337](file://include/cch/harness/ExecutionEnv.hpp#L1-L337)
- [SessionTree.hpp:1-148](file://include/cch/harness/session/SessionTree.hpp#L1-L148)
- [ToolRegistry.hpp:1-51](file://include/cch/agent/ToolRegistry.hpp#L1-L51)
- [ToolFactories.hpp:1-16](file://include/cch/tools/ToolFactories.hpp#L1-L16)
- [Skill.hpp:1-60](file://include/cch/coding_agent/Skill.hpp#L1-L60)
- [ProjectTrust.hpp:1-93](file://include/cch/coding_agent/ProjectTrust.hpp#L1-L93)

**Section sources**
- [README.md:135-151](file://README.md#L135-L151)
- [main.cpp:1-33](file://src/main.cpp#L1-L33)

## Core Components
- Multi-modal interaction modes:
  - CLI text mode prints semantic events for human readability.
  - JSON mode emits compact JSONL records for machine processing.
  - RPC mode provides a narrow JSONL stdin/stdout command loop for programmatic control.
- Persistent session management:
  - JSONL storage with v2 header and typed entries, plus v3 tree metadata for advanced navigation.
  - Session tree enables branch navigation, leaf tracking, and compaction-aware context reconstruction.
- Secure execution environment:
  - Workspace containment, symlink safety, atomic writes, and sanitized environment for shell execution.
- Extensible tool system:
  - Async tool registry with built-in file and bash tools; host can supply custom tools.
- Provider-agnostic AI integration:
  - Provider registry with OpenAI-compatible defaults and streaming support.
- Embeddable C++ SDK:
  - Source-level SDK surface for embedding the agent loop without CLI/RPC globals.

**Section sources**
- [README.md:157-173](file://README.md#L157-L173)
- [README.md:242-247](file://README.md#L242-L247)
- [README.md:248-255](file://README.md#L248-L255)
- [README.md:256-266](file://README.md#L256-L266)
- [README.md:174-241](file://README.md#L174-L241)
- [README.md:267-279](file://README.md#L267-L279)

## Architecture Overview
The harness follows an anti-fragile architecture with passive data contracts, replaceable capabilities, and event-driven flows. The CLI entry point parses arguments, validates workspace, and runs the async CLI runtime. The runtime composes provider clients, execution environments, tools, and session persistence, then drives the agent loop with streaming AI events.

```mermaid
sequenceDiagram
participant User as "User"
participant CLI as "CLI main.cpp"
participant Runtime as "Async CLI Runtime"
participant Provider as "Streaming Chat Client"
participant Env as "ExecutionEnv"
participant Store as "JsonlSessionStore"
User->>CLI : "Invoke cpp_harness with args"
CLI->>Runtime : "run_async_cli(config)"
Runtime->>Provider : "create default client (OpenAI-compatible)"
Runtime->>Env : "construct local execution environment"
Runtime->>Store : "create/open session JSONL"
Runtime->>Runtime : "agent loop : prompt -> stream events -> tool calls -> append to JSONL"
Provider-->>Runtime : "stream events (text/thinking/tool)"
Env-->>Runtime : "execute tools (read/write/edit/bash)"
Store-->>Runtime : "append typed entries"
Runtime-->>CLI : "emit text/json/rpc output"
CLI-->>User : "final output"
```

**Diagram sources**
- [main.cpp:7-32](file://src/main.cpp#L7-L32)
- [ProviderRegistry.hpp:40-56](file://include/cch/ai/ProviderRegistry.hpp#L40-L56)
- [ExecutionEnv.hpp:198-334](file://include/cch/harness/ExecutionEnv.hpp#L198-L334)
- [JsonlSessionStore.hpp:18-79](file://include/cch/harness/session/JsonlSessionStore.hpp#L18-L79)
- [StreamEvent.hpp:78-91](file://include/cch/ai/StreamEvent.hpp#L78-L91)

## Detailed Component Analysis

### Multi-modal Interaction Modes
- CLI text mode:
  - Emits semantic event lines for model requests, assistant messages, tool calls, tool results, provider errors, max turns, and completion reasons.
- JSON mode:
  - Emits compact JSONL records including a session header and lifecycle events such as agent_start, turn_start, message_update, tool_execution_start/end, turn_end, and runtime_terminal.
- RPC mode:
  - Reads JSONL stdin commands and writes responses/events to stdout; supports prompt, get_state, get_last_assistant_text, and shutdown; returns structured success:false for unsupported commands.

```mermaid
flowchart TD
Start(["Start"]) --> Mode{"Select Mode"}
Mode --> |Text| TextOut["Print semantic event lines"]
Mode --> |JSON| JsonOut["Emit JSONL records"]
Mode --> |RPC| RpcIn["Read stdin commands"]
RpcIn --> RpcOut["Write responses/events"]
TextOut --> End(["End"])
JsonOut --> End
RpcOut --> End
```

**Section sources**
- [README.md:157-173](file://README.md#L157-L173)
- [RpcMode.hpp:12-23](file://src/coding_agent/runtime/RpcMode.hpp#L12-L23)
- [JsonEventPrinter.hpp:12-26](file://src/coding_agent/runtime/JsonEventPrinter.hpp#L12-L26)

### Persistent Session Management with JSONL Storage and Tree Navigation
- JSONL storage:
  - Header with session/workspace/provider/model metadata.
  - Typed message entries with redacted content.
  - V3 tree metadata entries for model change, thinking level change, active tools change, custom/custom message, label, compaction, branch summary, session info, and leaf tracking.
- Session tree:
  - In-memory index enabling O(1) entry lookup and efficient traversal.
  - Leaf navigation, branch switching, and compaction-aware context reconstruction.
  - Optional branch summary hook to append summaries when switching branches.

```mermaid
classDiagram
class JsonlSessionStore {
+create_new(path, metadata)
+open_existing(path)
+load(path)
+open_as_tree(path)
+append(message)
+append_model_change(...)
+append_thinking_level_change(...)
+append_active_tools_change(...)
+append_custom_entry(...)
+append_custom_message_entry(...)
+append_label_change(...)
+append_compaction(...)
+append_branch_summary(...)
+append_session_info(...)
+append_leaf(...)
+path()
+metadata()
}
class SessionTree {
+getEntry(entry_id)
+getChildren(parent_id)
+entries()
+metadata()
+size()
+empty()
+leaf_id()
+leaf_entry()
+branch(entry_id)
+getBranch(from_id)
+root()
+buildSessionContext()
+branchWithSummary(entry_id, hook, append_writer)
}
JsonlSessionStore --> SessionTree : "open_as_tree()"
```

**Diagram sources**
- [JsonlSessionStore.hpp:18-79](file://include/cch/harness/session/JsonlSessionStore.hpp#L18-L79)
- [SessionTree.hpp:34-145](file://include/cch/harness/session/SessionTree.hpp#L34-L145)

**Section sources**
- [README.md:267-279](file://README.md#L267-L279)
- [JsonlSessionStore.hpp:18-79](file://include/cch/harness/session/JsonlSessionStore.hpp#L18-L79)
- [SessionTree.hpp:17-145](file://include/cch/harness/session/SessionTree.hpp#L17-L145)

### Secure Execution Environment with Workspace Containment
- Capability seam for asynchronous execution environment:
  - Workspace path and bash enablement.
  - File operations with stable error codes and pi-shaped metadata.
  - Shell execution with split stdout/stderr streams, timeouts, and sanitized environment.
- Safety guarantees:
  - Workspace containment, path validation, atomic writes, and symlink safety.
  - Bash tool requires explicit opt-in; environment filtering removes secrets.

```mermaid
classDiagram
class AsyncExecutionEnv {
+workspace() path
+bash_enabled() bool
+read_file(path, offset, limit)
+write_file(path, content, create_parents)
+edit_file(path, old_text, new_text)
+run_shell(command, timeout)
+absolutePath(parts)
+joinPath(parts)
+readTextFile(path)
+readTextLines(path, maxLines)
+readBinaryFile(path)
+writeFile(path, content)
+appendFile(path, content)
+fileInfo(path)
+listDir(path)
+canonicalPath(path)
+exists(path)
+createDir(path, recursive)
+remove(path, recursive)
+createTempDir(prefix)
+createTempFile(prefix, suffix)
+cleanup()
+exec(command, options)
}
```

**Diagram sources**
- [ExecutionEnv.hpp:198-334](file://include/cch/harness/ExecutionEnv.hpp#L198-L334)

**Section sources**
- [README.md:256-266](file://README.md#L256-L266)
- [ExecutionEnv.hpp:58-192](file://include/cch/harness/ExecutionEnv.hpp#L58-L192)

### Extensible Tool System with Built-in Capabilities
- Tool registry:
  - Async tool registry keyed by tool name; definitions exported for tool schema.
- Built-in tools:
  - read_file, write_file, edit_file, and bash (opt-in).
- Tool factories:
  - Factory functions to create async tools bound to an execution environment.

```mermaid
classDiagram
class AsyncToolRegistry {
+add(tool)
+find(name) AsyncAgentTool*
+definitions() vector<Tool>
}
class ToolFactories {
+make_async_read_file_tool(env)
+make_async_write_file_tool(env)
+make_async_edit_file_tool(env)
+make_async_bash_tool(env)
}
AsyncToolRegistry --> ToolFactories : "used by runtime"
```

**Diagram sources**
- [ToolRegistry.hpp:13-48](file://include/cch/agent/ToolRegistry.hpp#L13-L48)
- [ToolFactories.hpp:10-13](file://include/cch/tools/ToolFactories.hpp#L10-L13)

**Section sources**
- [README.md:256-266](file://README.md#L256-L266)
- [ToolRegistry.hpp:21-44](file://include/cch/agent/ToolRegistry.hpp#L21-L44)
- [ToolFactories.hpp:10-13](file://include/cch/tools/ToolFactories.hpp#L10-L13)

### Provider-Agnostic AI Integration with Streaming Support
- Provider registry:
  - Registers provider factories and constructs streaming chat clients.
  - OpenAI-compatible defaults with configurable base URL and API key environment.
- Streaming support:
  - Stream events cover assistant start, text deltas, thinking deltas, tool call deltas, and completion/error events.
- Real provider usage:
  - OpenAI-compatible mode with optional base URL and model selection.

```mermaid
sequenceDiagram
participant Runtime as "Agent Runtime"
participant Registry as "ProviderRegistry"
participant Client as "StreamingChatClient"
participant SSE as "SSE Parser"
Runtime->>Registry : "create(provider, context)"
Registry-->>Runtime : "StreamingChatClient"
Runtime->>Client : "send messages + tool schemas"
Client->>SSE : "parse SSE stream"
SSE-->>Runtime : "AssistantStreamEvent variants"
Runtime->>Runtime : "update context, schedule tool calls"
```

**Diagram sources**
- [ProviderRegistry.hpp:40-56](file://include/cch/ai/ProviderRegistry.hpp#L40-L56)
- [StreamEvent.hpp:78-91](file://include/cch/ai/StreamEvent.hpp#L78-L91)

**Section sources**
- [README.md:82-91](file://README.md#L82-L91)
- [README.md:170-171](file://README.md#L170-L171)
- [ProviderRegistry.hpp:18-53](file://include/cch/ai/ProviderRegistry.hpp#L18-L53)
- [StreamEvent.hpp:13-91](file://include/cch/ai/StreamEvent.hpp#L13-L91)

### Embeddable C++ SDK for Host Applications
- SDK surface:
  - Source-level only, experimental, not ABI-stable.
  - Create/resume sessions, subscribe to lifecycle events, run prompts, and close sessions.
- Supported v1 behavior:
  - Blocking prompt, event subscriptions via move-only sinks, host-provided chat clients and execution environments, built-in tool selection, custom tool registration, programmatic skills/templates/commands, project resource loading behind trust/resource controls.
- Not supported in SDK v1:
  - ABI stability, full pi runtime replacement APIs, tree navigation/compaction, concurrent prompts, cancellation, dynamic extensions, TUI, OAuth/subscription providers, or MCP integration.

```mermaid
classDiagram
class Sdk {
+create_agent_session(options) CreateAgentSessionResult
}
class CreateAgentSessionOptions {
+session_path
+resume_path
+workspace
+provider_config
+chat_client
+execution_env
+builtin_tools
+custom_tools
+skills
+prompt_templates
+commands
+load_project_resources
+default_project_trust
+project_skills_enablement
+max_turns
}
class AgentSession {
+prompt(text, options) PromptResult
+subscribe(sink) EventSubscription
+message_count() size_t
+last_assistant_text() optional<string>
+session_id() string
+session_path() path
+provider() string
+model() string
+workspace() path
+close() ExpectedVoid
+is_open() bool
+is_busy() bool
+skills() vector<Skill>
+templates() vector<PromptTemplate>
}
Sdk --> CreateAgentSessionOptions : "consumes"
Sdk --> AgentSession : "returns"
```

**Diagram sources**
- [Sdk.hpp:83-149](file://include/cch/coding_agent/Sdk.hpp#L83-L149)
- [Sdk.hpp:251-332](file://include/cch/coding_agent/Sdk.hpp#L251-L332)

**Section sources**
- [README.md:174-241](file://README.md#L174-L241)
- [Sdk.hpp:36-346](file://include/cch/coding_agent/Sdk.hpp#L36-L346)

### Skill System for Project-Local Knowledge Injection
- Discovery and loading:
  - At startup, CLI scans project-local .cpp-harness/skills for nested SKILL.md files when project resource load plan allows.
  - Skills use flat YAML frontmatter (name, description, optional disable-model-invocation) followed by markdown instructions.
- Visibility and invocation:
  - Valid skills are injected into model context through the pi-shaped <available_skills> block.
  - Skills with disable-model-invocation hidden from model-visible list but can be invoked explicitly with /skill:name.
- Diagnostics:
  - Malformed or duplicate skills produce warnings; editing during a running session is deferred.

```mermaid
flowchart TD
Scan[".cpp-harness/skills/**/SKILL.md"] --> Parse["Parse YAML frontmatter + content"]
Parse --> Validate{"Valid and unique?"}
Validate --> |No| Warn["Produce diagnostics (warning)"]
Validate --> |Yes| Inject["Inject into model context"]
Inject --> Ready["Available via /skill:name"]
```

**Section sources**
- [README.md:242-247](file://README.md#L242-L247)
- [Skill.hpp:31-57](file://include/cch/coding_agent/Skill.hpp#L31-L57)

### Project Trust and Resource Controls
- Trust decision-making:
  - Protected project markers include .cpp-harness/settings.json, .cpp-harness/skills, .cpp-harness/prompts, and others.
  - Default policies: ask, always, or never; nearest-parent inheritance via trust store.
- One-run overrides:
  - --approve/-a and --no-approve allow one-run trust overrides; --no-skills suppresses project-local skills for that run.
- Not a sandbox:
  - Trust controls are an input-loading guard; they do not restrict built-in tools, model output, prompt injection from chosen files, shell commands enabled with --enable-bash, or content already present in a resumed session.

```mermaid
flowchart TD
Start(["Startup"]) --> Detect["Detect protected project markers"]
Detect --> Policy{"Default policy?"}
Policy --> |Ask| Interactive["Prompt user (non-interactive default: untrusted)"]
Policy --> |Always| Trust["Trust project resources"]
Policy --> |Never| Deny["Do not load project resources"]
Interactive --> Decision{"User decision?"}
Decision --> |Trusted| Trust
Decision --> |Untrusted| Deny
Trust --> Apply["Apply trust to resource loading"]
Deny --> Apply
```

**Section sources**
- [README.md:248-255](file://README.md#L248-L255)
- [ProjectTrust.hpp:64-91](file://include/cch/coding_agent/ProjectTrust.hpp#L64-L91)

### Practical Examples

- CLI text mode:
  - Example invocation with a fake provider and JSONL session file, printing semantic events.
  - Example reading a file via tool and emitting semantic events.
  - Example using REPL mode with session persistence.
  - Example resuming a session and falling back to stored provider configuration.

- JSON mode:
  - Example emitting compact JSONL records with message_update events filtered via jq.

- RPC mode:
  - Example sending get_state and shutdown commands via stdin JSONL.

- Embedding with the SDK:
  - Example creating a session with provider config, subscribing to lifecycle events, running a prompt, and closing the session.

**Section sources**
- [README.md:70-78](file://README.md#L70-L78)
- [README.md:79-87](file://README.md#L79-L87)
- [README.md:170-171](file://README.md#L170-L171)
- [README.md:172-173](file://README.md#L172-L173)
- [README.md:183-216](file://README.md#L183-L216)

## Dependency Analysis
The CLI entry point depends on CLI parsing and preflight validation, then delegates to the async CLI runtime. The runtime composes provider clients, execution environments, tools, and session storage. The SDK mirrors this composition at a higher level for embedding.

```mermaid
graph LR
Main["src/main.cpp"] --> CLI["CLI Runtime"]
CLI --> ProviderReg["ProviderRegistry"]
CLI --> ExecEnv["AsyncExecutionEnv"]
CLI --> ToolReg["AsyncToolRegistry"]
CLI --> Jsonl["JsonlSessionStore"]
SDK["include/cch/coding_agent/Sdk.hpp"] --> ProviderReg
SDK --> ExecEnv
SDK --> ToolReg
SDK --> Jsonl
```

**Diagram sources**
- [main.cpp:7-32](file://src/main.cpp#L7-L32)
- [ProviderRegistry.hpp:40-56](file://include/cch/ai/ProviderRegistry.hpp#L40-L56)
- [ExecutionEnv.hpp:198-334](file://include/cch/harness/ExecutionEnv.hpp#L198-L334)
- [ToolRegistry.hpp:13-48](file://include/cch/agent/ToolRegistry.hpp#L13-L48)
- [JsonlSessionStore.hpp:18-79](file://include/cch/harness/session/JsonlSessionStore.hpp#L18-L79)
- [Sdk.hpp:343-344](file://include/cch/coding_agent/Sdk.hpp#L343-L344)

**Section sources**
- [README.md:135-151](file://README.md#L135-L151)
- [main.cpp:1-33](file://src/main.cpp#L1-L33)

## Performance Considerations
- Streaming AI responses reduce latency by incrementally exposing assistant text and tool calls.
- JSONL append is append-only and designed for redacted transcripts; tree navigation and compaction-aware context reconstruction are computed on demand.
- Workspace containment and sanitized environments avoid unnecessary overhead while preserving safety.
- The SDK is single-threaded and blocking per prompt; concurrency and cancellation are deferred.

[No sources needed since this section provides general guidance]

## Troubleshooting Guide
- Missing API key:
  - Ensure the correct environment variable is exported and passed via --api-key-env.
- Authentication or authorization failure:
  - Verify the key is valid for the selected provider and that the base URL is correct.
- Invalid model:
  - Use the supported model for the provider.
- Rate limit or quota error:
  - Retry later or check entitlements.
- Request unexpectedly going to another provider:
  - Ensure the base URL, model, and API key environment are all present.
- 403 Forbidden:
  - Confirm subscription/agent access for the provider’s coding endpoint.
- Provider or transport error:
  - Re-run with harmless prompts and inspect diagnostics without printing secrets.

**Section sources**
- [README.md:115-126](file://README.md#L115-L126)

## Conclusion
The C++ Coding Harness delivers a flexible, provider-agnostic coding agent with robust multi-modal interaction, persistent session management, secure execution, and an embeddable SDK surface. While experimental and not production-ready, it offers a solid foundation for integrating AI-assisted coding workflows in both CLI and embedded contexts. Users should expect deferred features such as full RPC parity, ABI stability, concurrent prompts, and advanced TUI capabilities, and treat the SDK as source-level only for now.

[No sources needed since this section summarizes without analyzing specific files]

## Appendices

### Deferred Features and Limitations
- Rich TUI, extensions, packages, global/config-driven skill directories, live skill reload, OAuth, full pi RPC command parity, MCP integration, permission prompts, native Windows shell process-tree termination semantics, tool execution streaming updates, subagents, C++26 reflection-generated schema, std::execution senders/receivers, ABI-stable binary distribution, or OS-level sandboxing.
- SDK v1 lacks ABI stability, full pi runtime replacement APIs, tree navigation/compaction, concurrent prompts, cancellation, dynamic extensions, TUI, OAuth/subscription providers, or MCP integration.

**Section sources**
- [README.md:303-308](file://README.md#L303-L308)