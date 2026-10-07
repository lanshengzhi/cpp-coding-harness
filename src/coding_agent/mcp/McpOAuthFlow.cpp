// The MCP OAuth authorization flow (spec #882, ticket #884). pi source at
// `7c10bd43` (v1.0.4): `packages/mcp/src/oauth/flow.ts` and
// `packages/coding-agent/src/extensions/mcp/oauth.ts`. The wire requests, the
// parameter set, the client-authentication choice, the error codes, and the
// retry rules are pi's; the state is the `mcp-auth.json` record, the transport
// is the injectable `ai::auth::OAuthHttpClient`, and the authorization code
// arrives from the caller, so no browser is started here.

#include "coding_agent/mcp/McpOAuthFlow.hpp"

#include "ai/JsonAccess.hpp"
#include "ai/auth/OAuthHttpClient.hpp"
#include "ai/auth/Pkce.hpp"

#include "support/ExpectedMacros.hpp"
#include "support/Json.hpp"

#include <cch/ai/Timestamps.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <expected>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::coding_agent::mcp {
namespace {

/// pi's Client ID Metadata Document location: `client.json` and
/// `<callback id>/client.json` under this base (`CLIENT_METADATA_BASE_URL`).
constexpr std::string_view kClientMetadataBaseUrl = "https://pi.dev/oauth";
constexpr std::string_view kCallbackPath = "/callback";
/// pi's `APP_NAME`, the default `client_name`.
constexpr std::string_view kDefaultClientName = "pi";

/// pi `OAuthError` plus the plain (non-OAuth) failure case: `oauth_code` is
/// empty for a parse or transport failure, which the flow propagates rather
/// than treating as an authorization-server answer.
struct FlowError {
    std::string oauth_code;
    support::Error error;
};

template <typename T> using FlowResult = std::expected<T, FlowError>;

[[nodiscard]] FlowError plain_error(support::Error error) {
    return FlowError{.oauth_code = {}, .error = std::move(error)};
}

[[nodiscard]] FlowError oauth_code_error(std::string code, support::Error error) {
    return FlowError{.oauth_code = std::move(code), .error = std::move(error)};
}

[[nodiscard]] support::Error oauth_error(std::string message, std::string detail = {}) {
    return support::make_error(support::ErrorCode::OAuth, std::move(message), std::move(detail));
}

[[nodiscard]] support::Error invalid(std::string message, std::string detail = {}) {
    return support::make_error(support::ErrorCode::Validation, std::move(message), std::move(detail));
}

[[nodiscard]] bool contains(const std::optional<std::vector<std::string>>& values, std::string_view needle) {
    if (!values) {
        return false;
    }
    return std::find(values->begin(), values->end(), needle) != values->end();
}

/// pi's `||` scope chain: an empty value falls through to the next source.
[[nodiscard]] std::optional<std::string> first_non_empty(
        std::optional<std::string> first, std::optional<std::string> second) {
    if (first && !first->empty()) {
        return first;
    }
    if (second && !second->empty()) {
        return second;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::string> join_scopes(const std::optional<std::vector<std::string>>& scopes) {
    if (!scopes || scopes->empty()) {
        return std::nullopt;
    }
    std::string joined;
    for (const auto& scope : *scopes) {
        if (!joined.empty()) {
            joined += " ";
        }
        joined += scope;
    }
    return joined;
}

[[nodiscard]] bool scope_contains(std::string_view scope, std::string_view needle) {
    std::size_t start = 0;
    while (start <= scope.size()) {
        const auto end = scope.find(' ', start);
        const auto token = scope.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
        if (token == needle) {
            return true;
        }
        if (end == std::string_view::npos) {
            return false;
        }
        start = end + 1;
    }
    return false;
}

/// The origin (`scheme://authority`) of an absolute URL, and its path.
struct SplitUrl {
    std::string origin;
    std::string path;
};

[[nodiscard]] std::optional<SplitUrl> split_url(std::string_view value) {
    const auto scheme_end = value.find("://");
    if (scheme_end == std::string_view::npos || scheme_end == 0) {
        return std::nullopt;
    }
    std::string_view rest = value.substr(scheme_end + 3);
    const auto path_start = rest.find_first_of("/?#");
    if (path_start == std::string_view::npos) {
        return SplitUrl{.origin = std::string{value}, .path = "/"};
    }
    std::string_view path = rest.substr(path_start);
    if (const auto cut = path.find_first_of("#?"); cut != std::string_view::npos) {
        path = path.substr(0, cut);
    }
    return SplitUrl{
            .origin = std::string{value.substr(0, scheme_end + 3 + path_start)},
            .path = path.empty() ? std::string{"/"} : std::string{path},
    };
}

/// The host of an `authority` (`host[:port]`, brackets kept for an IPv6
/// literal), lowercased.
[[nodiscard]] std::string authority_host(std::string_view authority) {
    if (const auto userinfo = authority.rfind('@'); userinfo != std::string_view::npos) {
        authority = authority.substr(userinfo + 1);
    }
    std::string host{authority};
    if (!authority.empty() && authority.front() == '[') {
        if (const auto close = authority.find(']'); close != std::string_view::npos) {
            host = std::string{authority.substr(0, close + 1)};
        }
    } else if (const auto port = authority.find(':'); port != std::string_view::npos) {
        host = std::string{authority.substr(0, port)};
    }
    for (char& character : host) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    return host;
}

/// pi `loopback`: the hosts treated as the local machine.
[[nodiscard]] bool is_loopback_host(std::string_view host) {
    return host == "localhost" || host == "127.0.0.1" || host == "::1" || host == "[::1]";
}

/// pi `secureEndpoint`: credentials are never sent to a non-HTTPS endpoint
/// except a loopback one (RFC 8252).
[[nodiscard]] std::optional<support::Error> insecure_endpoint(std::string_view url) {
    if (url.starts_with("https://")) {
        return std::nullopt;
    }
    if (url.starts_with("http://")) {
        auto authority = url.substr(std::string_view{"http://"}.size());
        if (const auto slash = authority.find('/'); slash != std::string_view::npos) {
            authority = authority.substr(0, slash);
        }
        if (!authority.empty() && is_loopback_host(authority_host(authority))) {
            return std::nullopt;
        }
    }
    return oauth_error("Refusing to send OAuth credentials to non-HTTPS endpoint " + std::string{url});
}

[[nodiscard]] std::string append_query(std::string url, std::string_view key, std::string_view value) {
    url += url.find('?') == std::string::npos ? '?' : '&';
    url += key;
    url += '=';
    url += ai::auth::url_query_encode(value);
    return url;
}

[[nodiscard]] std::string form_encode(const std::vector<std::pair<std::string, std::string>>& params) {
    std::string body;
    for (const auto& [key, value] : params) {
        if (!body.empty()) {
            body += '&';
        }
        body += ai::auth::url_query_encode(key);
        body += '=';
        body += ai::auth::url_query_encode(value);
    }
    return body;
}

/// The absolute URL `relative` resolves to against `base`'s origin: an
/// origin-relative well-known or endpoint fallback.
[[nodiscard]] std::string origin_of(std::string_view url) {
    const auto split = split_url(url);
    return split ? split->origin : std::string{url};
}

/// pi `parseOAuthTokens`. `null` and `""` count as absent for the optional
/// fields.
[[nodiscard]] FlowResult<McpOAuthTokenResponse> parse_token_response(std::string_view body) {
    auto parsed = support::read_json(body);
    if (!parsed) {
        return std::unexpected(plain_error(invalid("Invalid OAuth token response", "response is not JSON")));
    }
    const auto* object = ai::json_object(*parsed);
    if (object == nullptr) {
        return std::unexpected(plain_error(invalid("Invalid OAuth token response")));
    }
    const auto access = ai::json_string_member(*object, "access_token");
    if (!access || access->empty()) {
        return std::unexpected(plain_error(invalid("Invalid access_token")));
    }
    const auto token_type = ai::json_string_member(*object, "token_type");
    if (!token_type || token_type->empty()) {
        return std::unexpected(plain_error(invalid("Invalid token_type")));
    }
    McpOAuthTokenResponse tokens;
    tokens.access_token = std::string{*access};
    tokens.token_type = std::string{*token_type};
    if (const auto* expires = ai::json_member(*object, "expires_in"); expires != nullptr) {
        const auto* text = expires->get_if<std::string>();
        const auto* number = expires->get_if<double>();
        const bool absent = (text != nullptr && text->empty()) || expires->holds<support::JsonValue::null_t>();
        if (!absent) {
            double seconds = 0.0;
            if (number != nullptr) {
                seconds = *number;
            } else if (text != nullptr) {
                // pi's `Number(expires_in)` also accepts a numeric string.
                const auto converted = std::from_chars(text->data(), text->data() + text->size(), seconds);
                if (converted.ec != std::errc{} || converted.ptr != text->data() + text->size()) {
                    return std::unexpected(plain_error(invalid("Invalid expires_in")));
                }
            } else {
                return std::unexpected(plain_error(invalid("Invalid expires_in")));
            }
            if (!std::isfinite(seconds)) {
                return std::unexpected(plain_error(invalid("Invalid expires_in")));
            }
            tokens.expires_in = seconds;
        }
    }
    const auto optional_text = [&](std::string_view key) -> std::optional<std::string> {
        const auto* member = ai::json_member(*object, key);
        if (member == nullptr) {
            return std::nullopt;
        }
        const auto* value = member->get_if<std::string>();
        if (value == nullptr || value->empty()) {
            return std::nullopt;
        }
        return *value;
    };
    tokens.scope = optional_text("scope");
    tokens.refresh_token = optional_text("refresh_token");
    tokens.id_token = optional_text("id_token");
    return tokens;
}

/// pi `tokenRequest`: the OAuth error body is read before the status, because
/// servers report OAuth errors with any status.
[[nodiscard]] boost::asio::awaitable<FlowResult<McpOAuthTokenResponse>> token_request(
        std::shared_ptr<ai::auth::OAuthHttpClient> http,
        std::string authorization_server_url,
        const McpOAuthTokenRequest& request,
        std::vector<std::pair<std::string, std::string>> params) {
    const std::string url =
            request.metadata ? request.metadata->token_endpoint : origin_of(authorization_server_url) + "/token";
    if (auto insecure = insecure_endpoint(url); insecure) {
        co_return std::unexpected(plain_error(std::move(*insecure)));
    }
    std::map<std::string, std::string, std::less<>> headers{
            {"Accept", "application/json"},
            {"Content-Type", "application/x-www-form-urlencoded"},
    };
    if (request.resource) {
        params.emplace_back("resource", *request.resource);
    }
    const std::vector<std::string> supported =
            request.metadata
                    ? request.metadata->token_endpoint_auth_methods_supported.value_or(std::vector<std::string>{})
                    : std::vector<std::string>{};
    const std::string method = select_client_auth_method(request.client, supported);
    if (method == "client_secret_basic") {
        if (!request.client.client_secret) {
            co_return std::unexpected(
                    plain_error(invalid("client_secret_basic requires a client secret", request.client.client_id)));
        }
        headers.insert_or_assign("Authorization",
                "Basic " + ai::auth::base64_encode(request.client.client_id + ":" + *request.client.client_secret));
    } else {
        params.emplace_back("client_id", request.client.client_id);
        if (method == "client_secret_post" && request.client.client_secret) {
            params.emplace_back("client_secret", *request.client.client_secret);
        }
    }
    auto response = co_await http->post(std::move(url), std::move(headers), form_encode(params), std::stop_token{});
    if (!response) {
        co_return std::unexpected(plain_error(std::move(response.error())));
    }
    if (const auto parsed = support::read_json(response->body); parsed) {
        if (const auto* object = ai::json_object(*parsed); object != nullptr) {
            if (const auto code = ai::json_string_member(*object, "error"); code && !code->empty()) {
                const auto description = ai::json_string_member(*object, "error_description");
                auto error = oauth_error(
                        description && !description->empty() ? std::string{*description} : std::string{*code});
                if (const auto uri = ai::json_string_member(*object, "error_uri"); uri && !uri->empty()) {
                    error.detail = std::string{*uri};
                }
                co_return std::unexpected(oauth_code_error(std::string{*code}, std::move(error)));
            }
        }
    }
    if (response->status_code < 200 || response->status_code >= 300) {
        // pi `OAuthError("server_error", "HTTP <status>: <text>")`: an HTTP
        // failure without an OAuth error body. It is the one token failure a
        // refresh keeps quiet about.
        co_return std::unexpected(oauth_code_error(
                "server_error", oauth_error("HTTP " + std::to_string(response->status_code) + ": " + response->body)));
    }
    co_return parse_token_response(response->body);
}

/// The authorization-code grant, in the flow's error channel.
[[nodiscard]] boost::asio::awaitable<FlowResult<McpOAuthTokenResponse>> exchange_code_flow(
        std::shared_ptr<ai::auth::OAuthHttpClient> http,
        std::string authorization_server_url,
        McpOAuthTokenRequest request,
        std::string code,
        std::string code_verifier,
        std::string redirect_url) {
    co_return co_await token_request(std::move(http),
            std::move(authorization_server_url),
            request,
            {
                    {"grant_type", "authorization_code"},
                    {"code", std::move(code)},
                    {"code_verifier", std::move(code_verifier)},
                    {"redirect_uri", std::move(redirect_url)},
            });
}

/// The refresh grant, in the flow's error channel, with pi's merge of the old
/// refresh token under the response.
[[nodiscard]] boost::asio::awaitable<FlowResult<McpOAuthTokenResponse>> refresh_flow(
        std::shared_ptr<ai::auth::OAuthHttpClient> http,
        std::string authorization_server_url,
        McpOAuthTokenRequest request,
        std::string refresh_token) {
    auto result = co_await token_request(std::move(http),
            std::move(authorization_server_url),
            request,
            {
                    {"grant_type", "refresh_token"},
                    {"refresh_token", refresh_token},
            });
    if (!result) {
        co_return std::unexpected(std::move(result.error()));
    }
    if (!result->refresh_token) {
        result->refresh_token = std::move(refresh_token);
    }
    co_return std::move(*result);
}

/// pi `clientMetadataDocument`'s callback id: the first 9 bytes of the SHA-256
/// of the MCP server URL, base64url (12 characters).
[[nodiscard]] support::Expected<std::string> callback_id(std::string_view server_url) {
    const auto split = split_url(server_url);
    if (!split) {
        return std::unexpected(invalid("Invalid MCP server URL", std::string{server_url}));
    }
    auto digest = ai::auth::sha256_digest(split->origin + split->path);
    if (!digest) {
        return std::unexpected(std::move(digest.error()));
    }
    return ai::auth::base64url_encode(digest->substr(0, 9));
}

/// A state for a server with no stored record yet.
[[nodiscard]] McpOAuthState empty_state(std::string url) {
    McpOAuthState state;
    state.server_url = std::move(url);
    return state;
}

/// pi's `invalidateCredentials` kinds the flow uses: `all` drops the client
/// registration, the tokens, the verifier and the state; `tokens` drops only
/// the tokens.
enum class InvalidateKind {
    All,
    Tokens,
};

[[nodiscard]] support::ExpectedVoid invalidate_credentials(const std::shared_ptr<McpAuthStore>& store,
        const std::string& name,
        const std::string& url,
        InvalidateKind kind) {
    auto loaded = store->load(name, url);
    if (!loaded) {
        return std::unexpected(std::move(loaded.error()));
    }
    McpOAuthState state = loaded->value_or(empty_state(url));
    state.server_url = url;
    if (kind == InvalidateKind::All) {
        state.client_information = std::nullopt;
        state.code_verifier = std::nullopt;
        state.oauth_state = std::nullopt;
    }
    state.tokens = std::nullopt;
    state.tokens_expire_at = std::nullopt;
    return store->save(name, url, state);
}

/// The token response written onto the stored state: the tokens replace the old
/// ones, the expiry follows `expires_in`, and `scope` is recorded because a
/// response without one grants the requested scope (pi's `withScope`).
[[nodiscard]] McpOAuthState state_with_tokens(
        const McpOAuthState& base, const McpOAuthTokenResponse& tokens, const std::optional<std::string>& scope) {
    McpOAuthState state = base;
    McpOAuthTokens stored;
    stored.access_token = tokens.access_token;
    stored.token_type = tokens.token_type;
    stored.refresh_token = tokens.refresh_token;
    stored.scope = tokens.scope ? tokens.scope : scope;
    state.tokens = std::move(stored);
    if (tokens.expires_in) {
        state.tokens_expire_at = ai::current_timestamp_ms() + static_cast<std::int64_t>(*tokens.expires_in * 1000.0);
    } else {
        state.tokens_expire_at = std::nullopt;
    }
    return state;
}

/// pi `runFlow`. The discovery cache pi keeps in the state is not part of this
/// slice: every run rediscovers the authorization server.
[[nodiscard]] boost::asio::awaitable<FlowResult<McpOAuthFlowOutcome>> run_flow(std::shared_ptr<McpAuthStore> store,
        std::string server_name,
        std::string server_url,
        McpOAuthConfig oauth,
        std::string redirect_url,
        std::shared_ptr<ai::auth::OAuthHttpClient> http,
        McpOAuthFlowOptions options) {
    const std::optional<std::string> metadata_url = oauth.auth_server_metadata_url;
    if (metadata_url) {
        if (auto insecure = insecure_endpoint(*metadata_url); insecure) {
            co_return std::unexpected(plain_error(std::move(*insecure)));
        }
    }
    McpOAuthDiscoveryOptions discovery;
    discovery.resource_metadata_url = options.resource_metadata_url;
    discovery.authorization_server_metadata_url = metadata_url;
    auto discovered = co_await discover_oauth_server_info(http, server_url, discovery);
    if (!discovered) {
        co_return std::unexpected(plain_error(std::move(discovered.error())));
    }
    const auto& metadata = discovered->authorization_server_metadata;
    auto selected = select_resource(server_url, discovered->resource_metadata);
    if (!selected) {
        co_return std::unexpected(plain_error(std::move(selected.error())));
    }
    const std::optional<std::string> resource = std::move(*selected);
    // pi's `||` chain: the caller's scope, then the scopes the protected
    // resource advertises, then the client metadata's own scope.
    const std::optional<std::string> scope = first_non_empty(options.scope,
            join_scopes(
                    discovered->resource_metadata ? discovered->resource_metadata->scopes_supported : std::nullopt));

    auto loaded = store->load(server_name, server_url);
    if (!loaded) {
        co_return std::unexpected(plain_error(std::move(loaded.error())));
    }
    McpOAuthState working = loaded->value_or(empty_state(server_url));
    working.server_url = server_url;
    const auto persist = [&]() -> support::ExpectedVoid { return store->save(server_name, server_url, working); };

    std::optional<McpOAuthClientInformation> client;
    if (oauth.client_id) {
        client = McpOAuthClientInformation{
                .client_id = *oauth.client_id,
                .client_secret = oauth.client_secret,
        };
    } else if (working.client_information) {
        client = working.client_information;
    }
    std::optional<McpOAuthClientMetadataDocument> document;
    if (!client && oauth.client_registration == McpClientRegistration::Cimd) {
        auto derived = client_metadata_document(server_url, redirect_url, metadata);
        if (!derived) {
            co_return std::unexpected(plain_error(std::move(derived.error())));
        }
        document = std::move(*derived);
        const auto split = split_url(document->url);
        if (!document->url.starts_with("https://") || !split || split->path == "/") {
            co_return std::unexpected(plain_error(invalid("Invalid OAuth client metadata URL", document->url)));
        }
        client = McpOAuthClientInformation{.client_id = document->url, .client_secret = std::nullopt};
    }
    if (!client) {
        if (options.authorization_code) {
            co_return std::unexpected(plain_error(invalid("OAuth client information is missing during code exchange")));
        }
        auto registered = co_await register_client(
                http, discovered->authorization_server_url, metadata, client_metadata_for(redirect_url, oauth), scope);
        if (!registered) {
            co_return std::unexpected(plain_error(std::move(registered.error())));
        }
        client = std::move(*registered);
        working.client_information = *client;
        if (auto saved = persist(); !saved) {
            co_return std::unexpected(plain_error(std::move(saved.error())));
        }
    }
    // The document's redirect URI may differ from the caller's, for example by
    // a server-specific path.
    const std::string exchange_redirect_url = document ? document->redirect_url : redirect_url;
    const McpOAuthTokenRequest token_inputs{
            .metadata = metadata,
            .client = *client,
            .resource = resource,
    };
    if (options.authorization_code) {
        // RFC 9207: never send a code from another authorization server to this
        // one. A server that promises `iss` must send it.
        if (metadata &&
                (options.iss.has_value() || metadata->authorization_response_iss_parameter_supported.value_or(false))) {
            if (!options.iss || *options.iss != metadata->issuer) {
                co_return std::unexpected(plain_error(
                        oauth_error("OAuth issuer mismatch: expected \"" + metadata->issuer + "\", received " +
                                    (options.iss ? "\"" + *options.iss + "\"" : std::string{"none"}))));
            }
        }
        if (!working.code_verifier) {
            co_return std::unexpected(plain_error(invalid("No OAuth PKCE code verifier is stored")));
        }
        auto tokens = co_await exchange_code_flow(http,
                discovered->authorization_server_url,
                token_inputs,
                *options.authorization_code,
                *working.code_verifier,
                exchange_redirect_url);
        if (!tokens) {
            co_return std::unexpected(std::move(tokens.error()));
        }
        working = state_with_tokens(working, *tokens, scope);
        if (auto saved = persist(); !saved) {
            co_return std::unexpected(plain_error(std::move(saved.error())));
        }
        co_return McpOAuthFlowOutcome{.result = McpOAuthFlowResult::Authorized, .authorization_url = std::nullopt};
    }
    if (!options.skip_refresh && working.tokens && working.tokens->refresh_token) {
        auto tokens = co_await refresh_flow(
                http, discovered->authorization_server_url, token_inputs, *working.tokens->refresh_token);
        if (!tokens) {
            // pi keeps a plain HTTP failure quiet (the caller then authorizes)
            // and propagates every other failure, including `invalid_grant`.
            if (tokens.error().oauth_code != "server_error") {
                co_return std::unexpected(std::move(tokens.error()));
            }
        } else {
            // A refresh without `scope` keeps the scope of the grant (RFC 6749
            // §6).
            working = state_with_tokens(working, *tokens, working.tokens->scope);
            if (auto saved = persist(); !saved) {
                co_return std::unexpected(plain_error(std::move(saved.error())));
            }
            co_return McpOAuthFlowOutcome{.result = McpOAuthFlowResult::Authorized, .authorization_url = std::nullopt};
        }
    }
    // pi `provider.state()`: reuse the stored state, or make one.
    if (!working.oauth_state || working.oauth_state->empty()) {
        auto created = ai::auth::create_oauth_state();
        if (!created) {
            co_return std::unexpected(plain_error(std::move(created.error())));
        }
        working.oauth_state = std::move(*created);
    }
    auto authorization = start_authorization(discovered->authorization_server_url,
            metadata,
            *client,
            exchange_redirect_url,
            scope,
            working.oauth_state,
            resource);
    if (!authorization) {
        co_return std::unexpected(plain_error(std::move(authorization.error())));
    }
    working.code_verifier = std::move(authorization->code_verifier);
    if (auto saved = persist(); !saved) {
        co_return std::unexpected(plain_error(std::move(saved.error())));
    }
    co_return McpOAuthFlowOutcome{
            .result = McpOAuthFlowResult::Redirect,
            .authorization_url = std::move(authorization->url),
    };
}

} // namespace

McpOAuthClientMetadata client_metadata_for(std::string_view redirect_url, const McpOAuthConfig& oauth) {
    McpOAuthClientMetadata metadata;
    metadata.redirect_uris = {std::string{redirect_url}};
    metadata.token_endpoint_auth_method = oauth.client_secret ? "client_secret_post" : "none";
    metadata.grant_types = {"authorization_code", "refresh_token"};
    metadata.response_types = {"code"};
    metadata.client_name = oauth.client_name.value_or(std::string{kDefaultClientName});
    return metadata;
}

std::string derive_application_type(const std::vector<std::string>& redirect_uris) {
    for (const auto& uri : redirect_uris) {
        if (uri.empty() || !uri.starts_with("http://")) {
            if (!uri.starts_with("https://")) {
                // A custom scheme is a native-app redirect (RFC 8252).
                if (uri.find(':') != std::string::npos) {
                    return "native";
                }
                continue;
            }
        }
        auto authority = std::string_view{uri}.substr(uri.find("://") + 3);
        if (const auto slash = authority.find_first_of("/?#"); slash != std::string_view::npos) {
            authority = authority.substr(0, slash);
        }
        if (is_loopback_host(authority_host(authority))) {
            return "native";
        }
    }
    return "web";
}

std::string select_client_auth_method(
        const McpOAuthClientInformation& client, const std::vector<std::string>& supported_methods) {
    const auto supports = [&](std::string_view method) {
        return std::find(supported_methods.begin(), supported_methods.end(), method) != supported_methods.end();
    };
    if (supported_methods.empty()) {
        return client.client_secret ? "client_secret_basic" : "none";
    }
    if (client.client_secret && supports("client_secret_basic")) {
        return "client_secret_basic";
    }
    if (client.client_secret && supports("client_secret_post")) {
        return "client_secret_post";
    }
    if (supports("none")) {
        return "none";
    }
    return client.client_secret ? "client_secret_post" : "none";
}

support::Expected<std::string> mcp_callback_id(std::string_view server_url) { return callback_id(server_url); }

support::Expected<McpOAuthClientMetadataDocument> client_metadata_document(std::string_view server_url,
        std::string_view redirect_url,
        const std::optional<McpAuthorizationServerMetadata>& metadata) {
    if (!metadata || !metadata->client_id_metadata_document_supported.value_or(false) ||
            !contains(metadata->token_endpoint_auth_methods_supported, "none")) {
        return std::unexpected(invalid(
                "The authorization server does not support Client ID Metadata Documents for public clients; remove "
                "oauth.clientRegistration \"cimd\""));
    }
    if (metadata->authorization_response_iss_parameter_supported.value_or(false)) {
        return McpOAuthClientMetadataDocument{
                .url = std::string{kClientMetadataBaseUrl} + "/client.json",
                .redirect_url = std::string{redirect_url},
        };
    }
    auto id = callback_id(server_url);
    if (!id) {
        return std::unexpected(std::move(id.error()));
    }
    const auto split = split_url(redirect_url);
    if (!split) {
        return std::unexpected(invalid("Invalid OAuth redirect URI", std::string{redirect_url}));
    }
    return McpOAuthClientMetadataDocument{
            .url = std::string{kClientMetadataBaseUrl} + "/" + *id + "/client.json",
            .redirect_url = split->origin + std::string{kCallbackPath} + "/" + *id,
    };
}

boost::asio::awaitable<support::Expected<McpOAuthClientInformation>> register_client(
        std::shared_ptr<ai::auth::OAuthHttpClient> http,
        std::string authorization_server_url,
        std::optional<McpAuthorizationServerMetadata> metadata,
        McpOAuthClientMetadata client_metadata,
        std::optional<std::string> scope) {
    if (metadata && !metadata->registration_endpoint) {
        co_return std::unexpected(invalid("Authorization server does not support dynamic client registration"));
    }
    const std::string url =
            metadata ? *metadata->registration_endpoint : origin_of(authorization_server_url) + "/register";
    support::JsonValue::array_t redirect_uris;
    redirect_uris.reserve(client_metadata.redirect_uris.size());
    for (const auto& uri : client_metadata.redirect_uris) {
        redirect_uris.emplace_back(uri);
    }
    support::JsonValue::array_t grant_types;
    for (const auto& grant : client_metadata.grant_types) {
        grant_types.emplace_back(grant);
    }
    support::JsonValue::array_t response_types;
    for (const auto& response : client_metadata.response_types) {
        response_types.emplace_back(response);
    }
    support::JsonValue::object_t body{
            {"redirect_uris", std::move(redirect_uris)},
            {"token_endpoint_auth_method", client_metadata.token_endpoint_auth_method},
            {"grant_types", std::move(grant_types)},
            {"response_types", std::move(response_types)},
            // MCP SEP-837: without `application_type` an OpenID Connect server
            // assumes `web` and rejects an `http` loopback redirect URI.
            {"application_type", derive_application_type(client_metadata.redirect_uris)},
    };
    if (!client_metadata.client_name.empty()) {
        body.emplace("client_name", client_metadata.client_name);
    }
    if (scope && !scope->empty()) {
        body.emplace("scope", *scope);
    }
    auto serialized = support::write_json(support::JsonValue{std::move(body)});
    if (!serialized) {
        co_return std::unexpected(invalid("could not serialize the client registration", serialized.error().message));
    }
    std::map<std::string, std::string, std::less<>> headers{
            {"Accept", "application/json"},
            {"Content-Type", "application/json"},
    };
    CCH_TRY(response, co_await http->post(url, std::move(headers), *serialized, std::stop_token{}));
    if (response.status_code < 200 || response.status_code >= 300) {
        co_return std::unexpected(oauth_error("OAuth dynamic client registration failed with status " +
                                              std::to_string(response.status_code) + ": " + response.body));
    }
    auto parsed = support::read_json(response.body);
    if (!parsed) {
        co_return std::unexpected(invalid("Invalid OAuth client registration response", "response is not JSON"));
    }
    const auto* object = ai::json_object(*parsed);
    if (object == nullptr) {
        co_return std::unexpected(invalid("Invalid OAuth client registration response"));
    }
    const auto client_id = ai::json_string_member(*object, "client_id");
    if (!client_id || client_id->empty()) {
        co_return std::unexpected(invalid("Invalid client_id"));
    }
    McpOAuthClientInformation client{.client_id = std::string{*client_id}, .client_secret = std::nullopt};
    if (const auto secret = ai::json_string_member(*object, "client_secret"); secret && !secret->empty()) {
        client.client_secret = std::string{*secret};
    }
    // pi `registeredRedirectUrls`: the registration names the redirect URIs the
    // client may use. A server that echoes none is taken to accept the ones
    // the registration requested, so a later sign-in or refresh still knows
    // which redirect URI the client is bound to.
    if (const auto* uris = ai::json_array_member(*object, "redirect_uris"); uris != nullptr) {
        for (const auto& uri : *uris) {
            if (const auto* text = uri.get_if<std::string>(); text != nullptr && !text->empty()) {
                client.redirect_uris.push_back(*text);
            }
        }
    }
    if (client.redirect_uris.empty()) {
        client.redirect_uris = client_metadata.redirect_uris;
    }
    co_return client;
}

support::Expected<McpOAuthAuthorizationRequest> start_authorization(std::string_view authorization_server_url,
        const std::optional<McpAuthorizationServerMetadata>& metadata,
        const McpOAuthClientInformation& client,
        std::string_view redirect_url,
        const std::optional<std::string>& scope,
        const std::optional<std::string>& state,
        const std::optional<std::string>& resource) {
    if (metadata && !contains(metadata->response_types_supported, "code")) {
        return std::unexpected(invalid("Authorization server does not support authorization codes"));
    }
    if (metadata && metadata->code_challenge_methods_supported &&
            !contains(metadata->code_challenge_methods_supported, "S256")) {
        return std::unexpected(invalid("Authorization server does not support PKCE S256"));
    }
    std::string url = metadata ? metadata->authorization_endpoint : origin_of(authorization_server_url) + "/authorize";
    auto pkce = ai::auth::generate_pkce();
    if (!pkce) {
        return std::unexpected(std::move(pkce.error()));
    }
    url = append_query(std::move(url), "response_type", "code");
    url = append_query(std::move(url), "client_id", client.client_id);
    url = append_query(std::move(url), "code_challenge", pkce->challenge);
    url = append_query(std::move(url), "code_challenge_method", "S256");
    url = append_query(std::move(url), "redirect_uri", redirect_url);
    if (state && !state->empty()) {
        url = append_query(std::move(url), "state", *state);
    }
    if (scope && !scope->empty()) {
        url = append_query(std::move(url), "scope", *scope);
        if (scope_contains(*scope, "offline_access")) {
            url = append_query(std::move(url), "prompt", "consent");
        }
    }
    if (resource && !resource->empty()) {
        url = append_query(std::move(url), "resource", *resource);
    }
    return McpOAuthAuthorizationRequest{.url = std::move(url), .code_verifier = std::move(pkce->verifier)};
}

boost::asio::awaitable<support::Expected<McpOAuthTokenResponse>> exchange_authorization_code(
        std::shared_ptr<ai::auth::OAuthHttpClient> http,
        std::string authorization_server_url,
        McpOAuthTokenRequest request,
        std::string code,
        std::string code_verifier,
        std::string redirect_url) {
    auto result = co_await exchange_code_flow(std::move(http),
            std::move(authorization_server_url),
            std::move(request),
            std::move(code),
            std::move(code_verifier),
            std::move(redirect_url));
    if (!result) {
        co_return std::unexpected(std::move(result.error().error));
    }
    co_return std::move(*result);
}

boost::asio::awaitable<support::Expected<McpOAuthTokenResponse>> refresh_authorization(
        std::shared_ptr<ai::auth::OAuthHttpClient> http,
        std::string authorization_server_url,
        McpOAuthTokenRequest request,
        std::string refresh_token) {
    auto result = co_await refresh_flow(
            std::move(http), std::move(authorization_server_url), std::move(request), std::move(refresh_token));
    if (!result) {
        co_return std::unexpected(std::move(result.error().error));
    }
    co_return std::move(*result);
}

boost::asio::awaitable<support::Expected<McpOAuthFlowOutcome>> authorize_mcp(std::shared_ptr<McpAuthStore> store,
        std::string server_name,
        std::string server_url,
        McpOAuthConfig oauth,
        std::string redirect_url,
        std::shared_ptr<ai::auth::OAuthHttpClient> http,
        McpOAuthFlowOptions options) {
    auto attempt = co_await run_flow(store, server_name, server_url, oauth, redirect_url, http, options);
    if (attempt) {
        co_return std::move(*attempt);
    }
    const std::string code = attempt.error().oauth_code;
    if (code == "invalid_client" || code == "unauthorized_client") {
        if (auto invalidated = invalidate_credentials(store, server_name, server_url, InvalidateKind::All);
                !invalidated) {
            co_return std::unexpected(std::move(invalidated.error()));
        }
        auto retried = co_await run_flow(
                store, server_name, server_url, std::move(oauth), std::move(redirect_url), std::move(http), options);
        if (!retried) {
            co_return std::unexpected(std::move(retried.error().error));
        }
        co_return std::move(*retried);
    }
    if (code == "invalid_grant") {
        if (auto invalidated = invalidate_credentials(store, server_name, server_url, InvalidateKind::Tokens);
                !invalidated) {
            co_return std::unexpected(std::move(invalidated.error()));
        }
        auto retried = co_await run_flow(
                store, server_name, server_url, std::move(oauth), std::move(redirect_url), std::move(http), options);
        if (!retried) {
            co_return std::unexpected(std::move(retried.error().error));
        }
        co_return std::move(*retried);
    }
    co_return std::unexpected(std::move(attempt.error().error));
}

} // namespace cch::coding_agent::mcp
