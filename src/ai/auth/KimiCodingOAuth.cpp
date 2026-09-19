#include "KimiCodingOAuth.hpp"

#include "KimiOAuthWire.hpp"
#include "OAuthHttpClient.hpp"
#include "OAuthShared.hpp"
#include "Pkce.hpp"
#include "ai/JsonAccess.hpp"
#include "support/AsyncResultBridge.hpp"
#include "support/Json.hpp"
#include "support/ExpectedMacros.hpp"

#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <optional>
#include <string>
#include <stop_token>
#include <utility>

namespace cch::ai::auth {

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

    notify_best_effort(interaction.notify,
            ai::AuthDeviceCode{
                    .user_code = device.user_code,
                    .verification_uri = device.verification_uri_complete,
                    .interval_seconds = device.interval_seconds,
                    .expires_in_seconds = device.expires_in_seconds,
            });

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
        const auto* object = json ? json_object(*json) : nullptr;
        const auto error = object ? json_string_member(*object, "error") : std::nullopt;
        const auto description = object ? json_string_member(*object, "error_description") : std::nullopt;
        const auto description_suffix = !description ? std::string{} : ": " + std::string{*description};

        // Unauthorized: the stored credential is dead; Models preserves it and
        // every subsequent request fails with re-auth guidance.
        if (response->status_code == 401 || response->status_code == 403 || (error && *error == "invalid_grant")) {
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
    auto impl = std::make_shared<KimiCodingOAuth>(std::move(http_client), std::move(options));
    // The request-path refresh is uncancellable: no stop token is passed,
    // reproducing pi's frozen Kimi refresh-signal defect as no-divergence.
    return bind_oauth_auth("Kimi Code (subscription)", std::move(impl));
}

} // namespace cch::ai::auth
