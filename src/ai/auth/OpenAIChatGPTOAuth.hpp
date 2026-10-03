#pragma once

#include "OAuthHttpClient.hpp"

#include <cch/ai/Auth.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace cch::ai::auth {

struct OpenAIChatGPTOAuthOptions {
    /// Callback server bind host. When empty, PI_OAUTH_CALLBACK_HOST (then
    /// 127.0.0.1) is resolved at login time, matching pi's `getCallbackHost`.
    std::optional<std::string> callback_host{std::nullopt};
    /// Callback server bind port; the frozen default is 1455.
    std::uint16_t callback_port{1455};
};

/// OpenAI Responses API token sharing through Sign in with ChatGPT (pi
/// baseline `packages/ai/src/auth/oauth/openai-chatgpt.ts`). Public-client
/// flow with dynamic client registration: every login registers a new client,
/// OpenAI returns the issued client ID in the callback, and the resulting
/// user access token is sent directly to api.openai.com.
class OpenAIChatGPTOAuth final : public std::enable_shared_from_this<OpenAIChatGPTOAuth> {
public:
    explicit OpenAIChatGPTOAuth(
        std::shared_ptr<OAuthHttpClient> http_client,
        OpenAIChatGPTOAuthOptions options = {});
    ~OpenAIChatGPTOAuth();
    OpenAIChatGPTOAuth(const OpenAIChatGPTOAuth&) = delete;
    OpenAIChatGPTOAuth& operator=(const OpenAIChatGPTOAuth&) = delete;

    [[nodiscard]] boost::asio::awaitable<support::Expected<ai::OAuthCredential>> login(
        ai::AuthInteraction interaction,
        std::optional<ai::LoginOptions> options = std::nullopt);
    [[nodiscard]] boost::asio::awaitable<support::Expected<ai::OAuthCredential>> refresh(
        ai::OAuthCredential credential);
    [[nodiscard]] boost::asio::awaitable<support::Expected<ai::ModelAuth>> to_auth(
        const ai::OAuthCredential& credential) const;

private:
    [[nodiscard]] boost::asio::awaitable<support::Expected<ai::OAuthCredential>> exchange_code(
        std::string code,
        std::string client_id,
        std::string verifier,
        std::stop_token stop_token);

    std::shared_ptr<OAuthHttpClient> http_client_;
    OpenAIChatGPTOAuthOptions options_;
};

/// Build the `OAuthAuth` hooks for the openai provider's "Sign in with
/// ChatGPT" method. The default HTTP client is the Boost.Beast
/// implementation; tests inject scripted fakes.
[[nodiscard]] ai::OAuthAuth make_openai_chatgpt_oauth_auth(
    std::shared_ptr<OAuthHttpClient> http_client = nullptr,
    OpenAIChatGPTOAuthOptions options = {});

} // namespace cch::ai::auth
