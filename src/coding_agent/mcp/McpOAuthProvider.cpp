// MCP OAuth provider (spec #865, ticket #875). The flow is the authorization
// code + PKCE S256 shape pi's `packages/mcp/src/oauth/flow.ts` implements,
// narrowed to this slice: configured endpoints (no RFC 9728 discovery, no
// dynamic client registration), a loopback callback on 127.0.0.1 with a manual
// redirect-URL fallback, and refresh-token rotation. It reuses the shared
// `ai::auth` helpers (`generate_pkce`, `OAuthCallbackServer`,
// `post_with_login_cancellation`, `parse_authorization_input`) and the existing
// `AuthInteraction` presentation, and it returns the existing
// `ai::OAuthCredential`; persistence is the caller's, through the shared
// AuthStorage. The transport is private and reached only through
// `login_mcp_server` / `McpOAuthTokenResolver`.

#include "coding_agent/mcp/McpOAuthProvider.hpp"

#include "coding_agent/mcp/McpNamespace.hpp"

#include <cch/ai/JsonAccess.hpp>
#include <cch/ai/OAuthCallbackServer.hpp>
#include <cch/ai/OAuthHttpClient.hpp>
#include <cch/ai/OAuthShared.hpp>
#include <cch/ai/Pkce.hpp>
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

#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>

namespace cch::coding_agent::mcp {
namespace {

/// A token response that reports no `expires_in` is treated as long-lived
/// (pi leaves `tokensExpireAt` unset); 2^53-1 is exactly representable in the
/// JSON double the AuthStorage serializer uses, so it round-trips.
constexpr std::int64_t kNoReportedExpiry = 9007199254740991LL;

const std::map<std::string, std::string, std::less<>> kFormHeaders{
        {"Accept", "application/json"},
        {"Content-Type", "application/x-www-form-urlencoded"},
};

[[nodiscard]] support::Error oauth_error(std::string message, std::string detail = {}) {
    return support::make_error(support::ErrorCode::OAuth, std::move(message), std::move(detail));
}

[[nodiscard]] support::Error login_cancelled() {
    return support::make_error(support::ErrorCode::Cancelled, "Login cancelled");
}

/// The `error`/`error_description` pair of an OAuth token endpoint failure.
[[nodiscard]] std::string token_error_detail(std::string_view body, int status) {
    std::string detail = "HTTP " + std::to_string(status);
    if (auto json = support::read_json(body); json) {
        if (const auto* object = ai::json_object(*json)) {
            const auto code = ai::json_string_member(*object, "error");
            const auto description = ai::json_string_member(*object, "error_description");
            if (code) {
                detail += ": ";
                detail += std::string{*code};
            }
            if (description) {
                detail += " (";
                detail += std::string{*description};
                detail += ")";
            }
        }
    }
    return detail;
}

/// Build a fresh credential from a token endpoint response. `previous` supplies
/// the refresh token and account id a refresh response may omit.
[[nodiscard]] support::Expected<ai::OAuthCredential> credential_from_response(
        const std::string& client_id, std::string_view body, int status, const ai::OAuthCredential* previous) {
    if (status < 200 || status >= 300) {
        return std::unexpected(oauth_error("MCP OAuth token request failed", token_error_detail(body, status)));
    }
    auto json = support::read_json(body);
    if (!json) {
        return std::unexpected(oauth_error("MCP OAuth token response is not JSON", std::string{body}));
    }
    const auto* object = ai::json_object(*json);
    if (object == nullptr) {
        return std::unexpected(oauth_error("MCP OAuth token response is not a JSON object", std::string{body}));
    }
    const auto access = ai::json_string_member(*object, "access_token");
    if (!access || access->empty()) {
        return std::unexpected(oauth_error("MCP OAuth token response carries no access_token"));
    }
    const auto token_type = ai::json_string_member(*object, "token_type");
    if (!token_type || token_type->empty()) {
        return std::unexpected(oauth_error("MCP OAuth token response carries no token_type"));
    }

    ai::OAuthCredential credential;
    credential.access = std::string{*access};
    if (const auto refresh = ai::json_string_member(*object, "refresh_token"); refresh && !refresh->empty()) {
        credential.refresh = std::string{*refresh};
    } else if (previous != nullptr) {
        // A refresh response may omit the (unchanged) refresh token.
        credential.refresh = previous->refresh;
    }
    if (const auto expires_in = ai::json_number_member(*object, "expires_in"); expires_in) {
        if (!std::isfinite(*expires_in) || *expires_in < 0) {
            return std::unexpected(oauth_error("MCP OAuth token response carries an invalid expires_in"));
        }
        credential.expires = ai::auth::oauth_token_expiry_ms(*expires_in, ai::current_timestamp_ms());
    } else {
        credential.expires = kNoReportedExpiry;
    }
    credential.client_id = client_id;
    credential.account_id = previous != nullptr ? previous->account_id : std::nullopt;
    return credential;
}

[[nodiscard]] std::string callback_url(const std::string& host, std::uint16_t port, const std::string& path) {
    std::string authority = host;
    if (authority.find(':') != std::string::npos && !authority.starts_with('[')) {
        authority = "[" + authority + "]";
    }
    return "http://" + authority + ":" + std::to_string(port) + path;
}

[[nodiscard]] std::string build_authorize_url(const McpOAuthServerConfig& config,
        const std::string& redirect_url,
        const std::string& challenge,
        const std::string& state) {
    std::string url = config.authorization_url;
    const char separator = config.authorization_url.find('?') == std::string::npos ? '?' : '&';
    url += separator;
    url += "response_type=code";
    url += "&client_id=" + ai::auth::url_query_encode(config.client_id);
    url += "&code_challenge=" + ai::auth::url_query_encode(challenge);
    url += "&code_challenge_method=S256";
    url += "&redirect_uri=" + ai::auth::url_query_encode(redirect_url);
    url += "&state=" + ai::auth::url_query_encode(state);
    if (!config.scope.empty()) {
        url += "&scope=" + ai::auth::url_query_encode(config.scope);
    }
    return url;
}

[[nodiscard]] std::string authorization_code_body(const McpOAuthServerConfig& config,
        const std::string& code,
        const std::string& verifier,
        const std::string& redirect_url) {
    std::string body = "grant_type=authorization_code";
    body += "&code=" + ai::auth::url_query_encode(code);
    body += "&code_verifier=" + ai::auth::url_query_encode(verifier);
    body += "&client_id=" + ai::auth::url_query_encode(config.client_id);
    body += "&redirect_uri=" + ai::auth::url_query_encode(redirect_url);
    return body;
}

[[nodiscard]] std::string refresh_body(const McpOAuthServerConfig& config, const std::string& refresh_token) {
    std::string body = "grant_type=refresh_token";
    body += "&refresh_token=" + ai::auth::url_query_encode(refresh_token);
    body += "&client_id=" + ai::auth::url_query_encode(config.client_id);
    return body;
}

/// The manual/redirect fallback prompt state, mirroring the built-in
/// OpenRouter flow's callback-vs-manual race.
struct ManualState {
    std::mutex mutex;
    std::stop_source manual_stop;
    std::optional<std::string> manual_input{std::nullopt};
    std::optional<support::Error> manual_error{std::nullopt};
};

using PromptDoneChannel = boost::asio::experimental::channel<void(boost::system::error_code)>;

} // namespace

std::string mcp_oauth_provider_id(std::string_view server_name) {
    // The credential-store provider id is the MCP tool namespace by
    // construction (McpNamespace.hpp).
    return detail::mcp_namespace(server_name);
}

McpOAuthProvider::McpOAuthProvider(McpOAuthServerConfig config, std::shared_ptr<ai::auth::OAuthHttpClient> http_client)
    : config_(std::move(config)), http_client_(std::move(http_client)) {}

McpOAuthProvider::McpOAuthProvider(McpOAuthProvider&&) noexcept = default;
McpOAuthProvider& McpOAuthProvider::operator=(McpOAuthProvider&&) noexcept = default;
McpOAuthProvider::~McpOAuthProvider() = default;

boost::asio::awaitable<support::Expected<ai::OAuthCredential>> McpOAuthProvider::request_token(
        std::string body, std::stop_token stop_token) {
    auto response = co_await ai::auth::post_with_login_cancellation(
            http_client_, config_.token_url, kFormHeaders, std::move(body), stop_token);
    if (!response) {
        co_return std::unexpected(std::move(response.error()));
    }
    co_return credential_from_response(config_.client_id, response->body, response->status_code, nullptr);
}

boost::asio::awaitable<support::Expected<ai::OAuthCredential>> McpOAuthProvider::login(
        ai::AuthInteraction interaction) {
    if (auto valid = validate_mcp_oauth_server_config(config_); !valid) {
        co_return std::unexpected(std::move(valid.error()));
    }
    if (!interaction.prompt) {
        co_return std::unexpected(oauth_error("MCP OAuth login has no prompt hook"));
    }
    if (interaction.stop_token.stop_requested()) {
        co_return std::unexpected(login_cancelled());
    }

    CCH_TRY(pkce, ai::auth::generate_pkce());
    CCH_TRY(state, ai::auth::create_oauth_state());
    const std::string callback_path = "/callback";

    auto redirect_url_holder = std::make_shared<std::string>();
    CCH_TRY(server,
            co_await ai::auth::OAuthCallbackServer::start(ai::auth::OAuthCallbackServerOptions{
                    .host = config_.callback_host,
                    .port = config_.callback_port,
                    .path = callback_path,
                    .state = state,
                    .validate_state = true,
                    .success_message = "MCP server authorization completed. You may now close this page.",
                    .exchange_error_message = "MCP server authorization failed.",
            }));

    const std::string redirect_url = callback_url(config_.callback_host, server->bound_port(), callback_path);
    *redirect_url_holder = redirect_url;
    const std::string authorize_url = build_authorize_url(config_, redirect_url, pkce.challenge, state);

    if (interaction.stop_token.stop_requested()) {
        server->close();
        co_return std::unexpected(login_cancelled());
    }

    if (interaction.notify) {
        ai::auth::notify_best_effort(interaction.notify,
                ai::AuthProgress{.message = "Listening for MCP server authorization callback on " + redirect_url});
        ai::auth::notify_best_effort(interaction.notify,
                ai::AuthUrl{
                        .url = authorize_url,
                        .instructions = "Complete MCP server authorization in your browser. "
                                        "If the browser is on another machine, paste the final redirect URL here.",
                });
    }

    auto manual_state = std::make_shared<ManualState>();
    auto executor = co_await boost::asio::this_coro::executor;
    auto prompt_done = std::make_shared<PromptDoneChannel>(executor, 1);
    auto interaction_shared = std::make_shared<ai::AuthInteraction>(std::move(interaction));

    boost::asio::co_spawn(
            executor,
            [interaction_shared, manual_state, prompt_done, server, redirect_url]() -> boost::asio::awaitable<void> {
                ai::AuthPrompt prompt;
                prompt.kind = ai::AuthPromptManualCode{
                        .message = "Complete MCP server authorization in your browser, "
                                   "or paste the authorization code / redirect URL here:",
                        .placeholder = redirect_url,
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
                }
                server->cancel_wait();
                prompt_done->try_send(boost::system::error_code{});
            },
            boost::asio::detached);

    struct Cleanup {
        std::shared_ptr<ManualState> state;
        std::shared_ptr<ai::auth::OAuthCallbackServer> server;

        ~Cleanup() {
            state->manual_stop.request_stop();
            server->close();
        }
    };
    Cleanup cleanup{manual_state, server};

    const auto login_stop_token = interaction_shared->stop_token;
    std::stop_callback login_stop_callback{login_stop_token, [manual_state, server] {
                                               manual_state->manual_stop.request_stop();
                                               server->cancel_wait();
                                           }};

    CCH_TRY(callback_result, co_await server->wait_for_code());
    if (login_stop_token.stop_requested()) {
        co_return std::unexpected(login_cancelled());
    }
    if (callback_result.value) {
        co_return co_await request_token(
                authorization_code_body(config_, *callback_result.value, pkce.verifier, redirect_url),
                login_stop_token);
    }

    std::optional<support::Error> manual_error;
    std::optional<std::string> manual_input;
    {
        std::scoped_lock lock(manual_state->mutex);
        manual_error = manual_state->manual_error;
        manual_input = manual_state->manual_input;
    }
    if (!manual_error && !manual_input) {
        boost::system::error_code receive_error;
        co_await prompt_done->async_receive(boost::asio::redirect_error(boost::asio::use_awaitable, receive_error));
        if (login_stop_token.stop_requested()) {
            co_return std::unexpected(login_cancelled());
        }
        std::scoped_lock lock(manual_state->mutex);
        manual_error = manual_state->manual_error;
        manual_input = manual_state->manual_input;
    }
    if (manual_error) {
        co_return std::unexpected(std::move(*manual_error));
    }
    if (!manual_input) {
        co_return std::unexpected(oauth_error("MCP OAuth login received no authorization code"));
    }

    const auto parsed = ai::auth::parse_authorization_input(*manual_input);
    if (!parsed.code || parsed.code->empty()) {
        co_return std::unexpected(oauth_error("MCP OAuth login received no authorization code"));
    }
    co_return co_await request_token(
            authorization_code_body(config_, *parsed.code, pkce.verifier, redirect_url), login_stop_token);
}

boost::asio::awaitable<support::Expected<ai::OAuthCredential>> McpOAuthProvider::refresh(
        ai::OAuthCredential credential) {
    if (auto valid = validate_mcp_oauth_server_config(config_); !valid) {
        co_return std::unexpected(std::move(valid.error()));
    }
    if (credential.refresh.empty()) {
        co_return std::unexpected(
                oauth_error("MCP OAuth credential has no refresh token", "re-authentication is required"));
    }
    auto response = co_await ai::auth::post_with_login_cancellation(http_client_,
            config_.token_url,
            kFormHeaders,
            refresh_body(config_, credential.refresh),
            std::stop_token{});
    if (!response) {
        co_return std::unexpected(std::move(response.error()));
    }
    co_return credential_from_response(config_.client_id, response->body, response->status_code, &credential);
}

boost::asio::awaitable<support::Expected<ai::ModelAuth>> McpOAuthProvider::to_auth(
        const ai::OAuthCredential& credential) const {
    co_return ai::ModelAuth{.headers = {{"Authorization", "Bearer " + credential.access}}};
}

McpOAuthState mcp_oauth_state_from_credential(const ai::OAuthCredential& credential, std::string server_url) {
    McpOAuthState state;
    state.server_url = std::move(server_url);
    McpOAuthTokens tokens;
    tokens.access_token = credential.access;
    tokens.token_type = "Bearer";
    if (!credential.refresh.empty()) {
        tokens.refresh_token = credential.refresh;
    }
    state.tokens = std::move(tokens);
    state.tokens_expire_at = credential.expires;
    if (credential.client_id) {
        state.client_information = McpOAuthClientInformation{.client_id = *credential.client_id};
    }
    return state;
}

std::optional<ai::OAuthCredential> mcp_oauth_credential_from_state(const McpOAuthState& state) {
    if (!state.tokens) {
        return std::nullopt;
    }
    ai::OAuthCredential credential;
    credential.access = state.tokens->access_token;
    credential.refresh = state.tokens->refresh_token.value_or("");
    credential.expires = state.tokens_expire_at.value_or(kNoReportedExpiry);
    if (state.client_information) {
        credential.client_id = state.client_information->client_id;
    }
    return credential;
}

support::AsyncResult<void> login_mcp_server(std::shared_ptr<McpAuthStore> store,
        std::string server_name,
        std::string server_url,
        McpOAuthProvider& provider,
        ai::AuthInteraction interaction) {
    return support::detail::make_async_result(
            [store = std::move(store),
                    server_name = std::move(server_name),
                    server_url = std::move(server_url),
                    &provider,
                    interaction = std::move(interaction)]() mutable -> boost::asio::awaitable<support::ExpectedVoid> {
                auto credential = co_await provider.login(std::move(interaction));
                if (!credential) {
                    // Login-flow failures propagate unwrapped to the host.
                    co_return std::unexpected(std::move(credential.error()));
                }
                auto state = mcp_oauth_state_from_credential(*credential, server_url);
                auto stored = store->save(server_name, server_url, state);
                if (!stored) {
                    co_return std::unexpected(std::move(stored.error()));
                }
                co_return support::ExpectedVoid{};
            });
}

support::AsyncResult<void> logout_mcp_server(
        std::shared_ptr<McpAuthStore> store, std::string server_name, std::string server_url) {
    return support::detail::make_async_result(
            [store = std::move(store),
                    server_name = std::move(server_name),
                    server_url = std::move(server_url)]() -> boost::asio::awaitable<support::ExpectedVoid> {
                auto removed = store->remove(server_name, server_url);
                if (!removed) {
                    co_return std::unexpected(std::move(removed.error()));
                }
                co_return support::ExpectedVoid{};
            });
}

} // namespace cch::coding_agent::mcp
