---
status: proposed
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

**Baselines — two, not one.** Every **pi** path, size, and line count in this table is measured at pi
`a13d35a742c6ef8462812a28fbe1d8c8b7431c32` (`v1.0.0`) with `git ls-tree`/`git cat-file`. Every **Pike**
figure is measured in the Pike tree at `54c736a0d`, where it appears. The two are never mixed: pi figures and
Pike figures are read against their own baseline, as stated here.

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
| `packages/mcp` (whole package) | 20 files / 3,179 LOC (`client.ts` 21KB, `protocol/`, `transports/`, `oauth/`) | **No decision.** Whether external MCP server connectivity is in Pike's product boundary has not been decided. |
| `packages/codemode` (whole package) | 11 files / 1,655 LOC (`declarations.ts` 13KB, `runtime/`) | **No decision.** Tied upstream to grammar tool-call machinery that Pike already carries as Deferred. |
| Image generation (in-package capability, `packages/ai`) | `packages/ai/src/image-models.ts` (50 lines), `images-api-registry.ts` (53), `images.ts` (26) | **No decision.** Pike has image *input* handling (`ImageInput.cpp`); upstream image *generation* is a separate outbound API surface. |
| Classifier models (in-package capability) | `packages/ai/src/types.ts:1161` (`ModelTypeMap.classifier: ClassifierModel<ClassifierApi>`), `models.ts` (`classify()` declarations at :228/:348/:966), `api/llama-cpp-classify.ts` (458 lines) + `.lazy.ts` (6), `coding-agent/src/core/model-registry.ts:77` (`findOfType("classifier", …)`) | **No decision.** Scope is the classifier model kind only: at this baseline `ModelTypeMap` has exactly `chat`, `image`, and `classifier`. Pike has no `ClassifierModel`, `classify()`, or `findOfType` equivalent. |
| Durable execution layer (in-package capability of `packages/durable`) | scheduler 1,337 · generation 677 · output 288 · view 237 · submissions 207 · task-graph 222 · live 175 · inbox 132 · registry 114 · define 44 — the 10 listed files are 3,433 lines; `packages/durable` totals 15,483 | **No decision.** Subset membership not yet ruled per capability. |
| SQLite session store (in-package capability) | `packages/durable/src/storage/sqlite/` — `database.ts` 38 · `index.ts` 8 · `migrations.ts` 125 · `node.ts` 210 · `storage.ts` 870 = **1,251 lines** | **No decision.** Subset membership not yet ruled per capability. |
| Session transactions | `packages/durable/src/session/transaction.ts` (1,023 lines) | **No decision.** Subset membership not yet ruled per capability. |
| Extension/registry system | `packages/durable/src/harness/define.ts` (44), `harness/registry.ts` (114); `defineExtension` / `createRegistry` / `wrapTool` | **No decision.** Subset membership not yet ruled per capability. |
| `streamProxy` (server-side LLM forwarding, server-held auth) | `packages/agent/src/proxy.ts` 406 lines | **No decision.** Subset membership not yet ruled per capability. |
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
  future proposal to add **MCP or codemode finds an explicitly undecided entry** — the discussion
  starts there rather than being restarted, and no reader can cite this ADR as having excluded them.
- The prompt reference defect is recorded as a **finding with an open remedy**, so it is neither
  silently fixed nor silently ratified.
- MCP and codemode are recorded as **undecided**, so nobody can later cite this ADR as having
  excluded them.

## References

- [ADR 0036](0036-own-the-scoped-pi-coding-agent-application-layer-capabilities-for-the-three-provider-paths.md) — one-layer scope classification precedent
- [ADR 0053](0053-replace-pi-parity-authority-with-the-product-architecture-contract.md) — per-capability product decisions
- `src/coding_agent/ModelConfig.hpp:49` — the existing Radius exclusion
- `src/coding_agent/prompt/SystemPromptBuilder.cpp:124-131` — the prompt reference list
- `src/coding_agent/AgentSessionExecution.cpp:381-387` — `docsPath` resolution