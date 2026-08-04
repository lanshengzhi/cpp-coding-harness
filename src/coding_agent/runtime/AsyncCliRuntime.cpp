#include "coding_agent/runtime/AsyncCliRuntime.hpp"

#include "cli/InitialPrompt.hpp"
#include "cli/JsonCliRenderer.hpp"
#include "cli/OneShotCliFrontend.hpp"
#include "cli/TextCliRenderer.hpp"
#include "coding_agent/AgentSessionBridge.hpp"
#include "coding_agent/runtime/RpcMode.hpp"
#include "coding_agent/tui/InteractiveMode.hpp"
#include <cch/coding_agent/AgentConfigDir.hpp>
#include <cch/tui/ProcessTerminal.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>

#include <exception>
#include <future>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <variant>

namespace cch::cli {
namespace {

[[nodiscard]] util::Expected<std::string> read_piped_input(bool stdin_is_terminal) {
    if (stdin_is_terminal) return std::string{};

    std::ostringstream input;
    input << std::cin.rdbuf();
    if (std::cin.bad()) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Unknown,
            "could not read piped stdin"));
    }
    return input.str();
}

void print_creation_error(
    const CliConfig& config,
    const util::Error& error) {
    std::cerr << (std::holds_alternative<coding_agent::ExplicitResumeSessionTarget>(config.session_target)
                      ? "could not resume session: "
                      : "could not create session: ")
              << error.message;
    if (!error.detail.empty() && error.detail != error.message) {
        std::cerr << ": " << error.detail;
    }
    std::cerr << '\n';
    if (error.context && !error.context->empty()) {
        std::cerr << "note: " << *error.context << '\n';
    }
}

void print_session_diagnostics(const std::vector<coding_agent::SdkDiagnostic>& diagnostics) {
    for (const auto& diag : diagnostics) {
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
}

[[nodiscard]] int run_native_tui(
    coding_agent::AgentSession& session,
    PreparedInitialPrompt initial_prompt) {
    cch::tui::ProcessTerminal terminal;
    boost::asio::io_context io;
    auto future = boost::asio::co_spawn(
        io,
        coding_agent::tui::run_interactive_mode(
            session,
            terminal,
            coding_agent::tui::InteractiveModeConfig{
                .agent_config_directory = coding_agent::agent_config_dir(),
                .initial_prompt = initial_prompt.text.empty()
                    ? std::nullopt
                    : std::optional<std::string>{std::move(initial_prompt.text)},
                .initial_prompt_options = coding_agent::PromptOptions{
                    .expand_prompt_templates = true,
                    .images = std::move(initial_prompt.images),
                },
            }),
        boost::asio::use_future);
    io.run();

    try {
        if (auto result = future.get(); !result) {
            std::cerr << "Native TUI failed: " << result.error().message;
            if (!result.error().detail.empty() &&
                result.error().detail != result.error().message) {
                std::cerr << ": " << result.error().detail;
            }
            std::cerr << '\n';
            return 2;
        }
    } catch (const std::exception& error) {
        std::cerr << "Native TUI failed: " << error.what() << '\n';
        return 2;
    } catch (...) {
        std::cerr << "Native TUI failed: unknown exception\n";
        return 2;
    }
    return 0;
}

} // namespace

[[nodiscard]] int run_async_cli(
    const CliConfig& config,
    Frontend frontend,
    FrontendEnvironment environment) {
    auto initial_prompt = prepare_initial_prompt(
        config.prompt,
        config.file_arguments,
        config.workspace);
    if (!initial_prompt) {
        std::cerr << "could not prepare initial prompt: "
                  << initial_prompt.error().message;
        if (!initial_prompt.error().detail.empty() &&
            initial_prompt.error().detail != initial_prompt.error().message) {
            std::cerr << ": " << initial_prompt.error().detail;
        }
        std::cerr << '\n';
        return 2;
    }

    if (frontend != Frontend::Rpc) {
        if (auto piped_input = read_piped_input(environment.stdin_is_terminal);
            !piped_input) {
            std::cerr << piped_input.error().message << '\n';
            return 2;
        } else {
            initial_prompt->text.insert(0, std::move(*piped_input));
        }
    }
    if ((frontend == Frontend::Print || frontend == Frontend::Json) &&
        initial_prompt->text.empty() && initial_prompt->images.empty()) {
        std::cerr << "prompt is required for non-interactive output\n";
        return 2;
    }

    coding_agent::runtime::AgentSessionCreationRequest request;
    request.fake = config.fake;
    request.enable_bash = config.enable_bash;
    // The interactive Native TUI always receives its independent User Shell
    // (ADR 0026); one-shot, JSON, and RPC frontends keep ordinary-prompt
    // semantics for leading '!' text.
    request.provide_user_shell = frontend == Frontend::NativeTui;
    request.project_trust_override = config.project_trust_override;
    request.disable_project_skills = config.no_skills;
    request.disable_prompt_templates = config.no_prompt_templates;
    request.prompt_template_paths = config.prompt_template_paths;
    request.workspace_explicit = config.workspace_explicit;
    request.max_turns = config.max_turns;
    request.workspace = config.workspace;
    request.session_target = config.session_target;
    request.session_dir = config.session_dir;
    request.provider = config.provider;
    request.model = config.model;
    request.models = config.models;
    request.api_key = config.api_key;

    auto created = coding_agent::create_agent_session(std::move(request));
    if (!created) {
        print_creation_error(config, created.error());
        return 2;
    }
    print_session_diagnostics(created->diagnostics);

    auto& session = *created->session;
    if (frontend == Frontend::Rpc) {
        return coding_agent::runtime::run_rpc_mode(coding_agent::runtime::RpcModeConfig{
            std::cin,
            std::cout,
            session,
            created->provider,
            created->model,
            created->workspace,
        });
    }
    if (frontend == Frontend::NativeTui) {
        return run_native_tui(session, std::move(*initial_prompt));
    }

    OneShotCliFrontendConfig frontend_config{
        .output = std::cout,
        .error = std::cerr,
        .prompt = std::move(initial_prompt->text),
    };
    auto run_frontend = [&](CliRenderer& renderer) {
        OneShotCliFrontend one_shot{
            session,
            renderer,
            created->metadata,
            std::move(frontend_config),
            coding_agent::PromptOptions{
                .expand_prompt_templates = true,
                .images = std::move(initial_prompt->images),
            }};
        return one_shot_exit_code_for(one_shot.run());
    };

    if (frontend == Frontend::Json) {
        JsonCliRenderer renderer{std::cout, std::cerr};
        return run_frontend(renderer);
    }

    TextCliRenderer renderer{
        std::cout,
        std::cerr,
        environment.stdin_is_terminal && environment.stdout_is_terminal};
    return run_frontend(renderer);
}

} // namespace cch::cli
