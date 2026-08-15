#pragma once

#include "OAuthHttpClient.hpp"

#include <cch/ai/Auth.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace cch::ai::auth {

struct OpenAICodexOAuthOptions {
    /// Callback server bind host. When empty, PI_OAUTH_CALLBACK_HOST (then
    /// 127.0.0.1) is resolved at login time, matching pi's `getCallbackHost`.
    std::optional<std::string> callback_host{std::nullopt};
    /// Callback server bind port; the frozen default is 1455.
    std::uint16_t callback_port{1455};
};

/// OpenAI Codex browser/device OAuth content, ported from pi baseline
/// 83114817 `packages/ai/src/auth/oauth/openai-codex.ts`. Owns the prompt
/// texts, instructions, HTML pages, and error messages; never renders.
class OpenAICodexOAuth final : public std::enable_shared_from_this<OpenAICodexOAuth> {
public:
    explicit OpenAICodexOAuth(
        std::shared_ptr<OAuthHttpClient> http_client,
        OpenAICodexOAuthOptions options = {});
    ~OpenAICodexOAuth();
    OpenAICodexOAuth(const OpenAICodexOAuth&) = delete;
    OpenAICodexOAuth& operator=(const OpenAICodexOAuth&) = delete;

    [[nodiscard]] boost::asio::awaitable<support::Expected<ai::OAuthCredential>> login(
        ai::AuthInteraction interaction);
    [[nodiscard]] boost::asio::awaitable<support::Expected<ai::OAuthCredential>> refresh(
        ai::OAuthCredential credential);
    [[nodiscard]] boost::asio::awaitable<support::Expected<ai::ModelAuth>> to_auth(
        const ai::OAuthCredential& credential) const;

private:
    [[nodiscard]] boost::asio::awaitable<support::Expected<ai::OAuthCredential>> login_browser(
        ai::AuthInteraction interaction);
    [[nodiscard]] boost::asio::awaitable<support::Expected<ai::OAuthCredential>> login_device_code(
        ai::AuthInteraction interaction);
    [[nodiscard]] boost::asio::awaitable<support::Expected<ai::OAuthCredential>> exchange_code(
        std::string code,
        std::string verifier,
        std::string redirect_uri,
        std::stop_token stop_token);

    std::shared_ptr<OAuthHttpClient> http_client_;
    OpenAICodexOAuthOptions options_;
};

/// Build the `OAuthAuth` hooks for the openai-codex provider. The default
/// HTTP client is the Boost.Beast implementation; tests inject scripted fakes.
[[nodiscard]] ai::OAuthAuth make_openai_codex_oauth_auth(
    std::shared_ptr<OAuthHttpClient> http_client = nullptr,
    OpenAICodexOAuthOptions options = {});

} // namespace cch::ai::auth
