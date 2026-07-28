#pragma once

#include "coding_agent/CommandRegistry.hpp"
#include <cch/coding_agent/Sdk.hpp>
#include <cch/harness/session/SessionEntry.hpp>

#include <iosfwd>
#include <string>

namespace cch::coding_agent {
class AgentSession;
}

namespace cch::cli {

class CliRenderer;

/// Typed outcome of the one-shot CLI frontend: one meaning per enumerator
/// at both the run and prompt call sites. `one_shot_exit_code_for` maps a finished run
/// to the historical CLI process exit codes.
enum class OneShotCliOutcome {
    Success,          // clean completion (exit 0)
    RuntimeError,     // prompt or runtime error (exit 1)
    StartupFailure,   // frontend startup failure (exit 2)
    ShutdownRequested // prompt-level only: a frontend command requested shutdown (exit 0)
};

/// Historical CLI process exit code for a frontend outcome: 0 for success or
/// operator-requested shutdown, 1 for a prompt or runtime error, 2 for a
/// frontend startup failure.
[[nodiscard]] constexpr int one_shot_exit_code_for(OneShotCliOutcome outcome) {
    switch (outcome) {
    case OneShotCliOutcome::Success:
    case OneShotCliOutcome::ShutdownRequested:
        return 0;
    case OneShotCliOutcome::RuntimeError:
        return 1;
    case OneShotCliOutcome::StartupFailure:
        return 2;
    }
    return 2;
}

/// Control-flow configuration for one supplied prompt.
struct OneShotCliFrontendConfig {
    std::ostream& output;
    std::ostream& error;
    std::string prompt;
};

/// The one-shot CLI frontend adapter owns frontend command dispatch, event
/// subscription, and run outcomes for text and direct-JSON modes. Presentation
/// lives behind CliRenderer.
class OneShotCliFrontend final {
public:
    OneShotCliFrontend(
        coding_agent::AgentSession& session,
        CliRenderer& renderer,
        const harness::session::SessionMetadata& session_metadata,
        OneShotCliFrontendConfig config,
        coding_agent::PromptOptions initial_prompt_options = {});

    /// Run to completion and report the typed outcome; callers convert it to
    /// a process exit code with one_shot_exit_code_for.
    [[nodiscard]] OneShotCliOutcome run();

private:
    [[nodiscard]] OneShotCliOutcome run_prompt(
        const std::string& prompt,
        coding_agent::CommandRegistry& commands,
        coding_agent::PromptOptions options = {});
    [[nodiscard]] coding_agent::CommandContext make_command_context() const;

    coding_agent::AgentSession& session_;
    CliRenderer& renderer_;
    const harness::session::SessionMetadata& session_metadata_;
    OneShotCliFrontendConfig config_;
    coding_agent::PromptOptions initial_prompt_options_;
};

} // namespace cch::cli
