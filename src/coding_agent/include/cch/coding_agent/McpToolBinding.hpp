#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace cch::coding_agent {

/// The longest Qualified Tool Name the MCP Host ever registers, in characters
/// (spec #833 naming decision). A name that would be longer keeps a
/// deterministic hash suffix instead of its tail, so two different Upstream
/// tools never collapse onto one registered name by truncation alone.
///
/// This is a containment bound on a name a third party chose, recorded in
/// `docs/runtime-capacities.md`.
inline constexpr std::size_t kMcpQualifiedToolNameMaxLength{64};

/// The Qualified Tool Name of one Upstream tool: `mcp__<Server Id>__<tool>`,
/// sanitized to `[a-zA-Z0-9_-]` and truncated with a deterministic hash
/// suffix when it would exceed `kMcpQualifiedToolNameMaxLength` (ADR 0065,
/// `CONTEXT.md`).
///
/// Sanitizing is lossy and truncating is lossy, so this name is an *identity*,
/// not a parse target: the reverse mapping back to the Upstream's own Server Id
/// and tool name is held by the session, and every consumer that needs the
/// original names reads it from there rather than decoding the name.
[[nodiscard]] std::string mcp_qualified_tool_name(std::string_view server_id, std::string_view tool_name);

/// One published Upstream tool, in the form this Owner projects it (issue
/// #842). The row is the reverse mapping the display surface needs: the
/// Qualified Tool Name the model calls, beside the Server Id and the Upstream's
/// own tool name the call actually targets, so schema, execution target, and
/// display target cannot drift apart.
struct McpPublishedTool {
    /// `mcp__<Server Id>__<tool>` — the name in the model's tool payload.
    std::string qualified_name{};
    /// Server Id of the Upstream MCP Server that advertised the tool.
    std::string server_id{};
    /// The Upstream's own tool name, as its `tools/list` entry spelled it.
    std::string tool_name{};
    /// The Upstream's one-line description, or empty when it offered none.
    std::string description{};
};

} // namespace cch::coding_agent
