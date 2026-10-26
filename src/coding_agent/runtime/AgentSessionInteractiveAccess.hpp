#pragma once

#include "coding_agent/AgentSession.hpp"
#include <cch/ai/Tool.hpp>
#include <cch/coding_agent/session/SessionStore.hpp>
#include <cch/support/Error.hpp>
#include "coding_agent/runtime/McpSessionManager.hpp"
#include "coding_agent/runtime/UserBash.hpp"

#include <boost/asio/awaitable.hpp>

#include <memory>
#include <string>
#include <vector>

namespace cch::coding_agent::detail {

/// Private Native TUI bridge to the Agent Session runtime. This keeps direct
/// editor execution out of the session handle and machine-facing protocols.
class AgentSessionInteractiveAccess {
public:
    [[nodiscard]] static bool has_user_shell(const AgentSession& session);

    /// The session's live MCP server manager (spec #882, ticket #884), or
    /// null when assembly ran without one. The `/mcp` panel host builds its
    /// presenter over this; the reference is non-owning and valid while the
    /// session lives (pi's manager dies with the session).
    [[nodiscard]] static runtime::McpSessionManager* mcp_manager(AgentSession& session);

    /// Whether the session's project scope is trusted (pi
    /// `settingsManager.isProjectTrusted()`).
    [[nodiscard]] static bool is_project_trusted(const AgentSession& session);

    /// Export values come from the live store; no writable store escapes.
    [[nodiscard]] static support::Expected<harness::session::SessionWireSnapshot> export_wire_snapshot(
            const AgentSession& session);
    [[nodiscard]] static support::Expected<std::string> export_transcript(
            const AgentSession& session, std::string destination);

    /// pi `AgentState.tools` for `/export`'s HTML lane: the active tools'
    /// definitions (name, description, parameters) in active-tool order.
    [[nodiscard]] static std::vector<ai::Tool> active_tool_definitions(const AgentSession& session);

    [[nodiscard]] static boost::asio::awaitable<support::Expected<runtime::UserBashCompletion>>
    run_user_bash(
        AgentSession& session,
        std::string command,
        bool exclude_from_context,
        runtime::UserBashProgressSink progress_sink);

    static void cancel_user_bash(AgentSession& session);

private:
    [[nodiscard]] static boost::asio::awaitable<support::Expected<runtime::UserBashCompletion>>
    run_user_bash_impl(
        std::shared_ptr<AgentSession::Impl> impl,
        std::string command,
        bool exclude_from_context,
        runtime::UserBashProgressSink progress_sink);
};

} // namespace cch::coding_agent::detail
