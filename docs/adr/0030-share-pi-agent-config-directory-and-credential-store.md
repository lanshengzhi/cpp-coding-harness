---
status: accepted
---

# Share pi's agent config directory and credential store

The Agent Config Directory is no longer a harness-private root: it is pi's own directory, read and written interoperably so that a C++ session and a real pi installation share one `auth.json` and one `models.json`. The default is `~/.pi/agent`, overridden by `PI_CODING_AGENT_DIR` or an SDK `agentDir`; `CCH_CODING_AGENT_DIR` and the `~/.cpp-harness/agent` default are removed without fallback or migration. This supersedes the directory-root consequence of [ADR 0002](0002-root-user-state-in-agent-config-directory.md) (its single-path-module and `settings.json` vocabulary decisions still stand) and implements part of parity-map ticket [#326](https://github.com/lanshengzhi/cpp-coding-harness/issues/326).

## Considered options

- Keep an independent harness directory and import pi files once: rejected because import-once snapshots drift immediately, and the parity destination requires live credential interoperability (login in either product is visible in the other).
- Keep both roots with fallback reads: rejected by the repository guardrail against compatibility-only machinery and by the no-backward-compatibility rule for this subset alignment.
- Lock per-provider credential files independently: rejected because pi locks the whole `auth.json` via `proper-lockfile`; per-provider physical locks would corrupt interleaved pi/C++ writes.
- Reimplement pi's file format loosely: rejected because the products share the physical file; the serializer must preserve what it does not understand.

## Consequences

- One public path module resolves the Agent Config Directory: `PI_CODING_AGENT_DIR` first, then `~/.pi/agent`; an SDK `agentDir` overrides both for that session's default-created runtime. An injected `ModelRuntime` wins over `agentDir` path derivation (see [ADR 0029](0029-align-models-provider-and-authentication-ownership-with-pi.md)).
- `auth.json` is shared with pi. Writes hold a whole-file lock compatible with pi's `proper-lockfile` implementation, and `modify` callbacks run under that lock so all provider writes serialize. Directory permissions `0o700` and file permissions `0o600`, and last-valid read behavior, match pi.
- The credential serializer is private and lossless: unknown provider records and unknown OAuth extra fields round-trip unchanged; Codex's `accountId` is explicitly understood. Credential variants match pi's `api_key` and `oauth` shapes. The `CredentialStore` interface (`read`, metadata-only `list`, serialized `modify`, `delete`) lives in `cch::ai`; the file-backed `AuthStorage` lives in coding-agent and is constructed by `ModelRuntime` from `<agentDir>/auth.json`, then injected into `Models` — the ai layer never derives agent-directory paths.
- `models.json` follows pi's schema subset (registered providers, supported Model fields, custom-model upsert, model overrides) and pi's failure behavior: invalid or unreadable content becomes empty user config plus diagnostics rather than a hard failure or a fallback to another location.
- Old harness locations (`~/.cpp-harness/agent`, `CCH_CODING_AGENT_DIR`) are ignored without fallback reads; users move their files once.
- Cohabitation with a real pi installation is deliberate: both products observe each other's logins, logouts, refreshes, and model configuration through the same files.
