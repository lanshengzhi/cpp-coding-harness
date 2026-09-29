#pragma once

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>

namespace cch::coding_agent {

/// One Upstream MCP Server's connection as this Owner projects it (issue
/// #841, spec #833 story 8). The values are a passive mirror of the
/// connection machinery's five states: `cch_mcp` owns the state machine, and
/// `frontend_tui` reads Upstream Connection Status only from this
/// projection, never from the MCP Host package (ADR 0065).
enum class McpUpstreamState {
    /// A connection attempt is in flight and the Upstream has not answered
    /// yet. It is also the state a session starts in for a trusted server.
    Pending,
    /// A request reached the Upstream and nothing has contradicted it.
    Connected,
    /// The last transport-level attempt failed, or the transport reported the
    /// connection closed. Health is inferred from request outcomes and
    /// transport closure; the 2026-07-28 revision removed `ping`, so there is
    /// no heartbeat to keep a connection healthy.
    Failed,
    /// The Upstream answered 401/403 and is waiting for a credential the
    /// session does not have.
    NeedsAuth,
    /// The user did not enable this Upstream, or enabled it no longer. A
    /// server whose first-enable consent is undecided, declined, cancelled,
    /// or unaskable is disabled rather than failed, and has made no upstream
    /// request at all.
    Disabled,
};

/// The state name as the user sees it, in the glossary's spelling.
[[nodiscard]] std::string_view to_string(McpUpstreamState state) noexcept;

/// One Upstream MCP Server's status row for the `/mcp` overview: the Server Id
/// is the sole stable identity, and every field is a bounded, redacted value
/// the connection machinery already published.
struct McpUpstreamStatus {
    /// Server Id — the `mcpServers` map key, the credential key suffix, and
    /// the namespace of every Qualified Tool Name from this server.
    std::string server_id{};
    McpUpstreamState state{McpUpstreamState::Pending};
    /// Bounded, redacted explanation of a failure or an authentication
    /// challenge; empty otherwise.
    std::string status_message{};
    /// Consecutive transport-level failures on the current reconnect
    /// ladder, and the delay before the next attempt. A non-zero delay is
    /// what a bounded, storm-protected reconnect looks like from the outside.
    std::size_t consecutive_failures{0};
    std::chrono::milliseconds next_reconnect_delay{std::chrono::milliseconds{0}};
};

} // namespace cch::coding_agent
