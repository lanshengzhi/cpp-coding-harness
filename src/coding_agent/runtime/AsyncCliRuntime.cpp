#include "coding_agent/runtime/AsyncCliRuntime.hpp"

#include "../include/cch/coding_agent/Config.hpp"
#include "coding_agent/AgentSessionBridge.hpp"
#include "coding_agent/CommandRegistry.hpp"
#include "coding_agent/runtime/EventPrinter.hpp"
#include "coding_agent/runtime/JsonEventPrinter.hpp"
#include "coding_agent/runtime/RpcMode.hpp"
#include "coding_agent/prompt/SlashCommandParser.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>

#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
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

} // namespace

void write_text_prompt_result(
    const coding_agent::PromptResult& result,
    std::ostream& output) {
    const bool has_local_feedback = result.code == "command_handled" ||
        result.code == "shutdown" || result.code == "command_handler_failed";
    if (has_local_feedback && !result.message.empty()) {
        output << result.message << '\n';
    } else if (result.code == "completed" && result.last_assistant_text) {
        output << *result.last_assistant_text << '\n';
    }
}

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

    auto present_result = [&](coding_agent::PromptResult result) -> coding_agent::PromptResult {
        if (!result.success) {
            if (json_mode) {
                if (auto printed = json_printer->print_terminal(false, result.code, result.message); !printed) {
                    std::cerr << "event printer failed: " << printed.error().message << '\n';
                }
            } else if (result.code == "session_persist_failed") {
                std::cerr << result.message << '\n';
            } else {
                std::cerr << "loop failed: " << result.message << '\n';
            }
            return result;
        }
        if (json_mode) {
            if (auto printed = json_printer->print_terminal(true, result.code, result.message); !printed) {
                coding_agent::PromptResult failed;
                failed.success = false;
                failed.code = "event_print_failed";
                failed.message = "failed to print terminal event";
                return failed;
            }
            return result;
        }
        write_text_prompt_result(result, std::cout);
        return result;
    };

    auto run_prompt = [&](const std::string& prompt) -> coding_agent::PromptResult {
        // Frontend commands are resolved by the CLI adapter before ordinary input reaches AgentSession.
        if (auto command_result = dispatch_text_cli_command(cli_command_registry, prompt, make_command_context())) {
            coding_agent::PromptResult result;
            result.success = true;
            result.code = command_result->shutdown_requested ? "shutdown" : "command_handled";
            result.message = command_result->display_text;

            if (!json_mode && prompt == "/clear") {
                if (!clear_text_terminal()) {
                    result.success = false;
                    result.code = "event_print_failed";
                    result.message = "failed to clear terminal";
                }
            }
            return present_result(result);
        }

        std::optional<boost::asio::io_context> print_io;
        std::optional<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> print_work;
        std::optional<std::jthread> print_thread;
        if (!json_mode) {
            print_io.emplace();
            print_work.emplace(boost::asio::make_work_guard(*print_io));
            print_thread.emplace([&]() { print_io->run(); });
        }

        auto prompt_result = session.prompt(
            prompt,
            coding_agent::PromptOptions{
                .event_sink = [&](const agent::AgentLifecycleEvent& event) -> util::ExpectedVoid {
                    if (json_mode) {
                        return json_printer->print_agent_event(event);
                    }
                    try {
                        boost::asio::post(*print_io, [event]() {
                            try {
                                coding_agent::runtime::print_agent_event(event, std::cout);
                            } catch (const std::exception& e) {
                                std::cerr << "event printer failed: " << e.what() << '\n';
                            }
                        });
                        return util::ExpectedVoid{};
                    } catch (const std::exception& e) {
                        return std::unexpected(util::make_error(util::ErrorCode::Tool, "event printer failed", e.what()));
                    }
                },
            });

        if (!json_mode) {
            print_work->reset();
            print_thread->join();
        }
        std::cout.flush();

        coding_agent::PromptResult result;
        if (prompt_result) {
            result = std::move(*prompt_result);
        } else {
            result.success = false;
            result.code = "runtime_error";
            result.message = prompt_result.error().message;
        }

        return present_result(result);
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

            auto result = run_prompt(line);
            if (!result.success) return 1;
            if (result.code == "shutdown") return 0;
        }
        return 0;
    }

    auto result = run_prompt(config.prompt);
    return result.success ? 0 : 1;
}

} // namespace cch::cli
