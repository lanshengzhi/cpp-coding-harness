---
status: accepted
---

# Streamline Pike for the local Magpie gateway

Following [ADR 0068](0068-retire-cloud-provider-adapters-and-delegate-routing-to-magpie.md) and
[ADR 0070](0070-evict-non-ai-infrastructure-from-cch-ai-to-support-and-mcp.md), Pike is dedicated
exclusively to local execution in synergy with a local Magpie gateway instance
(`127.0.0.1:3425`). This record retires the multi-vendor authentication surface and static
`models.json` catalog maintenance without carrying backward-compatibility debt or fallbacks. The
Native TUI keeps main's frontend implementation ([ADR 0067](0067-align-the-linux-tui-toolkit-and-native-tui-with-pi-v1-0-4.md)):
this record changes which model surface the TUI drives, not its rendering, clipboard, dock,
fullscreen, progress, editor, or terminal restoration behavior. The state root remains the XDG
root of [ADR 0058](0058-fix-the-product-state-root-under-xdg-in-pi-json-shapes.md).

## Decisions

- **Retire interactive provider login/logout flows, their dialogs, and the top-level
  commands.** `AuthFlowController`, `LoginDialog`, `LoginPresentation`, `OAuthSelector`, and
  their tests are deleted. The Native TUI carries no `/login` or `/logout` command: Pike binds
  to the loopback gateway and requires no provider login, so those spellings follow the
  unknown-slash pass-through and become ordinary Agent Prompts. MCP authentication keeps its own
  `/mcp login` and `/mcp logout` subcommands.
- **Omit a `/gateway` command.** The gateway diagnostics command from the source design is not
  required by this product: startup discovery, `/reload`, and `/model` surface gateway state and
  diagnostics. The autocomplete catalog does not advertise it.
- **Streamline authentication to zero-state gateway credentials.** Effective Request
  Authentication is the explicit invocation key, else a nonempty `MAGPIE_API_KEY`, else the
  default `magpie` key ([ADR 0073](0073-concentrate-gateway-operations-in-models.md)). Pike does
  not read or write model-provider credentials in `auth.json`.
- **Dynamic model catalog from the gateway with no fallbacks.** `/v1/models` is queried on
  startup and during `/reload`; the response populates passive `ai::Model` values. Static
  `models.json` providers and hard-coded provider defaults do not participate. Discovery failure
  clears the catalog and reports an actionable diagnostic; there is no static model fallback.
- **Default model resolution.** Session assembly selects the configured settings default model
  when it is in the discovered catalog, then `MAGPIE_DEFAULT_MODEL`, then the first catalog
  model, and finally the concrete unknown `kDefaultModel`, whose stream fails through the normal
  gateway contract.
- **Gateway observability and routing.** The HTTP transport identifies as `magpie-pike/0.1`, and a
  session-scoped request carries `X-Magpie-Session` (ADR 0074).
- **Retire the obsolete tests** that exercised the deleted login dialogs and credential
  composition.

## Consequences

- The setup is zero-configuration: running `magpie serve` makes the configured models available
  without touching `models.json`.
- Pike sessions are attributed in the gateway by the shared session header and the product user
  agent.
- Independent cloud-provider login flows and backward-compatibility shims for `auth.json` /
  `models.json` model execution are not supported. MCP OAuth keeps its own flow and
  `mcp-auth.json` state.
