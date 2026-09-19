#pragma once

#include "KimiCodingOAuth.hpp"
#include "OAuthHttpClient.hpp"
#include "OAuthShared.hpp"

#include <cch/support/Error.hpp>

#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>

namespace cch::ai::auth {

/// Kimi Code OAuth wire layer: device authorization, token exchange, polling,
/// and response parsing. The implementation TU owns login/refresh policy
/// (retry loops, credential shaping) and calls into this surface.

constexpr std::string_view kDefaultOauthHost = "https://auth.kimi.com";
constexpr std::string_view kDeviceAuthorizationPath = "/api/oauth/device_authorization";
constexpr std::string_view kTokenPath = "/api/oauth/token";
constexpr std::string_view kClientId = "17e5f671-d194-4dfb-9706-5516cb48c098";
constexpr std::string_view kGrantTypeDeviceCode = "urn:ietf:params:oauth:grant-type:device_code";
constexpr std::string_view kGrantTypeRefresh = "refresh_token";
constexpr int kDeviceCodeTimeoutSeconds = 15 * 60;
constexpr int kDefaultPollIntervalSeconds = 5;

/// Device authorization response carrying pi's frozen field set: the RFC 8628
/// fields plus the 5s/15min interval/expires defaults.
struct DeviceAuthorization {
    std::string device_code{};
    std::string user_code{};
    std::string verification_uri{};
    std::string verification_uri_complete{};
    int interval_seconds{kDefaultPollIntervalSeconds};
    int expires_in_seconds{kDeviceCodeTimeoutSeconds};
};

/// OAuth host from options, defaulting to the Kimi auth host, with trailing
/// slashes trimmed.
[[nodiscard]] std::string resolve_oauth_host(const KimiCodingOAuthOptions& options);

/// POST with the composed per-request timeout and login cancellation. An
/// aborted login normalizes to the stable "Login cancelled" error; a fired
/// request timeout surfaces as `Timeout`; other transport failures propagate.
[[nodiscard]] boost::asio::awaitable<support::Expected<OAuthHttpResponse>> post_kimi_request(
        const std::shared_ptr<OAuthHttpClient>& http_client,
        std::string url,
        std::string body,
        std::stop_token stop_token,
        std::chrono::milliseconds request_timeout);

/// pi `startDeviceAuthorization`: request a device code and validate the
/// http(s)-only verification URIs.
[[nodiscard]] boost::asio::awaitable<support::Expected<DeviceAuthorization>> start_device_authorization(
        const std::shared_ptr<OAuthHttpClient>& http_client,
        std::string oauth_host,
        std::stop_token stop_token,
        std::chrono::milliseconds request_timeout);

/// pi `pollForToken`: the shared device-poll loop with wait-before-first-poll
/// and the RFC 8628 error decision table.
[[nodiscard]] boost::asio::awaitable<support::Expected<OAuthToken>> poll_for_token(
        const std::shared_ptr<OAuthHttpClient>& http_client,
        const std::string& oauth_host,
        const DeviceAuthorization& device,
        std::stop_token stop_token,
        std::chrono::milliseconds request_timeout);

/// Token triple from one token-endpoint response; `operation` names the caller
/// in the missing-field diagnostics.
[[nodiscard]] support::Expected<OAuthToken> parse_token_response(
        const OAuthHttpResponse& response, std::string_view operation);

/// Frozen retryable-status set for the refresh path.
[[nodiscard]] bool is_retryable_refresh_failure(int status_code);

/// Plain (uncancellable) backoff sleep used between refresh attempts.
[[nodiscard]] boost::asio::awaitable<void> backoff_sleep(std::chrono::milliseconds duration);

} // namespace cch::ai::auth
