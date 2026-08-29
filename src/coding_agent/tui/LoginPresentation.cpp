#include "LoginPresentation.hpp"

#include <cch/coding_agent/ModelRuntime.hpp>

#include <algorithm>
#include <cctype>
#include <utility>

namespace cch::coding_agent::tui {
namespace {

[[nodiscard]] std::string lowercase(std::string_view text) {
    std::string lowered;
    lowered.reserve(text.size());
    for (const char ch : text) {
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return lowered;
}

[[nodiscard]] std::string_view trimmed(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\n\r");
    if (first == std::string_view::npos) return {};
    const auto last = text.find_last_not_of(" \t\n\r");
    return text.substr(first, last - first + 1);
}

} // namespace

std::vector<AuthSelectorProvider> compute_login_provider_options(
    coding_agent::ModelRuntime& runtime,
    std::optional<AuthSelectorType> filter) {
    std::vector<AuthSelectorProvider> options;
    for (const auto& provider : runtime.providers()) {
        // pi: `getProviderAuthStatus` + `isUsingOAuth` for the row status.
        const auto auth_status = runtime.get_provider_auth_status(provider.id);
        std::optional<AuthSelectorStatus> status;
        if (auth_status && auth_status->configured) {
            status = AuthSelectorStatus{
                    .type = runtime.is_using_oauth(provider.id) ? AuthSelectorType::OAuth : AuthSelectorType::ApiKey,
                    .source = auth_status->label ? auth_status->label : auth_status->source,
            };
        }
        for (const auto& method : provider.auth_methods) {
            const auto selector_type =
                    method.type == ai::AuthType::OAuth ? AuthSelectorType::OAuth : AuthSelectorType::ApiKey;
            if (filter && *filter != selector_type) {
                continue;
            }
            options.push_back(AuthSelectorProvider{
                    .id = provider.id,
                    .name = provider.name,
                    .auth_type = selector_type,
                    .method_name = method.name,
                    .status = status,
                    .has_login = method.has_login,
            });
        }
    }
    // pi `a.name.localeCompare(b.name)`; the catalog names are ASCII.
    std::sort(options.begin(), options.end(), [](const auto& left, const auto& right) {
        return left.name < right.name;
    });
    return options;
}

std::vector<AuthSelectorProvider> find_login_provider_options(
    const std::vector<AuthSelectorProvider>& options,
    std::string_view provider_ref) {
    const auto normalized = lowercase(trimmed(provider_ref));
    if (normalized.empty()) return {};
    std::vector<AuthSelectorProvider> matches;
    for (const auto& option : options) {
        if (lowercase(option.id) == normalized || lowercase(option.name) == normalized) {
            matches.push_back(option);
        }
    }
    return matches;
}

std::vector<AuthSelectorProvider> compute_logout_provider_options(
    coding_agent::ModelRuntime& runtime,
    std::vector<ai::CredentialInfo> credentials) {
    std::vector<AuthSelectorProvider> options;
    options.reserve(credentials.size());
    for (auto& credential : credentials) {
        const auto type = credential.type == "oauth"
            ? AuthSelectorType::OAuth
            : AuthSelectorType::ApiKey;
        const auto provider = runtime.provider(credential.provider_id);
        options.push_back(AuthSelectorProvider{
                .id = credential.provider_id,
                .name = provider ? provider->name : credential.provider_id,
                .auth_type = type,
                .method_name = std::nullopt,
                .status =
                        AuthSelectorStatus{
                                .type = type,
                                .source = "stored credential",
                        },
        });
    }
    std::sort(options.begin(), options.end(), [](const auto& left, const auto& right) {
        return left.name < right.name;
    });
    return options;
}

std::string login_action_label(
    AuthSelectorType type,
    const std::string& provider_name) {
    return type == AuthSelectorType::OAuth
        ? "Logged in to " + provider_name
        : "Saved API key for " + provider_name;
}

std::string auth_path_display(const std::filesystem::path& agent_dir) {
    return (agent_dir / "auth.json").string();
}

std::string login_success_status(
    const std::string& action_label,
    const std::optional<std::string>& selected_model_id,
    const std::string& auth_path) {
    return action_label +
        (selected_model_id ? ". Selected " + *selected_model_id : "") +
        ". Credentials saved to " + auth_path;
}

std::string login_selection_error_no_default_model(
    const std::string& action_label,
    const std::string& provider_id) {
    return action_label + ", but no default model is configured for provider \"" +
        provider_id + "\". Use /model to select a model.";
}

std::string login_selection_error_no_models(const std::string& action_label) {
    return action_label +
        ", but no models are available for that provider. Use /model to select a model.";
}

std::string login_selection_error_default_unavailable(
    const std::string& action_label,
    const std::string& default_model_id) {
    return action_label + ", but its default model \"" + default_model_id +
        "\" is not available. Use /model to select a model.";
}

std::string login_selection_error_select_failed(
    const std::string& action_label,
    const std::string& error_message) {
    return action_label + ", but selecting its default model failed: " + error_message +
        ". Use /model to select a model.";
}

std::string_view login_provider_selector_empty_message(
    std::optional<AuthSelectorType> filter) {
    if (!filter) return "No login providers available.";
    return *filter == AuthSelectorType::OAuth
        ? "No subscription providers available."
        : "No API key providers available.";
}

std::string logout_success_message(
    AuthSelectorType type,
    const std::string& provider_name) {
    return type == AuthSelectorType::OAuth
        ? "Logged out of " + provider_name
        : "Removed stored API key for " + provider_name +
              ". Environment variables and models.json config are unchanged.";
}

} // namespace cch::coding_agent::tui
