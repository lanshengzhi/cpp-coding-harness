// The interactive and request-time sides of the MCP OAuth flow (spec #882,
// ticket #884). pi source at `7c10bd43` (v1.0.4):
// `packages/coding-agent/src/extensions/mcp/oauth.ts` (`callbackSettings`,
// `signInMcpServer`, `createMcpAuthProvider`) and `packages/mcp/src/oauth/
// callback.ts` (`waitForCallback`). The wire protocol and the credential state
// are `McpOAuthFlow` and `McpAuthStore`; this module adds the loopback callback
// with its pasted-redirect-URL fallback and a request-time resolver that runs
// the flow when an `oauth` block has no pre-resolved endpoints.

#include "coding_agent/mcp/McpOAuthSignIn.hpp"

#include "coding_agent/mcp/McpNamespace.hpp"
#include "coding_agent/mcp/McpOAuthFlow.hpp"
#include "coding_agent/mcp/McpOAuthProvider.hpp"

#include "ai/auth/OAuthCallbackServer.hpp"
#include "ai/auth/OAuthHttpClient.hpp"
#include "ai/auth/Pkce.hpp"
#include "support/AsyncResultBridge.hpp"
#include "support/ExpectedMacros.hpp"

#include <cch/ai/Timestamps.hpp>
#include <cch/coding_agent/AuthGuidance.hpp>
#include <cch/support/Error.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/experimental/channel.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace cch::coding_agent::mcp {
namespace {

/// The request-time refresh margin, matching `McpOAuthTokenResolver` (the
/// Models runtime's `kOAuthMinimumValidity`).
inline constexpr auto kOAuthMinimumValidity = std::chrono::minutes{5};

constexpr std::string_view kFallbackRedirectUrl = "http://127.0.0.1/callback";

[[nodiscard]] support::Error oauth_error(std::string message, std::string detail = {}) {
    return support::make_error(support::ErrorCode::OAuth, std::move(message), std::move(detail));
}

/// The shared re-login error, so the transport and the CLI show the same
/// `/login` guidance `McpOAuthTokenResolver` does.
[[nodiscard]] support::Error re_login_error(const std::string& server_name, std::string detail) {
    return support::make_error(support::ErrorCode::OAuth,
            format_oauth_reauthenticate_message(mcp_oauth_provider_id(server_name)),
            "MCP server '" + server_name + "': " + std::move(detail));
}

/// `scheme://authority` and `/path` of an absolute URL.
struct SplitUrl {
    std::string origin;
    std::string authority;
    std::string path;
};

[[nodiscard]] std::optional<SplitUrl> split_url(std::string_view value) {
    const auto scheme_end = value.find("://");
    if (scheme_end == std::string_view::npos || scheme_end == 0) {
        return std::nullopt;
    }
    std::string_view rest = value.substr(scheme_end + 3);
    const auto path_start = rest.find_first_of("/?#");
    std::string_view authority = path_start == std::string_view::npos ? rest : rest.substr(0, path_start);
    SplitUrl split;
    split.origin = std::string{value.substr(0, scheme_end + 3 + authority.size())};
    split.authority = std::string{authority};
    if (path_start == std::string_view::npos) {
        split.path = "/";
        return split;
    }
    std::string_view path = rest.substr(path_start);
    if (const auto cut = path.find_first_of("#?"); cut != std::string_view::npos) {
        path = path.substr(0, cut);
    }
    split.path = path.empty() ? std::string{"/"} : std::string{path};
    return split;
}

/// The host and optional port of an `authority`, brackets kept for an IPv6
/// literal.
struct HostPort {
    std::string host;
    std::optional<std::uint16_t> port;
};

[[nodiscard]] HostPort split_host_port(std::string_view authority) {
    if (const auto userinfo = authority.rfind('@'); userinfo != std::string_view::npos) {
        authority = authority.substr(userinfo + 1);
    }
    HostPort result;
    std::string_view rest = authority;
    if (!authority.empty() && authority.front() == '[') {
        const auto close = authority.find(']');
        if (close == std::string_view::npos) {
            result.host = std::string{authority};
            return result;
        }
        result.host = std::string{authority.substr(0, close + 1)};
        rest = authority.substr(close + 1);
    } else {
        const auto colon = authority.rfind(':');
        if (colon == std::string_view::npos) {
            result.host = std::string{authority};
            return result;
        }
        result.host = std::string{authority.substr(0, colon)};
        rest = authority.substr(colon);
    }
    if (rest.starts_with(':')) {
        std::uint16_t port = 0;
        const auto text = rest.substr(1);
        const auto converted = std::from_chars(text.data(), text.data() + text.size(), port);
        if (converted.ec == std::errc{} && converted.ptr == text.data() + text.size()) {
            result.port = port;
        }
    }
    return result;
}

/// A host as it appears in a redirect URI: an IPv6 literal gains its brackets.
[[nodiscard]] std::string redirect_authority(std::string_view host, std::optional<std::uint16_t> port) {
    std::string authority{host};
    if (authority.find(':') != std::string::npos && !authority.starts_with('[')) {
        authority = "[" + authority + "]";
    }
    if (port) {
        authority += ":" + std::to_string(*port);
    }
    return authority;
}

[[nodiscard]] std::string query_param(std::string_view url, std::string_view key) {
    const auto query_start = url.find('?');
    if (query_start == std::string_view::npos) {
        return {};
    }
    const auto pairs = ai::auth::parse_query_pairs(url.substr(query_start + 1));
    const auto found = pairs.find(std::string{key});
    return found == pairs.end() ? std::string{} : found->second;
}

/// pi `responseFromRedirectUrl`: the pasted redirect URL must name this
/// sign-in's exact redirect URI (a server-specific one tells authorization
/// servers apart), carry no `error`, and carry the sign-in's `state`.
struct AuthorizationResponse {
    std::string code;
    std::optional<std::string> iss;
};

[[nodiscard]] support::Expected<AuthorizationResponse> response_from_redirect_url(
        const std::string& input, const std::string& state, const std::string& redirect_url) {
    const auto trimmed = std::string_view{input}.substr(0, input.find_last_not_of(" \t\r\n") + 1);
    const auto candidate = split_url(trimmed);
    const auto expected = split_url(redirect_url);
    if (!candidate || !expected || candidate->origin != expected->origin || candidate->path != expected->path) {
        return std::unexpected(oauth_error("The redirect URL does not match this sign-in's redirect URI"));
    }
    const auto query_start = trimmed.find('?');
    const auto pairs = query_start == std::string_view::npos
                               ? std::map<std::string, std::string, std::less<>>{}
                               : ai::auth::parse_query_pairs(trimmed.substr(query_start + 1));
    if (const auto error = pairs.find("error"); error != pairs.end() && !error->second.empty()) {
        std::string message = error->second;
        if (const auto description = pairs.find("error_description");
                description != pairs.end() && !description->second.empty()) {
            message = description->second;
        }
        return std::unexpected(oauth_error(std::move(message)));
    }
    if (const auto found = pairs.find("state"); found == pairs.end() || found->second != state) {
        return std::unexpected(oauth_error("The redirect URL belongs to a different sign-in"));
    }
    const auto code = pairs.find("code");
    if (code == pairs.end() || code->second.empty()) {
        return std::unexpected(oauth_error("The redirect URL does not contain an authorization code"));
    }
    AuthorizationResponse response;
    response.code = code->second;
    if (const auto iss = pairs.find("iss"); iss != pairs.end() && !iss->second.empty()) {
        response.iss = iss->second;
    }
    return response;
}

using RaceOutcome = support::Expected<std::optional<AuthorizationResponse>>;

/// pi `waitForAuthorizationResponse`: the browser callback or the pasted
/// redirect URL, whichever settles first, bounded by the sign-in timeout. The
/// losing side is aborted.
[[nodiscard]] boost::asio::awaitable<support::Expected<AuthorizationResponse>> wait_for_authorization_response(
        const std::shared_ptr<ai::auth::OAuthCallbackServer>& server,
        const std::string& state,
        const std::string& authorization_url,
        const std::string& redirect_url,
        const McpOAuthSignInPrompt& prompt,
        std::chrono::milliseconds timeout) {
    std::string authorization_redirect = query_param(authorization_url, "redirect_uri");
    if (authorization_redirect.empty()) {
        authorization_redirect = redirect_url;
    }
    auto executor = co_await boost::asio::this_coro::executor;
    using Channel = boost::asio::experimental::channel<void(boost::system::error_code, RaceOutcome)>;
    auto settled = std::make_shared<Channel>(executor, 1);
    auto prompt_stop = std::make_shared<std::stop_source>();
    auto timer = std::make_shared<boost::asio::steady_timer>(executor);

    boost::asio::co_spawn(
            executor,
            [server, settled]() -> boost::asio::awaitable<void> {
                auto result = co_await server->wait_for_code();
                if (!result) {
                    settled->try_send(
                            boost::system::error_code{}, RaceOutcome{std::unexpected(std::move(result.error()))});
                    co_return;
                }
                if (!result->value) {
                    settled->try_send(boost::system::error_code{}, RaceOutcome{std::nullopt});
                    co_return;
                }
                AuthorizationResponse response;
                response.code = std::move(*result->value);
                response.iss = result->iss;
                settled->try_send(boost::system::error_code{},
                        RaceOutcome{std::optional<AuthorizationResponse>{std::move(response)}});
            },
            boost::asio::detached);

    if (prompt.prompt_for_redirect_url) {
        const auto paste = prompt.prompt_for_redirect_url;
        boost::asio::co_spawn(
                executor,
                [paste, settled, prompt_stop, state, authorization_redirect]() -> boost::asio::awaitable<void> {
                    auto pasted = co_await support::detail::await_async_result(paste(prompt_stop->get_token()));
                    if (!pasted) {
                        settled->try_send(
                                boost::system::error_code{}, RaceOutcome{std::unexpected(std::move(pasted.error()))});
                        co_return;
                    }
                    if (!pasted->has_value() || pasted->value().empty()) {
                        settled->try_send(boost::system::error_code{}, RaceOutcome{std::nullopt});
                        co_return;
                    }
                    auto parsed = response_from_redirect_url(**pasted, state, authorization_redirect);
                    if (!parsed) {
                        settled->try_send(
                                boost::system::error_code{}, RaceOutcome{std::unexpected(std::move(parsed.error()))});
                        co_return;
                    }
                    settled->try_send(boost::system::error_code{},
                            RaceOutcome{std::optional<AuthorizationResponse>{std::move(*parsed)}});
                },
                boost::asio::detached);
    }

    timer->expires_after(timeout);
    timer->async_wait([settled](const boost::system::error_code&) {
        settled->try_send(boost::system::error_code{}, RaceOutcome{std::nullopt});
    });

    boost::system::error_code receive_error;
    RaceOutcome outcome =
            co_await settled->async_receive(boost::asio::redirect_error(boost::asio::use_awaitable, receive_error));
    prompt_stop->request_stop();
    timer->cancel();
    server->cancel_wait();
    if (!outcome) {
        co_return std::unexpected(std::move(outcome.error()));
    }
    if (!outcome->has_value()) {
        co_return std::unexpected(
                support::make_error(support::ErrorCode::Cancelled, "MCP OAuth sign-in was cancelled or timed out"));
    }
    co_return std::move(**outcome);
}

[[nodiscard]] bool token_expires_soon(const ai::OAuthCredential& credential) {
    return credential.expires <= ai::current_timestamp_ms() + kOAuthMinimumValidity.count() * 1000;
}

/// The `Authorization: Bearer <access>` header for a stored state, or
/// `std::nullopt` when it carries no tokens.
[[nodiscard]] std::optional<std::map<std::string, std::string>> bearer_headers(const McpOAuthState& state) {
    const auto credential = mcp_oauth_credential_from_state(state);
    if (!credential) {
        return std::nullopt;
    }
    return std::map<std::string, std::string>{{"Authorization", "Bearer " + credential->access}};
}

} // namespace

std::string mcp_fallback_redirect_url() { return std::string{kFallbackRedirectUrl}; }

McpOAuthCallbackSettings mcp_oauth_callback_settings(const McpOAuthConfig& oauth) {
    McpOAuthCallbackSettings settings;
    const std::string raw = oauth.callback_url.value_or(std::string{kFallbackRedirectUrl});
    const auto split = split_url(raw);
    if (!split) {
        return settings;
    }
    const HostPort host_port = split_host_port(split->authority);
    std::string address = host_port.host;
    if (address.size() >= 2 && address.front() == '[' && address.back() == ']') {
        address = address.substr(1, address.size() - 2);
    }
    settings.path = split->path.empty() ? std::string{"/"} : split->path;
    std::optional<std::uint16_t> port = host_port.port;
    if (!port) {
        port = oauth.callback_port;
    }
    if (host_port.port) {
        // A configured URI with a port is sent exactly as written, since
        // servers compare it as a string.
        settings.fixed_redirect_url = raw;
    } else if (port) {
        settings.fixed_redirect_url = "http://" + redirect_authority(address, port) + settings.path;
    }
    // `localhost` is served on 127.0.0.1; browsers fall back to it when ::1
    // refuses.
    settings.host = address == "localhost" ? std::string{"127.0.0.1"} : address;
    settings.redirect_host = address;
    settings.port = port;
    return settings;
}

std::string mcp_oauth_bound_redirect_url(const McpOAuthCallbackSettings& settings, std::uint16_t bound_port) {
    if (settings.fixed_redirect_url) {
        return *settings.fixed_redirect_url;
    }
    return "http://" + redirect_authority(settings.redirect_host, bound_port) + settings.path;
}

std::string mcp_oauth_registered_redirect_url(const std::optional<McpOAuthState>& state) {
    if (state && state->client_information && !state->client_information->redirect_uris.empty()) {
        return state->client_information->redirect_uris.front();
    }
    return mcp_fallback_redirect_url();
}

support::AsyncResult<void> sign_in_mcp_server(McpOAuthSignInRequest request) {
    return support::detail::make_async_result([request = std::move(request)]() mutable
                                                      -> boost::asio::awaitable<support::ExpectedVoid> {
        auto http = request.http ? request.http : std::make_shared<ai::auth::BoostBeastOAuthHttpClient>();
        auto loaded = request.store->load(request.server_name, request.server_url);
        if (!loaded) {
            co_return std::unexpected(std::move(loaded.error()));
        }
        McpOAuthState working;
        if (loaded->has_value()) {
            working = **loaded;
        }
        working.server_url = request.server_url;
        // Every sign-in gets a fresh `state` parameter.
        auto state = ai::auth::create_oauth_state();
        if (!state) {
            co_return std::unexpected(std::move(state.error()));
        }
        working.oauth_state = std::move(*state);
        working.code_verifier = std::nullopt;
        if (!request.oauth.client_id) {
            // A registered client is bound to the redirect URI it was
            // registered with and its tokens belong to it; this sign-in
            // binds the loopback callback, so the registration is
            // replaced.
            working.client_information = std::nullopt;
            working.tokens = std::nullopt;
            working.tokens_expire_at = std::nullopt;
        }

        const McpOAuthCallbackSettings settings = mcp_oauth_callback_settings(request.oauth);
        std::vector<std::string> extra_paths;
        if (request.oauth.client_registration == McpClientRegistration::Cimd) {
            auto callback_id = mcp_callback_id(request.server_url);
            if (!callback_id) {
                co_return std::unexpected(std::move(callback_id.error()));
            }
            extra_paths.push_back(settings.path + "/" + *callback_id);
        }
        auto started = co_await ai::auth::OAuthCallbackServer::start(ai::auth::OAuthCallbackServerOptions{
                .host = settings.host,
                .port = settings.port.value_or(0),
                .path = settings.path,
                .extra_paths = std::move(extra_paths),
                .state = *working.oauth_state,
                .validate_state = true,
                .success_message = "MCP server authorization completed. You may now close this page.",
                .exchange_error_message = "MCP server authorization failed.",
        });
        if (!started) {
            co_return std::unexpected(std::move(started.error()));
        }
        auto server = *started;
        struct ServerGuard {
            std::shared_ptr<ai::auth::OAuthCallbackServer> server;
            ~ServerGuard() { server->close(); }
        } guard{server};

        const std::string redirect_url = mcp_oauth_bound_redirect_url(settings, server->bound_port());
        if (auto saved = request.store->save(request.server_name, request.server_url, working); !saved) {
            co_return std::unexpected(std::move(saved.error()));
        }

        auto outcome = co_await authorize_mcp(request.store,
                request.server_name,
                request.server_url,
                request.oauth,
                redirect_url,
                http,
                McpOAuthFlowOptions{});
        if (!outcome) {
            co_return std::unexpected(std::move(outcome.error()));
        }
        if (outcome->result == McpOAuthFlowResult::Authorized) {
            co_return support::ExpectedVoid{};
        }
        if (!outcome->authorization_url) {
            co_return std::unexpected(oauth_error("OAuth flow did not produce an authorization URL"));
        }
        if (request.prompt.show_authorization_url) {
            request.prompt.show_authorization_url(*outcome->authorization_url);
        }
        auto authorization = co_await wait_for_authorization_response(server,
                *working.oauth_state,
                *outcome->authorization_url,
                redirect_url,
                request.prompt,
                request.timeout);
        if (!authorization) {
            co_return std::unexpected(std::move(authorization.error()));
        }
        McpOAuthFlowOptions exchange;
        exchange.authorization_code = authorization->code;
        exchange.iss = authorization->iss;
        auto authorized = co_await authorize_mcp(
                request.store, request.server_name, request.server_url, request.oauth, redirect_url, http, exchange);
        if (!authorized) {
            co_return std::unexpected(std::move(authorized.error()));
        }
        if (authorized->result != McpOAuthFlowResult::Authorized) {
            co_return std::unexpected(oauth_error("OAuth flow did not complete"));
        }
        co_return support::ExpectedVoid{};
    });
}

McpOAuthFlowTokenResolver::McpOAuthFlowTokenResolver(std::shared_ptr<McpAuthStore> store,
        std::string server_name,
        std::string server_url,
        McpOAuthConfig oauth,
        std::shared_ptr<ai::auth::OAuthHttpClient> http)
    : store_(std::move(store)), server_name_(std::move(server_name)), server_url_(std::move(server_url)),
      oauth_(std::move(oauth)), http_(std::move(http)) {}

McpOAuthFlowTokenResolver::McpOAuthFlowTokenResolver(McpOAuthFlowTokenResolver&&) noexcept = default;
McpOAuthFlowTokenResolver& McpOAuthFlowTokenResolver::operator=(McpOAuthFlowTokenResolver&&) noexcept = default;
McpOAuthFlowTokenResolver::~McpOAuthFlowTokenResolver() = default;

support::AsyncResult<std::map<std::string, std::string>> McpOAuthFlowTokenResolver::current_headers() {
    return support::detail::make_async_result(
            [store = store_, server_name = server_name_, server_url = server_url_, oauth = oauth_, http = http_]()
                    -> boost::asio::awaitable<support::Expected<std::map<std::string, std::string>>> {
                auto loaded = store->load(server_name, server_url);
                if (!loaded) {
                    co_return std::unexpected(std::move(loaded.error()));
                }
                // The fast path: a stored, unexpired access token is sent with
                // no network call.
                if (loaded->has_value()) {
                    const auto credential = mcp_oauth_credential_from_state(**loaded);
                    if (credential && !token_expires_soon(*credential)) {
                        auto headers = bearer_headers(**loaded);
                        if (headers) {
                            co_return std::move(*headers);
                        }
                    }
                }
                // A missing, expired, or refreshable credential needs the flow:
                // it discovers the authorization server, registers a client if
                // none is stored, and refreshes the tokens.
                const std::string redirect_url = mcp_oauth_registered_redirect_url(*loaded);
                auto outcome = co_await authorize_mcp(
                        store, server_name, server_url, oauth, redirect_url, http, McpOAuthFlowOptions{});
                if (!outcome) {
                    co_return std::unexpected(std::move(outcome.error()));
                }
                if (outcome->result != McpOAuthFlowResult::Authorized) {
                    co_return std::unexpected(re_login_error(server_name, "sign-in required"));
                }
                auto refreshed = store->load(server_name, server_url);
                if (!refreshed) {
                    co_return std::unexpected(std::move(refreshed.error()));
                }
                if (refreshed->has_value()) {
                    if (auto headers = bearer_headers(**refreshed); headers) {
                        co_return std::move(*headers);
                    }
                }
                co_return std::unexpected(re_login_error(server_name, "no stored OAuth credential"));
            });
}

} // namespace cch::coding_agent::mcp
