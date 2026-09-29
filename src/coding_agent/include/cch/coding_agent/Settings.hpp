#pragma once

#include <cch/coding_agent/ProjectTrust.hpp>
#include <cch/support/Error.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cch::coding_agent {

/// pi `compaction` — nested automatic-compaction settings. Every field is
/// optional: a missing field falls back to pi's `DEFAULT_COMPACTION_SETTINGS`
/// (`enabled: true`, `reserveTokens: 16384`, `keepRecentTokens: 20000`) when
/// the trigger policy resolves them. Field names are pi's wire names
/// (`reserveTokens`/`keepRecentTokens`) in camelCase.
struct UserCompactionSettings {
    std::optional<bool> enabled{std::nullopt};
    std::optional<std::uint64_t> reserve_tokens{std::nullopt};
    std::optional<std::uint64_t> keep_recent_tokens{std::nullopt};
};

/// pi `retry` — nested turn auto-retry settings (pi `RetrySettings`). Every
/// field is optional: a missing field falls back to pi's defaults
/// (`enabled: true`, `maxRetries: 3`, `baseDelayMs: 2000`, exponential
/// backoff `baseDelayMs * 2^(attempt-1)`) when the session-assembly policy
/// resolves them. Field names are pi's wire names (`maxRetries`/`baseDelayMs`)
/// in camelCase.
struct UserRetrySettings {
    std::optional<bool> enabled{std::nullopt};
    std::optional<std::uint64_t> max_retries{std::nullopt};
    std::optional<std::uint64_t> base_delay_ms{std::nullopt};
};

/// Tool-activation policy for one configured Upstream MCP Server (issue #835,
/// spec #833 story 5). Wire names are `lazy` and `eager`; the default is
/// `lazy`. Interim semantics, until Lazy Tool Activation ships: a `lazy`
/// server's tools stay dormant after connect — nothing is injected into the
/// tool surface and no activation path exists yet — and a `lazy` server is
/// never silently treated as `eager`.
enum class McpServerActivation { Lazy, Eager };

/// Call-authorization policy for one configured Upstream MCP Server (issue
/// #835, spec #833 story 5). Wire names are `allow` and `ask`; the default is
/// `allow`. `ask` routes every upstream tool call through the session's
/// before-tool-call policy hook (spec #833 story 22).
enum class McpServerApproval { Allow, Ask };

/// One configured Upstream MCP Server entry from the `mcpServers` map, keyed
/// by its Server Id (issue #835, spec #833 stories 1, 2, 3, 5, 6).
///
/// `settings.json` carries no credential material: `bearer_env_var` holds the
/// *name* of an environment variable, extracted from a `bearer-env:<VAR>`
/// reference in `auth` or in `headers.Authorization`, and the value itself is
/// resolved from the environment at use time. Validation is fail-closed, so an
/// entry that reaches this type carries an `http`/`https` `url` with no
/// userinfo, an environment-variable reference instead of a secret, and no
/// stdio `command`/`args`/`env` (stdio is a Deferred Capability that fails
/// validation, ADR 0064).
struct UserMcpServerSettings {
    /// Server Id — the map key, constrained to `[A-Za-z0-9_-]`, at most 48
    /// characters (spec #833 naming decision). It is the sole stable identity
    /// used for namespacing, credentials, trust, and status.
    std::string server_id;
    /// Streamable HTTP endpoint of the Upstream MCP Server.
    std::string url;
    /// Environment-variable name holding the bearer token, from a
    /// `bearer-env:<VAR>` reference. `std::nullopt` means the server is
    /// declared without an `auth` reference.
    std::optional<std::string> bearer_env_var{std::nullopt};
    /// Wire value of `activation`; resolved by `activation_policy()`.
    std::optional<McpServerActivation> activation{std::nullopt};
    /// Wire value of `approval`; resolved by `approval_policy()`.
    std::optional<McpServerApproval> approval{std::nullopt};

    /// Defaulted activation: `Lazy` when the config omits the field.
    [[nodiscard]] McpServerActivation activation_policy() const noexcept {
        return activation.value_or(McpServerActivation::Lazy);
    }
    /// Defaulted approval: `Allow` when the config omits the field.
    [[nodiscard]] McpServerApproval approval_policy() const noexcept {
        return approval.value_or(McpServerApproval::Allow);
    }
};

/// Whether `server_id` is a well-formed Server Id: non-empty, at most 48
/// characters, and drawn from `[A-Za-z0-9_-]` (spec #833 naming decision).
/// `mcpServers` validation applies it to every configured entry, and the
/// Upstream MCP Server trust store applies it to every key it persists.
[[nodiscard]] bool is_valid_mcp_server_id(std::string_view server_id);

/// User settings following pi's two-scope `settings.json` contract (ADR 0031).
/// All fields are optional — CLI flags and built-in defaults fill any gaps.
/// Settings never carry secrets: credential material enters configuration only
/// as an environment-variable reference (`bearer-env:<VAR>` on `mcpServers`
/// entries, `$VAR` templates in `models.json`), never as a literal value, and
/// `apiKey` appears only in `models.json`.
struct UserSettings {
    /// pi `defaultProvider` — default provider name (e.g. `openai-codex`).
    std::optional<std::string> default_provider{std::nullopt};
    /// pi `defaultModel` — default model id.
    std::optional<std::string> default_model{std::nullopt};
    /// pi `defaultThinkingLevel` — one of
    /// `off`/`minimal`/`low`/`medium`/`high`/`xhigh`/`max`.
    std::optional<std::string> default_thinking_level{std::nullopt};
    /// pi `enabledModels` — model patterns for cycling (same format as the
    /// `--models` CLI flag).
    std::optional<std::vector<std::string>> enabled_models{std::nullopt};
    /// pi `sessionDir` — CLI session-storage preference (same format as the
    /// `--session-dir` flag). Consumed only by CLI automatic-directory
    /// resolution; default persistence never reads it.
    std::optional<std::string> session_dir{std::nullopt};
    /// pi `defaultProjectTrust` — global-only; never honored from the project
    /// scope.
    std::optional<DefaultProjectTrust> default_project_trust{std::nullopt};
    /// pi `shellPath` — optional Shell executable; a leading home marker is
    /// expanded by the local Shell adapter when execution is attempted.
    std::optional<std::string> shell_path{std::nullopt};
    /// pi `shellCommandPrefix` — script prefix applied only at the local
    /// process-launch boundary.
    std::optional<std::string> shell_command_prefix{std::nullopt};
    /// pi `theme` — Native TUI theme selected by name.
    std::optional<std::string> theme{std::nullopt};
    /// pi `compaction` — nested automatic-compaction settings consumed by the
    /// session-assembly trigger policy.
    std::optional<UserCompactionSettings> compaction{std::nullopt};
    /// pi `retry` — nested turn auto-retry settings consumed by the
    /// session-assembly retry policy.
    std::optional<UserRetrySettings> retry{std::nullopt};
    /// pi `hideThinkingBlock` — hide thinking blocks in assistant responses
    /// (default false). Graduated into the #327 field subset with decision 10
    /// of the G2 record; consumed by the interactive assistant-message
    /// rendering and `app.thinking.toggle`.
    std::optional<bool> hide_thinking_block{std::nullopt};
    /// pi `outputPad` — horizontal padding for user messages, assistant
    /// messages, and thinking (default 1; only 0 or 1). Graduated into the
    /// #327 field subset with decision 10 of the G2 record; any non-zero
    /// stored value resolves as 1 (pi `settings.outputPad === 0 ? 0 : 1`).
    std::optional<std::size_t> output_pad{std::nullopt};
    /// pi `enableSkillCommands` — register skills as `/skill:name` commands
    /// (default true). Graduated into the settings subset with decision 24
    /// of the G4 record; gates `/skill:` registration and autocomplete.
    std::optional<bool> enable_skill_commands{std::nullopt};
    /// `mcpServers` — Upstream MCP Servers the MCP Host is configured to
    /// aggregate, keyed by Server Id and listed in Server Id order. The first
    /// harness-specific `settings.json` field (issue #835, ADR 0031 addendum);
    /// project-scope entries load only while the project is trusted, and the
    /// two scopes deep-merge per Server Id with the project scope winning.
    /// Each scope's entry is self-contained — a `url` is required wherever a
    /// Server Id is declared — and a field the project entry omits keeps the
    /// global value.
    std::optional<std::vector<UserMcpServerSettings>> mcp_servers{std::nullopt};
};

/// One `settings.json` scope (pi `SettingsScope`).
enum class SettingsScope { Global, Project };

/// A scope whose load failed and why. A scope whose load failed suppresses
/// writes to that scope and resolves as empty so the other scope keeps working
/// (pi `SettingsError`).
struct SettingsError {
    SettingsScope scope{SettingsScope::Global};
    std::string message{};
};

/// Two-scope user settings manager (pi `SettingsManager` subset).
///
/// Global scope: `<agentDir>/settings.json`. Project scope:
/// `<cwd>/.pi/settings.json`, loaded and written only while the project is
/// trusted. Reads deep-merge with the project scope winning; writes are
/// surgical field-level merges under a proper-lockfile-compatible lock that
/// re-read the current file, apply pi's read-time migrations, and preserve
/// unmodified and unknown fields. `defaultProjectTrust` is global-only. No
/// scope carries a schema version marker.
class SettingsManager {
public:
    /// Load both scopes from disk. `cwd` is the project root used to derive
    /// the project scope path; `agent_dir` is the Agent Config Directory.
    /// When `project_trusted` is false the project scope is skipped until
    /// `set_project_trusted(true)` reloads it.
    [[nodiscard]] static SettingsManager create(
        std::filesystem::path cwd,
        std::filesystem::path agent_dir,
        bool project_trusted = true);

    SettingsManager(SettingsManager&&) noexcept;
    SettingsManager& operator=(SettingsManager&&) noexcept;
    ~SettingsManager();
    SettingsManager(const SettingsManager&) = delete;
    SettingsManager& operator=(const SettingsManager&) = delete;

    /// Resolved global settings path (`<agentDir>/settings.json`).
    [[nodiscard]] const std::filesystem::path& global_path() const noexcept;
    /// Resolved project settings path (`<cwd>/.pi/settings.json`).
    [[nodiscard]] const std::filesystem::path& project_path() const noexcept;
    /// The project root `cwd` this manager was created for (the path the
    /// project scope derives from).
    [[nodiscard]] const std::filesystem::path& cwd() const noexcept;

    /// Global scope settings.
    [[nodiscard]] const UserSettings& global_settings() const noexcept;
    /// Project scope settings (empty when untrusted).
    [[nodiscard]] const UserSettings& project_settings() const noexcept;
    /// Deep-merged view: the project scope wins. `defaultProjectTrust` is
    /// excluded from the project scope at load, so the merged view never
    /// carries a project-authored trust default.
    [[nodiscard]] const UserSettings& settings() const noexcept;
    [[nodiscard]] bool is_project_trusted() const noexcept;
    /// Global-only default project trust (pi `getDefaultProjectTrust`).
    [[nodiscard]] std::optional<DefaultProjectTrust> default_project_trust() const noexcept;
    /// Scope load errors recorded at create/reload. Empty when both scopes
    /// loaded cleanly.
    [[nodiscard]] const std::vector<SettingsError>& errors() const noexcept;

    /// Flip project trust. Untrusting drops the project scope in memory;
    /// trusting reloads it from disk. Project-scope writes are refused while
    /// the project is untrusted.
    [[nodiscard]] support::ExpectedVoid set_project_trusted(bool trusted);
    /// Reload both scopes from disk, re-recording load errors.
    [[nodiscard]] support::ExpectedVoid reload();

    /// Surgical field-level write of the `theme` field in one scope. Preserves
    /// every other field, including fields unknown to this build. A scope
    /// whose load failed suppresses its write; project-scope writes require
    /// trust. No-op when the value is unchanged.
    [[nodiscard]] support::ExpectedVoid set_theme(SettingsScope scope, std::string_view value);

    /// Surgical field-level write of the pi `defaultThinkingLevel` field in
    /// one scope (pi `SettingsManager.setDefaultThinkingLevel`). Validates the
    /// value against the seven-level set (`off`/`minimal`/`low`/`medium`/
    /// `high`/`xhigh`/`max`); preserves every other field. A scope whose load
    /// failed suppresses its write; project-scope writes require trust. No-op
    /// when the value is unchanged.
    [[nodiscard]] support::ExpectedVoid set_default_thinking_level(
        SettingsScope scope,
        std::string_view value);

    /// Resolved pi `hideThinkingBlock` over the merged view (default false).
    [[nodiscard]] bool hide_thinking_block() const noexcept;
    /// Resolved pi `outputPad` over the merged view (default 1; any stored
    /// non-zero value resolves as 1).
    [[nodiscard]] std::size_t output_pad() const noexcept;
    /// Resolved pi `enableSkillCommands` over the merged view (default true;
    /// pi `SettingsManager.getEnableSkillCommands`).
    [[nodiscard]] bool get_enable_skill_commands() const noexcept;

    /// Merged `mcpServers` over the two-scope view: the global scope's entries
    /// in Server Id order, then the project scope's project-only entries in
    /// Server Id order, with a project entry's set fields winning
    /// field-by-field. Empty when neither scope declares a server.
    /// Project-scope entries are absent while the project is untrusted, and a
    /// scope whose `mcpServers` failed validation contributes nothing at
    /// all.
    [[nodiscard]] const std::vector<UserMcpServerSettings>& mcp_servers() const noexcept;

    /// Surgical field-level write of the pi `enableSkillCommands` field in
    /// the global scope (pi `SettingsManager.setEnableSkillCommands`, which
    /// always writes `globalSettings`); preserves every other field. A
    /// global load failure suppresses the write. No-op when unchanged.
    [[nodiscard]] support::ExpectedVoid set_enable_skill_commands(bool enabled);

    /// Surgical field-level write of the pi `hideThinkingBlock` field in the
    /// global scope (pi `SettingsManager.setHideThinkingBlock`, which always
    /// writes `globalSettings`); preserves every other field. A global load
    /// failure suppresses the write. No-op when the value is unchanged.
    [[nodiscard]] support::ExpectedVoid set_hide_thinking_block(bool hide);

    /// Surgical field-level write of the pi `outputPad` field in the global
    /// scope (pi `SettingsManager.setOutputPad`, which always writes
    /// `globalSettings`); preserves every other field. Only 0 and 1 are
    /// accepted (pi's `outputPad: 0 | 1`); anything else is rejected. A global
    /// load failure suppresses the write. No-op when the value is unchanged.
    [[nodiscard]] support::ExpectedVoid set_output_pad(std::size_t padding);

    /// Surgical field-level write of the pi `defaultProjectTrust` field in the
    /// global scope (pi `SettingsManager.setDefaultProjectTrust`, which always
    /// writes `globalSettings` and is global-only); preserves every other
    /// field. A global load failure suppresses the write. No-op when
    /// unchanged.
    [[nodiscard]] support::ExpectedVoid set_default_project_trust(
        DefaultProjectTrust trust);

    /// Surgical field-level write of the pi `defaultProvider` and
    /// `defaultModel` fields in the global scope (pi
    /// `SettingsManager.setDefaultModelAndProvider`, which always writes
    /// `globalSettings`); preserves every other field. A global load failure
    /// suppresses the write. No-op when both values are unchanged.
    [[nodiscard]] support::ExpectedVoid set_default_model_and_provider(
        std::string provider,
        std::string model);

    /// Surgical field-level write of the pi `enabledModels` field in the
    /// global scope (pi `SettingsManager.setEnabledModels`, which always
    /// writes `globalSettings`); preserves every other field. `std::nullopt`
    /// removes the field (pi writes `undefined`, which its serializer drops).
    /// A global load failure suppresses the write. No-op when unchanged.
    [[nodiscard]] support::ExpectedVoid set_enabled_models(
        std::optional<std::vector<std::string>> patterns);

private:
    struct Impl;
    explicit SettingsManager(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace cch::coding_agent
