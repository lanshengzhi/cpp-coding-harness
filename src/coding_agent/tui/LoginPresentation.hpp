#pragma once

#include "coding_agent/tui/OAuthSelector.hpp"

#include <cch/ai/CredentialStore.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cch::coding_agent {
class ModelRuntime;
} // namespace cch::coding_agent

namespace cch::coding_agent::tui {

/// pi `getLoginProviderOptions`: one row per (provider, offered auth type)
/// carrying the auth-status snapshot, sorted by provider name.
[[nodiscard]] std::vector<AuthSelectorProvider> compute_login_provider_options(
    coding_agent::ModelRuntime& runtime,
    std::optional<AuthSelectorType> filter = std::nullopt);

/// pi `findLoginProviderOptions`: exact provider id/name match
/// (case-insensitive); an empty reference matches nothing.
[[nodiscard]] std::vector<AuthSelectorProvider> find_login_provider_options(
    const std::vector<AuthSelectorProvider>& options,
    std::string_view provider_ref);

/// pi `getLogoutProviderOptions`: stored credentials mapped to selector rows
/// (provider name fallback to id, `stored credential` source), sorted.
[[nodiscard]] std::vector<AuthSelectorProvider> compute_logout_provider_options(
    coding_agent::ModelRuntime& runtime,
    std::vector<ai::CredentialInfo> credentials);

/// pi: "Logged in to X" / "Saved API key for X".
[[nodiscard]] std::string login_action_label(
    AuthSelectorType type,
    const std::string& provider_name);

/// pi `getAuthPath()` display path: `<agentDir>/auth.json`.
[[nodiscard]] std::string auth_path_display(const std::filesystem::path& agent_dir);

/// pi post-login status: `<actionLabel>[. Selected <modelId>]. Credentials
/// saved to <authPath>`.
[[nodiscard]] std::string login_success_status(
    const std::string& action_label,
    const std::optional<std::string>& selected_model_id,
    const std::string& auth_path);

// pi's four verbatim post-login selection-error messages.

[[nodiscard]] std::string login_selection_error_no_default_model(
    const std::string& action_label,
    const std::string& provider_id);
[[nodiscard]] std::string login_selection_error_no_models(
    const std::string& action_label);
[[nodiscard]] std::string login_selection_error_default_unavailable(
    const std::string& action_label,
    const std::string& default_model_id);
[[nodiscard]] std::string login_selection_error_select_failed(
    const std::string& action_label,
    const std::string& error_message);

/// pi's login provider-selector empty messages.
[[nodiscard]] std::string_view login_provider_selector_empty_message(
    std::optional<AuthSelectorType> filter);

/// pi's auth-type picker labels ("Sign in with an account" is the default
/// subscription label; the C++ OAuthAuth has no `loginLabel` surface).
[[nodiscard]] inline std::string_view login_subscription_label() {
    return "Sign in with an account";
}
[[nodiscard]] inline std::string_view login_api_key_label() {
    return "Sign in with an API key";
}
[[nodiscard]] inline std::string_view login_methods_empty_message() {
    return "No login methods available.";
}

/// pi's logout flow messages.
[[nodiscard]] inline std::string_view logout_no_credentials_message() {
    return "No stored credentials to remove. /logout only removes credentials "
           "saved by /login; environment variables and models.json config are "
           "unchanged.";
}
[[nodiscard]] std::string logout_success_message(
    AuthSelectorType type,
    const std::string& provider_name);

} // namespace cch::coding_agent::tui
