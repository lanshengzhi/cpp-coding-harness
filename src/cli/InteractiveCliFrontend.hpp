#pragma once

#include "coding_agent/CommandRegistry.hpp"
#include <cch/coding_agent/Sdk.hpp>
#include "../../include/cch/harness/session/SessionEntry.hpp"

#include <iosfwd>
#include <string>

namespace cch::coding_agent {
class AgentSession;
}

namespace cch::cli {

class CliRenderer;

/// Typed outcome of the interactive CLI frontend: one meaning per enumerator
/// at both the run and prompt call sites. `exit_code_for` maps a finished run
/// to the historical CLI process exit codes.
enum class InteractiveCliOutcome {
    Success,          // clean completion (exit 0)
    RuntimeError,     // prompt or runtime error (exit 1)
    StartupFailure,   // frontend startup failure (exit 2)
    ShutdownRequested // prompt-level only: a frontend command requested shutdown (exit 0)
};

/// Historical CLI process exit code for a frontend outcome: 0 for success or
/// operator-requested shutdown, 1 for a prompt or runtime error, 2 for a
/// frontend startup failure.
[[nodiscard]] constexpr int exit_code_for(InteractiveCliOutcome outcome) {
    switch (outcome) {
    case InteractiveCliOutcome::Success:
    case InteractiveCliOutcome::ShutdownRequested:
        return 0;
    case InteractiveCliOutcome::RuntimeError:
        return 1;
    case InteractiveCliOutcome::StartupFailure:
        return 2;
    }
    return 2;
}

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
/// frontend command dispatch, event subscription, and run outcomes for the
/// text and direct-JSON modes. Presentation lives behind CliRenderer.
class InteractiveCliFrontend final {
public:
    InteractiveCliFrontend(
        coding_agent::AgentSession& session,
        CliRenderer& renderer,
        const harness::session::SessionMetadata& session_metadata,
        InteractiveCliFrontendConfig config,
        coding_agent::PromptOptions initial_prompt_options = {});

    /// Run to completion and report the typed outcome; callers convert it to
    /// a process exit code with exit_code_for (observable CLI exit behavior).
    [[nodiscard]] InteractiveCliOutcome run();

private:
    [[nodiscard]] InteractiveCliOutcome run_prompt(
        const std::string& prompt,
        coding_agent::CommandRegistry& commands,
        coding_agent::PromptOptions options = {});
    [[nodiscard]] coding_agent::CommandContext make_command_context() const;

    coding_agent::AgentSession& session_;
    CliRenderer& renderer_;
    const harness::session::SessionMetadata& session_metadata_;
    InteractiveCliFrontendConfig config_;
    coding_agent::PromptOptions initial_prompt_options_;
};

} // namespace cch::cli
