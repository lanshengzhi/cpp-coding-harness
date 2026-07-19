#include "ProviderConfigResolution.hpp"
#include "../../include/cch/coding_agent/AgentConfigDir.hpp"
#include "../../include/cch/coding_agent/AuthLoader.hpp"
#include "../../include/cch/coding_agent/Settings.hpp"

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
    const UserSettings& settings) {
    if (explicit_request.api_key_env && !explicit_request.api_key_env->empty()) {
        return *explicit_request.api_key_env;
    }
    if (settings.api_key_env && !settings.api_key_env->empty()) {
        return *settings.api_key_env;
    }
    return {"OPENAI_API_KEY"};
}

} // namespace

ResolvedProviderSettings resolve_provider_settings(
    std::string_view provider_registry_name,
    const ProviderRequest& explicit_request,
    const UserSettings& settings,
    const std::optional<std::string>& stored_provider,
    const std::optional<std::string>& stored_model) {
    const std::string registry_name{provider_registry_name};

    std::string resolved_provider = registry_name;
    if (explicit_request.provider) {
        resolved_provider = *explicit_request.provider;
    } else if (stored_provider) {
        resolved_provider = *stored_provider;
    } else if (settings.provider) {
        resolved_provider = *settings.provider;
    }

    std::string resolved_model;
    if (explicit_request.model) {
        resolved_model = *explicit_request.model;
    } else if (stored_model) {
        resolved_model = *stored_model;
    } else if (settings.model) {
        resolved_model = *settings.model;
    } else {
        resolved_model = provider_default_model(registry_name);
    }

    std::string resolved_base_url;
    if (explicit_request.base_url) {
        resolved_base_url = *explicit_request.base_url;
    } else if (settings.base_url) {
        resolved_base_url = *settings.base_url;
    } else {
        resolved_base_url = "https://api.openai.com";
    }

    const auto resolved_api_key_env_chain = resolve_api_key_env_chain(explicit_request, settings);

    std::string resolved_auth;
    if (explicit_request.auth) {
        resolved_auth = *explicit_request.auth;
    } else if (settings.auth) {
        resolved_auth = *settings.auth;
    }

    std::string resolved_api_key;
    if (!resolved_auth.empty()) {
        auto auth_entries = AuthLoader::load(auth_file_path());
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

} // namespace cch::coding_agent
