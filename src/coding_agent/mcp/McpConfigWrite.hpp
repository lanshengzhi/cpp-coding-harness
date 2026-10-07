#pragma once

// The write half of pi's `mcp.json` persistence (spec #882, ticket #884). pi
// source at `7c10bd43` (v1.0.4): `packages/coding-agent/src/extensions/mcp/
// config.ts` (`updateMcpServerConfig`, `addMcpServerConfig`,
// `removeMcpServerConfig`, `editMcpServers`). The `/mcp` manager and `pike mcp`
// CLI subcommands drive these; the loader in `McpConfigFile.hpp` is the read
// half, so the two stay in one module's file format.

#include "coding_agent/mcp/McpExposure.hpp"

#include <cch/support/Error.hpp>
#include <cch/support/JsonValue.hpp>

#include <filesystem>
#include <optional>
#include <string_view>

namespace cch::coding_agent::mcp {

/// pi `McpServerConfigPatch`: the settings `/mcp` changes. `enabled: true` and
/// `exposure: "codemode"` are the defaults, so setting them on a plain entry
/// removes the key.
struct McpServerConfigPatch {
    std::optional<bool> enabled;
    std::optional<McpExposure> exposure;
};

/// pi `updateMcpServerConfig`: change one server's `enabled`/`exposure` in the
/// `mcp.json` that defines or overrides it. With `override`, a missing entry is
/// added as an override (an entry with no `command`/`url`/`type`). An override
/// keeps its default values, since it replaces the global server's; a plain
/// entry drops a default key. Every other field and every other top-level key
/// is kept, and the file is rewritten with its own indentation.
[[nodiscard]] support::ExpectedVoid update_mcp_server_config(const std::filesystem::path& path,
        std::string_view name,
        const McpServerConfigPatch& patch,
        bool override = false);

/// pi `addMcpServerConfig`: add a server entry to an `mcp.json`, creating the
/// file when missing. An existing entry with the same name is replaced. Returns
/// true when an entry was replaced.
[[nodiscard]] support::Expected<bool> add_mcp_server_config(
        const std::filesystem::path& path, std::string_view name, const support::JsonValue& config);

/// pi `removeMcpServerConfig`: remove a server entry from an `mcp.json`.
/// Returns false when the file does not exist or does not define the server.
[[nodiscard]] support::Expected<bool> remove_mcp_server_config(
        const std::filesystem::path& path, std::string_view name);

} // namespace cch::coding_agent::mcp
