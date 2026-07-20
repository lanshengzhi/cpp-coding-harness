#pragma once

#include "coding_agent/CommandRegistry.hpp"

#include "../../include/cch/harness/session/SessionEntry.hpp"

#include <iosfwd>
#include <string>

namespace cch::coding_agent {
class AgentSession;
}

namespace cch::cli {

class CliRenderer;

/// Control-flow configuration for one interactive CLI run: a REPL on the
/// input stream, or one supplied prompt when `repl` is false.
struct InteractiveCliFrontendConfig {
    std::istream& input;
    std::ostream& output;
    std::ostream& error;
    bool repl{false};
    std::string prompt;
};

/// The interactive CLI frontend adapter: owns the prompt/REPL control flow,
/// frontend command dispatch, event subscription, and exit codes for the
/// text and direct-JSON modes. Presentation lives behind CliRenderer.
class InteractiveCliFrontend final {
public:
    InteractiveCliFrontend(
        coding_agent::AgentSession& session,
        CliRenderer& renderer,
        const harness::session::SessionMetadata& session_metadata,
        InteractiveCliFrontendConfig config);

    /// Run to completion. 0 = success/shutdown, 1 = prompt or runtime error,
    /// 2 = frontend startup failure (the historical CLI exit codes).
    int run();

private:
    int run_prompt(
        const std::string& prompt,
        coding_agent::CommandRegistry& commands);
    [[nodiscard]] coding_agent::CommandContext make_command_context() const;

    coding_agent::AgentSession& session_;
    CliRenderer& renderer_;
    const harness::session::SessionMetadata& session_metadata_;
    InteractiveCliFrontendConfig config_;
};

} // namespace cch::cli
