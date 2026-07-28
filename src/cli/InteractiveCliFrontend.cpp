#include "InteractiveCliFrontend.hpp"

#include "CliRenderer.hpp"

#include "../../include/cch/coding_agent/Sdk.hpp"
#include "coding_agent/prompt/SlashCommandParser.hpp"

#include <istream>
#include <optional>
#include <ostream>
#include <string_view>
#include <utility>

namespace cch::cli {
namespace {

[[nodiscard]] std::optional<coding_agent::CommandResult> dispatch_cli_command(
    coding_agent::CommandRegistry& registry,
    std::string_view input,
    const coding_agent::CommandContext& base_context) {
    const auto parsed = coding_agent::prompt::try_parse_slash_command(input);
    if (!parsed) {
        return std::nullopt;
    }

    auto context = base_context;
    context.available_commands = registry.list_commands();
    try {
        return registry.dispatch(parsed->first, context, parsed->second);
    } catch (...) {
        return coding_agent::CommandResult{
            .display_text = "Command handler failed.",
        };
    }
}

[[nodiscard]] bool is_user_bash(std::string_view input) {
    return !input.empty() && input.front() == '!';
}

} // namespace

InteractiveCliFrontend::InteractiveCliFrontend(
    coding_agent::AgentSession& session,
    CliRenderer& renderer,
    const harness::session::SessionMetadata& session_metadata,
    InteractiveCliFrontendConfig config)
    : session_(session),
      renderer_(renderer),
      session_metadata_(session_metadata),
      config_(std::move(config)) {}

InteractiveCliOutcome InteractiveCliFrontend::run() {
    coding_agent::CommandRegistry commands;
    if (auto registered = coding_agent::register_builtin_commands(commands); !registered) {
        config_.error << "could not register built-in command: "
                      << registered.error().message << '\n';
        return InteractiveCliOutcome::RuntimeError;
    }

    if (auto started = renderer_.on_session_start(session_metadata_); !started) {
        config_.error << "event printer failed: " << started.error().message << '\n';
        return InteractiveCliOutcome::StartupFailure;
    }

    auto subscribed = session_.subscribe(
        [this](const agent::AgentLifecycleEvent& event) -> util::ExpectedVoid {
            return renderer_.on_event(event);
        });
    if (!subscribed) {
        config_.error << "could not subscribe event renderer: "
                      << subscribed.error().message << '\n';
        return InteractiveCliOutcome::StartupFailure;
    }
    coding_agent::EventSubscription event_subscription{std::move(*subscribed)};

    if (config_.repl) {
        std::string line;
        while (config_.output << "> " && std::getline(config_.input, line)) {
            if (line == "exit" || line == "quit") {
                break;
            }
            if (line.empty()) {
                continue;
            }
            if (is_user_bash(line)) {
                config_.output << "Shell passthrough (!) is not yet implemented.\n";
                continue;
            }

            const auto outcome = run_prompt(line, commands);
            if (outcome == InteractiveCliOutcome::RuntimeError) {
                return InteractiveCliOutcome::RuntimeError;
            }
            if (outcome == InteractiveCliOutcome::ShutdownRequested) {
                return InteractiveCliOutcome::Success;
            }
        }
        return InteractiveCliOutcome::Success;
    }

    const auto outcome = run_prompt(config_.prompt, commands);
    return outcome == InteractiveCliOutcome::RuntimeError ? InteractiveCliOutcome::RuntimeError
                                                          : InteractiveCliOutcome::Success;
}

InteractiveCliOutcome InteractiveCliFrontend::run_prompt(
    const std::string& prompt,
    coding_agent::CommandRegistry& commands) {
    // Frontend commands are resolved by the CLI adapter before ordinary
    // input reaches AgentSession.
    if (auto command_result = dispatch_cli_command(commands, prompt, make_command_context())) {
        if (auto presented = renderer_.on_command_result(prompt, command_result->display_text);
            !presented) {
            config_.error << presented.error().message << '\n';
            return InteractiveCliOutcome::RuntimeError;
        }
        return command_result->effect == coding_agent::CommandEffect::Shutdown
            ? InteractiveCliOutcome::ShutdownRequested
            : InteractiveCliOutcome::Success;
    }

    // Unmatched slash input reaches AgentSession via ordinary prompt.
    auto prompt_result = session_.prompt_blocking(prompt);

    config_.output.flush();

    if (!prompt_result) {
        renderer_.on_prompt_error(prompt_result.error().message);
        return InteractiveCliOutcome::RuntimeError;
    }

    // Responses and tool activity have already been rendered by the
    // persistent subscription; do not present session state a second time.
    return InteractiveCliOutcome::Success;
}

coding_agent::CommandContext InteractiveCliFrontend::make_command_context() const {
    const auto& session_path = session_.session_path();
    return coding_agent::CommandContext{
        .session_id = session_.session_id(),
        .session_path = session_path
            ? std::optional<std::string>(session_path->string())
            : std::nullopt,
        .workspace_path = session_.workspace().string(),
        .provider = session_.provider(),
        .model = session_.model(),
        .message_count = session_.message_count(),
        .available_commands = {},
    };
}

} // namespace cch::cli
