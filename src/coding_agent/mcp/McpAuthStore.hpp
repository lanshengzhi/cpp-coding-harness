#pragma once

// pi's MCP OAuth credential store (`mcp-auth.json`), spec #882 ticket #884. pi
// source at `7c10bd43` (v1.0.4): `packages/coding-agent/src/extensions/mcp/
// oauth.ts` (`McpOAuthCredentialStore`, `storeKeys`) and
// `packages/mcp/src/oauth/provider.ts` (`OAuthState`). The file lives at
// `<agentDir>/mcp-auth.json`, keys are `mcp__<server>|<url>`, and the legacy
// bare-URL key written by older versions is taken over on load. This replaces
// the `auth.json`-backed store of #875: existing `mcp__<server>` OAuth records
// are migrated in.

#include <cch/mcp/OAuthState.hpp>

#include <cch/support/Error.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cch::coding_agent::mcp {

/// The per-server OAuth state in `<agentDir>/mcp-auth.json`.
class McpAuthStore final : public cch::mcp::OAuthStateStore {
public:
    /// The store path pi uses: `<agent_dir>/mcp-auth.json`.
    [[nodiscard]] static std::filesystem::path default_path(const std::filesystem::path& agent_dir);

    /// The key of one server's state: `mcp__<server>|<url>` (pi `storeKeys`).
    [[nodiscard]] static std::string store_key(std::string_view name, std::string_view server_url);

    explicit McpAuthStore(std::filesystem::path file);

    /// The server's state, taking over the legacy bare-URL key on first load
    /// (pi `load`: "the first server to load legacy state takes it over").
    /// `std::nullopt` when neither key is present.
    [[nodiscard]] support::Expected<std::optional<cch::mcp::OAuthState>> load(
            std::string_view name, std::string_view server_url) override;

    /// Write the server's state under its `mcp__<server>|<url>` key.
    [[nodiscard]] support::ExpectedVoid save(
            std::string_view name, std::string_view server_url, const cch::mcp::OAuthState& state) override;

    /// The stored tokens, for noticing another process's sign-in. Does not take
    /// over legacy state (pi `tokens`).
    [[nodiscard]] support::Expected<std::optional<cch::mcp::OAuthTokens>> tokens(
            std::string_view name, std::string_view server_url);

    /// Delete the server's credentials, whether they sit under the namespaced
    /// key or the legacy bare-URL key. Returns false when none were stored
    /// (pi `remove`).
    [[nodiscard]] support::Expected<bool> remove(std::string_view name, std::string_view server_url);

    /// Migrate a `#875`-era `mcp__<server>` OAuth record out of `auth.json`
    /// into this store: convert it to a `OAuthState` under the namespaced
    /// key and remove the old record. Returns false when `auth.json` holds no
    /// matching OAuth record. A missing or non-object `auth.json` is not an
    /// error.
    [[nodiscard]] support::Expected<bool> migrate_from_auth_json(
            const std::filesystem::path& auth_json, std::string_view name, std::string_view server_url);

private:
    std::filesystem::path file_;
};

} // namespace cch::coding_agent::mcp
