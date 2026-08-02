---
status: accepted
---

# Align settings, shared-file, CLI, and resume configuration contracts with pi

`settings.json`, the CLI configuration flag surface, resume metadata, and the shared-file write contract are no longer harness-private designs: they are pi's own contracts, adopted verbatim within the three-path scope. This implements parity-map ticket [#327](https://github.com/lanshengzhi/cpp-coding-harness/issues/327), whose question surfaces not already frozen by [#326](https://github.com/lanshengzhi/cpp-coding-harness/issues/326) are settled here. C++ remains a pi capability subset by omission only; no backward compatibility, migration shims, aliases, or fallback reads.

## Considered options

- Keep the harness-private settings fields (`provider`, `model`, `base_url`, `api_key_env`, `auth`) alongside pi's `defaultProvider`/`defaultModel`: rejected because they are a second, observable-semantics-substituting precedence system that conflicts with the frozen request-level auth chain.
- Keep `--auth`/`--api-key-env`/`--base-url` as compatibility aliases: rejected by the no-backward-compatibility rule; each has a pi-native replacement (provider-implicit auth.json selection, `$ENV` templates, models.json config-only providers).
- Persist baseUrl/key-source in session files so resume restores the original runtime context: rejected because it creates cross-run authentication snapshots, violating the frozen "auth resolves live before each request" rule; pi persists only `model_change {provider, modelId}`.
- Treat `accountId` as displayable login metadata: rejected on verification against frozen pi — `CredentialInfo` is exactly `{providerId, type}` and no pi surface renders `accountId`; its only outlets are the auth.json record and the Codex `chatgpt-account-id` request header.
- Write settings with whole-file replace, like auth.json: rejected because settings files are hand-edited far more often; pi's surgical field-level merge is what lets two products and a human editor share one file without clobbering each other.

## Consequences

### Settings contract

- `settings.json` follows pi's two-scope model: global `<agentDir>/settings.json` plus project `<cwd>/.pi/settings.json`, deep-merged with project winning; the project scope loads and writes only when the project is trusted; `defaultProjectTrust` is global-only. pi's read-time migrations apply; no C++-private migrations.
- Field subset (consumer-driven): `defaultProvider`, `defaultModel`, `defaultThinkingLevel`, `enabledModels`, `sessionDir`, `defaultProjectTrust`, `shellPath`, `shellCommandPrefix`, `theme`. Harness-private `provider`/`model`/`base_url`/`api_key_env`/`auth` fields and the `project_resources` policy fields are removed; project resource loading is trust-gated per pi. Further pi fields graduate individually when a consumer exists.
- `settings.json` never carries secrets or secret-reference fields; `apiKey` appears only in `models.json`.

### Authentication precedence and CLI surface

- The full pi four-level chain applies: runtime API key override (`ModelRuntime::set_runtime_api_key`, CLI `--api-key`; in-memory, never persisted) → stored `auth.json` credential → provider environment variables → `models.json` configured `apiKey` (`source: "configured API key"`).
- The CLI configuration surface is pi's: `--provider`, `--model`, `--models`, `--api-key`; `--api-key` requires an explicit model. `--auth`, `--api-key-env`, and `--base-url` are removed without shims. DeepSeek custom endpoints become config-only providers in `models.json`.
- `models.json` `apiKey` supports pi's three value forms: literal, `$VAR`/`${VAR}` env template, and `!command` shell execution with process-lifetime caching. `models.json` is thereby an executable configuration surface; the documented "not a sandbox" containment boundary applies unchanged.
- Following [ADR 0030](0030-share-pi-agent-config-directory-and-credential-store.md)'s naming rule, the session-directory override variable is pi's `PI_CODING_AGENT_SESSION_DIR`; `CCH_CODING_AGENT_SESSION_DIR` is removed without fallback. This supersedes the variable name in [ADR 0003](0003-store-default-sessions-in-agent-config-directory.md); its precedence chain (`--session-dir` → env → `sessionDir` → workspace-keyed default) and resolution rules still stand.

### Resume metadata

- Session files persist only pi's `model_change {provider, modelId}` and thinking-level entries — never baseUrl, key-source, or any authentication material. Resume re-resolves the recorded identity against the live Models Runtime catalog; a model that no longer resolves fails through normal diagnostics without silent substitution. The CLI default chain on resume is: session `model_change` → settings `defaultProvider`/`defaultModel` → Models Runtime default.

### Secret ownership at the OAuth login boundary

- Displayable: the authorization URL as a whole, device verification URL and `user_code`, interaction prompts, and success messages carrying only provider name and auth type. Never rendered, logged, or persisted outside auth.json: authorization codes, PKCE verifiers, tokens, `accountId`, and other token-response fields. `CredentialStore::modify` remains the sole persistence exit; `list` is strictly `{providerId, type}`; transients are discarded at login completion. Redaction follows pi's model — secrets never enter the rendered surface — with the repo's generic redaction/output bounding retained as a defensive layer.

### Shared-file write contract

- `models.json` is never written by the runtime; `auth.json` writes follow [ADR 0030](0030-share-pi-agent-config-directory-and-credential-store.md); `settings.json` writes use pi's surgical field-level merge under a proper-lockfile-compatible lock (re-read, migrate, apply only session-modified fields, preserve unknown fields), suppress writes to a scope whose load failed, and require trust for project-scope writes. None of the three files carries a schema version marker; upgrade/downgrade interop is migrate-on-read plus lossless preservation on write. pi's async write queue is expressed as an equivalent serialized write channel.
