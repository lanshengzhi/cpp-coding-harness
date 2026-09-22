#pragma once

#include "OAuthHttpClient.hpp"

#include <cch/ai/Auth.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <stop_token>

namespace cch::ai::auth {

struct OpenRouterOAuthOptions {
    /// Callback server bind host. When empty, PI_OAUTH_CALLBACK_HOST (then
    /// 127.0.0.1) is resolved at login time.
    std::optional<std::string> callback_host{std::nullopt};
    /// The production default is an OS-assigned ephemeral port.
    std::uint16_t callback_port{0};
};

/// OpenRouter account authorization through its PKCE flow. The exchanged
/// value is an API key carried in the existing OAuthCredential contract; no
/// request-time refresh traffic is needed.
class OpenRouterOAuth final : public std::enable_shared_from_this<OpenRouterOAuth> {
public:
    explicit OpenRouterOAuth(std::shared_ptr<OAuthHttpClient> http_client, OpenRouterOAuthOptions options = {});
    ~OpenRouterOAuth();
    OpenRouterOAuth(const OpenRouterOAuth&) = delete;
    OpenRouterOAuth& operator=(const OpenRouterOAuth&) = delete;

    [[nodiscard]] boost::asio::awaitable<support::Expected<ai::OAuthCredential>> login(ai::AuthInteraction interaction);
    [[nodiscard]] boost::asio::awaitable<support::Expected<ai::OAuthCredential>> refresh(
            ai::OAuthCredential credential);
    [[nodiscard]] boost::asio::awaitable<support::Expected<ai::ModelAuth>> to_auth(
            const ai::OAuthCredential& credential) const;

private:
    [[nodiscard]] boost::asio::awaitable<support::Expected<std::string>> exchange_code(
            std::string code, std::string verifier, std::stop_token stop_token);

    std::shared_ptr<OAuthHttpClient> http_client_;
    OpenRouterOAuthOptions options_;
};

/// Build OpenRouter's OAuth hooks. Tests inject a scripted HTTP client; the
/// default uses the production HTTPS client.
[[nodiscard]] ai::OAuthAuth make_openrouter_oauth_auth(
        std::shared_ptr<OAuthHttpClient> http_client = nullptr, OpenRouterOAuthOptions options = {});

} // namespace cch::ai::auth
