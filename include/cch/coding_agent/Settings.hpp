#pragma once

#include <cch/coding_agent/ProjectResources.hpp>
#include <cch/coding_agent/ProjectTrust.hpp>
#include <cch/util/Error.hpp>

#include <filesystem>
#include <optional>
#include <string>
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
    /// Auth provider name in the agent config directory's `auth.json`. If set,
    /// the corresponding static API key is used before falling back to api_key_env.
    std::optional<std::string> auth;
    /// User-controlled project trust default. Project-local settings cannot set this.
    std::optional<DefaultProjectTrust> default_project_trust;
    /// User-controlled project skill resource enablement.
    std::optional<ResourceEnablement> project_skills;
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
    /// Static API key loaded from the agent config directory's `auth.json`, if any.
    std::string api_key;
    /// Name of the auth entry used to obtain api_key.
    std::string auth;
    /// Full environment variable chain used for API key lookup and secret
    /// filtering. The first element is the resolved single env var name.
    std::vector<std::string> api_key_env_chain;
};

} // namespace cch::coding_agent
