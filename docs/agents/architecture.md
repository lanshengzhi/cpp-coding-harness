# Architecture and package graph

Read this for architecture, Owner packages, module structure, Owner Interface headers, dependency direction, provider/tool/session contracts, CMake visibility, physical capabilities, or security boundaries. Accepted ADRs record the rationale; `CODING_STANDARDS.md` records code-level consequences.

Mirror note: the machine-readable authority is the Parity Architecture Manifest (`cmake/parity/manifest.json`); the fail-closed check is the Parity Architecture Gate (`ctest --preset vcpkg -L architecture`). On conflict the manifest wins; this file links, it does not re-decide.

## Accepted package migration

[ADR 0075](../adr/0075-align-selected-owner-packages-and-linux-capabilities-with-pi-v1-3-0.md)
selects pi-v1.3.0 for non-AI capability alignment and a staged Owner migration, including independent
durable, MCP, codemode and durable-required Chord owners. It also replaces historical behavioral
exemptions for selected capabilities, including extra Agent queue limits and MCP TLS-only/no-redirect
behavior. Its target graph is not yet the configured graph: update the manifest and architecture
tests with each implementation slice. The current tables below describe the configured ownership;
they are not authority to retain placements superseded by that migration. Magpie/XDG and the explicit
AI exclusions remain; do not infer an overall cch_ai baseline advancement.

## Capability Owner Packages

The repository has four authoritative Capability Owner Packages and one pi-neutral C++ Support Package (ADR 0039, ADR 0045, `cmake/parity/manifest.json`). The only released product executable is `pike` (ADR 0045).

| Package | Role | Source Root | Interface Root | Authoritative Ownership | Legal Owner Dependencies |
|---|---|---|---|---|---|
| `cch_ai` | owner | `src/ai/` | `<cch/ai/...>` | Gateway discovery and connection facts, Model catalog, Request Authentication, model-stream (ADR 0040, ADR 0073) | none |
| `cch_agent_core` | owner | `src/agent/` | `<cch/agent/...>` | Agent loop, agent harness, and Tool behavior (ADR 0005, ADR 0039) | `cch_ai` |
| `cch_tui` | owner | `src/tui/` | `<cch/tui/...>` | Reusable terminal, input, rendering, TUI toolkit (ADR 0025) | none |
| `cch_coding_agent` | owner (private) | `src/coding_agent/` (headless core; `src/coding_agent/tui/` belongs to `frontend_tui`) | `<cch/coding_agent/...>` | Agent Session, Models Runtime (ADR 0036, ADR 0040) | `cch_agent_core`, `cch_ai`; `cch_tui` only for its non-`owner` targets (manifest `implementation_owner_dependencies`) |
| `cch_support` | support | `src/support/` | `<cch/support/...>` | Pi-neutral C++ values and mechanics (`AsyncResult`, `Expected`, `JsonValue`, ADR 0046) | none |

The `pike` executable compiles only `src/main.cpp` (role `composition`, owner `cch_coding_agent`) and depends only on the `frontend_cli` implementation target.

### Implementation and composition targets

Owner packages are the authority layer. Repository-private targets that are not Owners build the product on top of them; the manifest records their `role`, and the headless-no-frontend invariant is enforced on these edges rather than on include spelling alone (ADR 0053 addendum; #658).

| Target | Role | Source Root | Owner | Depends On |
|---|---|---|---|---|
| `frontend_tui` | implementation | `src/coding_agent/tui/` | `cch_coding_agent` | `cch_coding_agent`, `cch_agent_core`, `cch_ai`, `cch_tui`, `cch_support` |
| `frontend_cli` | implementation | `src/coding_agent/cli/` | `cch_coding_agent` | `frontend_tui`, `cch_coding_agent`, `cch_agent_core`, `cch_ai`, `cch_tui`, `cch_support` |
| `pike` | composition | `src/main.cpp` | `cch_coding_agent` | `frontend_cli` |

`cch_coding_agent` itself depends on neither frontend: the only `cch_tui` edges in the repository are the two implementation targets'. `pike` is a thin closure — it compiles only the entry point and holds no Owner edge of its own.

Every production source compiles once; the production graph is acyclic. Every unlisted cross-Owner edge is forbidden and fails closed at configure/test time via the Parity Architecture Gate (`ctest --preset vcpkg -L architecture`).

## Owner seams

One line per package. Headers are the stable pointers; class lists are not enumerated here (they drift with refactors — resolve via `lsp symbols`).

- `cch_agent_core`: Agent loop consumes abstract `Tool` values (`<cch/agent/AgentTool.hpp>`), `ToolRegistry`, and `cch::ai` `MessageVariant` + `ModelStreamFactory`; emits `AgentLifecycleEvent` to weak observers vs strong committer. Harness exposes abstract `AsyncFileSystem` (`harness/FileSystem.hpp`) / `AsyncShell` (`harness/Shell.hpp`); concrete file/shell adapters stay private to `src/agent/harness/`. The compaction door is published (`harness/session/Compaction.hpp`, ADR 0062) with its summarization seam typed as `support::AsyncResult`. Built-in tools (`<cch/agent/tools/ToolFactories.hpp>`) depend only on the abstract filesystem/shell. Assembly (binding Harness file/shell to Tools, injecting tools into Agent, wiring Agent events to `SessionStore`) lives outside core in `cch_coding_agent` (`src/coding_agent/runtime/SessionFactory.cpp`, `SessionEventCommitment`).
- `cch_ai`: owns Model values, the passive model catalog, gateway Request Authentication and model-stream (`<cch/ai/...>`). `ai::Models` accepts gateway startup configuration and owns discovery, refresh, status inspection and inference using that connection (ADR 0073); protocol execution and test injection stay private. Model-provider registries, credential stores and OAuth flows are retired (ADR 0068, ADR 0072); MCP owns its independent OAuth values and interaction (ADR 0070).
- `cch_tui`: owns reusable terminal, input, rendering, and TUI toolkit (`<cch/tui/...>`); the product TUI lives in the `frontend_tui` implementation target (`src/coding_agent/tui/`, owner `cch_coding_agent`).
- `cch_coding_agent`: repository-private headless core (Agent Session, Models Runtime); the product frontends are its implementation targets — `frontend_tui` (Native TUI, `src/coding_agent/tui/`) and `frontend_cli` (CLI, `src/coding_agent/cli/`) — and `pike` composes only the latter. Models Runtime retains application model-selection policy and diagnostic wording, privately holds `ai::Models`, and exposes passive model values plus an AI-owned `ModelStreamFactory`; discovery and connection facts belong to cch_ai (ADR 0073). `SessionFactory` remains the sole Session assembly point (ADR 0072).
- `cch_support`: pi-neutral C++ values and mechanics only (`<cch/support/...>`); owns no Supported Capability, depends on no Capability Owner Package. Publishes the bounded/redacted text mechanics and the pi output limiter (`BoundedText.hpp`, `Redactor.hpp`, `OutputLimiter.hpp`, ADR 0062); the Boost.Asio completion bridge stays private (ADR 0046).

### Interactive run boundary

The CLI adapter packs an Interactive Session Run once, combining CLI-owned facts, host capabilities, and the intent to bind an existing Agent Session or defer creation until the boot trust prompt. The Native TUI consumes that run intent and its outcomes rather than an independent configuration-field bundle. The seam and its ownership rules live in [`InteractiveSessionRun.hpp`](../../src/coding_agent/tui/InteractiveSessionRun.hpp); production assembly is in [`AsyncCliRuntime.cpp`](../../src/coding_agent/cli/AsyncCliRuntime.cpp).

## Passive value contracts

Data crossing Owner Interfaces is passive value state. Use aggregate-friendly `struct`, `std::variant`, `std::expected`, and `cch::support::JsonValue` values.

## Capability seams

Physical capabilities cross explicit seams. Chat clients, stream transports, execution environments, session stores, and tools use narrow interfaces or dependency-heavy implementations hidden behind Owner Interfaces.

## Connection strength

Connection strength is explicit. Ordinary Agent Session, TUI, status, and diagnostic observers are weak. Model-stream delivery and Agent-to-Session commitment are separately named strong, awaited, backpressured connections. Stored operations on both paths remain move-only; copying must be an explicit contract rather than an accidental `std::function` requirement. See ADR 0040.

## Local generic machinery

Generic and serialization machinery stays local. Glaze DTOs, schema conversion, visitors, parsing helpers, and similar machinery belong in serialization or implementation layers rather than Owner Interfaces.

## Security and containment

Filesystem access aligns with host user process permissions: all path resolution follows pi's `resolveToCwd` contract uniformly, with no workspace containment, path allowlists, or resolution-scope splits (ADR 0057). Shell, file, environment-variable, provider, and session changes preserve the remaining security policies — secret redaction, output truncation, the bash environment filter, and the no-follow symlink policy with atomic writes — and the documented “not a sandbox” boundary.

## Retired surfaces

Keep the clean end state: the legacy synchronous tool surface, `util::Result`, Boost.JSON domain contracts, `src` as a public include surface, compatibility-only empty flags, and fail-closed workspace containment with its split path-resolution scopes remain absent.

## Authoritative ADR index

Minimal routing set; the full rationale lives in `docs/adr/`.

| Topic | ADR | Core Decision |
|---|---|---|
| Package graph & Parity Gate | [ADR 0039](../adr/0039-own-the-capability-owner-package-graph-and-parity-architecture-gate.md) | Four Owner packages, single static libraries, fail-closed Parity Gate |
| Async operations & Runtime | [ADR 0040](../adr/0040-own-asynchronous-operations-and-the-serialized-runtime-lifecycle.md) | Move-only `AsyncResult`, `RuntimeRoot`, admission lanes, serialized execution domains |
| Shared message variant | [ADR 0005](../adr/0005-keep-provider-and-product-messages-in-their-owning-modules.md) | `cch_ai` owns `MessageVariant`; `cch_agent_core` consolidates agent & harness |
| Released product identity | [ADR 0045](../adr/0045-name-the-released-runtime-pike-and-preserve-owner-package-names.md) | Single released product is `pike`; Owner packages preserve internal names |
| Path resolution & containment | [ADR 0057](../adr/0057-retire-workspace-containment-and-align-path-resolution-with-pi-resolvetocwd.md) | Uniform pi `resolveToCwd` resolution at host-process permissions; workspace containment and scope splits retired |
| Tool-execution rendering | [ADR 0061](../adr/0061-align-tool-execution-rendering-with-pi-v0-87-1.md) | Application-layer Tool Renderer registry keyed by tool name; `read`/`bash` truncation metadata moves from model-visible `content` to structured `details` |
| Selected pi-v1.3.0 migration | [ADR 0075](../adr/0075-align-selected-owner-packages-and-linux-capabilities-with-pi-v1-3-0.md) | Accepted target for selected non-AI capabilities and the staged Owner migration: complete Linux durable/TUI, independent MCP/codemode and durable-required Chord; clause-level supersessions; the manifest changes with implementation |
| Linux TUI toolkit & Native TUI | [ADR 0067](../adr/0067-align-the-linux-tui-toolkit-and-native-tui-with-pi-v1-0-4.md) | Historical complete pi-v1.0.4 target, superseded as target by ADR 0075 (pi-v1.3.0); retained toolkit rendering and frontend product policy; implementation proceeds through #946's native blockers |
| Owner Interface promotion | [ADR 0062](../adr/0062-publish-the-text-limiting-mechanics-and-the-compaction-door.md) | Text-limiting mechanics and the compaction door become Owner Interfaces; `no-agent-private-reach-through` closes the seams in the Gate |
| Local gateway model runtime | [ADR 0072](../adr/0072-narrow-models-to-a-single-local-llm-router.md) | One local gateway is the sole catalog and request destination; model-provider credentials, registries and `models.json` execution are retired (implements [ADR 0068](../adr/0068-retire-cloud-provider-adapters-and-delegate-routing-to-magpie.md)) |
| Gateway operations & stream contract | [ADR 0073](../adr/0073-concentrate-gateway-operations-in-models.md), [ADR 0074](../adr/0074-the-chat-completions-adapter-is-the-gateway-contract.md) | Discovery, key fixation and inference concentrate in `ai::Models`; the Chat Completions adapter is the one wire contract (supersedes ADR 0063) |
