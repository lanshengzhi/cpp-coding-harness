#include "coding_agent/runtime/AsyncCliRuntime.hpp"

#include "cli/CliParse.hpp"
#include "cli/InitialPrompt.hpp"
#include "cli/OneShotCliFrontend.hpp"
#include "cli/TextCliRenderer.hpp"
#include "coding_agent/AgentSession.hpp"
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

[[nodiscard]] util::Expected<std::string> read_piped_input(
    std::istream& input,
    bool stdin_is_terminal) {
    if (stdin_is_terminal) return std::string{};

    std::ostringstream collected;
    collected << input.rdbuf();
    if (input.bad()) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Unknown,
            "could not read piped stdin"));
    }
    return collected.str();
}

void print_creation_error(
    const CliConfig& config,
    std::ostream& error_stream,
    const util::Error& error) {
    error_stream << (std::holds_alternative<coding_agent::ExplicitResumeSessionTarget>(config.session_target)
                         ? "could not resume session: "
                         : "could not create session: ")
                 << error.message;
    if (!error.detail.empty() && error.detail != error.message) {
        error_stream << ": " << error.detail;
    }
    error_stream << '\n';
    if (error.context && !error.context->empty()) {
        error_stream << "note: " << *error.context << '\n';
    }
}

void print_session_diagnostics(
    std::ostream& error_stream,
    const std::vector<coding_agent::SessionDiagnostic>& diagnostics) {
    for (const auto& diag : diagnostics) {
        const char* severity = "info";
        switch (diag.severity) {
        case coding_agent::SessionDiagnostic::Severity::Info:
            severity = "info";
            break;
        case coding_agent::SessionDiagnostic::Severity::Warning:
            severity = "warn";
            break;
        case coding_agent::SessionDiagnostic::Severity::Error:
            severity = "error";
            break;
        }
        std::string category = "session";
        std::string code = diag.code;
        if (const auto split = code.find(':'); split != std::string::npos) {
            category = code.substr(0, split);
            code = code.substr(split + 1);
        }
        error_stream << '[' << category << ':' << severity << "] " << code << ": " << diag.message;
        if (diag.path) {
            error_stream << " (" << *diag.path << ')';
        }
        error_stream << '\n';
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
    CliStreams streams,
    FrontendEnvironment environment,
    std::shared_ptr<ai::Models> models) {
    auto initial_prompt = prepare_initial_prompt(
        config.prompt,
        config.file_arguments,
        config.workspace);
    if (!initial_prompt) {
        streams.error << "could not prepare initial prompt: "
                      << initial_prompt.error().message;
        if (!initial_prompt.error().detail.empty() &&
            initial_prompt.error().detail != initial_prompt.error().message) {
            streams.error << ": " << initial_prompt.error().detail;
        }
        streams.error << '\n';
        return 2;
    }

    if (auto piped_input = read_piped_input(
            streams.input, environment.stdin_is_terminal);
        !piped_input) {
        streams.error << piped_input.error().message << '\n';
        return 2;
    } else {
        initial_prompt->text.insert(0, std::move(*piped_input));
    }
    if (frontend == Frontend::Print &&
        initial_prompt->text.empty() && initial_prompt->images.empty()) {
        streams.error << "prompt is required for non-interactive output\n";
        return 2;
    }

    coding_agent::runtime::AgentSessionCreationRequest request;
    // The interactive Native TUI always receives its independent User Shell
    // (ADR 0026); the one-shot print path keeps ordinary-prompt semantics
    // for leading '!' text.
    request.provide_user_shell = frontend == Frontend::Interactive;
    request.project_trust_override = config.project_trust_override;
    request.disable_project_skills = config.no_skills;
    request.disable_prompt_templates = config.no_prompt_templates;
    request.prompt_template_paths = config.prompt_template_paths;
    request.workspace = config.workspace;
    request.session_target = config.session_target;
    request.session_dir = config.session_dir;
    request.provider = config.provider;
    request.model = config.model;
    request.models = config.models;
    request.api_key = config.api_key;

    auto created = models
        ? coding_agent::create_agent_session_for_testing(
              std::move(request), std::move(models))
        : coding_agent::create_agent_session(std::move(request));
    if (!created) {
        print_creation_error(config, streams.error, created.error());
        return 2;
    }
    print_session_diagnostics(streams.error, created->diagnostics);

    auto& session = *created->session;
    if (frontend == Frontend::Interactive) {
        return run_native_tui(session, std::move(*initial_prompt));
    }

    OneShotCliFrontendConfig frontend_config{
        .output = streams.output,
        .error = streams.error,
        .prompt = std::move(initial_prompt->text),
    };
    TextCliRenderer renderer{
        streams.output,
        streams.error,
        environment.stdin_is_terminal && environment.stdout_is_terminal};
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
}

[[nodiscard]] int run_cli_entry(
    int argc,
    char** argv,
    CliStreams streams,
    CliRuntimeOptions options) {
    auto parsed = parse_args(argc, argv);
    if (!parsed) {
        // Parse errors carry the full help text in detail.
        const auto& error = parsed.error();
        streams.error << (error.detail.empty() ? error.message : error.detail) << '\n';
        return 2;
    }
    CliConfig config = std::move(*parsed);
    if (config.help) {
        streams.output << config.help_text;
        return 0;
    }
    if (config.version) {
        streams.output << project_version() << '\n';
        return 0;
    }

    const FrontendEnvironment environment = options.environment_explicit
        ? options.environment
        : detect_frontend_environment();
    if (auto frontend = select_frontend(config, environment); !frontend) {
        const auto& error = frontend.error();
        streams.error << error.message;
        if (!error.detail.empty() && error.detail != error.message) {
            streams.error << ": " << error.detail;
        }
        streams.error << '\n';
        return 2;
    } else {
        return run_async_cli(
            config, *frontend, streams, environment, std::move(options.models));
    }
}

} // namespace cch::cli
