#include "OpenAICodexOAuthWire.hpp"

#include "DevicePoll.hpp"
#include "OAuthCallbackServer.hpp"
#include "OAuthShared.hpp"
#include "Pkce.hpp"
#include "ai/JsonAccess.hpp"
#include "ai/Timestamps.hpp"
#include "support/AsyncResultBridge.hpp"
#include "support/ExpectedMacros.hpp"
#include "support/Json.hpp"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/experimental/channel.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <chrono>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
namespace cch::ai::auth {

[[nodiscard]] std::string resolve_callback_host(const OpenAICodexOAuthOptions& options) {
    if (options.callback_host) {
        return *options.callback_host;
    }
    if (const char* override_value = std::getenv("PI_OAUTH_CALLBACK_HOST");
            override_value != nullptr && *override_value != '\0') {
        return override_value;
    }
    return "127.0.0.1";
}

/// pi `createAuthorizationFlow`: the authorize URL with URLSearchParams
/// encoding in pi's exact insertion order, `originator=pi` byte-identical.
[[nodiscard]] std::string build_authorize_url(const std::string& challenge, const std::string& state) {
    return std::string{kAuthorizeUrl} + "?response_type=code" + "&client_id=" + url_query_encode(kClientId) +
           "&redirect_uri=" + url_query_encode(kRedirectUri) + "&scope=" + url_query_encode(kScope) +
           "&code_challenge=" + url_query_encode(challenge) + "&code_challenge_method=S256" +
           "&state=" + url_query_encode(state) + "&id_token_add_organizations=true" +
           "&codex_cli_simplified_flow=true" + "&originator=pi";
}

/// pi `parseAuthorizationInput` validation applied to manual code entry.
[[nodiscard]] support::Expected<std::string> parse_manual_code(
        const std::string& input, const std::string& expected_state) {
    const auto parsed = parse_authorization_input(input);
    if (parsed.state && *parsed.state != expected_state) {
        return std::unexpected(support::make_error(support::ErrorCode::OAuth, "State mismatch"));
    }
    if (!parsed.code) {
        return std::unexpected(support::make_error(support::ErrorCode::OAuth, "Missing authorization code"));
    }
    return *parsed.code;
}

/// pi `fetchWithLoginCancellation`: an aborted request normalizes to the
/// stable "Login cancelled" error; other transport failures propagate.
[[nodiscard]] boost::asio::awaitable<support::Expected<OAuthHttpResponse>> post_with_login_cancellation(
        const std::shared_ptr<OAuthHttpClient>& http_client,
        std::string url,
        std::map<std::string, std::string, std::less<>> headers,
        std::string body,
        std::stop_token stop_token) {
    auto response = co_await http_client->post(std::move(url), std::move(headers), std::move(body), stop_token);
    if (!response) {
        if (stop_token.stop_requested()) {
            co_return std::unexpected(support::make_error(support::ErrorCode::Cancelled, "Login cancelled"));
        }
        co_return std::unexpected(std::move(response.error()));
    }
    co_return *response;
}

/// pi `readTokenResponse`: non-2xx and missing-field responses carry the
/// frozen message with the raw body; success yields the OAuth token with
/// `expires` as a wall-clock millisecond timestamp.
[[nodiscard]] support::Expected<OAuthToken> read_token_response(
        const OAuthHttpResponse& response, std::string_view operation) {
    const auto missing_fields = [&response, operation]() {
        return support::make_error(support::ErrorCode::OAuth,
                "OpenAI Codex token " + std::string{operation} + " response missing fields: " + response.body);
    };
    if (response.status_code < 200 || response.status_code >= 300) {
        return std::unexpected(support::make_error(support::ErrorCode::OAuth,
                "OpenAI Codex token " + std::string{operation} + " failed (" + std::to_string(response.status_code) +
                        "): " + (response.body.empty() ? "unknown" : response.body)));
    }
    if (auto json = support::read_json(response.body); !json) {
        return std::unexpected(missing_fields());
    } else if (const auto* object = json_object(*json)) {
        const auto access = json_string_member(*object, "access_token");
        const auto refresh = json_string_member(*object, "refresh_token");
        const auto expires = json_number_member(*object, "expires_in");
        if (!access || !refresh || !expires) {
            return std::unexpected(missing_fields());
        }
        return OAuthToken{
                .access = std::string{*access},
                .refresh = std::string{*refresh},
                .expires = oauth_token_expiry_ms(*expires, current_timestamp_ms()),
        };
    } else {
        return std::unexpected(missing_fields());
    }
}

/// pi `credentialsFromToken`: accountId extraction from the unverified JWT is
/// mandatory; absence fails the operation.
[[nodiscard]] support::Expected<ai::OAuthCredential> credentials_from_token(const OAuthToken& token) {
    if (auto account_id = extract_account_id(token.access); !account_id) {
        return std::unexpected(
                support::make_error(support::ErrorCode::OAuth, "Failed to extract accountId from token"));
    } else {
        return ai::OAuthCredential{
                .refresh = token.refresh,
                .access = token.access,
                .expires = token.expires,
                .account_id = *account_id,
        };
    }
}

[[nodiscard]] support::Expected<DeviceAuthInfo> parse_device_auth_response(const OAuthHttpResponse& response) {
    if (response.status_code == 404) {
        return std::unexpected(support::make_error(support::ErrorCode::OAuth,
                "OpenAI Codex device code login is not enabled for this server. "
                "Use browser login or verify the server URL."));
    }
    if (response.status_code < 200 || response.status_code >= 300) {
        return std::unexpected(support::make_error(support::ErrorCode::OAuth,
                "OpenAI Codex device code request failed with status " + std::to_string(response.status_code) +
                        (response.body.empty() ? "" : ": " + response.body)));
    }
    if (auto json = support::read_json(response.body); !json) {
        return std::unexpected(support::make_error(
                support::ErrorCode::OAuth, "Invalid OpenAI Codex device code response: " + response.body));
    } else {
        const auto* object = json_object(*json);
        const auto device_auth_id = object ? json_string_member(*object, "device_auth_id") : std::nullopt;
        const auto user_code = object ? json_string_member(*object, "user_code") : std::nullopt;
        int interval_seconds = -1;
        if (object != nullptr) {
            if (const auto interval = json_number_member(*object, "interval")) {
                interval_seconds = static_cast<int>(*interval);
            } else if (const auto interval_text = json_string_member(*object, "interval")) {
                // Numeric interval may arrive as a string; validated below.
                int parsed = -1;
                const auto [position, error] =
                        std::from_chars(interval_text->data(), interval_text->data() + interval_text->size(), parsed);
                if (error == std::errc{} && position == interval_text->data() + interval_text->size()) {
                    interval_seconds = parsed;
                }
            }
        }
        if (!device_auth_id || !user_code || interval_seconds < 0) {
            return std::unexpected(support::make_error(
                    support::ErrorCode::OAuth, "Invalid OpenAI Codex device code response: " + response.body));
        }
        return DeviceAuthInfo{
                .device_auth_id = std::string{*device_auth_id},
                .user_code = std::string{*user_code},
                .interval_seconds = interval_seconds,
        };
    }
}

[[nodiscard]] support::Expected<DevicePollResult<DeviceTokenSuccess>> poll_device_token(
        const OAuthHttpResponse& response) {
    if (response.status_code >= 200 && response.status_code < 300) {
        if (auto json = support::read_json(response.body); !json) {
            return DevicePollResult<DeviceTokenSuccess>{
                    .kind =
                            DevicePollResult<DeviceTokenSuccess>::Failed{
                                    .message = "Invalid OpenAI Codex device auth token "
                                               "response: " +
                                               response.body,
                            },
            };
        } else if (const auto* object = json_object(*json)) {
            const auto authorization_code = json_string_member(*object, "authorization_code");
            const auto code_verifier = json_string_member(*object, "code_verifier");
            if (!authorization_code || !code_verifier) {
                return DevicePollResult<DeviceTokenSuccess>{
                        .kind =
                                DevicePollResult<DeviceTokenSuccess>::Failed{
                                        .message = "Invalid OpenAI Codex device auth token "
                                                   "response: " +
                                                   response.body,
                                },
                };
            }
            return DevicePollResult<DeviceTokenSuccess>{
                    .kind =
                            DevicePollResult<DeviceTokenSuccess>::Complete{
                                    .value =
                                            DeviceTokenSuccess{
                                                    .authorization_code = std::string{*authorization_code},
                                                    .code_verifier = std::string{*code_verifier},
                                            },
                            },
            };
        } else {
            return DevicePollResult<DeviceTokenSuccess>{
                    .kind =
                            DevicePollResult<DeviceTokenSuccess>::Failed{
                                    .message = "Invalid OpenAI Codex device auth token "
                                               "response: " +
                                               response.body,
                            },
            };
        }
    }
    if (response.status_code == 403 || response.status_code == 404) {
        return DevicePollResult<DeviceTokenSuccess>{
                .kind = DevicePollResult<DeviceTokenSuccess>::Pending{},
        };
    }
    std::optional<std::string> error_code;
    if (auto json = support::read_json(response.body); json) {
        if (const auto* object = json_object(*json)) {
            const auto error_found = object->find("error");
            if (error_found != object->end()) {
                if (const auto* code = error_found->second.get_if<std::string>()) {
                    error_code = *code;
                } else if (const auto* error_object = error_found->second.get_if<support::JsonValue::object_t>()) {
                    if (const auto code = json_string_member(*error_object, "code")) {
                        error_code = std::string{*code};
                    }
                }
            }
        }
    }
    if (error_code == "deviceauth_authorization_pending") {
        return DevicePollResult<DeviceTokenSuccess>{
                .kind = DevicePollResult<DeviceTokenSuccess>::Pending{},
        };
    }
    if (error_code == "slow_down") {
        return DevicePollResult<DeviceTokenSuccess>{
                .kind = DevicePollResult<DeviceTokenSuccess>::SlowDown{},
        };
    }
    return DevicePollResult<DeviceTokenSuccess>{
            .kind =
                    DevicePollResult<DeviceTokenSuccess>::Failed{
                            .message = "OpenAI Codex device auth failed with status " +
                                       std::to_string(response.status_code) +
                                       (response.body.empty() ? "" : ": " + response.body),
                    },
    };
}

} // namespace cch::ai::auth
