# Architecture and package graph

Read this for architecture, Owner packages, module structure, Owner Interface headers, dependency direction, provider/tool/session contracts, CMake visibility, physical capabilities, or security boundaries. Accepted ADRs record the rationale; `CODING_STANDARDS.md` records code-level consequences.

Mirror note: the machine-readable authority is the Parity Architecture Manifest (`cmake/parity/manifest.json`); the fail-closed check is the Parity Architecture Gate (`ctest --preset vcpkg -L architecture`). On conflict the manifest wins; this file links, it does not re-decide.

## Capability Owner Packages

The repository has four authoritative Capability Owner Packages and one pi-neutral C++ Support Package (ADR 0039, ADR 0045, `cmake/parity/manifest.json`). The only released product executable is `pike` (ADR 0045).

| Package | Role | Source Root | Interface Root | Authoritative Ownership | Legal Owner Dependencies |
|---|---|---|---|---|---|
| `cch_ai` | owner | `src/ai/` | `<cch/ai/...>` | Model, Provider, authentication, model-stream (ADR 0029, ADR 0040) | none |
| `cch_agent_core` | owner | `src/agent/` | `<cch/agent/...>` | Agent loop, agent harness, and Tool behavior (ADR 0005, ADR 0039) | `cch_ai` |
| `cch_tui` | owner | `src/tui/` | `<cch/tui/...>` | Reusable terminal, input, rendering, TUI toolkit (ADR 0025) | none |
| `cch_coding_agent` | owner (private) | `src/coding_agent/` + `src/cli/` | `<cch/coding_agent/...>` | Agent Session, Models Runtime, Native TUI application, CLI, Runtime composition (ADR 0036, ADR 0040) | `cch_agent_core`, `cch_ai`, `cch_tui` |
| `cch_support` | support | `src/support/` | `<cch/support/...>` | Pi-neutral C++ values and mechanics (`AsyncResult`, `Expected`, `JsonValue`, ADR 0046) | none |

The `pike` executable compiles only `src/main.cpp` (role `composition`, owner `cch_coding_agent`) over the `cch_coding_agent` library.

Every production source compiles once; the production graph is acyclic. Every unlisted cross-Owner edge is forbidden and fails closed at configure/test time via the Parity Architecture Gate (`ctest --preset vcpkg -L architecture`).

## Owner seams

One line per package. Headers are the stable pointers; class lists are not enumerated here (they drift with refactors — resolve via `lsp symbols`).

- `cch_agent_core`: Agent loop consumes abstract `Tool` values (`<cch/agent/AgentTool.hpp>`), `ToolRegistry`, and `cch::ai` `MessageVariant` + `ModelStreamFactory`; emits `AgentLifecycleEvent` to weak observers vs strong committer. Harness exposes abstract `AsyncFileSystem` (`harness/FileSystem.hpp`) / `AsyncShell` (`harness/Shell.hpp`); concrete file/shell adapters stay private to `src/agent/harness/`. Built-in tools (`<cch/agent/tools/ToolFactories.hpp>`) depend only on the abstract filesystem/shell. Assembly (binding Harness file/shell to Tools, injecting tools into Agent, wiring Agent events to `SessionStore`) lives outside core in `cch_coding_agent` (`src/coding_agent/runtime/SessionFactory.cpp`, `SessionEventCommitment`).
- `cch_ai`: owns Model, Provider, authentication, and model-stream (`<cch/ai/...>`); wire adapters and OAuth flows stay private under `src/ai/`.
- `cch_tui`: owns reusable terminal, input, rendering, and TUI toolkit (`<cch/tui/...>`); the product TUI lives in `cch_coding_agent`.
- `cch_coding_agent`: repository-private composition of all Owner packages into `pike` (Agent Session, Models Runtime, prompt, Native TUI application, CLI from `src/cli/`); sole assembly point `SessionFactory`.
- `cch_support`: pi-neutral C++ values and mechanics only (`<cch/support/...>`); owns no Supported Capability, depends on no Capability Owner Package.

## Passive value contracts

Data crossing Owner Interfaces is passive value state. Use aggregate-friendly `struct`, `std::variant`, `std::expected`, and `cch::support::JsonValue` values.

## Capability seams

Physical capabilities cross explicit seams. Chat clients, stream transports, execution environments, session stores, and tools use narrow interfaces or dependency-heavy implementations hidden behind Owner Interfaces.

## Connection strength

Connection strength is explicit. Ordinary Agent Session, TUI, status, and diagnostic observers are weak. Model-stream delivery and Agent-to-Session commitment are separately named strong, awaited, backpressured connections. Stored operations on both paths remain move-only; copying must be an explicit contract rather than an accidental `std::function` requirement. See ADR 0040.

## Local generic machinery

Generic and serialization machinery stays local. Glaze DTOs, schema conversion, visitors, parsing helpers, and similar machinery belong in serialization or implementation layers rather than Owner Interfaces.

## Security and containment

Shell, file, environment-variable, provider, and session changes preserve workspace containment, secret redaction, output truncation, and the documented “not a sandbox” boundary.

## Retired surfaces

Keep the clean end state: the legacy synchronous tool surface, `util::Result`, Boost.JSON domain contracts, `src` as a public include surface, and compatibility-only empty flags remain absent.

## Authoritative ADR index

Minimal routing set; the full rationale lives in `docs/adr/`.

| Topic | ADR | Core Decision |
|---|---|---|
| Package graph & Parity Gate | [ADR 0039](../adr/0039-own-the-capability-owner-package-graph-and-parity-architecture-gate.md) | Four Owner packages, single static libraries, fail-closed Parity Gate |
| Async operations & Runtime | [ADR 0040](../adr/0040-own-asynchronous-operations-and-the-serialized-runtime-lifecycle.md) | Move-only `AsyncResult`, `RuntimeRoot`, admission lanes, serialized execution domains |
| Shared message variant | [ADR 0005](../adr/0005-keep-provider-and-product-messages-in-their-owning-modules.md) | `cch_ai` owns `MessageVariant`; `cch_agent_core` consolidates agent & harness |
| Released product identity | [ADR 0045](../adr/0045-name-the-released-runtime-pike-and-preserve-owner-package-names.md) | Single released product is `pike`; Owner packages preserve internal names |
