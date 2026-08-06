#pragma once

#include <cch/ai/CredentialStore.hpp>
#include <cch/util/Error.hpp>

#include <boost/asio/awaitable.hpp>

#include <functional>
#include <map>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace cch::ai {

using ProviderEnv = std::map<std::string, std::string, std::less<>>;
using ProviderHeaders = std::map<std::string, std::string, std::less<>>;

/// Short-lived request authentication. Secrets stay within trusted in-process
/// authentication and Provider capabilities and never enter Model values.
struct ModelAuth {
    std::optional<std::string> api_key{std::nullopt};
    ProviderHeaders headers{};
    std::optional<std::string> base_url{std::nullopt};
};

struct AuthResult {
    ModelAuth auth{};
    ProviderEnv env{};
    std::optional<std::string> source{std::nullopt};
};

enum class AuthType { ApiKey, OAuth };

struct AuthCheck {
    std::optional<std::string> source{std::nullopt};
    AuthType type{AuthType::ApiKey};
};

/// Injectable ambient-auth capability. Implementations may consult process
/// environment and user-controlled files; Models itself owns no such globals.
class AuthContext {
public:
    virtual ~AuthContext() = default;

    [[nodiscard]] virtual boost::asio::awaitable<util::Expected<std::optional<std::string>>> environment(
        std::string name) const = 0;
    [[nodiscard]] virtual boost::asio::awaitable<util::Expected<bool>> file_exists(
        std::string path) const = 0;
};

/// One selectable option in an AuthPromptSelect.
struct AuthPromptOption {
    std::string id{};
    std::string label{};
    std::optional<std::string> description{std::nullopt};
};

struct AuthPromptText {
    std::string message{};
    std::optional<std::string> placeholder{std::nullopt};
};

struct AuthPromptSecret {
    std::string message{};
    std::optional<std::string> placeholder{std::nullopt};
};

struct AuthPromptSelect {
    std::string message{};
    std::vector<AuthPromptOption> options{};
};

struct AuthPromptManualCode {
    std::string message{};
    std::optional<std::string> placeholder{std::nullopt};
};

/// Prompt shown to the user during login. `Select` answers with the selected
/// option id; every other kind answers with the entered text. The optional
/// per-prompt `std::stop_token` lets the flow cancel a pending prompt when an
/// out-of-band event resolves the step, e.g. a `manual_code` prompt raced
/// against the Codex callback server is aborted when the callback wins.
// §3.3 names variant aliases `*Variant`; `AuthPromptKind` predates the rule
// and keeps its kind-suffixed name to avoid public API churn (debt recorded
// in #372).
using AuthPromptKind = std::variant<
    AuthPromptText,
    AuthPromptSecret,
    AuthPromptSelect,
    AuthPromptManualCode>;

struct AuthPrompt {
    AuthPromptKind kind{AuthPromptText{}};
    std::optional<std::stop_token> stop_token{std::nullopt};
};

/// A displayable link on an AuthInfo.
struct AuthInfoLink {
    std::string url{};
    std::optional<std::string> label{std::nullopt};
};

struct AuthInfo {
    std::string message{};
    std::vector<AuthInfoLink> links{};
};

struct AuthUrl {
    std::string url{};
    std::optional<std::string> instructions{std::nullopt};
};

struct AuthDeviceCode {
    std::string user_code{};
    std::string verification_uri{};
    std::optional<int> interval_seconds{std::nullopt};
    std::optional<int> expires_in_seconds{std::nullopt};
};

struct AuthProgress {
    std::string message{};
};

/// Best-effort display event emitted during login. Never carries secrets:
/// authorization codes, PKCE verifiers, tokens, and account ids are excluded
/// by contract (secret boundary, #327).
// §3.3 names variant aliases `*Variant`; `AuthEventKind` predates the rule and
// keeps its kind-suffixed name to avoid public API churn (debt recorded in
// #372).
using AuthEventKind = std::variant<AuthInfo, AuthUrl, AuthDeviceCode, AuthProgress>;

struct AuthEvent {
    AuthEventKind kind{AuthInfo{}};
};

/// Returns the entered/selected string (`Select` returns the option id).
/// Rejects on cancel/abort; the returned error is the login failure. The
/// interaction's `std::stop_token` aborts the whole flow; per-prompt
/// cancellation uses AuthPrompt::stop_token.
using AuthPromptHook = std::move_only_function<
    boost::asio::awaitable<util::Expected<std::string>>(AuthPrompt)>;

/// Best-effort display callback. Never throws; secrets never appear here.
using AuthNotifyHook = std::move_only_function<void(const AuthEvent&)>;

/// Login interaction callbacks serving both api-key and OAuth flows. The host
/// owns the `std::stop_source`; this object observes its `std::stop_token`.
/// Hooks are invoked on the moved-to interaction, so the flow may own it.
struct AuthInteraction {
    std::stop_token stop_token{};
    AuthPromptHook prompt{};
    AuthNotifyHook notify{};
};

/// Borrowed hook inputs remain valid until each returned awaitable completes.
using ApiKeyCheckHook = std::move_only_function<
    boost::asio::awaitable<util::Expected<std::optional<AuthCheck>>>(
        const AuthContext&,
        std::optional<ApiKeyCredential>)>;
using ApiKeyResolveHook = std::move_only_function<
    boost::asio::awaitable<util::Expected<std::optional<AuthResult>>>(
        const AuthContext&,
        std::optional<ApiKeyCredential>)>;
using ApiKeyLoginHook = std::move_only_function<
    boost::asio::awaitable<util::Expected<ApiKeyCredential>>(
        AuthInteraction)>;
using OAuthRefreshHook = std::move_only_function<
    boost::asio::awaitable<util::Expected<OAuthCredential>>(
        OAuthCredential)>;
using OAuthToAuthHook = std::move_only_function<
    boost::asio::awaitable<util::Expected<ModelAuth>>(
        const OAuthCredential&)>;
using OAuthLoginHook = std::move_only_function<
    boost::asio::awaitable<util::Expected<OAuthCredential>>(
        AuthInteraction)>;

struct ApiKeyAuth {
    std::string name{};
    ApiKeyCheckHook check{};
    ApiKeyResolveHook resolve{};
    /// Interactive setup (prompt for key/provider env). Absent = ambient-only.
    ApiKeyLoginHook login{};
};

struct OAuthAuth {
    std::string name{};
    /// Interactive login. Runs the provider-owned flow and returns the
    /// credential; `Models::login` persists it via CredentialStore::modify.
    OAuthLoginHook login{};
    OAuthRefreshHook refresh{};
    OAuthToAuthHook to_auth{};
};

struct ProviderAuth {
    std::optional<ApiKeyAuth> api_key{std::nullopt};
    std::optional<OAuthAuth> oauth{std::nullopt};
};

} // namespace cch::ai
