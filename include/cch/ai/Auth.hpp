#pragma once

#include <cch/ai/CredentialStore.hpp>
#include <cch/util/Error.hpp>

#include <boost/asio/awaitable.hpp>

#include <functional>
#include <map>
#include <optional>
#include <string>

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

/// Borrowed hook inputs remain valid until each returned awaitable completes.
using ApiKeyCheckHook = std::move_only_function<
    boost::asio::awaitable<util::Expected<std::optional<AuthCheck>>>(
        const AuthContext&,
        std::optional<ApiKeyCredential>)>;
using ApiKeyResolveHook = std::move_only_function<
    boost::asio::awaitable<util::Expected<std::optional<AuthResult>>>(
        const AuthContext&,
        std::optional<ApiKeyCredential>)>;
using OAuthRefreshHook = std::move_only_function<
    boost::asio::awaitable<util::Expected<OAuthCredential>>(
        OAuthCredential)>;
using OAuthToAuthHook = std::move_only_function<
    boost::asio::awaitable<util::Expected<ModelAuth>>(
        const OAuthCredential&)>;

struct ApiKeyAuth {
    std::string name{};
    ApiKeyCheckHook check{};
    ApiKeyResolveHook resolve{};
};

struct OAuthAuth {
    std::string name{};
    OAuthRefreshHook refresh{};
    OAuthToAuthHook to_auth{};
};

struct ProviderAuth {
    std::optional<ApiKeyAuth> api_key{std::nullopt};
    std::optional<OAuthAuth> oauth{std::nullopt};
};

} // namespace cch::ai
