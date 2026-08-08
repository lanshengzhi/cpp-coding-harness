#pragma once

#include <cch/coding_agent/ProjectTrust.hpp>
#include <cch/util/Error.hpp>

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

/// User settings following pi's two-scope `settings.json` contract (ADR 0031).
/// All fields are optional — CLI flags and built-in defaults fill any gaps.
/// Settings never carry secrets or secret references; `apiKey` appears only in
/// `models.json`.
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
    [[nodiscard]] util::ExpectedVoid set_project_trusted(bool trusted);
    /// Reload both scopes from disk, re-recording load errors.
    [[nodiscard]] util::ExpectedVoid reload();

    /// Surgical field-level write of the `theme` field in one scope. Preserves
    /// every other field, including fields unknown to this build. A scope
    /// whose load failed suppresses its write; project-scope writes require
    /// trust. No-op when the value is unchanged.
    [[nodiscard]] util::ExpectedVoid set_theme(SettingsScope scope, std::string_view value);

    /// Surgical field-level write of the pi `defaultThinkingLevel` field in
    /// one scope (pi `SettingsManager.setDefaultThinkingLevel`). Validates the
    /// value against the seven-level set (`off`/`minimal`/`low`/`medium`/
    /// `high`/`xhigh`/`max`); preserves every other field. A scope whose load
    /// failed suppresses its write; project-scope writes require trust. No-op
    /// when the value is unchanged.
    [[nodiscard]] util::ExpectedVoid set_default_thinking_level(
        SettingsScope scope,
        std::string_view value);

private:
    struct Impl;
    explicit SettingsManager(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace cch::coding_agent
