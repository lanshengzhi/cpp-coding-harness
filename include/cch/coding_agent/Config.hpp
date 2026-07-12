#pragma once

#include <cch/coding_agent/ProjectResources.hpp>
#include <cch/coding_agent/ProjectTrust.hpp>
#include <cch/util/Error.hpp>

#include <optional>
#include <string>
#include <vector>

namespace cch::coding_agent {

/// Configuration values loaded from ~/.cpp-harness/config.json.
/// All fields are optional — CLI flags and built-in defaults fill any gaps.
struct ConfigData {
    std::optional<std::string> provider;
    std::optional<std::string> model;
    std::optional<std::string> base_url;
    /// Env var chain: first found value is used as the resolved API key.
    std::optional<std::vector<std::string>> api_key_env;
    /// Auth provider name in ~/.cpp-harness/agent/auth.json. If set, the corresponding
    /// static API key is used before falling back to api_key_env.
    std::optional<std::string> auth;
    /// User-controlled project trust default. Project-local settings cannot set this.
    std::optional<DefaultProjectTrust> default_project_trust;
    /// User-controlled project skill resource enablement.
    std::optional<ResourceEnablement> project_skills;
};

/// Loads configuration from a JSON file and resolves API keys from environment.
class ConfigLoader {
public:
    /// Load config from the given file path. Returns default-constructed ConfigData
    /// (all fields nullopt) if the file does not exist or is unreadable.
    /// Returns an error only if the file exists but is malformed JSON.
    [[nodiscard]] static util::Expected<ConfigData> load(const std::string& config_path);

    /// Resolve an API key from an env var chain. Returns the value of the first
    /// environment variable that is set and non-empty. Returns nullopt if none are set.
    [[nodiscard]] static std::optional<std::string> resolve_api_key(
        const std::vector<std::string>& env_chain);

    /// Default user config path: `$HOME/.cpp-harness/config.json`, or empty when HOME is unset.
    [[nodiscard]] static std::string default_config_path();
};

/// Explicit CLI overrides for provider-facing settings. Unset fields fall through to
/// session-stored values, config file, and built-in defaults.
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
    /// Static API key loaded from ~/.cpp-harness/agent/auth.json, if any.
    std::string api_key;
    /// Name of the auth entry used to obtain api_key.
    std::string auth;
    /// Full environment variable chain used for API key lookup and secret
    /// filtering. The first element is the resolved single env var name.
    std::vector<std::string> api_key_env_chain;
};

/// Priority: CLI explicit > session stored provider/model > config file > provider default.
[[nodiscard]] ResolvedProviderSettings resolve_provider_settings(
    const std::string& provider_registry_name,
    bool fake,
    const CliProviderOverrides& cli,
    const ConfigData& config,
    const std::optional<std::string>& stored_provider,
    const std::optional<std::string>& stored_model);

/// Env var chain used for API key lookup after resolution (CLI single env or config chain).
[[nodiscard]] std::vector<std::string> resolved_api_key_env_chain(
    const CliProviderOverrides& cli,
    const ConfigData& config);

} // namespace cch::coding_agent
