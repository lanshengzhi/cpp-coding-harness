---
status: accepted
---

# Retire cloud provider adapters and delegate model routing to the local gateway

Spec [#1026](https://github.com/lanshengzhi/cpp-coding-harness/issues/1026) proposed simplifying
`cch_ai` by delegating upstream provider routing, credential management, token refresh, and
protocol adaptation to a local LLM router. The product now executes every model request against
the local Magpie gateway (`http://127.0.0.1:3425`) on one OpenAI Chat Completions SSE wire path.
This record supersedes the direct-cloud-provider architecture of ADR 0029, ADR 0033, ADR 0047,
ADR 0055, and ADR 0063; the follow-on records are ADR 0069 (stream lifecycle), ADR 0070
(support/MCP relocation), ADR 0071 (product surface), ADR 0072 (single-router Models), ADR 0073
(gateway operations in Models), and ADR 0074 (one adapter contract).

## Scope

This record adopts only the model-execution decisions of #1026:

- Retire the embedded model catalog and its generator (`DefaultModelsJson`, the Python catalog
  generator, and the pi-ai catalog parity fixtures tied to it).
- Retire vendor OAuth flows, per-provider credential stores, the Provider registry, and the
  multi-adapter wire specializations from `cch_ai`.
- Lock model execution to one OpenAI Chat Completions adapter over the local gateway.

## Excluded decisions

Two #1026 direction points are **not** adopted by this repository and remain governed by their
existing records:

- **State root and pi interop.** Pike keeps its own XDG Agent Config Directory and its one-time
  `pike import`; `PI_AGENT_DIR` and shared `~/.pi/agent` state are not introduced
  ([ADR 0058](0058-fix-the-product-state-root-under-xdg-in-pi-json-shapes.md) stands).
- **Terminal toolkit and Native TUI internals.** Main's terminal restoration, fullscreen default,
  rendering, viewport, theme, progress, clipboard, and editor behavior are preserved; the
  gateway migration adapts the Native TUI to the new model interface rather than replacing it
  ([ADR 0067](0067-align-the-linux-tui-toolkit-and-native-tui-with-pi-v1-0-4.md) stands).

## Decisions

- `cch_ai` owns one gateway connection: discovered catalog values, request authentication and the
  single Chat Completions stream contract. No static fallback catalog and no Provider registry
  substitute for discovery.
- Model values are credential-free and destination-free: identity and capability only, without
  `base_url` or typed `compat` (ADR 0073, ADR 0074).
- Model-provider OAuth and credential persistence retire. MCP OAuth remains functional and
  separately persisted, including the one-way migration of legacy records from `auth.json`
  (ADR 0070, ADR 0072).
- Live model requests go to one local gateway root; the default is
  `http://127.0.0.1:3425` and an explicit invocation key precedes a nonempty `MAGPIE_API_KEY`
  before the default `magpie` key (ADR 0073).

## Consequences

- `cch_ai`, its tests, and its fixtures shrink by the retired catalog and adapter families; the
  Gateway Request Authentication path is the only request-credential path.
- Pike requires an OpenAI-compatible local endpoint for upstream model access. Existing
  `models.json` files are ignored by model execution and left untouched; `auth.json` remains the
  legacy MCP import source.
- The supported wire surface is the gateway's Chat Completions contract, so upstream vendor
  behavior differences become the gateway's concern (ADR 0074).
