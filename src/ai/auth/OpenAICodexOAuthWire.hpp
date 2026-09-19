#pragma once

#include "DevicePoll.hpp"
#include "OAuthHttpClient.hpp"
#include "OAuthShared.hpp"
#include "OpenAICodexOAuth.hpp"

#include <cch/ai/Auth.hpp>
#include <cch/support/Error.hpp>

#include <boost/asio/awaitable.hpp>

#include <map>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>

namespace cch::ai::auth {

/// OpenAI Codex OAuth wire layer: authorize URL, callback/manual-code parsing,
/// the device-auth exchange, and token response parsing. The implementation TU
/// owns login sequencing, credential shaping, and the public hooks.

inline constexpr std::string_view kClientId = "app_EMoamEEZ73f0CkXaXp7hrann";
inline constexpr std::string_view kTokenUrl = "https://auth.openai.com/oauth/token";
inline constexpr std::string_view kAuthorizeUrl = "https://auth.openai.com/oauth/authorize";
inline constexpr std::string_view kRedirectUri = "http://localhost:1455/auth/callback";
inline constexpr std::string_view kDeviceUserCodeUrl = "https://auth.openai.com/api/accounts/deviceauth/usercode";
inline constexpr std::string_view kDeviceTokenUrl = "https://auth.openai.com/api/accounts/deviceauth/token";
inline constexpr std::string_view kDeviceVerificationUri = "https://auth.openai.com/codex/device";
inline constexpr std::string_view kDeviceRedirectUri = "https://auth.openai.com/deviceauth/callback";
inline constexpr std::string_view kScope = "openid profile email offline_access";
inline constexpr std::string_view kBrowserMethod = "browser";
inline constexpr std::string_view kDeviceCodeMethod = "device_code";
inline constexpr int kDeviceCodeTimeoutSeconds = 15 * 60;

struct DeviceAuthInfo {
    std::string device_auth_id{};
    std::string user_code{};
    int interval_seconds{0};
};

struct DeviceTokenSuccess {
    std::string authorization_code{};
    std::string code_verifier{};
};

inline const std::map<std::string, std::string, std::less<>> kFormHeaders{
        {"Content-Type", "application/x-www-form-urlencoded"},
};

inline const std::map<std::string, std::string, std::less<>> kJsonHeaders{
        {"Content-Type", "application/json"},
};

[[nodiscard]] std::string resolve_callback_host(const OpenAICodexOAuthOptions& options);

[[nodiscard]] std::string build_authorize_url(const std::string& challenge, const std::string& state);

[[nodiscard]] support::Expected<std::string> parse_manual_code(
        const std::string& input, const std::string& expected_state);

[[nodiscard]] boost::asio::awaitable<support::Expected<OAuthHttpResponse>> post_with_login_cancellation(
        const std::shared_ptr<OAuthHttpClient>& http_client,
        std::string url,
        std::map<std::string, std::string, std::less<>> headers,
        std::string body,
        std::stop_token stop_token);

[[nodiscard]] support::Expected<OAuthToken> read_token_response(
        const OAuthHttpResponse& response, std::string_view operation);

[[nodiscard]] support::Expected<ai::OAuthCredential> credentials_from_token(const OAuthToken& token);

[[nodiscard]] support::Expected<DeviceAuthInfo> parse_device_auth_response(const OAuthHttpResponse& response);

[[nodiscard]] support::Expected<DevicePollResult<DeviceTokenSuccess>> poll_device_token(
        const OAuthHttpResponse& response);

} // namespace cch::ai::auth
