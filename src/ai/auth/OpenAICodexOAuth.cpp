#include "OpenAICodexOAuth.hpp"

#include "DevicePoll.hpp"
#include "OAuthCallbackServer.hpp"
#include "Pkce.hpp"
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
namespace {

constexpr std::string_view kClientId = "app_EMoamEEZ73f0CkXaXp7hrann";
constexpr std::string_view kTokenUrl = "https://auth.openai.com/oauth/token";
constexpr std::string_view kAuthorizeUrl = "https://auth.openai.com/oauth/authorize";
constexpr std::string_view kRedirectUri = "http://localhost:1455/auth/callback";
constexpr std::string_view kDeviceUserCodeUrl =
    "https://auth.openai.com/api/accounts/deviceauth/usercode";
constexpr std::string_view kDeviceTokenUrl =
    "https://auth.openai.com/api/accounts/deviceauth/token";
constexpr std::string_view kDeviceVerificationUri =
    "https://auth.openai.com/codex/device";
constexpr std::string_view kDeviceRedirectUri =
    "https://auth.openai.com/deviceauth/callback";
constexpr std::string_view kScope = "openid profile email offline_access";
constexpr std::string_view kBrowserMethod = "browser";
constexpr std::string_view kDeviceCodeMethod = "device_code";
constexpr int kDeviceCodeTimeoutSeconds = 15 * 60;

const std::map<std::string, std::string, std::less<>> kFormHeaders{
    {"Content-Type", "application/x-www-form-urlencoded"},
};
const std::map<std::string, std::string, std::less<>> kJsonHeaders{
    {"Content-Type", "application/json"},
};

struct OAuthToken {
    std::string access{};
    std::string refresh{};
    std::int64_t expires{0};
};

struct DeviceAuthInfo {
    std::string device_auth_id{};
    std::string user_code{};
    int interval_seconds{0};
};

struct DeviceTokenSuccess {
    std::string authorization_code{};
    std::string code_verifier{};
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

[[nodiscard]] std::string resolve_callback_host(
    const OpenAICodexOAuthOptions& options) {
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
[[nodiscard]] std::string build_authorize_url(
    const std::string& challenge,
    const std::string& state) {
    return std::string{kAuthorizeUrl} +
        "?response_type=code" +
        "&client_id=" + url_query_encode(kClientId) +
        "&redirect_uri=" + url_query_encode(kRedirectUri) +
        "&scope=" + url_query_encode(kScope) +
        "&code_challenge=" + url_query_encode(challenge) +
        "&code_challenge_method=S256" +
        "&state=" + url_query_encode(state) +
        "&id_token_add_organizations=true" +
        "&codex_cli_simplified_flow=true" +
        "&originator=pi";
}

/// pi `parseAuthorizationInput` validation applied to manual code entry.
[[nodiscard]] support::Expected<std::string> parse_manual_code(
    const std::string& input,
    const std::string& expected_state) {
    const auto parsed = parse_authorization_input(input);
    if (parsed.state && *parsed.state != expected_state) {
        return std::unexpected(support::make_error(
            support::ErrorCode::OAuth,
            "State mismatch"));
    }
    if (!parsed.code) {
        return std::unexpected(support::make_error(
            support::ErrorCode::OAuth,
            "Missing authorization code"));
    }
    return *parsed.code;
}

/// pi `fetchWithLoginCancellation`: an aborted request normalizes to the
/// stable "Login cancelled" error; other transport failures propagate.
[[nodiscard]] boost::asio::awaitable<support::Expected<OAuthHttpResponse>>
post_with_login_cancellation(
    const std::shared_ptr<OAuthHttpClient>& http_client,
    std::string url,
    std::map<std::string, std::string, std::less<>> headers,
    std::string body,
    std::stop_token stop_token) {
    auto response = co_await http_client->post(
        std::move(url),
        std::move(headers),
        std::move(body),
        stop_token);
    if (!response) {
        if (stop_token.stop_requested()) {
            co_return std::unexpected(support::make_error(
                support::ErrorCode::Cancelled,
                "Login cancelled"));
        }
        co_return std::unexpected(std::move(response.error()));
    }
    co_return *response;
}

/// pi `readTokenResponse`: non-2xx and missing-field responses carry the
/// frozen message with the raw body; success yields the OAuth token with
/// `expires` as a wall-clock millisecond timestamp.
[[nodiscard]] support::Expected<OAuthToken> read_token_response(
    const OAuthHttpResponse& response,
    std::string_view operation) {
    const auto missing_fields = [&response, operation]() {
        return support::make_error(
            support::ErrorCode::OAuth,
            "OpenAI Codex token " + std::string{operation} +
                " response missing fields: " + response.body);
    };
    if (response.status_code < 200 || response.status_code >= 300) {
        return std::unexpected(support::make_error(
            support::ErrorCode::OAuth,
            "OpenAI Codex token " + std::string{operation} + " failed (" +
                std::to_string(response.status_code) + "): " +
                (response.body.empty() ? "unknown" : response.body)));
    }
    if (auto json = support::read_json(response.body); !json) {
        return std::unexpected(missing_fields());
    } else {
        const auto* object = json->get_if<support::JsonValue::object_t>();
        const auto* access = object == nullptr
            ? nullptr
            : json_object_string_field(*object, "access_token");
        const auto* refresh = object == nullptr
            ? nullptr
            : json_object_string_field(*object, "refresh_token");
        const auto* expires = object == nullptr
            ? nullptr
            : json_object_number_field(*object, "expires_in");
        if (access == nullptr || refresh == nullptr || expires == nullptr) {
            return std::unexpected(missing_fields());
        }
        return OAuthToken{
            .access = *access,
            .refresh = *refresh,
            .expires = current_timestamp_ms() +
                static_cast<std::int64_t>(*expires * 1000.0),
        };
    }
}

/// pi `credentialsFromToken`: accountId extraction from the unverified JWT is
/// mandatory; absence fails the operation.
[[nodiscard]] support::Expected<ai::OAuthCredential> credentials_from_token(
    const OAuthToken& token) {
    if (auto account_id = extract_account_id(token.access); !account_id) {
        return std::unexpected(support::make_error(
            support::ErrorCode::OAuth,
            "Failed to extract accountId from token"));
    } else {
        return ai::OAuthCredential{
            .refresh = token.refresh,
            .access = token.access,
            .expires = token.expires,
            .account_id = *account_id,
        };
    }
}

[[nodiscard]] support::Expected<DeviceAuthInfo> parse_device_auth_response(
    const OAuthHttpResponse& response) {
    if (response.status_code == 404) {
        return std::unexpected(support::make_error(
            support::ErrorCode::OAuth,
            "OpenAI Codex device code login is not enabled for this server. "
            "Use browser login or verify the server URL."));
    }
    if (response.status_code < 200 || response.status_code >= 300) {
        return std::unexpected(support::make_error(
            support::ErrorCode::OAuth,
            "OpenAI Codex device code request failed with status " +
                std::to_string(response.status_code) +
                (response.body.empty() ? "" : ": " + response.body)));
    }
    if (auto json = support::read_json(response.body); !json) {
        return std::unexpected(support::make_error(
            support::ErrorCode::OAuth,
            "Invalid OpenAI Codex device code response: " + response.body));
    } else {
        const auto* object = json->get_if<support::JsonValue::object_t>();
        const auto* device_auth_id = object == nullptr
            ? nullptr
            : json_object_string_field(*object, "device_auth_id");
        const auto* user_code = object == nullptr
            ? nullptr
            : json_object_string_field(*object, "user_code");
        int interval_seconds = -1;
        if (object != nullptr) {
            if (const auto* interval = json_object_number_field(*object, "interval")) {
                interval_seconds = static_cast<int>(*interval);
            } else if (const auto* interval_text =
                           json_object_string_field(*object, "interval")) {
                // Numeric interval may arrive as a string; validated below.
                int parsed = -1;
                const auto [position, error] =
                    std::from_chars(
                        interval_text->data(),
                        interval_text->data() + interval_text->size(),
                        parsed);
                if (error == std::errc{} &&
                    position == interval_text->data() + interval_text->size()) {
                    interval_seconds = parsed;
                }
            }
        }
        if (device_auth_id == nullptr || user_code == nullptr ||
            interval_seconds < 0) {
            return std::unexpected(support::make_error(
                support::ErrorCode::OAuth,
                "Invalid OpenAI Codex device code response: " + response.body));
        }
        return DeviceAuthInfo{
            .device_auth_id = *device_auth_id,
            .user_code = *user_code,
            .interval_seconds = interval_seconds,
        };
    }
}

[[nodiscard]] support::Expected<DevicePollResult<DeviceTokenSuccess>>
poll_device_token(const OAuthHttpResponse& response) {
    if (response.status_code >= 200 && response.status_code < 300) {
        if (auto json = support::read_json(response.body); !json) {
            return DevicePollResult<DeviceTokenSuccess>{
                .kind = DevicePollResult<DeviceTokenSuccess>::Failed{
                    .message = "Invalid OpenAI Codex device auth token "
                               "response: " + response.body,
                },
            };
        } else {
            const auto* object = json->get_if<support::JsonValue::object_t>();
            const auto* authorization_code = object == nullptr
                ? nullptr
                : json_object_string_field(*object, "authorization_code");
            const auto* code_verifier = object == nullptr
                ? nullptr
                : json_object_string_field(*object, "code_verifier");
            if (authorization_code == nullptr || code_verifier == nullptr) {
                return DevicePollResult<DeviceTokenSuccess>{
                    .kind = DevicePollResult<DeviceTokenSuccess>::Failed{
                        .message = "Invalid OpenAI Codex device auth token "
                                   "response: " + response.body,
                    },
                };
            }
            return DevicePollResult<DeviceTokenSuccess>{
                .kind = DevicePollResult<DeviceTokenSuccess>::Complete{
                    .value = DeviceTokenSuccess{
                        .authorization_code = *authorization_code,
                        .code_verifier = *code_verifier,
                    },
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
        if (const auto* object = json->get_if<support::JsonValue::object_t>()) {
            const auto error_found = object->find("error");
            if (error_found != object->end()) {
                if (const auto* code = error_found->second.get_if<std::string>()) {
                    error_code = *code;
                } else if (const auto* error_object =
                               error_found->second.get_if<support::JsonValue::object_t>()) {
                    if (const auto* code =
                            json_object_string_field(*error_object, "code")) {
                        error_code = *code;
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
        .kind = DevicePollResult<DeviceTokenSuccess>::Failed{
            .message = "OpenAI Codex device auth failed with status " +
                std::to_string(response.status_code) +
                (response.body.empty() ? "" : ": " + response.body),
        },
    };
}

} // namespace

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

    // Best-effort display: notify never vetoes login. The hook is
    // contractually non-throwing (AuthNotifyHook); the guarded conversion
    // only preserves the staged exception-enabled build.
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    try {
#endif
        interaction.notify(ai::AuthEvent{ai::AuthUrl{
            .url = authorize_url,
            .instructions =
                "A browser window should open. Complete login to finish.",
        }});
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    } catch (...) {
        // Best-effort display: notify never vetoes login.
    }
#endif

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
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
            try {
#endif
                result = co_await cch::support::detail::await_async_result(
                        interaction_shared->prompt(std::move(manual_prompt)));
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
            } catch (const std::exception& error) {
                result = std::unexpected(support::make_error(
                    support::ErrorCode::OAuth,
                    "login prompt failed",
                    error.what()));
            } catch (...) {
                result = std::unexpected(support::make_error(
                    support::ErrorCode::OAuth,
                    "login prompt failed"));
            }
#endif
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

#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    try {
#endif
        interaction.notify(ai::AuthEvent{ai::AuthDeviceCode{
            .user_code = device.user_code,
            .verification_uri = std::string{kDeviceVerificationUri},
            .interval_seconds = device.interval_seconds,
            .expires_in_seconds = kDeviceCodeTimeoutSeconds,
        }});
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    } catch (...) {
        // Best-effort display.
    }
#endif

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
    ai::OAuthAuth auth;
    auth.name = "OpenAI (ChatGPT Plus/Pro)";
    auth.login = [impl](ai::AuthInteraction interaction) -> cch::support::AsyncResult<ai::OAuthCredential> {
        return cch::support::detail::make_async_result(
                [impl, interaction = std::move(interaction)]() mutable
                        -> boost::asio::awaitable<support::Expected<ai::OAuthCredential>> {
                    co_return co_await impl->login(std::move(interaction));
                });
    };
    auth.refresh = [impl](ai::OAuthCredential credential) -> cch::support::AsyncResult<ai::OAuthCredential> {
        return cch::support::detail::make_async_result(
                [impl, credential = std::move(credential)]()
                        -> boost::asio::awaitable<support::Expected<ai::OAuthCredential>> {
                    co_return co_await impl->refresh(std::move(credential));
                });
    };
    auth.to_auth = [impl](const ai::OAuthCredential& credential) -> cch::support::AsyncResult<ai::ModelAuth> {
        return cch::support::detail::make_async_result(
                [impl,
                        credential =
                                std::move(credential)]() -> boost::asio::awaitable<support::Expected<ai::ModelAuth>> {
                    co_return co_await impl->to_auth(credential);
                });
    };
    return auth;
}

} // namespace cch::ai::auth
