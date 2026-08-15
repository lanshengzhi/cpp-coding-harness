#include "KimiCodingOAuth.hpp"

#include "DevicePoll.hpp"
#include "Pkce.hpp"
#include "ai/AsyncResultBridge.hpp"
#include "support/ExpectedMacros.hpp"
#include "support/Json.hpp"

#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/error_code.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>

namespace cch::ai::auth {
namespace {

constexpr std::string_view kClientId = "17e5f671-d194-4dfb-9706-5516cb48c098";
constexpr std::string_view kDefaultOauthHost = "https://auth.kimi.com";
constexpr std::string_view kDeviceAuthorizationPath = "/api/oauth/device_authorization";
constexpr std::string_view kTokenPath = "/api/oauth/token";
constexpr std::string_view kGrantTypeDeviceCode =
    "urn:ietf:params:oauth:grant-type:device_code";
constexpr std::string_view kGrantTypeRefresh = "refresh_token";
constexpr int kDeviceCodeTimeoutSeconds = 15 * 60;
constexpr int kDefaultPollIntervalSeconds = 5;

const std::map<std::string, std::string, std::less<>> kFormHeaders{
    {"Content-Type", "application/x-www-form-urlencoded"},
    {"Accept", "application/json"},
};

struct OAuthToken {
    std::string access{};
    std::string refresh{};
    std::int64_t expires{0};
};

struct DeviceAuthorization {
    std::string device_code{};
    std::string user_code{};
    std::string verification_uri{};
    std::string verification_uri_complete{};
    int interval_seconds{kDefaultPollIntervalSeconds};
    int expires_in_seconds{kDeviceCodeTimeoutSeconds};
};

[[nodiscard]] std::int64_t current_timestamp_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

[[nodiscard]] const std::string* json_object_string_field(
    const support::JsonValue::object_t& object,
    std::string_view name) {
    const auto found = object.find(std::string{name});
    return found == object.end() ? nullptr : found->second.get_if<std::string>();
}

[[nodiscard]] const double* json_object_number_field(
    const support::JsonValue::object_t& object,
    std::string_view name) {
    const auto found = object.find(std::string{name});
    return found == object.end() ? nullptr : found->second.get_if<double>();
}

/// pi `getOauthHost`: KIMI_CODE_OAUTH_HOST then KIMI_OAUTH_HOST, else the
/// default; trailing slashes stripped from the resolved host.
[[nodiscard]] std::string resolve_oauth_host(const KimiCodingOAuthOptions& options) {
    std::string host;
    if (options.oauth_host) {
        host = *options.oauth_host;
    } else if (const char* first = std::getenv("KIMI_CODE_OAUTH_HOST");
               first != nullptr && *first != '\0') {
        host = first;
    } else if (const char* second = std::getenv("KIMI_OAUTH_HOST");
               second != nullptr && *second != '\0') {
        host = second;
    }
    if (host.empty()) {
        host = std::string{kDefaultOauthHost};
    }
    while (host.size() > 1 && host.back() == '/') {
        host.pop_back();
    }
    return host;
}

/// pi `trustedHttpUrl`: only `http:`/`https:` URLs are trusted for opening in
/// the browser; anything else (including `javascript:` and `file:`) is not.
[[nodiscard]] bool trusted_http_url(std::string_view value) {
    if (value.empty()) {
        return false;
    }
    const auto scheme_end = value.find(':');
    if (scheme_end == std::string_view::npos) {
        return false;
    }
    const auto scheme = value.substr(0, scheme_end);
    if (scheme != "http" && scheme != "https" &&
        scheme != "HTTP" && scheme != "HTTPS") {
        return false;
    }
    const auto remainder = value.substr(scheme_end + 1);
    return remainder.starts_with("//") && remainder.size() > 2;
}

/// pi `requestSignal`: a per-request timeout composed with the login
/// cancellation token. The scope stays alive for the duration of one request;
/// its destructor cancels the timeout timer.
class RequestStopScope {
public:
    RequestStopScope(
        std::stop_token parent,
        boost::asio::any_io_executor executor,
        std::chrono::milliseconds timeout)
        : parent_callback_(parent, [source = source_] { source.request_stop(); }),
          timer_(std::make_shared<boost::asio::steady_timer>(executor, timeout)) {
        timer_->async_wait([source = source_](boost::system::error_code error) {
            if (!error) {
                source.request_stop();
            }
        });
    }

    ~RequestStopScope() {
        timer_->cancel();
    }

    RequestStopScope(const RequestStopScope&) = delete;
    RequestStopScope& operator=(const RequestStopScope&) = delete;

    [[nodiscard]] std::stop_token token() const { return source_.get_token(); }

    [[nodiscard]] bool timed_out() const { return source_.stop_requested(); }

private:
    std::stop_source source_{};
    std::stop_callback<std::function<void()>> parent_callback_;
    std::shared_ptr<boost::asio::steady_timer> timer_;
};

/// POST with the composed per-request timeout and login cancellation. An
/// aborted login normalizes to the stable "Login cancelled" error; a fired
/// request timeout surfaces as `Timeout`; other transport failures propagate.
[[nodiscard]] boost::asio::awaitable<support::Expected<OAuthHttpResponse>>
post_kimi_request(
    const std::shared_ptr<OAuthHttpClient>& http_client,
    std::string url,
    std::string body,
    std::stop_token stop_token,
    std::chrono::milliseconds request_timeout) {
    auto executor = co_await boost::asio::this_coro::executor;
    RequestStopScope scope(stop_token, executor, request_timeout);
    auto response = co_await http_client->post(
        std::move(url),
        kFormHeaders,
        std::move(body),
        scope.token());
    if (!response) {
        if (stop_token.stop_requested()) {
            co_return std::unexpected(support::make_error(
                support::ErrorCode::Cancelled,
                "Login cancelled"));
        }
        if (scope.timed_out()) {
            co_return std::unexpected(support::make_error(
                support::ErrorCode::Timeout,
                "Kimi Code OAuth request timed out"));
        }
        co_return std::unexpected(std::move(response.error()));
    }
    co_return *response;
}

/// pi `parseTokenResponse`: `access_token`/`refresh_token` non-empty strings
/// and a positive finite `expires_in`; `expires` is a wall-clock millisecond
/// timestamp.
[[nodiscard]] support::Expected<OAuthToken> parse_token_response(
    const OAuthHttpResponse& response,
    std::string_view operation) {
    const auto missing_fields = [&response, operation]() {
        return support::make_error(
            support::ErrorCode::OAuth,
            "Kimi Code token " + std::string{operation} +
                " response missing fields: " + response.body);
    };
    auto json = support::read_json(response.body);
    const auto* object = json ? json->get_if<support::JsonValue::object_t>() : nullptr;
    const auto* access = object == nullptr
        ? nullptr
        : json_object_string_field(*object, "access_token");
    const auto* refresh = object == nullptr
        ? nullptr
        : json_object_string_field(*object, "refresh_token");
    const auto* expires = object == nullptr
        ? nullptr
        : json_object_number_field(*object, "expires_in");
    if (access == nullptr || access->empty() ||
        refresh == nullptr || refresh->empty() ||
        expires == nullptr || !std::isfinite(*expires) || *expires <= 0) {
        return std::unexpected(missing_fields());
    }
    return OAuthToken{
        .access = *access,
        .refresh = *refresh,
        .expires = current_timestamp_ms() +
            static_cast<std::int64_t>(*expires * 1000.0),
    };
}

/// One token-poll outcome, pi `pollForToken`'s JSON decision table.
[[nodiscard]] DevicePollResult<OAuthToken> poll_kimi_token(
    const OAuthHttpResponse& response) {
    if (response.status_code >= 500) {
        return DevicePollResult<OAuthToken>{
            .kind = DevicePollResult<OAuthToken>::Failed{
                .message = "Kimi Code device token request failed with status " +
                    std::to_string(response.status_code) +
                    (response.body.empty() ? "" : ": " + response.body),
            },
        };
    }

    auto json = support::read_json(response.body);
    const auto* object = json ? json->get_if<support::JsonValue::object_t>() : nullptr;
    const auto* access = object == nullptr
        ? nullptr
        : json_object_string_field(*object, "access_token");
    if (response.status_code >= 200 && response.status_code < 300 &&
        access != nullptr) {
        if (auto parsed = parse_token_response(response, "poll"); parsed) {
            return DevicePollResult<OAuthToken>{
                .kind = DevicePollResult<OAuthToken>::Complete{
                    .value = std::move(*parsed),
                },
            };
        } else {
            return DevicePollResult<OAuthToken>{
                .kind = DevicePollResult<OAuthToken>::Failed{
                    .message = parsed.error().message,
                },
            };
        }
    }

    const auto* error = object == nullptr
        ? nullptr
        : json_object_string_field(*object, "error");
    const auto* description = object == nullptr
        ? nullptr
        : json_object_string_field(*object, "error_description");
    const auto description_suffix = description == nullptr
        ? std::string{}
        : ": " + *description;
    if (error == nullptr) {
        return DevicePollResult<OAuthToken>{
            .kind = DevicePollResult<OAuthToken>::Failed{
                .message = "Kimi Code device token request failed (status " +
                    std::to_string(response.status_code) + ")",
            },
        };
    }
    if (*error == "authorization_pending") {
        return DevicePollResult<OAuthToken>{
            .kind = DevicePollResult<OAuthToken>::Pending{},
        };
    }
    if (*error == "slow_down") {
        std::optional<int> interval;
        if (const auto* value = object == nullptr
                ? nullptr
                : json_object_number_field(*object, "interval");
            value != nullptr && std::isfinite(*value) && *value > 0) {
            interval = static_cast<int>(*value);
        }
        return DevicePollResult<OAuthToken>{
            .kind = DevicePollResult<OAuthToken>::SlowDown{
                .interval_seconds = interval,
            },
        };
    }
    if (*error == "expired_token") {
        return DevicePollResult<OAuthToken>{
            .kind = DevicePollResult<OAuthToken>::Failed{
                .message = "Kimi Code device authorization expired. "
                           "Please restart login.",
            },
        };
    }
    if (*error == "access_denied") {
        return DevicePollResult<OAuthToken>{
            .kind = DevicePollResult<OAuthToken>::Failed{
                .message = "Kimi Code login was denied.",
            },
        };
    }
    return DevicePollResult<OAuthToken>{
        .kind = DevicePollResult<OAuthToken>::Failed{
            .message = "Kimi Code device token request failed (status " +
                std::to_string(response.status_code) + ": " + *error +
                description_suffix + ")",
        },
    };
}

[[nodiscard]] bool is_retryable_refresh_failure(int status_code) {
    return status_code == 429 || status_code >= 500;
}

/// pi `startDeviceAuthorization`: POST the device_authorization form, validate
/// the RFC 8628 fields (with `verification_uri`/`verification_uri_complete`
/// http(s)-only), and apply the 5s/15min interval/expires defaults.
[[nodiscard]] boost::asio::awaitable<support::Expected<DeviceAuthorization>>
start_device_authorization(
    const std::shared_ptr<OAuthHttpClient>& http_client,
    std::string oauth_host,
    std::stop_token stop_token,
    std::chrono::milliseconds request_timeout) {
    const std::string body = "client_id=" + url_query_encode(kClientId);
    CCH_TRY(response, co_await post_kimi_request(
        http_client,
        oauth_host + std::string{kDeviceAuthorizationPath},
        body,
        stop_token,
        request_timeout));
    if (response.status_code < 200 || response.status_code >= 300) {
        co_return std::unexpected(support::make_error(
            support::ErrorCode::OAuth,
            "Kimi Code device authorization failed with status " +
                std::to_string(response.status_code) +
                (response.body.empty() ? "" : ": " + response.body)));
    }

    auto json = support::read_json(response.body);
    const auto* object = json ? json->get_if<support::JsonValue::object_t>() : nullptr;
    const auto* device_code = object == nullptr
        ? nullptr
        : json_object_string_field(*object, "device_code");
    const auto* user_code = object == nullptr
        ? nullptr
        : json_object_string_field(*object, "user_code");
    const auto* verification_uri = object == nullptr
        ? nullptr
        : json_object_string_field(*object, "verification_uri");
    const auto* verification_uri_complete = object == nullptr
        ? nullptr
        : json_object_string_field(*object, "verification_uri_complete");
    if (device_code == nullptr || user_code == nullptr ||
        verification_uri == nullptr || verification_uri_complete == nullptr ||
        !trusted_http_url(*verification_uri_complete) ||
        !trusted_http_url(*verification_uri)) {
        co_return std::unexpected(support::make_error(
            support::ErrorCode::OAuth,
            "Invalid Kimi Code device authorization response: " + response.body));
    }

    DeviceAuthorization device;
    device.device_code = *device_code;
    device.user_code = *user_code;
    device.verification_uri = *verification_uri;
    device.verification_uri_complete = *verification_uri_complete;
    if (const auto* interval = object == nullptr
            ? nullptr
            : json_object_number_field(*object, "interval");
        interval != nullptr && std::isfinite(*interval) && *interval > 0) {
        device.interval_seconds = static_cast<int>(*interval);
    }
    if (const auto* expires_in = object == nullptr
            ? nullptr
            : json_object_number_field(*object, "expires_in");
        expires_in != nullptr && std::isfinite(*expires_in) && *expires_in > 0) {
        device.expires_in_seconds = static_cast<int>(*expires_in);
    }
    co_return device;
}

/// pi `pollForToken`: the shared device-poll loop with wait-before-first-poll
/// and the RFC 8628 error decision table.
[[nodiscard]] boost::asio::awaitable<support::Expected<OAuthToken>>
poll_for_token(
    const std::shared_ptr<OAuthHttpClient>& http_client,
    const std::string& oauth_host,
    const DeviceAuthorization& device,
    std::stop_token stop_token,
    std::chrono::milliseconds request_timeout) {
    CCH_TRY(token, co_await poll_device_flow<OAuthToken>(
        DevicePollOptions<OAuthToken>{
            .interval_seconds = device.interval_seconds,
            .expires_in_seconds = device.expires_in_seconds,
            .wait_before_first_poll = true,
            .poll = [http_client, oauth_host, device, stop_token, request_timeout]()
                -> boost::asio::awaitable<
                       support::Expected<DevicePollResult<OAuthToken>>> {
                const std::string body =
                    "client_id=" + url_query_encode(kClientId) +
                    "&device_code=" + url_query_encode(device.device_code) +
                    "&grant_type=" + url_query_encode(kGrantTypeDeviceCode);
                CCH_TRY(response, co_await post_kimi_request(
                    http_client,
                    oauth_host + std::string{kTokenPath},
                    body,
                    stop_token,
                    request_timeout));
                co_return poll_kimi_token(response);
            },
            .stop_token = stop_token,
        }));
    co_return token;
}

/// pi `refreshToken`'s non-abortable backoff sleep (`setTimeout`), expressed
/// on the io_context executor so tests can advance real timers.
[[nodiscard]] boost::asio::awaitable<void> backoff_sleep(
    std::chrono::milliseconds duration) {
    auto executor = co_await boost::asio::this_coro::executor;
    boost::asio::steady_timer timer(executor, duration);
    boost::system::error_code error;
    co_await timer.async_wait(boost::asio::redirect_error(
        boost::asio::use_awaitable, error));
}

} // namespace

KimiCodingOAuth::KimiCodingOAuth(
    std::shared_ptr<OAuthHttpClient> http_client,
    KimiCodingOAuthOptions options)
    : http_client_(std::move(http_client)),
      options_(std::move(options)) {}

KimiCodingOAuth::~KimiCodingOAuth() = default;

boost::asio::awaitable<support::Expected<ai::OAuthCredential>>
KimiCodingOAuth::login(ai::AuthInteraction interaction) {
    if (!interaction.notify) {
        co_return std::unexpected(support::make_error(
            support::ErrorCode::OAuth,
            "login interaction has no notify hook"));
    }

    const auto oauth_host = resolve_oauth_host(options_);
    CCH_TRY(device, co_await start_device_authorization(
        http_client_,
        oauth_host,
        interaction.stop_token,
        options_.request_timeout));

    try {
        interaction.notify(ai::AuthEvent{ai::AuthDeviceCode{
            .user_code = device.user_code,
            .verification_uri = device.verification_uri_complete,
            .interval_seconds = device.interval_seconds,
            .expires_in_seconds = device.expires_in_seconds,
        }});
    } catch (...) {
        // Best-effort display: notify never vetoes login.
    }

    CCH_TRY(token, co_await poll_for_token(
        http_client_,
        oauth_host,
        device,
        interaction.stop_token,
        options_.request_timeout));
    co_return ai::OAuthCredential{
        .refresh = token.refresh,
        .access = token.access,
        .expires = token.expires,
    };
}

boost::asio::awaitable<support::Expected<ai::OAuthCredential>>
KimiCodingOAuth::refresh(
    ai::OAuthCredential credential,
    std::stop_token stop_token) {
    const auto oauth_host = resolve_oauth_host(options_);
    std::optional<support::Error> last_error;
    for (int attempt = 0; attempt <= options_.refresh_max_retries; ++attempt) {
        if (attempt > 0) {
            auto backoff = options_.refresh_backoff_base;
            for (int shift = 0; shift < attempt - 1; ++shift) {
                backoff *= 2;
            }
            co_await backoff_sleep(backoff);
        }
        if (stop_token.stop_requested()) {
            co_return std::unexpected(support::make_error(
                support::ErrorCode::Cancelled,
                "Kimi Code token refresh aborted"));
        }

        const std::string body =
            "client_id=" + url_query_encode(kClientId) +
            "&grant_type=" + url_query_encode(kGrantTypeRefresh) +
            "&refresh_token=" + url_query_encode(credential.refresh);
        auto response = co_await post_kimi_request(
            http_client_,
            oauth_host + std::string{kTokenPath},
            body,
            stop_token,
            options_.request_timeout);
        if (!response) {
            // Transport failure is retryable (pi's fetch catch); the per-request
            // timeout is one such case.
            last_error = response.error();
            continue;
        }

        if (response->status_code >= 200 && response->status_code < 300) {
            CCH_TRY(token, parse_token_response(*response, "refresh"));
            co_return ai::OAuthCredential{
                .refresh = token.refresh,
                .access = token.access,
                .expires = token.expires,
            };
        }

        auto json = support::read_json(response->body);
        const auto* object = json ? json->get_if<support::JsonValue::object_t>() : nullptr;
        const auto* error = object == nullptr
            ? nullptr
            : json_object_string_field(*object, "error");
        const auto* description = object == nullptr
            ? nullptr
            : json_object_string_field(*object, "error_description");
        const auto description_suffix = description == nullptr
            ? std::string{}
            : ": " + *description;

        // Unauthorized: the stored credential is dead; Models preserves it and
        // every subsequent request fails with re-auth guidance.
        if (response->status_code == 401 || response->status_code == 403 ||
            (error != nullptr && *error == "invalid_grant")) {
            co_return std::unexpected(support::make_error(
                support::ErrorCode::OAuth,
                "Kimi Code token refresh unauthorized (status " +
                    std::to_string(response->status_code) + ")" +
                    description_suffix));
        }

        if (is_retryable_refresh_failure(response->status_code) &&
            attempt < options_.refresh_max_retries) {
            last_error = support::make_error(
                support::ErrorCode::OAuth,
                "Kimi Code token refresh failed with status " +
                    std::to_string(response->status_code));
            continue;
        }

        co_return std::unexpected(support::make_error(
            support::ErrorCode::OAuth,
            "Kimi Code token refresh failed with status " +
                std::to_string(response->status_code) +
                (response->body.empty() ? "" : ": " + response->body)));
    }

    co_return std::unexpected(last_error.has_value()
        ? std::move(*last_error)
        : support::make_error(
              support::ErrorCode::OAuth,
              "Kimi Code token refresh failed"));
}

boost::asio::awaitable<support::Expected<ai::ModelAuth>>
KimiCodingOAuth::to_auth(const ai::OAuthCredential& credential) const {
    co_return ai::ModelAuth{
        .headers = {{"Authorization", "Bearer " + credential.access}},
    };
}

ai::OAuthAuth make_kimi_coding_oauth_auth(
    std::shared_ptr<OAuthHttpClient> http_client,
    KimiCodingOAuthOptions options) {
    if (!http_client) {
        http_client = std::make_shared<BoostBeastOAuthHttpClient>();
    }
    auto impl = std::make_shared<KimiCodingOAuth>(
        std::move(http_client),
        std::move(options));
    ai::OAuthAuth auth;
    auth.name = "Kimi Code (subscription)";
    auth.login = [impl](ai::AuthInteraction interaction)
        -> cch::support::AsyncResult<ai::OAuthCredential> {
        return cch::ai::detail::make_async_result(
            [impl, interaction = std::move(interaction)]() mutable
                -> boost::asio::awaitable<support::Expected<ai::OAuthCredential>> {
                co_return co_await impl->login(std::move(interaction));
            });
    };
    // The request-path refresh is uncancellable: no stop token is passed,
    // reproducing pi's frozen Kimi refresh-signal defect as no-divergence.
    auth.refresh = [impl](ai::OAuthCredential credential)
        -> cch::support::AsyncResult<ai::OAuthCredential> {
        return cch::ai::detail::make_async_result(
            [impl, credential = std::move(credential)]()
                -> boost::asio::awaitable<support::Expected<ai::OAuthCredential>> {
                co_return co_await impl->refresh(std::move(credential));
            });
    };
    auth.to_auth = [impl](const ai::OAuthCredential& credential)
        -> cch::support::AsyncResult<ai::ModelAuth> {
        return cch::ai::detail::make_async_result(
            [impl, credential = std::move(credential)]()
                -> boost::asio::awaitable<support::Expected<ai::ModelAuth>> {
                co_return co_await impl->to_auth(credential);
            });
    };
    return auth;
}

} // namespace cch::ai::auth
