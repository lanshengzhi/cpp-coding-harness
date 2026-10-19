---
status: accepted
---

# Narrow Models to a single Local LLM Router and retire model-provider credentials

> **ADR 0073 amendment:** [Concentrate gateway operations in Models](0073-concentrate-gateway-operations-in-models.md) moves discovery, connection facts and status inspection into cch_ai, removes Model connection addresses, and fixes request authentication at the first gateway request. Models Runtime retains application policy and the private Models lifetime.

[ADR 0068](0068-retire-cloud-provider-adapters-and-delegate-routing-to-magpie.md)
and [ADR 0071](0071-streamline-pike-for-local-magpie-gateway.md) selected a single
Local LLM Router, but the Models Owner Interface still carries Provider installation,
credential persistence, interactive login and OAuth refresh. Models Runtime compensates
with an EmptyCredentialStore and still reads models.json to assemble direct Providers.
The proposed design removes that live graph rather than carrying it under a new name:
Models accepts the gateway's passive model catalog and startup request configuration,
resolves Request Authentication, and produces the existing AI-owned ModelStream.

The user confirmed the complete design during the candidate-1 interview. This
record supersedes only the multi-Provider assembly and model-credential operations
retained by ADR 0047 and ADR 0055; their private Models lifetime and non-secret
stream-factory guarantees remain.

## Decisions

- Pike stops reading model models.json entirely. Existing files, including malformed
  files, do not affect model selection or boot diagnostics. Configured key templates
  and key commands are not evaluated. Files are neither migrated nor deleted. The
  gateway remains the sole production catalog source, with the existing diagnostic
  when discovery fails and no static model fallback.
- Remove ProviderDefinition, ProviderChange, Provider auth hooks and the runtime
  Provider registry. Model catalogs cross the Owner Interface as passive values,
  without authentication callbacks or provider installation/removal commands.
- Remove the Models CredentialStore dependency, model OAuth refresh/login/logout,
  EmptyCredentialStore, unused model AuthStorage, RuntimeApiKeyOverlay, and the
  ModelConfig/ProviderComposer path once their active consumers are replaced. Retire
  authentication-method projections and credential-based availability caches.
- Preserve a startup Runtime API Key Override. Effective Request Authentication uses
  the explicit override, otherwise a nonempty MAGPIE_API_KEY, otherwise the default
  `magpie` key. It is never persisted to auth.json or exposed in Model, messages,
  Session Entries or projection values. Remove unused per-turn key overrides and
  key-removal operations rather than maintaining mutable credential machinery.
- Keep MCP OAuth in cch_coding_agent's MCP module. Its active credential, request-auth
  and interaction values stop importing model-authentication types from cch_ai.
  Preserve mcp-auth.json and the existing one-way migration of legacy MCP records
  from auth.json; removing model credential storage does not remove MCP credentials.
- Allow --api-key with normal default or resumed model selection, without requiring
  an explicit model argument. Remove the independent --provider option. --model
  selects from the gateway catalog, including IDs containing `/`; a vendor-looking
  prefix does not establish a direct cloud Provider. Preserve existing explicit
  `magpie/<gateway-model-id>` selection where model matching already supports it.
- Preserve passive model identity fields in messages and Session Entries. Restore
  a historical model when its recorded identity is available; otherwise retain the
  existing warning and default-model selection. Do not synthesize a mapping from
  a legacy vendor/model pair to a gateway model ID or rewrite historical records.

## Module responsibilities and retained contracts

Models remains the cch_ai module for model operations and Request Authentication.
Models Runtime retains catalog discovery, user settings/default-model policy and
Session-facing presentation in this change; moving discovery into cch_ai is a separate
candidate. Removing Provider assembly must not turn Models Runtime into a second
credential or protocol module.

The existing HTTP production adapter and scripted test adapter remain behind the
private support transport seam. Neutral HTTP and SSE stay in cch_support under
ADR 0070. The Models interface does not expose Boost.Asio, transport configuration
or a generic gateway adapter registry.

Preserve the ModelStream single-consumption, lifetime, cancellation and completion
contracts of ADR 0040 and the lifecycle guarantees of ADR 0069: exactly one terminal
event and one terminal outcome for domain success/failure when event delivery
succeeds, no retries after output, direct sink failure propagation and
bounded/redacted public diagnostics. A failing sink does not receive a fabricated
terminal event.

Keep existing Chat Completions payload semantics and explicit unsupported-protocol
failure. Revisiting vendor wire specialization under ADR 0063 requires separate
gateway evidence and is outside this change.

## Validation

Exercise retained inference behavior through Models::stream with the existing
scripted transport, including key precedence, text/thinking/tool deltas, Usage,
terminal delivery, cancellation, pre-output retries, post-output retry rejection,
sink failure and diagnostic redaction. Replace duplicate lifecycle tests that
construct internal execution state after equivalent interface coverage exists;
retain useful pure transformation/parser tests.

Cover malformed/executable models.json being ignored, default/resumed selection
with --api-key, rejection of --provider, gateway IDs containing `/`, discovery
failure/recovery, and historical session model resolution. MCP authorization,
refresh, persistence and legacy credential migration retain offline coverage.
Update the Owner Interface and include evidence in the architecture contract,
run owning-shard Focused Validation and the architecture selection, then Full
Validation before implementation delivery.

## Trade-off

Pike gives up direct model-provider configuration and model credential persistence.
Keeping their interface for hypothetical callers would preserve dummy stores,
callback graphs and duplicate state even though the Local LLM Router owns the
upstream capability. Retaining a startup gateway key and passive model identity
preserves concrete uses without retaining the obsolete live Provider graph.
