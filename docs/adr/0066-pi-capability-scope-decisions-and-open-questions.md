---
status: accepted
---

# Pi capability scope decisions and open questions

## Context

[ADR 0053](0053-replace-pi-parity-authority-with-the-product-architecture-contract.md) made pi
*evidence* rather than design authority, and made "matching pi semantics" a per-capability product
decision. That is the right model, but it only pays off if the resulting decisions are **findable**.
Today they are not: a reader asking "why does Pike have less than pi?" must grep source comments and
changelog prose, and a capability with no record at all is indistinguishable from one nobody has
considered.

This ADR follows the pattern of [ADR 0036](0036-own-the-scoped-pi-coding-agent-application-layer-capabilities-for-the-three-provider-paths.md),
which recorded one layer's scope classification in a single document. It collects the capabilities
that pi provides and Pike does not, so the answer lives in one place.

It deliberately separates **owner decisions** from **findings that are not yet decided**. A decision
without a decider is worse than no record, because it reads as settled.

## Product principle: Pike is a subset, and within it behaviour matches pi

Stated by **@lansy, 2026-10-05**. Under it, @lansy holds the per-capability membership rulings; any
ruling made through an explicitly authorised representative is recorded as that representative's
decision, attributed to it and not to the owner.

> **Pike implements a functional subset of pi. For the subset Pike does implement, the behaviour a user
> experiences is the same as pi's.**

Pike 实现 pi 的功能子集；在 Pike 已声明 Supported 的范围内，用户可见行为与基线 pi 一致（Semantic Parity）；子集之外逐项记为 Deferred，未裁决的记为 No decision，不存在默示对齐。

Owner 原话“当前 pike 所实现功能全面对齐 ../pi”（2026-10-06）经 grill 澄清为上述子集内一致含义，不作零 Deferred 解读；“全面对齐”四字不进入正文断言。

This is the rule per-capability membership will be decided against, and it settles a distinction the
earlier drafts kept circling:

- **A capability inside the subset** must behave as pi does for the user. An internal mechanism may
  differ — that is permitted, and the user-visible result may not.
- **A capability outside the subset** is not implemented. That is a decision, and it is recorded as
  Deferred — distinct from "nobody has decided".
- **"Not decided"** means subset membership has not been settled for that capability.

**This rule does not by itself determine any individual capability's membership.** It states how an
included capability must behave; it does not enumerate which capabilities are included. Per-capability
membership remains with the owner, or with a representative the owner has explicitly authorised for
that item; the rows below stay `No decision` until such a ruling is recorded.

## Owner decisions

Decided by **@lansy, 2026-10-04**. The owner stated the outcome, not the reasoning: for the logo
the instruction was "不做 LOGO", and for Radius "Radius oauth 也不做". **The rationale column below
is analysis supplied by @Cindy, not owner-stated reasoning**, and is marked as such so a later reader
does not attribute it to @lansy.

| Capability | pi source | Pike status | Rationale (analysis, not owner-stated) |
|---|---|---|---|
| Animated pi logo / wordmark | `packages/coding-agent/src/modes/interactive/components/pi-logo-animation.ts` (1209), `pi-logo-animation.lazy.ts` (14), `pi-logo.ts` (40) — 1263 total | **Not implemented** | Branding. |
| Radius OAuth provider | `packages/ai/src/bun-oauth.ts:23` (`radius: createRadiusOAuth`), `packages/ai/src/env-api-keys.ts:99` (`radius: "RADIUS_API_KEY"`) | **Out of the supported subset** | Already decided and implemented at `src/coding_agent/ModelConfig.hpp:49` — `oauth: "radius"` is excluded. Scope note: the provider definition and its credential entry exist upstream, and its login selector is reachable only inside the `/login` flow. Pike's supported subset carries credential configuration but excludes OAuth definitions. |

### Extension Tool Source foundation — spec #865

Decided **2026-10-06**, authorized by the spec flow of
[#865](https://github.com/lanshengzhi/cpp-coding-harness/issues/865): the owner ruling was recorded through
the repository's explicitly authorised spec flow, so it is attributed to that spec rather than to a
named individual. This ruling covers **this slice's scope only**.

| Capability | pi source | Pike status | Scope of this ruling |
|---|---|---|---|
| Extension Tool Source foundation (loader / runner / registry) | `packages/coding-agent/src/core/extensions/loader.ts`, `runner.ts`, `registerTool` in `core/extensions/types.ts` at `7c10bd43` (`v1.0.4`) | **In the supported subset (minimal foundation, #867)** | A `cch_coding_agent` loader / runner / registry that converts an extension-provided tool into a `cch::agent::Tool` and registers it in the session's existing `ToolRegistry` at assembly time, with the Agent's execution path reused unchanged. It opens no MCP server, runs no codemode script, adds no CLI flag, and rules on no other capability in this ADR. |

The boundary is enforced by the Product Architecture Contract rule
`extension-tool-source-no-agent-execution-reach-through` (diagnostic `PARITY-8005`,
`cmake/parity/manifest.json`): sources under `src/coding_agent/extensions/` reach the Agent only
through the Tool Owner Interface (`<cch/agent/AgentTool.hpp>`, `<cch/agent/ToolRegistry.hpp>`) and
may not include the Agent's execution internals or construct an Agent.

### MCP stdio server — spec #865 (#869)

Decided **2026-10-06**, authorized by the spec flow of
[#865](https://github.com/lanshengzhi/cpp-coding-harness/issues/865): the owner ruling was recorded
through the repository's explicitly authorised spec flow, so it is attributed to that spec rather
than to a named individual. This ruling covers **the MCP stdio transport slice only**; every other
`packages/mcp` capability stays `No decision`.

| Capability | pi source | Pike status | Scope of this ruling |
|---|---|---|---|
| MCP server over **stdio** | `packages/mcp/src/transports/stdio.ts`, `client.ts`, `protocol/jsonrpc.ts` at `7c10bd43` (`v1.0.4`) | **In the supported subset (stdio transport, #869)** | A `cch_coding_agent` client that launches one configured MCP server over stdio, speaks newline-delimited compact JSON-RPC (`initialize` → `notifications/initialized` → `tools/list` → `tools/call`), and converts each advertised tool into an extension Tool (`mcp__<server>__<tool>`) through the #867 Extension Tool Source seam. The child is a long-lived per-session process group torn down close-stdin → grace → SIGTERM → SIGKILL; a per-call failure follows the Agent's existing per-call isolation. This ruling covers **stdio only**: **OAuth** (later decided in the #875 ruling below), **resource tools**, **server-management persistence (`mcp.json`)**, and **exposure policy (`codemode`/`deferred`/`hidden`)** remain `No decision` (the **streamable-HTTP transport** was later decided in the #873 ruling below), and no codemode, CLI flag, or extension machinery is opened here. |

The slice reuses the Agent's ordinary execution path and adds no Owner Interface; the transport and
conversion stay private under `src/coding_agent/mcp/`.

| MCP server over **streamable HTTP** | `packages/mcp/src/transports/streamable-http.ts` at `7c10bd43` (`v1.0.4`) | **In the supported subset (streamable-http transport, #873)** | A `cch_coding_agent` client that reaches one configured MCP server over the streamable-HTTP transport: one JSON-RPC POST per request with `accept: application/json, text/event-stream`, an `application/json` or `text/event-stream` response, and an `mcp-session-id` captured at `initialize` and echoed on later requests, converting each advertised tool into an extension Tool (`mcp__<server>__<tool>`) through the #867 Extension Tool Source seam. The HTTPS round trip reuses the existing outbound client transport (`ai::providers::StreamTransport`, ADR 0054) rather than opening a second HTTP stack, and is **TLS-only**: a non-`https://` URL is rejected at registration and no redirect is ever followed, so the connection cannot be downgraded to plaintext. This ruling covers **the transport's request path only**: the server-to-client GET stream, **OAuth** (later decided in the #875 ruling below), **resource tools**, **server-management persistence (`mcp.json`)**, and **exposure policy (`codemode`/`deferred`/`hidden`)** remain `No decision`, and no codemode, CLI flag, or extension machinery is opened here. |

The slice reuses the Agent's ordinary execution path and adds no Owner Interface; the transport and
conversion stay private under `src/coding_agent/mcp/`. The JSON-RPC framing and `initialize`
validation shared with the stdio client are factored into `src/coding_agent/mcp/McpProtocol.hpp`,
and the tool conversion is written once against the transport-independent `McpServerConnection`.

### Codemode declarations and source loading — spec #865

Decided **2026-10-06**, authorized by the spec flow of
[#865](https://github.com/lanshengzhi/cpp-coding-harness/issues/865), in the same attributed manner as the
foundation ruling above. This ruling covers **this slice's scope only**: declarations and loading,
not execution.

| Capability | pi source | Pike status | Scope of this ruling |
|---|---|---|---|
| Codemode declarations and source loading | `packages/codemode/src/declarations.ts`, `source.ts`, `types.ts` at `7c10bd43` (`v1.0.4`); `packages/coding-agent/src/extensions/codemode/` | **In the supported subset (declarations / loading, #870)** | A project-local declaration surface — `<workspace>/.pi/codemode/*.json` carrying `{name, description, inputSchema?, source}`, with the `source` `*.js` in pi's `source.ts` format — loaded through the Extension Tool Source foundation and exposed on the tool surface. An invalid declaration is a typed error with no silent skip, and loading never executes: a call returns an explicit "not wired until #874" error. It runs no script, opens no MCP server, adds no CLI flag, and rules on no other capability. |

**Intentional divergence:** pi v1.0.4 has no on-disk codemode declaration format. Codemode scripts
there are written inline in the model's `codemode` tool call, and "exposure" is an in-memory
registration concept, so there is no project-local codemode source directory to follow. Pike
defines this minimal format anyway so a project can declare script tools before the sandbox exists;
it follows pi where a shape exists (the `source.ts` script format and the `CodemodeTool`
model-facing fields). The format is documented for users at `fixtures/codemode/README.md`. **The
codemode sandbox and script execution remain undecided until #874.**

### MCP OAuth through the existing credential semantics — spec #865 (#875)

Decided **2026-10-06**, authorized by the spec flow of
[#865](https://github.com/lanshengzhi/cpp-coding-harness/issues/865), in the same attributed manner as
the rulings above. This ruling covers **the MCP OAuth credential slice only**.

| Capability | pi source | Pike status | Scope of this ruling |
|---|---|---|---|
| MCP server **OAuth** | `packages/mcp/src/oauth/` (`flow.ts`, `discovery.ts`, `callback.ts`, `provider.ts`) and `packages/coding-agent/src/extensions/mcp/oauth.ts` at `7c10bd43` (`v1.0.4`) | **In the supported subset (credential semantics, #875)** | The authorization-code + PKCE S256 flow, refresh-token rotation, and request-time token attachment of one configured MCP server, expressed entirely through the **existing** credential story: an `ai::OAuthCredential` stored in the shared `auth.json` through the coding-agent-owned AuthStorage under the provider id `mcp__<server>`, an OAuth-shaped login hook reusing the existing `AuthInteraction` presentation and the shared `ai::auth` PKCE/callback helpers, and per-request token resolution in the streamable-http transport. A `401` or an `invalid_grant` refresh is an explicit re-login error (never a silent unauthenticated request and never a retry loop); a server configured without OAuth is sent no credentials. |

**Intentional divergence:** pi stores MCP OAuth state in a separate `<agent-dir>/mcp-auth.json`; the
spec binds Pike to the existing AuthStorage/`auth.json` semantics instead, so there is no second
credential store and the user learns no second credential story. **Deferred by this ruling:** RFC 9728
authorization-server discovery and dynamic client registration (the endpoints and the pre-registered
`client_id` are configuration), and **the user-visible sign-in trigger** — nothing in #875 opens a
browser flow on its own; the trigger (a future `/mcp` surface or CLI command) is a follow-up decision,
and #876's `mcp.json` server entry gaining an `auth: "oauth"` block is the natural handshake point.
MCP **resource tools**, the server-to-client GET stream, and exposure policy remain `No decision`.

The transport and credential resolution stay private under `src/coding_agent/mcp/` and add no Owner
Interface.

## Finding: not a decision

The following was found during the Phase 1 capability inventory. **It is not an owner decision and
is not recorded as one.** The defect is objective; the remedy is a product choice that is still open.

**Pike's system prompt references 10 files that do not exist.** `src/coding_agent/prompt/SystemPromptBuilder.cpp:124-131`
lists 11 `docs/*.md` paths; Pike's `docs/` contains `agent-lifecycle`, `build-performance-baseline`,
`keybindings`, `runtime-capacities`, and `usage`. Only `docs/keybindings.md` resolves. The prompt also
directs the model to `examples/…`, and no `examples/` directory exists.

These are not user-supplied paths: `src/coding_agent/AgentSessionExecution.cpp:387` sets
`prompt_options.docsPath = std::string{kSourceDir} + "/docs"`, and the adjacent comment records that
Pike resolves its own source tree rather than pi's package location. The prompt then instructs the
model to read those files, so the model is directed at missing paths in the tree Pike points it to.

**Proposed principle** (owner ruling requested): *a system prompt must not reference capabilities or
paths the product does not provide.*

**Open remedy choice**, none of these chosen here:

1. Remove the missing references, narrowing what the prompt claims about Pike's capabilities.
2. Remap each reference to documentation Pike actually ships, and drop or repoint the rest.
   **Candidate set, per the reader-axis review — not an owner selection.** If the owner chooses
   this remedy, the reviewed candidates are `docs/usage.md`, `docs/keybindings.md`,
   `docs/agent-lifecycle.md`, `docs/runtime-capacities.md`, `docs/agents/architecture.md`; the other
   five files under `docs/agents/` and `docs/build-performance-baseline.md` are excluded as
   agent-workflow or maintainer-facing. **The owner has not chosen a remedy.**
3. Author the ten missing documents, which asserts a documentation surface Pike does not currently
   commit to maintaining.

`examples/…` is a separate axis from `docs/…` and must be decided with the same question: the
directory does not exist at all, and `prompt_options.examplesPath` resolves it the same way
`docsPath` resolves `docs/`. Any remedy has to state whether `examples/` is coming or goes.

**Scope of an Examples removal — path literals and the bare word.** `SystemPromptBuilder.cpp`
mentions `examples` in four places and two forms: the `- Examples: {examplesPath}` header (`:118-119`),
path-shaped references (`:121-122`, `:125` contain `examples/`), and a **bare word with no slash** at
`:132` — "read the docs and examples". A check that only matches slash-bearing paths would pass while
the prompt still sends the model to a directory tree that does not exist.

**Acceptance, if Examples are removed:** no occurrence of the standalone word `examples`
(case-insensitive, slash or not) anywhere in the prompt source **or in any rendered golden**; and for
every `docs/<path>` retained in the three goldens (`default`, `empty-tools`, `custom`), that file
exists in the tree. Verification was run on the tree at the commit cited in Baselines; the source has
6 occurrences and the goldens 7 each in `default` and `empty-tools`, so source-only counting is not
sufficient coverage.

**Superseded check, resolved.** `AgentSessionExecution.cpp:381-385` has an `#ifdef CCH_SOURCE_DIR`
branch that leaves `kSourceDir` empty, which would make `docsPath` resolve to `/docs`. Checked for
reachability: `CCH_SOURCE_DIR` is a `PRIVATE` compile definition on `cch_coding_agent`
(`cmake/targets/CodingAgent.cmake:68-71`, whose comment records that the documentation block resolves
the binary's own docs paths from the source tree), and `AgentSessionExecution.cpp` is compiled only as
part of that target — no test target recompiles it standalone. **The empty branch is therefore not
reachable in a supported build**, and the fallback is defensive only. No finding here; noted so a
later reader does not re-raise it.

## Open question: no decision exists

**Baselines — three pi revisions, each named with its full 40-character SHA.** Every **pi** path, size, and line count in this table is measured at pi
`a13d35a742c6ef8462812a28fbe1d8c8b7431c32` (`v1.0.0`) with `git ls-tree`/`git cat-file`, unless a row names `7c10bd43` explicitly. Every **Pike**
figure is measured in the Pike tree at `54c736a0d`, where it appears. The two sides are never mixed: pi figures and
Pike figures are read against their own baseline, as stated here.

Revision roles (ADR 0065 Named Baseline rule — selection by name, never by floating HEAD):
`f07218c4d4bbc12bef056a7058c3dd49dfe41abe` (`v0.87.1`, baseline `pi-v0.87.1`) is the captured evidence bundle;
`a13d35a742c6ef8462812a28fbe1d8c8b7431c32` (`v1.0.0`, baseline `pi-v1.0.0`) is the Phase 1 inventory reference (bundle not yet captured);
`7c10bd4337495ee613f2224843ecdf349b80d1df` (`v1.0.4`, baseline `pi-v1.0.4`) is registered in `fixtures/pi-ai/baselines.json` with no captured bundle yet —
tools selecting it fail loudly on the missing bundle rather than falling back, per ADR 0065.
The `v1.0.0` → `v1.0.4` delta (292 files, +17238/−4936) has not been per-capability inventoried; only the whole new packages below are recorded as rows.

The following are pi packages and capabilities — **two whole packages and several in-package
capabilities** — for which Pike has **no counterpart of that capability**, and for which **no decision
is recorded anywhere**: not in the ADRs, not in source. They are recorded here so they are not
mistaken for settled exclusions.

Note the scope of that claim: it is **per capability**, not per package or per Owner. The durable
execution layer below is the clearest case — Pike *does* have a harness (`src/agent/harness/`), and
what it lacks is durable's persistent task/scheduling layer. Read the Size and Status cells, not this
paragraph, for whether a counterpart exists.

Note on mapping: pi moved its harness out of `packages/agent` into `packages/durable` during
`f07218c4..a13d35a74` (110 deleted, 4 modified); `packages/agent/src/harness` is empty at this baseline.
Pike's counterpart is therefore `src/agent/harness/`, not that path.

| pi package or capability | Size | Status |
|---|---|---|
| `packages/mcp` (whole package) | 20 files / 3,179 LOC (`client.ts` 21KB, `protocol/`, `transports/`, `oauth/`) | **Decided in part.** The **stdio** and **streamable-HTTP** transports, the `initialize`/`tools/list`/`tools/call` client, MCP-tool-to-extension-Tool conversion, and **OAuth credential semantics** are an owner decision — see the Owner decisions section above (spec #865, #869, #873, and #875). The rest of the package — the **server-to-client GET stream**, **resource tools**, **server-management persistence**, and **exposure policy** — remains **No decision.** |
| `packages/codemode` (whole package) | 11 files / 1,655 LOC (`declarations.ts` 13KB, `runtime/`) | **Decided in part.** Project-local codemode declarations and source loading are an owner decision — see the Owner decisions section above (spec #865, #870). The sandboxed `runtime/` and `wasm.ts` execution path remains **No decision** until #874. |
| Image generation (in-package capability, `packages/ai`) | `packages/ai/src/image-models.ts` (50 lines), `images-api-registry.ts` (53), `images.ts` (26) | **No decision.** Pike has image *input* handling (`ImageInput.cpp`); upstream image *generation* is a separate outbound API surface. |
| Classifier models (in-package capability) | `packages/ai/src/types.ts:1161` (`ModelTypeMap.classifier: ClassifierModel<ClassifierApi>`), `models.ts` (`classify()` declarations at :228/:348/:966), `api/llama-cpp-classify.ts` (458 lines) + `.lazy.ts` (6), `coding-agent/src/core/model-registry.ts:77` (`findOfType("classifier", …)`) | **No decision.** Scope is the classifier model kind only: at this baseline `ModelTypeMap` has exactly `chat`, `image`, and `classifier`. Pike has no `ClassifierModel`, `classify()`, or `findOfType` equivalent. |
| Durable execution layer (in-package capability of `packages/durable`) | scheduler 1,337 · generation 677 · output 288 · view 237 · submissions 207 · task-graph 222 · live 175 · inbox 132 · registry 114 · define 44 — the 10 listed files are 3,433 lines; `packages/durable` totals 15,483 | **No decision.** Subset membership not yet ruled per capability. |
| SQLite session store (in-package capability) | `packages/durable/src/storage/sqlite/` — `database.ts` 38 · `index.ts` 8 · `migrations.ts` 125 · `node.ts` 210 · `storage.ts` 870 = **1,251 lines** | **No decision.** Subset membership not yet ruled per capability. |
| Session transactions | `packages/durable/src/session/transaction.ts` (1,023 lines) | **No decision.** Subset membership not yet ruled per capability. |
| Extension/registry system | `packages/durable/src/harness/define.ts` (44), `harness/registry.ts` (114); `defineExtension` / `createRegistry` / `wrapTool` | **Decided in part.** The coding-agent Extension Tool Source foundation (loader / runner / registry) is an owner decision — see the Owner decisions section above (spec #865, #867). This row's `packages/durable` `defineExtension` / `createRegistry` / `wrapTool` machinery remains **No decision.** |
| `streamProxy` (server-side LLM forwarding, server-held auth) | `packages/agent/src/proxy.ts` 406 lines | **No decision.** Subset membership not yet ruled per capability. |
| `packages/env` (whole package, new in v1.0.0→v1.0.4) | 36 files / ~7,020 TS+Rust LOC (`@earendil-works/pi-env`, SSH bootstrap, daemon, remote ExecutionEnv client) at `7c10bd43` | **No decision.** Whether remote execution environments are in Pike's product boundary has not been decided. |
| `packages/server` + `packages/protocol` + `packages/telemetry` (new package group in v1.0.0→v1.0.4) | 58 files / ~5,674 LOC at `7c10bd43` | **No decision.** Subset membership not yet ruled; per-capability deltas in durable watch/shell, MCP OAuth, codemode, and tui-alt-screen within the same range are likewise not yet inventoried. |
Note the distinction the other tables make necessary: recording "the prompt does not reference
MCP" is **not** a decision that MCP is unsupported. The two are independent, and only the first is
currently observable. Likewise, Pike's image *input* support says nothing about image *generation*,
and image generation is independent of classifier support — the rows above are separate decisions and
may be scoped differently. Only `packages/mcp` and `packages/codemode` are whole packages; the rest
are capabilities or paths, absent at the capability level, not at the package level.

All pi sizes above are measured at the pi baseline; all Pike sizes at the Pike commit stated above.

## Consequences

- "Why does Pike have less than pi?" has one entry point instead of a grep across source comments.
- A future proposal to add the logo or Radius finds a recorded owner decision and separately
  attributed analysis. A
  future proposal to add **MCP, or codemode's execution path, finds an explicitly undecided entry** — the discussion
  starts there rather than being restarted, and no reader can cite this ADR as having excluded them.
- The prompt reference defect is recorded as a **finding with an open remedy**, so it is neither
  silently fixed nor silently ratified.
- MCP is recorded as **undecided**, and codemode's execution path with it, so nobody can later cite
  this ADR as having excluded them; codemode declarations/loading are now an attributed owner decision
  (spec #865, #870).
- The Extension Tool Source foundation is now an **attributed owner decision** (spec #865, #867);
  its row states the slice's scope so a later reader cannot read it as having ruled on MCP, codemode,
  or the `packages/durable` extension machinery.
- The MCP **stdio** server slice is now an **attributed owner decision** (spec #865, #869), so it is
  no longer an undecided row; the rest of `packages/mcp` and codemode stay **undecided**, so nobody
  can later cite this ADR as having excluded them.
- The MCP **streamable-http** transport slice is now an **attributed owner decision** (spec #865,
  #873), TLS-only and reusing the existing outbound client transport (ADR 0054); the server-to-client
  GET stream and the rest of `packages/mcp` stay **undecided**.
- `pi-v1.0.4` (`7c10bd4337495ee613f2224843ecdf349b80d1df`) is registered by name with no captured bundle; a future capture is new evidence per ADR 0065 and needs its own step, not a silent edit.
- A future proposal to add a remaining MCP capability (server-to-client stream, OAuth, resources, persistence) or
codemode execution, or any other **No decision** row, starts from those rows and needs its own membership ruling plus implementation record; this ADR claims no such coverage.

## References

- [ADR 0036](0036-own-the-scoped-pi-coding-agent-application-layer-capabilities-for-the-three-provider-paths.md) — one-layer scope classification precedent
- [ADR 0053](0053-replace-pi-parity-authority-with-the-product-architecture-contract.md) — per-capability product decisions
- `src/coding_agent/ModelConfig.hpp:49` — the existing Radius exclusion
- `src/coding_agent/prompt/SystemPromptBuilder.cpp:124-131` — the prompt reference list
- `src/coding_agent/AgentSessionExecution.cpp:381-387` — `docsPath` resolution
- [ADR 0065](0065-bind-pi-ai-evidence-bundles-to-a-named-baseline-registry.md) — Named Baseline registry rule
- `fixtures/pi-ai/baselines.json` — `pi-v1.0.4` registration (`7c10bd43`, bundle not yet captured)