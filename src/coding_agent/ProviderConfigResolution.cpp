#include "../../include/cch/coding_agent/AuthLoader.hpp"
#include "../../include/cch/coding_agent/Config.hpp"

#include <cstdlib>
#include <map>
#include <string>
#include <utility>

namespace cch::coding_agent {
namespace {

[[nodiscard]] std::string provider_default_model(const std::string& provider) {
    static const std::map<std::string, std::string> default_model{
        {"openai-compatible", "gpt-4.1-mini"},
        {"fake", "fake-model"},
    };
    auto it = default_model.find(provider);
    return it != default_model.end() ? it->second : "gpt-4.1-mini";
}

[[nodiscard]] std::string provider_default_api(const std::string& provider_registry_name) {
    if (provider_registry_name == "fake") {
        return "scripted-fake";
    }
    return "openai-completions";
}


} // namespace

std::string ConfigLoader::default_config_path() {
    const char* home = std::getenv("HOME");
    return home ? std::string(home) + "/.cpp-harness/config.json" : "";
}

ResolvedProviderSettings resolve_provider_settings(
    const std::string& provider_registry_name,
    const bool fake,
    const CliProviderOverrides& cli,
    const ConfigData& config,
    const std::optional<std::string>& stored_provider,
    const std::optional<std::string>& stored_model) {
    std::string resolved_provider = provider_registry_name;
    if (stored_provider) {
        resolved_provider = *stored_provider;
    } else if (config.provider && resolved_provider == "openai-compatible" && !fake) {
        resolved_provider = *config.provider;
    }

    std::string resolved_model;
    if (cli.model) {
        resolved_model = *cli.model;
    } else if (stored_model) {
        resolved_model = *stored_model;
    } else if (config.model) {
        resolved_model = *config.model;
    } else {
        resolved_model = provider_default_model(provider_registry_name);
    }

    std::string resolved_base_url;
    if (cli.base_url) {
        resolved_base_url = *cli.base_url;
    } else if (config.base_url) {
        resolved_base_url = *config.base_url;
    } else {
        resolved_base_url = "https://api.openai.com";
    }

    std::string resolved_api_key_env;
    if (cli.api_key_env) {
        resolved_api_key_env = *cli.api_key_env;
    } else if (config.api_key_env) {
        resolved_api_key_env = config.api_key_env->front();
        if (ConfigLoader::resolve_api_key(*config.api_key_env)) {
            for (const auto& env_name : *config.api_key_env) {
                const char* val = std::getenv(env_name.c_str());
                if (val != nullptr && val[0] != '\0') {
                    resolved_api_key_env = env_name;
                    break;
                }
            }
        }
    } else {
        resolved_api_key_env = "OPENAI_API_KEY";
    }

    std::string resolved_auth;
    if (cli.auth) {
        resolved_auth = *cli.auth;
    } else if (config.auth) {
        resolved_auth = *config.auth;
    }

    std::string resolved_api_key;
    if (!resolved_auth.empty()) {
        auto auth_entries = AuthLoader::load(AuthLoader::default_path());
        if (auth_entries) {
            if (auto it = auth_entries->find(resolved_auth); it != auth_entries->end()) {
                resolved_api_key = it->second.key;
            }
        }
    }

    return ResolvedProviderSettings{
        .provider_registry_name = provider_registry_name,
        .provider = std::move(resolved_provider),
        .api = provider_default_api(provider_registry_name),
        .model = std::move(resolved_model),
        .base_url = std::move(resolved_base_url),
        .api_key_env = std::move(resolved_api_key_env),
        .api_key = std::move(resolved_api_key),
        .auth = std::move(resolved_auth),
    };
}

std::vector<std::string> resolved_api_key_env_chain(
    const CliProviderOverrides& cli,
    const ConfigData& config) {
    if (cli.api_key_env) {
        return {*cli.api_key_env};
    }
    if (config.api_key_env) {
        return *config.api_key_env;
    }
    return {"OPENAI_API_KEY"};
}

} // namespace cch::coding_agent
