#pragma once

#include <cch/coding_agent/ProjectResources.hpp>
#include <cch/coding_agent/ProjectTrust.hpp>
#include <cch/util/Error.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cch::coding_agent {

/// User settings loaded from the agent config directory's `settings.json`
/// (pi: `~/.pi/agent/settings.json`). All fields are optional — CLI flags and
/// built-in defaults fill any gaps.
struct UserSettings {
    std::optional<std::string> provider;
    std::optional<std::string> model;
    std::optional<std::string> base_url;
    /// Env var chain: first found value is used as the resolved API key.
    std::optional<std::vector<std::string>> api_key_env;
    /// Credential entry name in the agent config directory's `auth.json`. If
    /// set, the entry is resolved live before falling back to api_key_env.
    std::optional<std::string> auth;
    /// User-controlled project trust default. Project-local settings cannot set this.
    std::optional<DefaultProjectTrust> default_project_trust;
    /// User-controlled project skill resource enablement.
    std::optional<ResourceEnablement> project_skills;
    /// User-controlled Native TUI project theme resource enablement.
    std::optional<ResourceEnablement> project_themes{std::nullopt};
    /// Native TUI theme selected by name.
    std::optional<std::string> theme{std::nullopt};
    /// Optional Shell executable (pi: `shellPath`); a leading home marker is
    /// expanded by the local Shell adapter when execution is attempted.
    std::optional<std::string> shell_path{std::nullopt};
    /// Optional script prefix (pi: `shellCommandPrefix`) applied only at the
    /// local process-launch boundary.
    std::optional<std::string> shell_command_prefix{std::nullopt};
    /// CLI session-storage preference (pi: `sessionDir`, same format as the
    /// `--session-dir` flag). Consumed only by CLI automatic-directory
    /// resolution; SDK default persistence never reads it.
    std::optional<std::string> session_dir;
};

/// Loads user settings from a JSON file and resolves API keys from environment.
class SettingsLoader {
public:
    /// Load settings from the given file path. Returns default-constructed UserSettings
    /// (all fields nullopt) if the file does not exist or is unreadable.
    /// Returns an error only if the file exists but is malformed JSON.
    [[nodiscard]] static util::Expected<UserSettings> load(const std::filesystem::path& settings_path);

    /// Persist one Native TUI theme selection while preserving every other
    /// settings member, including members unknown to this build.
    [[nodiscard]] static util::ExpectedVoid save_theme_selection(
        const std::filesystem::path& settings_path,
        std::string_view theme_name);

    /// Resolve an API key from an env var chain. Returns the value of the first
    /// environment variable that is set and non-empty. Returns nullopt if none are set.
    [[nodiscard]] static std::optional<std::string> resolve_api_key(
        const std::vector<std::string>& env_chain);
};

/// Explicit CLI overrides for provider-facing settings. Unset fields fall through to
/// session-stored values, user settings, and built-in defaults.
struct CliProviderOverrides {
    std::optional<std::string> model;
    std::optional<std::string> base_url;
    std::optional<std::string> api_key_env;
    std::optional<std::string> auth;
};

struct ResolvedProviderSettings {
    /// Registry key used to construct the concrete provider adapter.
    std::string provider_registry_name;
    /// Provider/API identity stored in sessions and assistant messages.
    std::string provider;
    std::string api;
    std::string model;
    std::string base_url;
    std::string api_key_env;
    /// Name of the credential entry resolved live for each request.
    std::string auth;
    /// Full environment variable chain used for API key lookup and secret
    /// filtering. The first element is the resolved single env var name.
    std::vector<std::string> api_key_env_chain;
};

} // namespace cch::coding_agent
