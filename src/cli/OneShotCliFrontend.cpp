#include "OneShotCliFrontend.hpp"

#include "CliRenderer.hpp"
#include "coding_agent/prompt/SlashCommandParser.hpp"
#include "coding_agent/AgentSession.hpp"

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

} // namespace

OneShotCliFrontend::OneShotCliFrontend(
    coding_agent::AgentSession& session,
    CliRenderer& renderer,
    const harness::session::SessionMetadata& session_metadata,
    OneShotCliFrontendConfig config,
    coding_agent::PromptOptions initial_prompt_options)
    : session_(session),
      renderer_(renderer),
      session_metadata_(session_metadata),
      config_(std::move(config)),
      initial_prompt_options_(std::move(initial_prompt_options)) {}

OneShotCliOutcome OneShotCliFrontend::run() {
    coding_agent::CommandRegistry commands;
    if (auto registered = coding_agent::register_builtin_commands(commands); !registered) {
        config_.error << "could not register built-in command: "
                      << registered.error().message << '\n';
        return OneShotCliOutcome::RuntimeError;
    }

    if (auto started = renderer_.on_session_start(session_metadata_); !started) {
        config_.error << "event printer failed: " << started.error().message << '\n';
        return OneShotCliOutcome::StartupFailure;
    }

    auto subscribed = session_.subscribe(
        [this](const agent::AgentLifecycleEvent& event) -> util::ExpectedVoid {
            return renderer_.on_event(event);
        });
    if (!subscribed) {
        config_.error << "could not subscribe event renderer: "
                      << subscribed.error().message << '\n';
        return OneShotCliOutcome::StartupFailure;
    }
    coding_agent::EventSubscription event_subscription{std::move(*subscribed)};

    const auto outcome = run_prompt(
        config_.prompt,
        commands,
        std::move(initial_prompt_options_));
    return outcome == OneShotCliOutcome::RuntimeError ? OneShotCliOutcome::RuntimeError
                                                          : OneShotCliOutcome::Success;
}

OneShotCliOutcome OneShotCliFrontend::run_prompt(
    const std::string& prompt,
    coding_agent::CommandRegistry& commands,
    coding_agent::PromptOptions options) {
    // Frontend commands are resolved by the CLI adapter before ordinary
    // input reaches AgentSession.
    if (auto command_result = dispatch_cli_command(commands, prompt, make_command_context())) {
        if (auto presented = renderer_.on_command_result(prompt, command_result->display_text);
            !presented) {
            config_.error << presented.error().message << '\n';
            return OneShotCliOutcome::RuntimeError;
        }
        return command_result->effect == coding_agent::CommandEffect::Shutdown
            ? OneShotCliOutcome::ShutdownRequested
            : OneShotCliOutcome::Success;
    }

    // Unmatched slash input reaches AgentSession via ordinary prompt.
    auto prompt_result = session_.prompt_blocking(prompt, std::move(options));

    config_.output.flush();

    if (!prompt_result) {
        renderer_.on_prompt_error(prompt_result.error().message);
        return OneShotCliOutcome::RuntimeError;
    }

    // Responses and tool activity have already been rendered by the
    // persistent subscription; do not present session state a second time.
    return OneShotCliOutcome::Success;
}

coding_agent::CommandContext OneShotCliFrontend::make_command_context() const {
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
