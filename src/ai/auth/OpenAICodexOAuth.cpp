#include "OpenAICodexOAuth.hpp"

#include "OpenAICodexOAuthWire.hpp"
#include "OAuthCallbackServer.hpp"
#include "OAuthShared.hpp"
#include "Pkce.hpp"
#include "ai/JsonAccess.hpp"
#include "support/AsyncResultBridge.hpp"
#include "support/Json.hpp"
#include "support/ExpectedMacros.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/experimental/channel.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>

namespace cch::ai::auth {

OpenAICodexOAuth::OpenAICodexOAuth(
    std::shared_ptr<OAuthHttpClient> http_client,
    OpenAICodexOAuthOptions options)
    : http_client_(std::move(http_client)),
      options_(std::move(options)) {}

OpenAICodexOAuth::~OpenAICodexOAuth() = default;

boost::asio::awaitable<support::Expected<ai::OAuthCredential>>
OpenAICodexOAuth::login(ai::AuthInteraction interaction) {
    if (!interaction.prompt) {
        co_return std::unexpected(support::make_error(
            support::ErrorCode::OAuth,
            "login interaction has no prompt hook"));
    }

    ai::AuthPrompt method_prompt;
    method_prompt.kind = ai::AuthPromptSelect{
        .message = "Select OpenAI Codex login method:",
        .options = {
            ai::AuthPromptOption{
                .id = std::string{kBrowserMethod},
                .label = "Browser login (default)",
            },
            ai::AuthPromptOption{
                .id = std::string{kDeviceCodeMethod},
                .label = "Device code login (headless)",
            },
        },
    };
    CCH_TRY(method, co_await cch::support::detail::await_async_result(interaction.prompt(std::move(method_prompt))));
    if (method == kDeviceCodeMethod) {
        CCH_TRY(credential, co_await login_device_code(std::move(interaction)));
        co_return credential;
    }
    if (method != kBrowserMethod) {
        co_return std::unexpected(support::make_error(
            support::ErrorCode::OAuth,
            "Unknown OpenAI Codex login method: " + method));
    }
    CCH_TRY(credential, co_await login_browser(std::move(interaction)));
    co_return credential;
}

boost::asio::awaitable<support::Expected<ai::OAuthCredential>>
OpenAICodexOAuth::login_browser(ai::AuthInteraction interaction) {
    if (!interaction.notify) {
        co_return std::unexpected(support::make_error(
            support::ErrorCode::OAuth,
            "login interaction has no notify hook"));
    }

    CCH_TRY(pkce, generate_pkce());
    CCH_TRY(state, create_oauth_state());
    const std::string authorize_url = build_authorize_url(pkce.challenge, state);

    CCH_TRY(server, co_await OAuthCallbackServer::start(OAuthCallbackServerOptions{
        .host = resolve_callback_host(options_),
        .port = options_.callback_port,
        .state = state,
    }));

    // Best-effort display: notify never vetoes login.
    notify_best_effort(interaction.notify,
            ai::AuthUrl{
                    .url = authorize_url,
                    .instructions = "A browser window should open. Complete login to finish.",
            });

    struct ManualState {
        std::mutex mutex;
        std::stop_source manual_stop;
        bool prompt_settled{false};
        std::optional<std::string> manual_input{std::nullopt};
        std::optional<support::Error> manual_error{std::nullopt};
    };
    auto manual_state = std::make_shared<ManualState>();
    auto executor = co_await boost::asio::this_coro::executor;
    using PromptDoneChannel = boost::asio::experimental::channel<
        void(boost::system::error_code)>;
    auto prompt_done = std::make_shared<PromptDoneChannel>(executor, 1);

    const auto login_stop_token = interaction.stop_token;
    auto interaction_shared =
        std::make_shared<ai::AuthInteraction>(std::move(interaction));
    boost::asio::co_spawn(
        executor,
        [interaction_shared, manual_state, prompt_done, server]()
            -> boost::asio::awaitable<void> {
            ai::AuthPrompt manual_prompt;
            manual_prompt.kind = ai::AuthPromptManualCode{
                .message = "Complete login in your browser, or paste the "
                           "authorization code / redirect URL here:",
                .placeholder = std::string{kRedirectUri},
            };
            manual_prompt.stop_token = manual_state->manual_stop.get_token();
            support::Expected<std::string> result;
            result = co_await cch::support::detail::await_async_result(
                    interaction_shared->prompt(std::move(manual_prompt)));
            {
                std::scoped_lock lock(manual_state->mutex);
                if (result) {
                    manual_state->manual_input = std::move(*result);
                } else {
                    manual_state->manual_error = std::move(result.error());
                }
                manual_state->prompt_settled = true;
            }
            // Prompt win closes the acceptor wait (pi server.cancelWait()).
            server->cancel_wait();
            prompt_done->try_send(boost::system::error_code{});
        },
        boost::asio::detached);

    struct Cleanup {
        std::shared_ptr<ManualState> state;
        std::shared_ptr<OAuthCallbackServer> server;
        ~Cleanup() {
            // pi's finally: abort the per-prompt token and close the server on
            // every exit path (callback win cancels the manual prompt).
            state->manual_stop.request_stop();
            server->close();
        }
    };
    Cleanup cleanup{manual_state, server};

    // The race: the callback wait resolves with the code (callback win) or
    // `std::nullopt` once the manual prompt settles (prompt win) or the
    // server failed to listen (degrade to manual input only).
    CCH_TRY(callback_code, co_await server->wait_for_code());

    std::optional<support::Error> first_error;
    std::optional<std::string> manual_input;
    {
        std::scoped_lock lock(manual_state->mutex);
        if (manual_state->manual_error) {
            first_error = *manual_state->manual_error;
        }
        manual_input = manual_state->manual_input;
    }
    if (first_error) {
        co_return std::unexpected(*first_error);
    }

    std::optional<std::string> code;
    if (callback_code) {
        code = callback_code;
    } else if (manual_input) {
        CCH_TRY(parsed_code, parse_manual_code(*manual_input, state));
        code = parsed_code;
    }

    if (!code) {
        // Still-pending prompt (degraded server or early wait settlement):
        // await its outcome, then re-read the parsed input.
        {
            boost::system::error_code receive_error;
            co_await prompt_done->async_receive(
                boost::asio::redirect_error(boost::asio::use_awaitable, receive_error));
            // Channel closed: treat as no manual outcome.
        }
        std::scoped_lock lock(manual_state->mutex);
        if (manual_state->manual_error) {
            co_return std::unexpected(*manual_state->manual_error);
        }
        if (manual_state->manual_input) {
            CCH_TRY(parsed_code, parse_manual_code(*manual_state->manual_input, state));
            code = parsed_code;
        }
    }

    if (!code) {
        co_return std::unexpected(support::make_error(
            support::ErrorCode::OAuth,
            "Missing authorization code"));
    }

    CCH_TRY(credential, co_await exchange_code(
        *code, pkce.verifier, std::string{kRedirectUri}, login_stop_token));
    co_return credential;
}

boost::asio::awaitable<support::Expected<ai::OAuthCredential>>
OpenAICodexOAuth::login_device_code(ai::AuthInteraction interaction) {
    if (!interaction.notify) {
        co_return std::unexpected(support::make_error(
            support::ErrorCode::OAuth,
            "login interaction has no notify hook"));
    }

    CCH_TRY(usercode_json, support::write_json(support::JsonValue{
        support::JsonValue::object_t{{"client_id", std::string{kClientId}}}}));
    CCH_TRY(response, co_await post_with_login_cancellation(
        http_client_,
        std::string{kDeviceUserCodeUrl},
        kJsonHeaders,
        usercode_json,
        interaction.stop_token));
    CCH_TRY(device, parse_device_auth_response(response));

    notify_best_effort(interaction.notify,
            ai::AuthDeviceCode{
                    .user_code = device.user_code,
                    .verification_uri = std::string{kDeviceVerificationUri},
                    .interval_seconds = device.interval_seconds,
                    .expires_in_seconds = kDeviceCodeTimeoutSeconds,
            });

    CCH_TRY(success, co_await poll_device_flow<DeviceTokenSuccess>(
        DevicePollOptions<DeviceTokenSuccess>{
            .interval_seconds = device.interval_seconds,
            .expires_in_seconds = kDeviceCodeTimeoutSeconds,
            .poll = [this, device, stop = interaction.stop_token]()
                -> boost::asio::awaitable<
                       support::Expected<DevicePollResult<DeviceTokenSuccess>>> {
                CCH_TRY(poll_json, support::write_json(support::JsonValue{
                    support::JsonValue::object_t{
                        {"device_auth_id", device.device_auth_id},
                        {"user_code", device.user_code},
                    }}));
                CCH_TRY(poll_response, co_await post_with_login_cancellation(
                    http_client_,
                    std::string{kDeviceTokenUrl},
                    kJsonHeaders,
                    poll_json,
                    stop));
                co_return poll_device_token(poll_response);
            },
            .stop_token = interaction.stop_token,
        }));

    CCH_TRY(credential, co_await exchange_code(
        success.authorization_code,
        success.code_verifier,
        std::string{kDeviceRedirectUri},
        interaction.stop_token));
    co_return credential;
}

boost::asio::awaitable<support::Expected<ai::OAuthCredential>>
OpenAICodexOAuth::exchange_code(
    std::string code,
    std::string verifier,
    std::string redirect_uri,
    std::stop_token stop_token) {
    const std::string body =
        "grant_type=authorization_code"
        "&client_id=" + url_query_encode(kClientId) +
        "&code=" + url_query_encode(code) +
        "&code_verifier=" + url_query_encode(verifier) +
        "&redirect_uri=" + url_query_encode(redirect_uri);
    CCH_TRY(response, co_await post_with_login_cancellation(
        http_client_,
        std::string{kTokenUrl},
        kFormHeaders,
        body,
        stop_token));
    CCH_TRY(token, read_token_response(response, "exchange"));
    co_return credentials_from_token(token);
}

boost::asio::awaitable<support::Expected<ai::OAuthCredential>>
OpenAICodexOAuth::refresh(ai::OAuthCredential credential) {
    const std::string body =
        "grant_type=refresh_token"
        "&refresh_token=" + url_query_encode(credential.refresh) +
        "&client_id=" + url_query_encode(kClientId);
    // pi's request-path refresh is uncancellable: no stop token is passed.
    auto response = co_await http_client_->post(
        std::string{kTokenUrl},
        kFormHeaders,
        body,
        {});
    if (!response) {
        std::string detail = response.error().message;
        if (!response.error().detail.empty()) {
            detail += ": " + response.error().detail;
        }
        co_return std::unexpected(support::make_error(
            support::ErrorCode::OAuth,
            "OpenAI Codex token refresh error: " + detail));
    }
    CCH_TRY(token, read_token_response(*response, "refresh"));
    co_return credentials_from_token(token);
}

boost::asio::awaitable<support::Expected<ai::ModelAuth>>
OpenAICodexOAuth::to_auth(const ai::OAuthCredential& credential) const {
    co_return ai::ModelAuth{.api_key = credential.access};
}

ai::OAuthAuth make_openai_codex_oauth_auth(
    std::shared_ptr<OAuthHttpClient> http_client,
    OpenAICodexOAuthOptions options) {
    if (!http_client) {
        http_client = std::make_shared<BoostBeastOAuthHttpClient>();
    }
    auto impl = std::make_shared<OpenAICodexOAuth>(
        std::move(http_client),
        std::move(options));
    return bind_oauth_auth("OpenAI (ChatGPT Plus/Pro)", std::move(impl));
}

} // namespace cch::ai::auth
