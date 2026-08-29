---
status: accepted
---

# Own Provider assembly inside cch_ai and hide the Provider capability behind the deepened Models interface

The `Provider` virtual capability and its transports, catalogs, OAuth implementations, protocol adapters, and `ModelStreamBridge` lived as cch_ai private implementation, but `cch_coding_agent` reached into them through a cross-Owner private composition root (`ProviderComposer`, 7 private headers) and through `register_native_provider` and `Models::set_provider(shared_ptr<Provider>)`. That made cch_ai's private area a second de-facto seam beyond its Owner Interface, exactly the pattern ADR 0046 removed for pi-neutral mechanics. This decision resolves Seam 1 of issue [#540](https://github.com/lanshengzhi/cpp-coding-harness/issues/540): Provider assembly moves into `cch_ai`, `Provider` becomes a private virtual physical capability behind the deepened `cch::ai::Models` interface, and Models Runtime submits and reads only passive values.

The change refines [ADR 0029](0029-align-models-provider-and-authentication-ownership-with-pi.md) (Provider ownership and stream delegation), [ADR 0033](0033-own-the-supported-api-adapter-surface-for-the-three-provider-paths.md) (private adapter registration), and [ADR 0032](0032-own-oauth-lifecycle-and-frontend-interaction-division-with-pi.md) (ModelRuntime delegation), and implements the Provider/transport privacy already stated by [ADR 0040](0040-own-asynchronous-operations-and-the-serialized-runtime-lifecycle.md). It does not change any pi-observable capability behavior: authentication precedence, fallback, stream terminal events, cancellation, and the frozen catalogs are preserved.

## Decisions

- `cch::ai::Models` deepens into the one Owner-Interface seam for provider state. It accepts complete, move-only `ProviderDefinition` values (`apply_provider(ProviderChange)` for one install/remove, `clear_providers()` for a full refresh) and returns passive `ProviderInfo` values for host/TUI queries. `Provider` pointers no longer cross any Owner Interface: `set_provider`, `providers()`, `provider(id)`, and `ModelRuntime::register_native_provider` are removed.
- `Provider` (`Provider.hpp`) and `ProviderStreamOptions` move out of the `cch_ai` Owner Interface root into the private implementation root. `Provider` stays a narrow private virtual physical capability (ADR 0040); `ProviderStreamOptions` stays private to `Models`, the adapters, and message conversion. `SimpleStreamOptions` remains the public per-request options of `Models::stream`.
- HTTP and WebSocket transports become fully private to `cch_ai`. Models Runtime no longer constructs, holds, or names `StreamTransport`/`WebSocketTransport`/`CodexWebSocketCacheConfig`; Boost.Beast and the Codex socket cache stay implementation. Scripted transports exist only in cch_ai private test support.
- Built-in Codex/Kimi definitions originate with `cch::ai::builtin_provider_definitions()`, returning fresh definitions on every call. Models Runtime keeps the `models.json` loading and overlay policy, config-value resolution (`$VAR`/`${VAR}`/`!command`), recomposition timing, per-provider fallback, and error snapshots, and submits the completed definitions.
- The `ProviderComposer` composition root stays in `cch_coding_agent` but operates only on passive values: built-in `ProviderDefinition` + `ModelConfig` in, `ProviderChange` out. It no longer includes any cch_ai private header. Its config-value helpers, `configured_api_key_env_names`, `configured_request_auth_status`, and `default_model_for_provider` remain.
- Tests inject scripted providers through cch_ai private test support that builds scripted `ProviderDefinition` values, submitted through the existing `ModelRuntime` test seam; the production `SessionFactory.cpp` include of `ai/providers/FakeProvider.hpp` disappears.

## Considered options

- Keep `Provider` in the Owner Interface and hide only its construction: rejected because the interface stays as wide as the implementation (id/name/auth/models/stream plus a provider pointer every host can touch), so the second de-facto seam survives.
- Move `ProviderComposer` wholesale into `cch_ai`: rejected because cch_ai would then depend on `ModelConfig` and shell/process execution, inverting the Owner dependency direction (ADR 0039).
- Virtualize `Models` or add a generic command surface: rejected because the concrete coding-agent Models Runtime is settled (ADR 0039/0040) and a wide command variant makes the interface as complex as the implementation.
- Whole-catalog transactional `replace_all` only: rejected because it introduces a full-catalog transaction that does not exist and duplicates the per-provider fallback/error-recording policy; per-provider mutation plus `clear_providers()` covers the real recomposition flows.
- Keep `register_native_provider` for tests: rejected because a single real adapter (the scripted test provider) does not justify an open seam; one adapter means a hypothetical seam.

## Consequences

- cch_ai's Owner Interface shrinks to `Models` plus the passive `Provider Definition`/`Provider Change`/`Provider Info` values and the existing Model, authentication, request-option, and stream contracts; the Owner Interface is again the only seam.
- Models Runtime and the CLI/TUI stop naming any cch_ai private implementation type; `Provider.hpp`, `ProviderStreamOptions`, and the transport types vanish from every downstream include.
- Transport choice and the Codex socket cache are no longer a caller concern; replacing Boost.Beast later cannot change an Owner Interface.
- Tests cross the deepened `Models` interface (`apply_provider` / `provider_info` / `stream` / auth operations); the existing auth, stream, terminal, and fallback contract tests survive at that seam; adapter tests build scripted definitions.
- The Parity Architecture Gate needs updated include-root and interface-header evidence as `Provider.hpp` and `ProviderStreamOptions` leave the `cch_ai` Owner Interface root, and `ProviderComposer` loses its cch_ai private includes.
- ADR 0046's note that the ProviderComposer composition root is "ADR-0029-sanctioned" is superseded for the mechanism: composition policy stays in Models Runtime, but cross-Owner private include is no longer the delivery mechanism.
- `CONTEXT.md` gains the **Provider Definition** and **Provider Info** glossary terms.

## References

- Follow-up adjudication issue [#540](https://github.com/lanshengzhi/cpp-coding-harness/issues/540) (Seam 1).
- Architecture review findings and the ADR 0046 migration ([#539](https://github.com/lanshengzhi/cpp-coding-harness/issues/539)) that established the "no second de-facto seam" rule.
- Parity map [#2](https://github.com/lanshengzhi/cpp-coding-harness/issues/2) and the frozen pi baseline commit `83114817c68f5413e4d7ba6d7003ddc511cd31d2`.
- ADRs [0029](0029-align-models-provider-and-authentication-ownership-with-pi.md), [0033](0033-own-the-supported-api-adapter-surface-for-the-three-provider-paths.md), [0032](0032-own-oauth-lifecycle-and-frontend-interaction-division-with-pi.md), [0039](0039-own-the-capability-owner-package-graph-and-parity-architecture-gate.md), [0040](0040-own-asynchronous-operations-and-the-serialized-runtime-lifecycle.md), and [0046](0046-move-pi-neutral-mechanics-from-cch-ai-to-cch-support.md).
