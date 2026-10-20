#pragma once

// pi `McpOAuthState` and its token/client values (`packages/mcp/src/oauth/
// provider.ts`) plus the persistence seam the flow writes through. The file
// location and format (`mcp-auth.json`) belong to the application; cch_mcp
// owns only the value and store contract.

#include <cch/support/Error.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cch::mcp {

/// pi `OAuthTokens`, narrowed to the fields the MCP flow stores.
struct OAuthTokens {
    std::string access_token;
    std::string token_type;
    std::optional<std::string> refresh_token;
    /// The scope the grant carries: a token response without one grants the
    /// requested scope, and a refresh keeps the granted scope (RFC 6749
    /// §5.1/§6), so it is recorded rather than inferred.
    std::optional<std::string> scope;
};

/// pi `OAuthClientInformationMixed`, narrowed to the public-client fields:
/// the dynamically registered (or configured) client id, optional secret, and
/// the redirect URIs the registration lists.
struct OAuthClientInformation {
    std::string client_id;
    std::optional<std::string> client_secret{std::nullopt};
    std::vector<std::string> redirect_uris{};
};

/// pi `McpOAuthState`: one server's OAuth state.
struct OAuthState {
    std::string server_url;
    std::optional<OAuthClientInformation> client_information;
    std::optional<OAuthTokens> tokens;
    /// When the access token expires, in milliseconds since the epoch, from
    /// `expires_in` at the time it was saved.
    std::optional<std::int64_t> tokens_expire_at;
    std::optional<std::string> code_verifier;
    std::optional<std::string> oauth_state;
};

/// The persistence seam the OAuth flow reads and writes. The application owns
/// the concrete `mcp-auth.json` file; a generic caller supplies any store with
/// the same load/save contract.
class OAuthStateStore {
public:
    virtual ~OAuthStateStore() = default;

    [[nodiscard]] virtual support::Expected<std::optional<OAuthState>> load(
            std::string_view name, std::string_view server_url) = 0;
    [[nodiscard]] virtual support::ExpectedVoid save(
            std::string_view name, std::string_view server_url, const OAuthState& state) = 0;
};

} // namespace cch::mcp
