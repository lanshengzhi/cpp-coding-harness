#pragma once

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
};

} // namespace cch::coding_agent
