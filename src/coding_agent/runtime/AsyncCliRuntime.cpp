#include "coding_agent/runtime/AsyncCliRuntime.hpp"

#include "../include/cch/coding_agent/Config.hpp"
#include "coding_agent/AgentSessionBridge.hpp"
#include "coding_agent/CommandRegistry.hpp"
#include "coding_agent/runtime/EventPrinter.hpp"
#include "coding_agent/runtime/JsonEventPrinter.hpp"
#include "coding_agent/runtime/RpcMode.hpp"
#include "coding_agent/prompt/SlashCommandParser.hpp"

#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace cch::cli {
namespace {

[[nodiscard]] bool is_json_mode(OutputMode mode) {
    return mode == OutputMode::Json;
}

[[nodiscard]] bool is_rpc_mode(OutputMode mode) {
    return mode == OutputMode::Rpc;
}

[[nodiscard]] std::optional<coding_agent::CommandResult> dispatch_text_cli_command(
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
            .shutdown_requested = false,
        };
    }
}

[[nodiscard]] bool is_user_bash(std::string_view input) {
    return !input.empty() && input.front() == '!';
}

bool clear_text_terminal() {
    std::cout << "\033[2J\033[H";
    std::cout.flush();
    return static_cast<bool>(std::cout);
}

/// Print a text-mode error message for a failed prompt.
void present_text_error(std::string_view message) {
    std::cerr << "loop failed: " << message << '\n';
}

/// Print a text-mode command result to stdout.
void present_text_command(std::string_view display_text) {
    if (!display_text.empty()) {
        std::cout << display_text << '\n';
    }
}

/// Subscribe once to AgentSession events for frontend rendering.
[[nodiscard]] util::Expected<coding_agent::EventSubscription> subscribe_text_events(
    coding_agent::AgentSession& session) {
    return session.subscribe(
        [](const agent::AgentLifecycleEvent& event) -> util::ExpectedVoid {
            coding_agent::runtime::print_agent_event(event, std::cout);
            return {};
        });
}

[[nodiscard]] util::Expected<coding_agent::EventSubscription> subscribe_json_events(
    coding_agent::AgentSession& session,
    coding_agent::runtime::JsonEventPrinter& printer) {
    return session.subscribe(
        [&printer](const agent::AgentLifecycleEvent& event) -> util::ExpectedVoid {
            return printer.print_agent_event(event);
        });
}

} // namespace

int run_async_cli(const AsyncCliRuntimeConfig& config) {
    const auto json_mode = is_json_mode(config.output_mode);
    const std::string config_path = coding_agent::ConfigLoader::default_config_path();
    auto config_data = coding_agent::ConfigLoader::load(config_path);
    if (!config_data) {
        std::cerr << "warning: could not load config: " << config_data.error().message << '\n';
        config_data = coding_agent::ConfigData{};
    }

    coding_agent::CommandRegistry cli_command_registry;
    if (auto registered = register_builtin_commands(cli_command_registry); !registered) {
        std::cerr << "could not register built-in command: " << registered.error().message << '\n';
        return 1;
    }

    coding_agent::runtime::AgentSessionCreationRequest request;
    request.fake = config.fake;
    request.enable_bash = config.enable_bash;
    request.project_trust_override = config.project_trust_override;
    request.disable_project_skills = config.disable_project_skills;
    request.disable_prompt_templates = config.disable_prompt_templates;
    request.prompt_template_paths = config.prompt_template_paths;
    request.workspace_explicit = config.workspace_explicit;
    request.max_turns = config.max_turns;
    request.workspace = config.workspace;
    request.session_path = config.session_path;
    request.resume_path = config.resume_path;
    request.provider_overrides = config.provider_overrides;
    request.config = *config_data;

    auto created = coding_agent::create_agent_session(std::move(request));
    if (!created) {
        if (created.error().message == "resume workspace does not match session metadata") {
            std::cerr << created.error().detail << '\n';
        } else if (!config.resume_path.empty()) {
            std::cerr << "could not resume session: " << created.error().message << ": " << created.error().detail << '\n';
        } else {
            std::cerr << "could not create session: " << created.error().message << ": " << created.error().detail << '\n';
        }
        return 2;
    }

    auto& session = *created->session;
    const auto workspace = created->workspace;
    const auto resolved_provider = created->provider;
    const auto resolved_model = created->model;

    std::optional<coding_agent::runtime::JsonEventPrinter> json_printer;
    if (json_mode) {
        json_printer.emplace(std::cout);
        if (auto printed = json_printer->print_session_header(created->metadata); !printed) {
            std::cerr << "event printer failed: " << printed.error().message << '\n';
            return 2;
        }
    }

    for (const auto& diag : created->diagnostics) {
        const char* severity = "info";
        switch (diag.severity) {
        case coding_agent::SdkDiagnostic::Severity::Info:
            severity = "info";
            break;
        case coding_agent::SdkDiagnostic::Severity::Warning:
            severity = "warn";
            break;
        case coding_agent::SdkDiagnostic::Severity::Error:
            severity = "error";
            break;
        }
        std::string category = "session";
        std::string code = diag.code;
        if (const auto split = code.find(':'); split != std::string::npos) {
            category = code.substr(0, split);
            code = code.substr(split + 1);
        }
        std::cerr << '[' << category << ':' << severity << "] " << code << ": " << diag.message;
        if (diag.path) {
            std::cerr << " (" << *diag.path << ')';
        }
        std::cerr << '\n';
    }

    if (is_rpc_mode(config.output_mode)) {
        return coding_agent::runtime::run_rpc_mode(coding_agent::runtime::RpcModeConfig{
            std::cin,
            std::cout,
            session,
            resolved_provider,
            resolved_model,
            workspace,
        });
    }

    // Both frontends consume the same persistent AgentSession event stream.
    std::optional<coding_agent::EventSubscription> event_subscription;
    auto subscribed = json_mode
        ? subscribe_json_events(session, *json_printer)
        : subscribe_text_events(session);
    if (!subscribed) {
        std::cerr << "could not subscribe event renderer: "
                  << subscribed.error().message << '\n';
        return 2;
    }
    event_subscription.emplace(std::move(*subscribed));

    auto make_command_context = [&]() {
        return coding_agent::CommandContext{
            .session_id = session.session_id(),
            .workspace_path = session.workspace().string(),
            .provider = session.provider(),
            .model = session.model(),
            .message_count = session.message_count(),
            .available_commands = {},
        };
    };

    // ── Prompt loop ───────────────────────────────────────────────────────
    // Returns 0 to continue, 1 for error, 2 for shutdown.
    auto run_prompt = [&](const std::string& prompt) -> int {
        // Frontend commands are resolved by the CLI adapter before ordinary
        // input reaches AgentSession.
        if (auto command_result = dispatch_text_cli_command(
                cli_command_registry, prompt, make_command_context())) {
            if (json_mode) {
                // Frontend command outcomes are not AgentSession events. JSON
                // mode therefore emits no synthetic protocol record for them.
            } else if (prompt == "/clear") {
                if (!clear_text_terminal()) {
                    std::cerr << "failed to clear terminal\n";
                    return 1;
                }
            } else {
                present_text_command(command_result->display_text);
            }
            return command_result->shutdown_requested ? 2 : 0;
        }

        // Unmatched slash input reaches AgentSession via ordinary prompt.
        auto prompt_result = session.prompt(prompt);

        std::cout.flush();

        if (!prompt_result) {
            present_text_error(prompt_result.error().message);
            return 1;
        }

        // Responses and tool activity have already been rendered by the
        // persistent subscription; do not present session state a second time.
        return 0;
    };

    if (config.repl) {
        std::string line;
        while (std::cout << "> " && std::getline(std::cin, line)) {
            if (line == "exit" || line == "quit") break;
            if (line.empty()) continue;
            if (is_user_bash(line)) {
                std::cout << "Shell passthrough (!) is not yet implemented.\n";
                continue;
            }

            int rc = run_prompt(line);
            if (rc == 1) return 1;
            if (rc == 2) return 0;
        }
        return 0;
    }

    int rc = run_prompt(config.prompt);
    if (rc == 1) return 1;
    return 0;
}

} // namespace cch::cli
