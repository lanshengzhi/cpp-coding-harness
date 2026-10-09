#include "OpenAIChatGPTOAuth.hpp"

#include <cch/ai/OAuthCallbackServer.hpp>
#include <cch/ai/OAuthShared.hpp>
#include <cch/ai/Pkce.hpp>
#include <cch/ai/JsonAccess.hpp>
#include "support/AsyncResultBridge.hpp"
#include "support/ExpectedMacros.hpp"
#include "support/Json.hpp"
#include <cch/ai/Timestamps.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/experimental/channel.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/error_code.hpp>

#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <array>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>

namespace cch::ai::auth {
namespace {

// pi `openai-chatgpt.ts` constants (baseline v1.0.0 subset).
constexpr std::string_view kDynamicClientId = "dynamic_agent_client";
constexpr std::string_view kAgentNameHint = "Pi";
constexpr std::string_view kAuthorizeUrl = "https://auth.openai.com/api/accounts/authorize";
constexpr std::string_view kTokenUrl = "https://auth.openai.com/api/accounts/oauth/token";
constexpr std::string_view kResource = "https://api.openai.com/v1";
constexpr std::string_view kCallbackPath = "/auth/callback";
constexpr std::uint16_t kCallbackPort = 1455;
constexpr std::string_view kDirectTokenScope = "chatgpt.tokens.use.direct";
constexpr std::string_view kScope =
        "openid profile email offline_access resource.invoke chatgpt.tokens.use.direct";
/// pi `EXPIRY_MARGIN_MS`: refresh this long before the real expiry so a
/// request never starts with a token about to expire (3 minutes).
constexpr std::int64_t kExpiryMarginMs = 3 * 60 * 1000;

const std::map<std::string, std::string, std::less<>> kFormHeaders{
        {"Accept", "application/json"},
        {"Content-Type", "application/x-www-form-urlencoded"},
};

struct AuthorizationResult {
    std::string code{};
    std::string client_id{};
};

[[nodiscard]] std::string trim(std::string_view value) {
    std::size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
        ++begin;
    }
    std::size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }
    return std::string{value.substr(begin, end - begin)};
}

/// pi `UUID_PATTERN`: RFC 4122 shape check applied to the raw device ID.
[[nodiscard]] bool is_uuid(std::string_view value) noexcept {
    if (value.size() != 36) {
        return false;
    }
    for (std::size_t index = 0; index < value.size(); ++index) {
        const char ch = value[index];
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            if (ch != '-') {
                return false;
            }
            continue;
        }
        const bool hex_digit =
                (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
        if (!hex_digit) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::string resolve_callback_host(const OpenAIChatGPTOAuthOptions& options) {
    if (options.callback_host && !options.callback_host->empty()) {
        return *options.callback_host;
    }
    if (const char* override_value = std::getenv("PI_OAUTH_CALLBACK_HOST");
            override_value != nullptr && *override_value != '\0') {
        return override_value;
    }
    return "127.0.0.1";
}

[[nodiscard]] std::string redirect_uri() {
    return "http://127.0.0.1:" + std::to_string(kCallbackPort) + std::string{kCallbackPath};
}

/// pi `randomValue`: 32 random bytes base64url-encoded. Used for state/nonce.
[[nodiscard]] support::Expected<std::string> random_value() {
    std::array<std::uint8_t, 32> bytes{};
    std::random_device random;
    std::uniform_int_distribution<unsigned int> byte_distribution(0, 255);
    for (auto& byte : bytes) {
        byte = static_cast<std::uint8_t>(byte_distribution(random));
    }
    return base64url_encode(std::string_view{
            reinterpret_cast<const char*>(bytes.data()), bytes.size()});
}

/// pi `agentHostId`: OpenAI identifies each installation by a stable
/// `urn:uuid:<uuid>` URI; the raw device ID must be a UUID.
[[nodiscard]] support::Expected<std::string> agent_host_id(std::optional<ai::LoginOptions>& options) {
    std::string device_id;
    if (options && options->get_device_id) {
        // std::move_only_function::operator() is non-const in the supported
        // standard libraries, so the options arrive by non-const reference.
        device_id = options->get_device_id();
    }
    if (!is_uuid(device_id)) {
        return std::unexpected(support::make_error(
                support::ErrorCode::OAuth,
                "Sign in with ChatGPT requires a device ID (UUID) for this installation"));
    }
    std::string lower;
    lower.reserve(device_id.size());
    for (const char ch : device_id) {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return "urn:uuid:" + std::move(lower);
}

/// pi `authorizationResultFromCallback`: code + state + issued client_id.
[[nodiscard]] support::Expected<AuthorizationResult> authorization_result_from_pairs(
        const std::map<std::string, std::string, std::less<>>& pairs, const std::string& expected_state) {
    if (const auto found = pairs.find("error"); found != pairs.end()) {
        return std::unexpected(support::make_error(
                support::ErrorCode::OAuth, "ChatGPT authorization failed: " + found->second));
    }
    AuthorizationResult result;
    if (const auto found = pairs.find("code"); found != pairs.end() && !found->second.empty()) {
        result.code = found->second;
    } else {
        return std::unexpected(
                support::make_error(support::ErrorCode::OAuth, "Missing authorization code"));
    }
    const auto state_found = pairs.find("state");
    if (state_found == pairs.end() || state_found->second.empty()) {
        return std::unexpected(support::make_error(support::ErrorCode::OAuth, "Missing OAuth state"));
    }
    if (state_found->second != expected_state) {
        return std::unexpected(support::make_error(support::ErrorCode::OAuth, "OAuth state mismatch"));
    }
    if (const auto found = pairs.find("client_id"); found != pairs.end() && !found->second.empty()) {
        result.client_id = found->second;
    } else {
        return std::unexpected(support::make_error(
                support::ErrorCode::OAuth,
                "OpenAI OAuth registration callback did not contain an issued client ID"));
    }
    return result;
}

/// pi `authorizationResultFromManualInput`: the pasted URL must start with
/// the exact frozen callback URL; then the standard contract applies.
[[nodiscard]] support::Expected<AuthorizationResult> parse_manual_input(
        const std::string& input, const std::string& expected_state) {
    const auto trimmed = trim(input);
    const std::string expected_prefix = redirect_uri();
    if (!trimmed.starts_with(expected_prefix)) {
        return std::unexpected(support::make_error(
                support::ErrorCode::OAuth,
                "The pasted callback URL must start with " + expected_prefix));
    }
    std::string_view query = trimmed;
    const auto query_start = query.find('?');
    if (query_start == std::string_view::npos) {
        return std::unexpected(support::make_error(
                support::ErrorCode::OAuth, "Paste the full callback URL from the browser"));
    }
    query = query.substr(query_start + 1);
    if (const auto fragment = query.find('#'); fragment != std::string_view::npos) {
        query = query.substr(0, fragment);
    }
    return authorization_result_from_pairs(parse_query_pairs(query), expected_state);
}

/// pi `buildAuthorizeUrl` with URLSearchParams insertion order.
[[nodiscard]] std::string build_authorize_url(
        const std::string& host_id,
        const std::string& challenge,
        const std::string& state,
        const std::string& nonce) {
    return std::string{kAuthorizeUrl} + "?client_id=" + url_query_encode(kDynamicClientId) +
           "&agent_name_hint=" + url_query_encode(kAgentNameHint) +
           "&ext_agent_host_id=" + url_query_encode(host_id) + "&response_type=code" +
           "&redirect_uri=" + url_query_encode(redirect_uri()) +
           "&resource=" + url_query_encode(kResource) + "&scope=" + url_query_encode(kScope) +
           "&state=" + url_query_encode(state) +
           "&code_challenge=" + url_query_encode(challenge) + "&code_challenge_method=S256" +
           "&nonce=" + url_query_encode(nonce);
}

/// pi `requireTokenString`.
[[nodiscard]] support::Expected<std::string> require_token_string(
        const support::JsonValue::object_t& object, std::string_view field) {
    const auto found = object.find(std::string{field});
    const auto* value = found == object.end() ? nullptr : found->second.get_if<std::string>();
    if (value == nullptr || trim(*value).empty()) {
        return std::unexpected(support::make_error(
                support::ErrorCode::OAuth,
                "OpenAI OAuth token response has invalid " + std::string{field}));
    }
    return *value;
}

/// pi `credentialFromTokenResponse`: validates the token contract, enforces
/// the direct-token scope, and applies the 3-minute expiry margin.
/// `require_id_token` keeps the login-only presence check for the OpenID
/// `id_token` (pi does not parse it).
[[nodiscard]] support::Expected<ai::OAuthCredential> credential_from_token_response(
        const support::JsonValue::object_t& object,
        const std::string& client_id,
        bool require_id_token) {
    if (require_id_token) {
        auto id_token = require_token_string(object, "id_token");
        if (!id_token) {
            return std::unexpected(std::move(id_token.error()));
        }
    }
    auto access = require_token_string(object, "access_token");
    if (!access) {
        return std::unexpected(std::move(access.error()));
    }
    auto refresh = require_token_string(object, "refresh_token");
    if (!refresh) {
        return std::unexpected(std::move(refresh.error()));
    }
    auto scope = require_token_string(object, "scope");
    if (!scope) {
        return std::unexpected(std::move(scope.error()));
    }

    const auto expires_found = object.find("expires_in");
    const auto* expires_number =
            expires_found == object.end() ? nullptr : expires_found->second.get_if<double>();
    if (expires_number == nullptr || !std::isfinite(*expires_number) || *expires_number <= 0) {
        return std::unexpected(support::make_error(
                support::ErrorCode::OAuth, "OpenAI OAuth token response has invalid expires_in"));
    }

    bool direct_scope_present = false;
    std::istringstream scopes_stream(*scope);
    std::string item;
    while (scopes_stream >> item) {
        if (item == kDirectTokenScope) {
            direct_scope_present = true;
        }
    }
    if (!direct_scope_present) {
        return std::unexpected(support::make_error(
                support::ErrorCode::OAuth,
                "OpenAI OAuth grant did not include " + std::string{kDirectTokenScope}));
    }

    return ai::OAuthCredential{
            .refresh = std::move(*refresh),
            .access = std::move(*access),
            .expires = current_timestamp_ms() + static_cast<std::int64_t>(*expires_number * 1000.0) -
                    kExpiryMarginMs,
            .account_id = std::nullopt,
            .client_id = client_id,
    };
}

[[nodiscard]] boost::asio::awaitable<support::Expected<ai::OAuthCredential>> request_token(
        std::shared_ptr<OAuthHttpClient> http_client,
        const std::map<std::string, std::string, std::less<>>& body_pairs,
        const std::string& client_id,
        bool require_id_token,
        std::stop_token stop_token) {
    std::string body;
    bool first = true;
    for (const auto& [key, value] : body_pairs) {
        if (!first) {
            body += '&';
        }
        first = false;
        body += url_query_encode(key);
        body += '=';
        body += url_query_encode(value);
    }
    auto response = co_await post_with_login_cancellation(
            std::move(http_client), std::string{kTokenUrl}, kFormHeaders, std::move(body), stop_token);
    if (!response) {
        co_return std::unexpected(std::move(response.error()));
    }
    if (response->status_code < 200 || response->status_code >= 300) {
        co_return std::unexpected(support::make_error(
                support::ErrorCode::OAuth,
                "OpenAI OAuth token request failed (" + std::to_string(response->status_code) +
                        "): " + (response->body.empty() ? "unknown" : response->body)));
    }
    auto json = support::read_json(response->body);
    const auto* object = json ? json->get_if<support::JsonValue::object_t>() : nullptr;
    if (object == nullptr) {
        co_return std::unexpected(support::make_error(
                support::ErrorCode::OAuth, "OpenAI OAuth token response must be an object"));
    }
    co_return credential_from_token_response(*object, client_id, require_id_token);
}

struct ManualState {
    std::mutex mutex;
    std::stop_source manual_stop;
    std::optional<AuthorizationResult> manual_result{std::nullopt};
    std::optional<support::Error> manual_error{std::nullopt};
};

using PromptDoneChannel = boost::asio::experimental::channel<void(boost::system::error_code)>;

} // namespace

OpenAIChatGPTOAuth::OpenAIChatGPTOAuth(
    std::shared_ptr<OAuthHttpClient> http_client,
    OpenAIChatGPTOAuthOptions options)
    : http_client_(std::move(http_client)),
      options_(std::move(options)) {}

OpenAIChatGPTOAuth::~OpenAIChatGPTOAuth() = default;

boost::asio::awaitable<support::Expected<ai::OAuthCredential>>
OpenAIChatGPTOAuth::login(
    ai::AuthInteraction interaction,
    std::optional<ai::LoginOptions> options) {
    if (!interaction.prompt) {
        co_return std::unexpected(support::make_error(
            support::ErrorCode::OAuth,
            "login interaction has no prompt hook"));
    }

    CCH_TRY(host_id, agent_host_id(options));
    CCH_TRY(pkce, generate_pkce());
    CCH_TRY(state, random_value());
    CCH_TRY(nonce, random_value());

    const auto callback_host = resolve_callback_host(options_);
    CCH_TRY(server, co_await OAuthCallbackServer::start(OAuthCallbackServerOptions{
        .host = callback_host,
        .port = options_.callback_port,
        .path = std::string{kCallbackPath},
        .state = state,
        .success_message = "ChatGPT authentication completed. You can close this window.",
        .exchange_error_message = "ChatGPT authentication failed.",
    }));

    const std::string authorize_url =
        build_authorize_url(host_id, pkce.challenge, state, nonce);

    if (interaction.notify) {
        notify_best_effort(interaction.notify,
                ai::AuthUrl{
                    .url = authorize_url,
                    .instructions =
                        "Complete sign-in in your browser. If the callback does not complete, "
                        "paste the final redirect URL here.",
                });
    }

    auto manual_state = std::make_shared<ManualState>();
    auto executor = co_await boost::asio::this_coro::executor;
    auto prompt_done = std::make_shared<PromptDoneChannel>(executor, 1);
    auto interaction_shared =
        std::make_shared<ai::AuthInteraction>(std::move(interaction));

    boost::asio::co_spawn(
        executor,
        [interaction_shared, manual_state, prompt_done, server, state]() -> boost::asio::awaitable<void> {
            ai::AuthPrompt prompt;
            prompt.kind = ai::AuthPromptManualCode{
                .message = "Complete login in your browser, or paste the final redirect URL here:",
                .placeholder = redirect_uri(),
            };
            prompt.stop_token = manual_state->manual_stop.get_token();
            auto input = co_await support::detail::await_async_result(
                interaction_shared->prompt(std::move(prompt)));
            {
                std::scoped_lock lock(manual_state->mutex);
                if (input) {
                    auto parsed = parse_manual_input(*input, state);
                    if (parsed) {
                        manual_state->manual_result = std::move(*parsed);
                    } else {
                        manual_state->manual_error = std::move(parsed.error());
                    }
                } else {
                    manual_state->manual_error = std::move(input.error());
                }
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
        }};

    CCH_TRY(callback_result, co_await server->wait_for_code());
    if (login_stop_token.stop_requested()) {
        co_return std::unexpected(support::make_error(
            support::ErrorCode::Cancelled, "Login cancelled"));
    }

    std::optional<AuthorizationResult> authorization;
    if (callback_result.value) {
        // The local server already validated state; the issued dynamic client
        // ID rides alongside the code.
        if (!callback_result.client_id || callback_result.client_id->empty()) {
            co_return std::unexpected(support::make_error(
                support::ErrorCode::OAuth,
                "OpenAI OAuth registration callback did not contain an issued client ID"));
        }
        authorization = AuthorizationResult{
            .code = std::move(*callback_result.value),
            .client_id = std::move(*callback_result.client_id),
        };
    } else {
        std::optional<support::Error> manual_error;
        {
            std::scoped_lock lock(manual_state->mutex);
            manual_error = manual_state->manual_error;
            authorization = manual_state->manual_result;
        }
        if (manual_error) {
            co_return std::unexpected(std::move(*manual_error));
        }
        if (!authorization) {
            boost::system::error_code receive_error;
            co_await prompt_done->async_receive(
                boost::asio::redirect_error(boost::asio::use_awaitable, receive_error));
            std::scoped_lock lock(manual_state->mutex);
            if (manual_state->manual_error) {
                co_return std::unexpected(std::move(*manual_state->manual_error));
            }
            authorization = manual_state->manual_result;
        }
    }

    if (!authorization) {
        co_return std::unexpected(support::make_error(
            support::ErrorCode::OAuth, "Missing authorization code"));
    }

    if (interaction_shared->notify) {
        notify_best_effort(interaction_shared->notify,
                ai::AuthProgress{
                    .message = "Exchanging authorization code for tokens...",
                });
    }

    co_return co_await exchange_code(
        std::move(authorization->code),
        std::move(authorization->client_id),
        std::move(pkce.verifier),
        login_stop_token);
}

boost::asio::awaitable<support::Expected<ai::OAuthCredential>>
OpenAIChatGPTOAuth::exchange_code(
    std::string code,
    std::string client_id,
    std::string verifier,
    std::stop_token stop_token) {
    co_return co_await request_token(
        http_client_,
        std::map<std::string, std::string, std::less<>>{
            {"grant_type", "authorization_code"},
            {"client_id", client_id},
            {"code", std::move(code)},
            {"code_verifier", std::move(verifier)},
            {"redirect_uri", redirect_uri()},
            {"resource", std::string{kResource}},
        },
        client_id,
        /*require_id_token=*/true,
        stop_token);
}

boost::asio::awaitable<support::Expected<ai::OAuthCredential>>
OpenAIChatGPTOAuth::refresh(ai::OAuthCredential credential) {
    if (!credential.client_id || credential.client_id->empty()) {
        co_return std::unexpected(support::make_error(
            support::ErrorCode::OAuth,
            "Stored OpenAI OAuth credential does not contain an issued client ID; reconnect ChatGPT"));
    }
    const auto client_id = *credential.client_id;
    co_return co_await request_token(
        http_client_,
        std::map<std::string, std::string, std::less<>>{
            {"grant_type", "refresh_token"},
            {"client_id", client_id},
            {"refresh_token", credential.refresh},
            {"resource", std::string{kResource}},
        },
        client_id,
        /*require_id_token=*/false,
        /*stop_token=*/std::stop_token{});
}

boost::asio::awaitable<support::Expected<ai::ModelAuth>>
OpenAIChatGPTOAuth::to_auth(const ai::OAuthCredential& credential) const {
    co_return ai::ModelAuth{.api_key = credential.access};
}

ai::OAuthAuth make_openai_chatgpt_oauth_auth(
    std::shared_ptr<OAuthHttpClient> http_client,
    OpenAIChatGPTOAuthOptions options) {
    if (!http_client) {
        http_client = std::make_shared<BoostBeastOAuthHttpClient>();
    }
    auto impl = std::make_shared<OpenAIChatGPTOAuth>(std::move(http_client), std::move(options));
    return bind_oauth_auth("OpenAI (ChatGPT subscription)", std::move(impl));
}

} // namespace cch::ai::auth
