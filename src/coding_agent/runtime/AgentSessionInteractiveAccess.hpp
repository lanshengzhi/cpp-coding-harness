#pragma once

#include "coding_agent/AgentSession.hpp"
#include <cch/support/Error.hpp>
#include "coding_agent/runtime/UserBash.hpp"

#include <boost/asio/awaitable.hpp>

#include <memory>
#include <string>

namespace cch::coding_agent::detail {

/// Private Native TUI bridge to the Agent Session runtime. This keeps direct
/// editor execution out of the session handle and machine-facing protocols.
class AgentSessionInteractiveAccess {
public:
    [[nodiscard]] static bool has_user_shell(const AgentSession& session);

    /// Whether the session's project scope is trusted (pi
    /// `settingsManager.isProjectTrusted()`).
    [[nodiscard]] static bool is_project_trusted(const AgentSession& session);

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
