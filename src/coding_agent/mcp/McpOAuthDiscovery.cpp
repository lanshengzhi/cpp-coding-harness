// RFC 9728 protected-resource discovery and RFC 8414 / OpenID Connect
// authorization-server discovery (spec #882, ticket #884). pi source at
// `7c10bd43` (v1.0.4): `packages/mcp/src/oauth/discovery.ts`. The candidate
// URLs, the 4xx/502 miss rule, the parsing messages, and the issuer check are
// pi's; the transport is the shared injectable `ai::auth::OAuthHttpClient`, so
// the discovery speaks the same HTTPS client as the rest of the OAuth flow and
// a test scripts it without a network.

#include "coding_agent/mcp/McpOAuthDiscovery.hpp"
#include "coding_agent/mcp/McpUrl.hpp"

#include "ai/JsonAccess.hpp"
#include "ai/auth/OAuthHttpClient.hpp"

#include "support/ExpectedMacros.hpp"
#include "support/Json.hpp"

#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::coding_agent::mcp {
namespace {

[[nodiscard]] support::Error invalid(std::string name, std::string detail = {}) {
    return support::make_error(support::ErrorCode::Validation, "Invalid " + std::move(name), std::move(detail));
}

[[nodiscard]] support::Error metadata_http_error(std::string_view kind, std::string_view url, int status) {
    return support::make_error(support::ErrorCode::OAuth,
            "HTTP " + std::to_string(status) + " loading " + std::string{kind},
            "rejected metadata document: " + std::string{url});
}

/// 4xx and 502 mean "not here", so discovery tries the next candidate URL
/// (pi `isDiscoveryMiss`).
[[nodiscard]] bool is_discovery_miss(int status) { return (status >= 400 && status < 500) || status == 502; }

/// Path suffix for `/.well-known/<kind><path>`; empty for the root path
/// (pi `pathSuffix` strips one trailing slash).
[[nodiscard]] std::string path_suffix(std::string_view pathname) {
    return pathname.ends_with('/') ? std::string{pathname.substr(0, pathname.size() - 1)} : std::string{pathname};
}

/// The parts of an absolute URL discovery reasons about: the origin
/// (`scheme://authority`, lowercased host, default port dropped) and the
/// pathname. `std::nullopt` when the value is not an absolute URL.
struct DiscoveryUrl {
    std::string origin;
    std::string path;
};

[[nodiscard]] bool is_forbidden_scheme(std::string_view scheme) {
    return scheme == "javascript" || scheme == "data" || scheme == "vbscript";
}

[[nodiscard]] std::optional<DiscoveryUrl> parse_discovery_url(std::string_view value) {
    auto parsed = parse_mcp_url(value);
    if (!parsed.valid || is_forbidden_scheme(parsed.scheme)) {
        return std::nullopt;
    }
    return DiscoveryUrl{
            .origin = std::move(parsed.origin),
            .path = std::move(parsed.path),
    };
}

/// pi `safeUrl`: a non-empty absolute URL whose scheme is not a script or data
/// scheme.
[[nodiscard]] bool is_safe_url(std::string_view value) {
    return !value.empty() && parse_discovery_url(value).has_value();
}

/// pi `absent`: `undefined`, `null`, and `""` all mean "no value".
[[nodiscard]] bool absent(const support::JsonValue& value) {
    if (value.holds<support::JsonValue::null_t>()) {
        return true;
    }
    const auto* text = value.get_if<std::string>();
    return text != nullptr && text->empty();
}

/// pi `requiredString`: present, a string, and non-empty.
[[nodiscard]] std::optional<std::string> required_string(const support::JsonValue& value) {
    const auto* text = value.get_if<std::string>();
    if (text == nullptr || text->empty()) {
        return std::nullopt;
    }
    return *text;
}

/// pi `optionalString`: absent (`null`/`""`/missing) yields `std::nullopt`.
[[nodiscard]] std::optional<std::string> optional_string(const support::JsonValue& value) {
    if (absent(value)) {
        return std::nullopt;
    }
    return required_string(value);
}

/// pi `optionalStrings`: absent yields `std::nullopt`; a non-array or a
/// non-string element is a parse failure.
[[nodiscard]] std::optional<std::optional<std::vector<std::string>>> optional_strings(const support::JsonValue& value) {
    if (value.holds<support::JsonValue::null_t>()) {
        return std::optional<std::vector<std::string>>{};
    }
    const auto* array = value.get_if<support::JsonValue::array_t>();
    if (array == nullptr) {
        return std::nullopt;
    }
    std::vector<std::string> strings;
    strings.reserve(array->size());
    for (const auto& item : *array) {
        const auto* text = item.get_if<std::string>();
        if (text == nullptr) {
            return std::nullopt;
        }
        strings.push_back(*text);
    }
    return std::optional<std::vector<std::string>>{std::move(strings)};
}

[[nodiscard]] boost::asio::awaitable<support::Expected<ai::auth::OAuthHttpResponse>> fetch_metadata(
        std::shared_ptr<ai::auth::OAuthHttpClient> http, std::string url, std::string protocol_version) {
    std::map<std::string, std::string, std::less<>> headers{
            {"Accept", "application/json"},
            {"MCP-Protocol-Version", std::move(protocol_version)},
    };
    co_return co_await http->get(std::move(url), std::move(headers), std::stop_token{});
}

/// The parsed JSON of a metadata response body; a body that is not JSON is a
/// parse failure under the document's name.
[[nodiscard]] support::Expected<support::JsonValue> parse_metadata_body(
        const std::string& body, std::string_view name) {
    auto parsed = support::read_json(body);
    if (!parsed) {
        return std::unexpected(invalid(std::string{name}, "response is not JSON"));
    }
    return std::move(*parsed);
}

} // namespace

support::Expected<McpProtectedResourceMetadata> parse_protected_resource_metadata(const support::JsonValue& value) {
    const auto* object = ai::json_object(value);
    if (object == nullptr) {
        return std::unexpected(invalid("OAuth protected resource metadata"));
    }
    const auto* resource = ai::json_member(*object, "resource");
    const auto resource_text = resource != nullptr ? required_string(*resource) : std::nullopt;
    if (!resource_text) {
        return std::unexpected(invalid("OAuth protected resource metadata resource"));
    }
    if (!is_safe_url(*resource_text)) {
        return std::unexpected(invalid("OAuth protected resource metadata resource"));
    }
    McpProtectedResourceMetadata metadata;
    metadata.resource = *resource_text;
    if (const auto* servers = ai::json_member(*object, "authorization_servers"); servers != nullptr) {
        auto strings = optional_strings(*servers);
        if (!strings) {
            return std::unexpected(invalid("authorization_servers"));
        }
        if (strings->has_value()) {
            std::vector<std::string> urls;
            urls.reserve((*strings)->size());
            for (const auto& candidate : **strings) {
                if (!is_safe_url(candidate)) {
                    return std::unexpected(invalid("authorization server URL"));
                }
                urls.push_back(candidate);
            }
            metadata.authorization_servers = std::move(urls);
        }
    }
    if (const auto* scopes = ai::json_member(*object, "scopes_supported"); scopes != nullptr) {
        auto strings = optional_strings(*scopes);
        if (!strings) {
            return std::unexpected(invalid("scopes_supported"));
        }
        metadata.scopes_supported = std::move(*strings);
    }
    return metadata;
}

support::Expected<McpAuthorizationServerMetadata> parse_authorization_server_metadata(const support::JsonValue& value) {
    const auto* object = ai::json_object(value);
    if (object == nullptr) {
        return std::unexpected(invalid("authorization server metadata"));
    }
    const auto* response_types = ai::json_member(*object, "response_types_supported");
    auto response_type_list = response_types != nullptr ? optional_strings(*response_types)
                                                        : std::optional<std::optional<std::vector<std::string>>>{};
    if (!response_type_list || !response_type_list->has_value() || (**response_type_list).empty()) {
        return std::unexpected(invalid("response_types_supported"));
    }
    const auto url_field = [&](std::string_view key, std::string_view name) -> support::Expected<std::string> {
        const auto* member = ai::json_member(*object, key);
        const auto text = member != nullptr ? required_string(*member) : std::nullopt;
        if (!text || !is_safe_url(*text)) {
            return std::unexpected(invalid(std::string{name}));
        }
        return *text;
    };
    auto issuer = url_field("issuer", "authorization server issuer");
    if (!issuer) {
        return std::unexpected(std::move(issuer.error()));
    }
    auto authorization_endpoint = url_field("authorization_endpoint", "authorization endpoint");
    if (!authorization_endpoint) {
        return std::unexpected(std::move(authorization_endpoint.error()));
    }
    auto token_endpoint = url_field("token_endpoint", "token endpoint");
    if (!token_endpoint) {
        return std::unexpected(std::move(token_endpoint.error()));
    }

    McpAuthorizationServerMetadata metadata;
    metadata.issuer = std::move(*issuer);
    metadata.authorization_endpoint = std::move(*authorization_endpoint);
    metadata.token_endpoint = std::move(*token_endpoint);
    if (const auto* registration = ai::json_member(*object, "registration_endpoint"); registration != nullptr) {
        const auto text = optional_string(*registration);
        if (!text || !is_safe_url(*text)) {
            return std::unexpected(invalid("registration endpoint"));
        }
        metadata.registration_endpoint = std::move(*text);
    }
    metadata.response_types_supported = std::move(**response_type_list);
    const auto list_field =
            [&](std::string_view key,
                    std::string_view name) -> support::Expected<std::optional<std::vector<std::string>>> {
        const auto* member = ai::json_member(*object, key);
        if (member == nullptr) {
            return std::optional<std::vector<std::string>>{};
        }
        auto strings = optional_strings(*member);
        if (!strings) {
            return std::unexpected(invalid(std::string{name}));
        }
        return std::move(*strings);
    };
    const std::pair<std::string_view, std::string_view> list_fields[] = {
            {"scopes_supported", "scopes_supported"},
            {"grant_types_supported", "grant_types_supported"},
            {"token_endpoint_auth_methods_supported", "token_endpoint_auth_methods_supported"},
            {"code_challenge_methods_supported", "code_challenge_methods_supported"},
    };
    std::optional<std::vector<std::string>> lists[4];
    for (std::size_t index = 0; index < 4; ++index) {
        auto list = list_field(list_fields[index].first, list_fields[index].second);
        if (!list) {
            return std::unexpected(std::move(list.error()));
        }
        lists[index] = std::move(*list);
    }
    metadata.scopes_supported = std::move(lists[0]);
    metadata.grant_types_supported = std::move(lists[1]);
    metadata.token_endpoint_auth_methods_supported = std::move(lists[2]);
    metadata.code_challenge_methods_supported = std::move(lists[3]);
    if (const auto* flag = ai::json_member(*object, "client_id_metadata_document_supported"); flag != nullptr) {
        metadata.client_id_metadata_document_supported = flag->get_if<bool>();
    }
    if (const auto* flag = ai::json_member(*object, "authorization_response_iss_parameter_supported");
            flag != nullptr) {
        metadata.authorization_response_iss_parameter_supported = flag->get_if<bool>();
    }
    return metadata;
}

McpOAuthChallenge parse_www_authenticate(std::string_view header) {
    McpOAuthChallenge challenge;
    const auto first = header.find_first_not_of(" \t");
    if (first == std::string_view::npos) {
        return challenge;
    }
    const auto scheme_end = header.find_first_of(" \t", first);
    std::string scheme{
            header.substr(first, scheme_end == std::string_view::npos ? std::string_view::npos : scheme_end - first)};
    for (char& character : scheme) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    if (scheme != "bearer" && scheme != "dpop") {
        return challenge;
    }
    // pi's field pattern: `(?:^|[,\s])name=(?:"([^"]*)"|([^\s,]+))`,
    // case-insensitive, first match wins, an empty value counts as absent.
    const auto field = [&](std::string_view name) -> std::optional<std::string> {
        for (std::size_t index = 0; index + name.size() + 1 <= header.size(); ++index) {
            if (index != 0) {
                const char previous = header[index - 1];
                if (previous != ',' && previous != ' ' && previous != '\t') {
                    continue;
                }
            }
            bool matches = true;
            for (std::size_t offset = 0; offset < name.size(); ++offset) {
                const auto character = static_cast<unsigned char>(header[index + offset]);
                if (std::tolower(character) != static_cast<unsigned char>(name[offset])) {
                    matches = false;
                    break;
                }
            }
            if (!matches || header[index + name.size()] != '=') {
                continue;
            }
            const auto value_start = index + name.size() + 1;
            if (value_start >= header.size()) {
                continue;
            }
            if (header[value_start] == '"') {
                const auto close = header.find('"', value_start + 1);
                if (close == std::string_view::npos) {
                    continue;
                }
                const std::string value{header.substr(value_start + 1, close - value_start - 1)};
                return value.empty() ? std::nullopt : std::optional<std::string>{value};
            }
            const auto value_end = header.find_first_of(", \t", value_start);
            const std::string value{header.substr(value_start,
                    value_end == std::string_view::npos ? std::string_view::npos : value_end - value_start)};
            return value.empty() ? std::nullopt : std::optional<std::string>{value};
        }
        return std::nullopt;
    };
    if (const auto resource = field("resource_metadata"); resource) {
        if (parse_discovery_url(*resource).has_value()) {
            challenge.resource_metadata_url = *resource;
        }
    }
    challenge.scope = field("scope");
    challenge.error = field("error");
    challenge.error_description = field("error_description");
    return challenge;
}

support::Expected<std::optional<std::string>> select_resource(
        std::string_view server_url, const std::optional<McpProtectedResourceMetadata>& metadata) {
    if (!metadata.has_value()) {
        return std::optional<std::string>{};
    }
    const auto requested = parse_discovery_url(server_url);
    const auto configured = parse_discovery_url(metadata->resource);
    if (!requested || !configured) {
        return std::unexpected(invalid("OAuth protected resource metadata resource"));
    }
    const auto mismatch = [&]() {
        return support::make_error(support::ErrorCode::OAuth,
                "Protected resource " + metadata->resource + " does not match MCP server " + std::string{server_url});
    };
    if (requested->origin != configured->origin) {
        return std::unexpected(mismatch());
    }
    const auto with_trailing_slash = [](const std::string& path) { return path.ends_with('/') ? path : path + "/"; };
    const std::string requested_path = with_trailing_slash(requested->path);
    const std::string configured_path = with_trailing_slash(configured->path);
    if (!requested_path.starts_with(configured_path)) {
        return std::unexpected(mismatch());
    }
    return std::optional<std::string>{metadata->resource};
}

std::vector<std::string> authorization_server_discovery_urls(std::string_view authorization_server_url) {
    const auto issuer = parse_discovery_url(authorization_server_url);
    if (!issuer) {
        return {};
    }
    const std::string path = path_suffix(issuer->path);
    std::vector<std::string> urls;
    urls.push_back(issuer->origin + "/.well-known/oauth-authorization-server" + path);
    urls.push_back(issuer->origin + "/.well-known/openid-configuration" + path);
    if (!path.empty()) {
        urls.push_back(issuer->origin + path + "/.well-known/openid-configuration");
    }
    return urls;
}

boost::asio::awaitable<support::Expected<McpProtectedResourceMetadata>> discover_protected_resource_metadata(
        std::shared_ptr<ai::auth::OAuthHttpClient> http, std::string server_url, McpOAuthDiscoveryOptions options) {
    const auto server = parse_discovery_url(server_url);
    if (!server) {
        co_return std::unexpected(invalid("MCP server URL", server_url));
    }
    const bool configured_url = options.resource_metadata_url.has_value();
    const std::string first_url =
            configured_url ? *options.resource_metadata_url
                           : server->origin + "/.well-known/oauth-protected-resource" + path_suffix(server->path);
    CCH_TRY(response, co_await fetch_metadata(http, first_url, options.protocol_version));
    int status = response.status_code;
    std::string body = std::move(response.body);
    if (!configured_url && server->path != "/" && is_discovery_miss(status)) {
        CCH_TRY(retry,
                co_await fetch_metadata(
                        http, server->origin + "/.well-known/oauth-protected-resource", options.protocol_version));
        status = retry.status_code;
        body = std::move(retry.body);
    }
    if (status < 200 || status >= 300) {
        co_return std::unexpected(metadata_http_error("OAuth protected resource metadata", first_url, status));
    }
    CCH_TRY(parsed, parse_metadata_body(body, "OAuth protected resource metadata"));
    co_return parse_protected_resource_metadata(parsed);
}

boost::asio::awaitable<support::Expected<std::optional<McpAuthorizationServerMetadata>>>
discover_authorization_server_metadata(std::shared_ptr<ai::auth::OAuthHttpClient> http,
        std::string authorization_server_url,
        McpOAuthDiscoveryOptions options) {
    for (const auto& url : authorization_server_discovery_urls(authorization_server_url)) {
        CCH_TRY(response, co_await fetch_metadata(http, url, options.protocol_version));
        if (response.status_code < 200 || response.status_code >= 300) {
            if (is_discovery_miss(response.status_code)) {
                continue;
            }
            co_return std::unexpected(
                    metadata_http_error("authorization server metadata from " + url, url, response.status_code));
        }
        CCH_TRY(parsed, parse_metadata_body(response.body, "authorization server metadata"));
        CCH_TRY(metadata, parse_authorization_server_metadata(parsed));
        if (!options.skip_issuer_validation) {
            // URL parsing adds a trailing slash to a bare origin, so compare
            // without one on either side (pi's `trim`).
            const auto trim = [](std::string value) {
                if (value.ends_with('/')) {
                    value.pop_back();
                }
                return value;
            };
            if (trim(metadata.issuer) != trim(authorization_server_url)) {
                co_return std::unexpected(support::make_error(support::ErrorCode::OAuth,
                        "OAuth issuer mismatch: expected \"" + authorization_server_url + "\", received \"" +
                                metadata.issuer + "\"",
                        "the authorization server metadata names another issuer"));
            }
        }
        co_return std::optional<McpAuthorizationServerMetadata>{std::move(metadata)};
    }
    co_return std::optional<McpAuthorizationServerMetadata>{};
}

boost::asio::awaitable<support::Expected<McpOAuthServerInfo>> discover_oauth_server_info(
        std::shared_ptr<ai::auth::OAuthHttpClient> http, std::string server_url, McpOAuthDiscoveryOptions options) {
    std::optional<McpProtectedResourceMetadata> resource_metadata;
    auto discovered = co_await discover_protected_resource_metadata(http, server_url, options);
    if (discovered) {
        resource_metadata = std::move(*discovered);
    } else if (discovered.error().code == support::ErrorCode::Network ||
               discovered.error().code == support::ErrorCode::Cancelled) {
        // pi rethrows the fetch `TypeError`; a network failure is not a
        // "the server published nothing" answer.
        co_return std::unexpected(std::move(discovered.error()));
    }
    if (options.authorization_server_metadata_url) {
        const std::string& url = *options.authorization_server_metadata_url;
        CCH_TRY(response, co_await fetch_metadata(http, url, options.protocol_version));
        if (response.status_code < 200 || response.status_code >= 300) {
            co_return std::unexpected(
                    metadata_http_error("authorization server metadata from " + url, url, response.status_code));
        }
        CCH_TRY(parsed, parse_metadata_body(response.body, "authorization server metadata"));
        CCH_TRY(metadata, parse_authorization_server_metadata(parsed));
        co_return McpOAuthServerInfo{
                .authorization_server_url = metadata.issuer,
                .authorization_server_metadata = std::move(metadata),
                .resource_metadata = std::move(resource_metadata),
        };
    }
    const auto server = parse_discovery_url(server_url);
    if (!server) {
        co_return std::unexpected(invalid("MCP server URL", server_url));
    }
    const std::string authorization_server_url = resource_metadata.has_value() &&
                                                                 resource_metadata->authorization_servers.has_value() &&
                                                                 !resource_metadata->authorization_servers->empty()
                                                         ? resource_metadata->authorization_servers->front()
                                                         : server->origin + "/";
    CCH_TRY(metadata, co_await discover_authorization_server_metadata(http, authorization_server_url, options));
    co_return McpOAuthServerInfo{
            .authorization_server_url = authorization_server_url,
            .authorization_server_metadata = std::move(metadata),
            .resource_metadata = std::move(resource_metadata),
    };
}

} // namespace cch::coding_agent::mcp
