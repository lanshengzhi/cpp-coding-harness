#pragma once

// pi `McpServerConfigBase` (spec #882, ticket #884): the fields every MCP
// server entry carries regardless of transport. pi source at `7c10bd43`
// (v1.0.4): `packages/coding-agent/src/core/mcp-servers.ts`.

#include "coding_agent/mcp/McpExposure.hpp"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cch::coding_agent::mcp {

/// The transport-independent half of one `mcpServers` entry. pi keeps `enabled`
/// here too; Pike carries `enabled` on `McpConfigEntry`, which is where the
/// assembly reads it, so it is not repeated here.
struct McpServerConfigBase {
    /// pi `exposure` (default `codemode`). `std::nullopt` means the default.
    std::optional<McpExposure> exposure;
    /// pi `description`: what the server offers, in a sentence. The
    /// `mcp_servers` system prompt section lists the server with it.
    std::optional<std::string> description;
    /// pi `toolExposure`: per-tool exposure overrides, in declaration order. An
    /// exact tool name wins over patterns; among patterns the first match wins.
    std::vector<std::pair<std::string, McpExposure>> tool_exposure;
    /// pi `timeout` in seconds (default 60). `std::nullopt` means the default.
    std::optional<double> timeout;
};

} // namespace cch::coding_agent::mcp
