#pragma once

#include "coding_agent/mcp/McpHttpServerConfig.hpp"
#include "coding_agent/mcp/McpServerConfigBase.hpp"
#include "coding_agent/mcp/McpStdioServerConfig.hpp"

#include <cch/support/Error.hpp>
#include <cch/support/JsonValue.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace cch::coding_agent::mcp {

/// The project-scope configuration directory (pi `CONFIG_DIR_NAME`), so the
/// project MCP configuration is `<cwd>/.pi/mcp.json`.
inline constexpr std::string_view kMcpProjectConfigDir = ".pi";

/// One server loaded from a global or project `mcp.json` (pi `McpServerEntry`
/// narrowed to this slice). The effective configuration is a stdio or
/// streamable-http descriptor plus the pi `enabled` flag.
///
/// The descriptor is the stdio-or-http sum type behind a named alias (§4.2), so
/// dispatch sites can name the variant rather than respell its alternatives.
using McpServerConfigVariant = std::variant<McpStdioServerConfig, McpHttpServerConfig>;

struct McpConfigEntry {
    /// pi server name: the namespace of the server's tools
    /// (`mcp__<name>__<tool>`).
    std::string name;
    /// pi `enabled` (default true). A disabled entry is `Stopped`: it keeps its
    /// place in the configured list but is not connected and its tools are
    /// absent from the session.
    bool enabled{true};
    /// The `mcp.json` that defined the entry (the global file when a project
    /// entry only overrides `enabled`).
    std::filesystem::path source;
    /// The project `mcp.json` whose override keys (`enabled`/`exposure`/
    /// `toolExposure`) applied to this entry (pi `McpServerEntry.override`),
    /// or `std::nullopt` for a plain entry.
    std::optional<std::filesystem::path> override;
    /// The server descriptor, chosen by pi's `type`/`command`/`url` shape.
    McpServerConfigVariant config;
};

/// The merged global + (trusted-)project MCP configuration (pi
/// `LoadedMcpConfig`).
struct McpConfigLoad {
    /// Configured servers in file order, project entries replacing global
    /// entries with the same name.
    std::vector<McpConfigEntry> servers;
    /// pi `autoEnableCodemode` (default true): activate the codemode tool when
    /// a `codemode` server connects. A project value overrides the global one.
    bool auto_enable_codemode{true};
    /// Per-file read, parse, and validation failures, each `"<path>: <message>"`.
    /// A rejected entry is reported here and skipped, so a single bad entry
    /// never drops the working ones.
    std::vector<std::string> errors;
    /// The project `mcp.json` when the project is trusted (pi `projectConfig`).
    std::optional<std::filesystem::path> project_config;
};

/// Validate one `mcpServers` entry like pi `validateMcpServerConfig`: the
/// name, `exposure`/`toolExposure` (aliases resolved), `description`, `timeout`,
/// and the stdio-or-http shape, returning the parsed config or pi's error
/// message verbatim. `raw` is the entry value from `mcpServers`.
[[nodiscard]] support::Expected<McpServerConfigVariant> validate_mcp_server_config(
        std::string_view name, const support::JsonValue& raw);

/// Load the MCP server configuration (pi `loadMcpConfig`): the global
/// `<agent_dir>/mcp.json` always, and the project `<cwd>/.pi/mcp.json` only
/// when `project_trusted`. Project entries replace global entries with the same
/// name; a project entry without `command`, `url`, or `type` overrides only the
/// global entry's `enabled` flag (pi's `isOverride`), so a repository can turn
/// a server off without carrying its credentials. A missing file is empty
/// configuration; every read, parse, or validation failure lands in `errors`
/// with its file path, never as a silent drop. The streamable-http URL is not
/// validated here — assembly applies the TLS-only gate
/// (`validate_mcp_http_server_config`, ADR 0054) at registration.
[[nodiscard]] McpConfigLoad load_mcp_config(
        const std::filesystem::path& agent_dir, const std::filesystem::path& cwd, bool project_trusted);

} // namespace cch::coding_agent::mcp
