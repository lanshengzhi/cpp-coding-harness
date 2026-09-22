#include "OpenRouterOAuth.hpp"

#include "OAuthCallbackServer.hpp"
#include "OAuthShared.hpp"
#include "Pkce.hpp"
#include "ai/JsonAccess.hpp"
#include "support/ExpectedMacros.hpp"
#include "support/Json.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/experimental/channel.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/error_code.hpp>

#include <cstdint>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>

namespace cch::ai::auth {
namespace {

constexpr std::string_view kAuthorizeUrl = "https://openrouter.ai/auth";
constexpr std::string_view kTokenUrl = "https://openrouter.ai/api/v1/auth/keys";
constexpr std::int64_t kPermanentCredentialExpiry = 9007199254740991LL;

const std::map<std::string, std::string, std::less<>> kJsonHeaders{
        {"Accept", "application/json"},
        {"Content-Type", "application/json"},
};

[[nodiscard]] std::string resolve_callback_host(const OpenRouterOAuthOptions& options) {
    if (options.callback_host && !options.callback_host->empty()) {
        return *options.callback_host;
    }
    if (const char* override_value = std::getenv("PI_OAUTH_CALLBACK_HOST");
            override_value != nullptr && *override_value != '\0') {
        return override_value;
    }
    return "127.0.0.1";
}

[[nodiscard]] std::string callback_host_for_url(std::string host) {
    if (host.find(':') != std::string::npos && !host.starts_with('[') && !host.ends_with(']')) {
        return "[" + host + "]";
    }
    return host;
}

[[nodiscard]] std::string build_callback_url(const std::string& host, std::uint16_t port, const std::string& path) {
    return "http://" + callback_host_for_url(host) + ":" + std::to_string(port) + path;
}

[[nodiscard]] std::string build_authorize_url(const std::string& callback_url, const std::string& challenge) {
    return std::string{kAuthorizeUrl} + "?callback_url=" + url_query_encode(callback_url) +
           "&code_challenge=" + url_query_encode(challenge) + "&code_challenge_method=S256";
}

[[nodiscard]] support::Error login_cancelled() {
    return support::make_error(support::ErrorCode::Cancelled, "Login cancelled");
}

[[nodiscard]] std::optional<std::string> exchange_error_detail(const support::JsonValue::object_t& object) {
    if (const auto value = json_string_member(object, "error_description")) {
        return std::string{*value};
    }
    if (const auto value = json_string_member(object, "message")) {
        return std::string{*value};
    }
    const auto error = object.find("error");
    if (error == object.end()) {
        return std::nullopt;
    }
    if (const auto* text = error->second.get_if<std::string>()) {
        return *text;
    }
    if (const auto* nested = error->second.get_if<support::JsonValue::object_t>()) {
        if (const auto value = json_string_member(*nested, "message")) {
            return std::string{*value};
        }
    }
    return std::nullopt;
}

[[nodiscard]] support::Expected<std::string> read_exchange_key(const OAuthHttpResponse& response) {
    if (response.status_code < 200 || response.status_code >= 300) {
        std::string message =
                "OpenRouter OAuth key exchange failed (HTTP " + std::to_string(response.status_code) + ")";
        if (auto json = support::read_json(response.body); json) {
            if (const auto* object = json_object(*json)) {
                if (const auto detail = exchange_error_detail(*object)) {
                    message += ": ";
                    message += *detail;
                }
            }
        }
        return std::unexpected(support::make_error(support::ErrorCode::OAuth, std::move(message)));
    }

    if (auto json = support::read_json(response.body); json) {
        if (const auto* object = json_object(*json)) {
            if (const auto key = json_string_member(*object, "key"); key && !key->empty()) {
                return std::string{*key};
            }
        }
    }
    return std::unexpected(
            support::make_error(support::ErrorCode::OAuth, "OpenRouter OAuth response carries no \"key\""));
}

[[nodiscard]] boost::asio::awaitable<support::Expected<OAuthHttpResponse>> post_with_login_cancellation(
        const std::shared_ptr<OAuthHttpClient>& http_client,
        std::string url,
        std::map<std::string, std::string, std::less<>> headers,
        std::string body,
        std::stop_token stop_token) {
    auto response = co_await http_client->post(std::move(url), std::move(headers), std::move(body), stop_token);
    if (!response) {
        if (stop_token.stop_requested() || response.error().code == support::ErrorCode::Cancelled) {
            co_return std::unexpected(login_cancelled());
        }
        co_return std::unexpected(std::move(response.error()));
    }
    co_return *response;
}

[[nodiscard]] boost::asio::awaitable<support::Expected<std::string>> exchange_authorization_code(
        const std::shared_ptr<OAuthHttpClient>& http_client,
        std::string code,
        std::string verifier,
        std::stop_token stop_token) {
    auto body = support::write_json(support::JsonValue{support::JsonValue::object_t{
            {"code", std::move(code)},
            {"code_verifier", std::move(verifier)},
            {"code_challenge_method", "S256"},
    }});
    if (!body) {
        co_return std::unexpected(std::move(body.error()));
    }

    auto response = co_await post_with_login_cancellation(
            http_client, std::string{kTokenUrl}, kJsonHeaders, std::move(*body), stop_token);
    if (!response) {
        co_return std::unexpected(std::move(response.error()));
    }
    co_return read_exchange_key(*response);
}

[[nodiscard]] ai::OAuthCredential credential_from_key(std::string key) {
    return ai::OAuthCredential{
            .refresh = {},
            .access = std::move(key),
            .expires = kPermanentCredentialExpiry,
            .account_id = std::nullopt,
    };
}

struct ManualState {
    std::mutex mutex;
    std::stop_source manual_stop;
    bool prompt_settled{false};
    std::optional<std::string> manual_input{std::nullopt};
    std::optional<support::Error> manual_error{std::nullopt};
};

using PromptDoneChannel = boost::asio::experimental::channel<void(boost::system::error_code)>;

} // namespace

OpenRouterOAuth::OpenRouterOAuth(std::shared_ptr<OAuthHttpClient> http_client, OpenRouterOAuthOptions options)
    : http_client_(std::move(http_client)), options_(std::move(options)) {}

OpenRouterOAuth::~OpenRouterOAuth() = default;

boost::asio::awaitable<support::Expected<std::string>> OpenRouterOAuth::exchange_code(
        std::string code, std::string verifier, std::stop_token stop_token) {
    co_return co_await exchange_authorization_code(http_client_, std::move(code), std::move(verifier), stop_token);
}

boost::asio::awaitable<support::Expected<ai::OAuthCredential>> OpenRouterOAuth::login(ai::AuthInteraction interaction) {
    if (!interaction.prompt) {
        co_return std::unexpected(
                support::make_error(support::ErrorCode::OAuth, "login interaction has no prompt hook"));
    }
    if (interaction.stop_token.stop_requested()) {
        co_return std::unexpected(login_cancelled());
    }

    CCH_TRY(pkce, generate_pkce());
    CCH_TRY(path_suffix, create_oauth_state());
    const std::string callback_path = "/oauth/callback/" + path_suffix;
    const std::string callback_host = resolve_callback_host(options_);

    auto callback_handler = [http_client = http_client_, verifier = pkce.verifier, stop_token = interaction.stop_token](
                                    std::string code) -> boost::asio::awaitable<support::Expected<std::string>> {
        co_return co_await exchange_authorization_code(http_client, std::move(code), verifier, stop_token);
    };

    CCH_TRY(server,
            co_await OAuthCallbackServer::start(OAuthCallbackServerOptions{
                    .host = callback_host,
                    .port = options_.callback_port,
                    .path = callback_path,
                    .state = {},
                    .validate_state = false,
                    .success_message = "OpenRouter account authorization completed. You may now close this page.",
                    .exchange_error_message = "OpenRouter account authorization failed.",
                    .callback_handler = std::move(callback_handler),
            }));

    const auto callback_url = build_callback_url(callback_host, server->bound_port(), callback_path);
    const auto authorize_url = build_authorize_url(callback_url, pkce.challenge);

    if (interaction.stop_token.stop_requested()) {
        server->close();
        co_return std::unexpected(login_cancelled());
    }

    if (interaction.notify) {
        notify_best_effort(interaction.notify,
                ai::AuthProgress{
                        .message = "Listening for OpenRouter account authorization callback on " + callback_url,
                });
        notify_best_effort(interaction.notify,
                ai::AuthUrl{
                        .url = authorize_url,
                        .instructions = "Complete OpenRouter account authorization in your browser. "
                                        "If the browser is on another machine, paste the final redirect URL here.",
                });
    }

    auto manual_state = std::make_shared<ManualState>();
    auto executor = co_await boost::asio::this_coro::executor;
    auto prompt_done = std::make_shared<PromptDoneChannel>(executor, 1);
    auto interaction_shared = std::make_shared<ai::AuthInteraction>(std::move(interaction));

    boost::asio::co_spawn(
            executor,
            [interaction_shared, manual_state, prompt_done, server, callback_url]() -> boost::asio::awaitable<void> {
                ai::AuthPrompt prompt;
                prompt.kind = ai::AuthPromptManualCode{
                        .message = "Complete OpenRouter account authorization in your browser, "
                                   "or paste the authorization code / redirect URL here:",
                        .placeholder = callback_url,
                };
                prompt.stop_token = manual_state->manual_stop.get_token();
                auto result =
                        co_await support::detail::await_async_result(interaction_shared->prompt(std::move(prompt)));
                {
                    std::scoped_lock lock(manual_state->mutex);
                    if (result) {
                        manual_state->manual_input = std::move(*result);
                    } else {
                        manual_state->manual_error = std::move(result.error());
                    }
                    manual_state->prompt_settled = true;
                }
                server->cancel_wait();
                prompt_done->try_send(boost::system::error_code{});
            },
            boost::asio::detached);

    struct Cleanup {
        std::shared_ptr<ManualState> state;
        std::shared_ptr<OAuthCallbackServer> server;

        ~Cleanup() {
            state->manual_stop.request_stop();
            server->close();
        }
    };
    Cleanup cleanup{manual_state, server};

    const auto login_stop_token = interaction_shared->stop_token;
    std::stop_callback login_stop_callback{
            login_stop_token,
            [manual_state, server] {
                manual_state->manual_stop.request_stop();
                server->cancel_wait();
            },
    };

    CCH_TRY(callback_result, co_await server->wait_for_code());
    if (login_stop_token.stop_requested()) {
        co_return std::unexpected(login_cancelled());
    }
    if (callback_result) {
        co_return credential_from_key(std::move(*callback_result));
    }

    std::optional<support::Error> manual_error;
    std::optional<std::string> manual_input;
    {
        std::scoped_lock lock(manual_state->mutex);
        manual_error = manual_state->manual_error;
        manual_input = manual_state->manual_input;
    }
    if (manual_error) {
        co_return std::unexpected(std::move(*manual_error));
    }

    if (!manual_input) {
        boost::system::error_code receive_error;
        co_await prompt_done->async_receive(boost::asio::redirect_error(boost::asio::use_awaitable, receive_error));
        std::scoped_lock lock(manual_state->mutex);
        manual_error = manual_state->manual_error;
        manual_input = manual_state->manual_input;
    }
    if (manual_error) {
        co_return std::unexpected(std::move(*manual_error));
    }
    if (!manual_input) {
        co_return std::unexpected(support::make_error(support::ErrorCode::OAuth, "Missing authorization code"));
    }

    auto parsed = parse_authorization_input(*manual_input);
    if (!parsed.code || parsed.code->empty()) {
        co_return std::unexpected(support::make_error(support::ErrorCode::OAuth, "Missing authorization code"));
    }

    CCH_TRY(key, co_await exchange_code(std::move(*parsed.code), std::move(pkce.verifier), login_stop_token));
    co_return credential_from_key(std::move(key));
}

boost::asio::awaitable<support::Expected<ai::OAuthCredential>> OpenRouterOAuth::refresh(
        ai::OAuthCredential credential) {
    co_return credential;
}

boost::asio::awaitable<support::Expected<ai::ModelAuth>> OpenRouterOAuth::to_auth(
        const ai::OAuthCredential& credential) const {
    co_return ai::ModelAuth{.api_key = credential.access};
}

ai::OAuthAuth make_openrouter_oauth_auth(std::shared_ptr<OAuthHttpClient> http_client, OpenRouterOAuthOptions options) {
    if (!http_client) {
        http_client = std::make_shared<BoostBeastOAuthHttpClient>();
    }
    auto impl = std::make_shared<OpenRouterOAuth>(std::move(http_client), std::move(options));
    return bind_oauth_auth("OpenRouter OAuth", std::move(impl));
}

} // namespace cch::ai::auth
