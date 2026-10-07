#pragma once

// RFC 9728 protected-resource discovery and RFC 8414 / OpenID Connect
// authorization-server discovery (spec #882, ticket #884). pi source at
// `7c10bd43` (v1.0.4): `packages/mcp/src/oauth/discovery.ts` and
// `packages/mcp/src/oauth/types.ts` (`parseProtectedResourceMetadata`,
// `parseAuthorizationServerMetadata`). The metadata documents are parsed and
// validated with pi's messages, so a server that publishes a malformed
// document is an explicit error rather than a silently empty endpoint set.

#include <cch/support/Error.hpp>
#include <cch/support/JsonValue.hpp>

#include <boost/asio/awaitable.hpp>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cch::ai::auth {
class OAuthHttpClient;
} // namespace cch::ai::auth

namespace cch::coding_agent::mcp {

/// pi `OAuthProtectedResourceMetadata` (RFC 9728): the document at
/// `/.well-known/oauth-protected-resource<path>`.
struct McpProtectedResourceMetadata {
    std::string resource;
    std::optional<std::vector<std::string>> authorization_servers;
    std::optional<std::vector<std::string>> scopes_supported;
};

/// pi `AuthorizationServerMetadata` (RFC 8414 / OpenID Connect): the document
/// describing one authorization server. `response_types_supported` is
/// required; the endpoints are validated as absolute URLs.
struct McpAuthorizationServerMetadata {
    std::string issuer;
    std::string authorization_endpoint;
    std::string token_endpoint;
    std::optional<std::string> registration_endpoint;
    std::optional<std::vector<std::string>> scopes_supported;
    std::vector<std::string> response_types_supported;
    std::optional<std::vector<std::string>> grant_types_supported;
    std::optional<std::vector<std::string>> token_endpoint_auth_methods_supported;
    std::optional<std::vector<std::string>> code_challenge_methods_supported;
    std::optional<bool> client_id_metadata_document_supported;
    /// Whether authorization responses carry an `iss` parameter (RFC 9207).
    std::optional<bool> authorization_response_iss_parameter_supported;
};

/// pi `OAuthChallenge`: the fields of a `WWW-Authenticate` challenge that
/// steer sign-in. Every field is absent when the header is not a bearer/dpop
/// challenge or carries no value for it.
struct McpOAuthChallenge {
    std::optional<std::string> resource_metadata_url;
    std::optional<std::string> scope;
    std::optional<std::string> error;
    std::optional<std::string> error_description;
};

/// pi `OAuthServerInfo`: the authorization server discovered for one MCP
/// server, with the protected-resource metadata it came from.
struct McpOAuthServerInfo {
    std::string authorization_server_url;
    std::optional<McpAuthorizationServerMetadata> authorization_server_metadata;
    std::optional<McpProtectedResourceMetadata> resource_metadata;
};

/// The `MCP-Protocol-Version` header pi sends on discovery requests: its
/// `LATEST_PROTOCOL_VERSION` (`packages/mcp/src/protocol/types.ts`).
inline constexpr std::string_view kMcpDiscoveryProtocolVersion = "2025-11-25";

/// pi `parseProtectedResourceMetadata`. A non-object, a missing or
/// unparseable `resource`, or a malformed `authorization_servers` /
/// `scopes_supported` list is an error with pi's message ("Invalid OAuth
/// protected resource metadata" and friends); `null` and `""` count as absent.
[[nodiscard]] support::Expected<McpProtectedResourceMetadata> parse_protected_resource_metadata(
        const support::JsonValue& value);

/// pi `parseAuthorizationServerMetadata`. `issuer`, `authorization_endpoint`,
/// `token_endpoint`, and a non-empty `response_types_supported` are required;
/// the optional URL and string-list fields are validated when present.
[[nodiscard]] support::Expected<McpAuthorizationServerMetadata> parse_authorization_server_metadata(
        const support::JsonValue& value);

/// pi `parseWwwAuthenticate`: only the `bearer` and `dpop` schemes carry
/// fields, field names are case-insensitive, quoted and unquoted values are
/// both read, and an empty value counts as absent.
[[nodiscard]] McpOAuthChallenge parse_www_authenticate(std::string_view header);

/// pi `selectResource`: the OAuth `resource` parameter for `server_url`, or
/// `std::nullopt` when the server published no protected-resource metadata.
/// The metadata's `resource` must be the same origin and a path prefix of the
/// MCP server URL, else the error names both ("Protected resource ... does not
/// match MCP server ...") — a server never sees an authorization request for
/// resources it does not own.
[[nodiscard]] support::Expected<std::optional<std::string>> select_resource(
        std::string_view server_url, const std::optional<McpProtectedResourceMetadata>& metadata);

/// pi `buildAuthorizationServerDiscoveryUrls`, in its order: the RFC 8414
/// well-known path with the issuer path appended, the OpenID Connect
/// configuration path likewise, and for a non-root issuer path the
/// path-prefixed OpenID Connect form.
[[nodiscard]] std::vector<std::string> authorization_server_discovery_urls(std::string_view authorization_server_url);

/// Discovery options shared by the three discovery entry points.
struct McpOAuthDiscoveryOptions {
    /// pi `resourceMetadataUrl`: use this document instead of the server's
    /// `/.well-known/oauth-protected-resource` path.
    std::optional<std::string> resource_metadata_url;
    /// pi `authorizationServerMetadataUrl`: use this metadata document instead
    /// of discovery. It is trusted as configured, so its issuer is not checked.
    std::optional<std::string> authorization_server_metadata_url;
    std::string protocol_version{std::string{kMcpDiscoveryProtocolVersion}};
    /// pi `skipIssuerValidation` (tests and configured metadata documents).
    bool skip_issuer_validation{false};
};

/// pi `discoverProtectedResourceMetadata`: GET the well-known document at the
/// server's origin and path, retrying the bare origin path when the
/// path-suffixed one is a miss (4xx or 502) and the server path is not `/`.
/// The document must parse, else the error is explicit.
[[nodiscard]] boost::asio::awaitable<support::Expected<McpProtectedResourceMetadata>>
discover_protected_resource_metadata(
        std::shared_ptr<ai::auth::OAuthHttpClient> http, std::string server_url, McpOAuthDiscoveryOptions options = {});

/// pi `discoverAuthorizationServerMetadata`: try the candidate URLs in order
/// until one answers with metadata; a 4xx or 502 is a miss, any other failure
/// is an error. `std::nullopt` means every candidate missed. A hit whose
/// `issuer` differs from the authorization server URL (trailing-slash
/// insensitive) is rejected unless `skip_issuer_validation`.
[[nodiscard]] boost::asio::awaitable<support::Expected<std::optional<McpAuthorizationServerMetadata>>>
discover_authorization_server_metadata(std::shared_ptr<ai::auth::OAuthHttpClient> http,
        std::string authorization_server_url,
        McpOAuthDiscoveryOptions options = {});

/// pi `discoverOAuthServerInfo`: protected-resource discovery (best effort — a
/// non-network failure is ignored), then the authorization server, which is
/// `resourceMetadata.authorization_servers[0]` or the server's origin, and its
/// metadata. A configured `authorization_server_metadata_url` is fetched
/// instead and its `issuer` is the authorization-server URL.
[[nodiscard]] boost::asio::awaitable<support::Expected<McpOAuthServerInfo>> discover_oauth_server_info(
        std::shared_ptr<ai::auth::OAuthHttpClient> http, std::string server_url, McpOAuthDiscoveryOptions options = {});

} // namespace cch::coding_agent::mcp
