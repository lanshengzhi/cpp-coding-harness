#pragma once

// pi's `mcp_servers` system prompt section (spec #882, ticket #884): the list
// of enabled servers whose tools are not declared to the model, so the model
// learns which servers exist and how their tools are reached. pi source at
// `7c10bd43` (v1.0.4): `packages/coding-agent/src/extensions/mcp/index.ts`
// (`MCP_SERVERS_SECTION`, `MAX_SERVER_DESCRIPTION_CHARS`,
// `MAX_SERVERS_SECTION_CHARS`, `serversSectionIntro`, `truncate`,
// `McpServerListing`, `serverSummary`, `renderServersSection`,
// `configuredExposures`).

#include "coding_agent/mcp/McpConfigFile.hpp"
#include "coding_agent/mcp/McpExposure.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cch::coding_agent::mcp {

/// pi `MCP_SERVERS_SECTION`: the system prompt section tag.
inline constexpr std::string_view kMcpServersSection = "mcp_servers";
/// pi `MAX_SERVER_DESCRIPTION_CHARS`: characters of one server description.
inline constexpr std::size_t kMcpMaxServerDescriptionChars = 250;
/// pi `MAX_SERVERS_SECTION_CHARS`: characters of the whole section.
inline constexpr std::size_t kMcpMaxServersSectionChars = 4096;

/// pi `McpServerListing`: a configured server plus its live connection's
/// `instructions`, the summary fallback when the entry carries no description.
struct McpServerListing {
    const McpConfigEntry* entry{nullptr};
    std::optional<std::string> instructions;
};

/// pi `configuredExposures`: the exposures the server's tools can have, known
/// from its config before it connects — the entry's `exposure` plus every
/// `toolExposure` value.
[[nodiscard]] std::vector<McpExposure> mcp_configured_exposures(const McpConfigEntry& entry);

/// pi `renderServersSection`: the section body for every enabled server whose
/// tools are indirect (codemode or deferred). `std::nullopt` when there are no
/// such servers.
[[nodiscard]] std::optional<std::string> render_mcp_servers_section(std::span<const McpServerListing> servers);

} // namespace cch::coding_agent::mcp
