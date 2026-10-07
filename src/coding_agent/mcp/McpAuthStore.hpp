#pragma once

// pi's MCP OAuth credential store (`mcp-auth.json`), spec #882 ticket #884. pi
// source at `7c10bd43` (v1.0.4): `packages/coding-agent/src/extensions/mcp/
// oauth.ts` (`McpOAuthCredentialStore`, `storeKeys`) and
// `packages/mcp/src/oauth/provider.ts` (`McpOAuthState`). The file lives at
// `<agentDir>/mcp-auth.json`, keys are `mcp__<server>|<url>`, and the legacy
// bare-URL key written by older versions is taken over on load. This replaces
// the `auth.json`-backed store of #875: existing `mcp__<server>` OAuth records
// are migrated in.

#include <cch/support/Error.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cch::coding_agent::mcp {

/// pi `OAuthTokens`, narrowed to the fields the MCP flow stores.
struct McpOAuthTokens {
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
/// the redirect URIs the registration lists. The URIs are recorded because a
/// later sign-in reuses the registered port and a request-time refresh sends
/// the registered redirect URI, so the client stays valid (pi
/// `registeredRedirectUrls`).
struct McpOAuthClientInformation {
    std::string client_id;
    std::optional<std::string> client_secret{std::nullopt};
    std::vector<std::string> redirect_uris{};
};

/// pi `McpOAuthState`: one server's OAuth state. `discovery` (the resolved
/// authorization-server metadata) is not part of this slice.
struct McpOAuthState {
    std::string server_url;
    std::optional<McpOAuthClientInformation> client_information;
    std::optional<McpOAuthTokens> tokens;
    /// When the access token expires, in milliseconds since the epoch, from
    /// `expires_in` at the time it was saved.
    std::optional<std::int64_t> tokens_expire_at;
    std::optional<std::string> code_verifier;
    std::optional<std::string> oauth_state;
};

/// The per-server OAuth state in `<agentDir>/mcp-auth.json`.
class McpAuthStore {
public:
    /// The store path pi uses: `<agent_dir>/mcp-auth.json`.
    [[nodiscard]] static std::filesystem::path default_path(const std::filesystem::path& agent_dir);

    /// The key of one server's state: `mcp__<server>|<url>` (pi `storeKeys`).
    [[nodiscard]] static std::string store_key(std::string_view name, std::string_view server_url);

    explicit McpAuthStore(std::filesystem::path file);

    /// The server's state, taking over the legacy bare-URL key on first load
    /// (pi `load`: "the first server to load legacy state takes it over").
    /// `std::nullopt` when neither key is present.
    [[nodiscard]] support::Expected<std::optional<McpOAuthState>> load(
            std::string_view name, std::string_view server_url);

    /// Write the server's state under its `mcp__<server>|<url>` key.
    [[nodiscard]] support::ExpectedVoid save(
            std::string_view name, std::string_view server_url, const McpOAuthState& state);

    /// The stored tokens, for noticing another process's sign-in. Does not take
    /// over legacy state (pi `tokens`).
    [[nodiscard]] support::Expected<std::optional<McpOAuthTokens>> tokens(
            std::string_view name, std::string_view server_url);

    /// Delete the server's credentials, whether they sit under the namespaced
    /// key or the legacy bare-URL key. Returns false when none were stored
    /// (pi `remove`).
    [[nodiscard]] support::Expected<bool> remove(std::string_view name, std::string_view server_url);

    /// Migrate a `#875`-era `mcp__<server>` OAuth record out of `auth.json`
    /// into this store: convert it to a `McpOAuthState` under the namespaced
    /// key and remove the old record. Returns false when `auth.json` holds no
    /// matching OAuth record. A missing or non-object `auth.json` is not an
    /// error.
    [[nodiscard]] support::Expected<bool> migrate_from_auth_json(
            const std::filesystem::path& auth_json, std::string_view name, std::string_view server_url);

private:
    std::filesystem::path file_;
};

} // namespace cch::coding_agent::mcp
