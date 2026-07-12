#include "ProviderConfigResolution.hpp"
#include "../../include/cch/coding_agent/AuthLoader.hpp"
#include "../../include/cch/coding_agent/Config.hpp"

#include <cstdlib>
#include <format>
#include <map>
#include <string>
#include <utility>
#include <vector>

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

[[nodiscard]] std::vector<std::string> resolve_api_key_env_chain(
    const ProviderRequest& explicit_request,
    const ConfigData& config) {
    if (explicit_request.api_key_env && !explicit_request.api_key_env->empty()) {
        return *explicit_request.api_key_env;
    }
    if (config.api_key_env && !config.api_key_env->empty()) {
        return *config.api_key_env;
    }
    return {"OPENAI_API_KEY"};
}

} // namespace

std::string ConfigLoader::default_config_path() {
    const char* home = std::getenv("HOME");
    return home ? std::string(home) + "/.cpp-harness/config.json" : "";
}

ResolvedProviderSettings resolve_provider_settings(
    std::string_view provider_registry_name,
    const ProviderRequest& explicit_request,
    const ConfigData& config,
    const std::optional<std::string>& stored_provider,
    const std::optional<std::string>& stored_model) {
    const std::string registry_name{provider_registry_name};

    std::string resolved_provider = registry_name;
    if (explicit_request.provider) {
        resolved_provider = *explicit_request.provider;
    } else if (stored_provider) {
        resolved_provider = *stored_provider;
    } else if (resolved_provider == "openai-compatible" && config.provider) {
        resolved_provider = *config.provider;
    }

    std::string resolved_model;
    if (explicit_request.model) {
        resolved_model = *explicit_request.model;
    } else if (stored_model) {
        resolved_model = *stored_model;
    } else if (config.model) {
        resolved_model = *config.model;
    } else {
        resolved_model = provider_default_model(registry_name);
    }

    std::string resolved_base_url;
    if (explicit_request.base_url) {
        resolved_base_url = *explicit_request.base_url;
    } else if (config.base_url) {
        resolved_base_url = *config.base_url;
    } else {
        resolved_base_url = "https://api.openai.com";
    }

    const auto resolved_api_key_env_chain = resolve_api_key_env_chain(explicit_request, config);

    std::string resolved_auth;
    if (explicit_request.auth) {
        resolved_auth = *explicit_request.auth;
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
        .provider_registry_name = registry_name,
        .provider = std::move(resolved_provider),
        .api = provider_default_api(registry_name),
        .model = std::move(resolved_model),
        .base_url = std::move(resolved_base_url),
        .api_key_env = resolved_api_key_env_chain.empty() ? "" : resolved_api_key_env_chain.front(),
        .api_key = std::move(resolved_api_key),
        .auth = std::move(resolved_auth),
        .api_key_env_chain = std::move(resolved_api_key_env_chain),
    };
}

ResolvedProviderSettings resolve_provider_settings(
    const std::string& provider_registry_name,
    const bool fake,
    const CliProviderOverrides& cli,
    const ConfigData& config,
    const std::optional<std::string>& stored_provider,
    const std::optional<std::string>& stored_model) {
    ProviderRequest explicit_request;
    explicit_request.model = cli.model;
    explicit_request.base_url = cli.base_url;
    if (cli.api_key_env) {
        explicit_request.api_key_env = std::vector<std::string>{*cli.api_key_env};
    }
    explicit_request.auth = cli.auth;

    std::string registry = provider_registry_name;
    if (fake) {
        registry = "fake";
    }
    return resolve_provider_settings(registry, explicit_request, config, stored_provider, stored_model);
}

std::vector<std::string> resolved_api_key_env_chain(
    const CliProviderOverrides& cli,
    const ConfigData& config) {
    ProviderRequest explicit_request;
    if (cli.api_key_env) {
        explicit_request.api_key_env = std::vector<std::string>{*cli.api_key_env};
    }
    return resolve_api_key_env_chain(explicit_request, config);
}

} // namespace cch::coding_agent
