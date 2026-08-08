#include "coding_agent/runtime/AsyncCliRuntime.hpp"

#include "cli/CliParse.hpp"
#include "cli/InitialPrompt.hpp"
#include "cli/ListModels.hpp"
#include "cli/PrintMode.hpp"
#include "cli/SessionFamily.hpp"
#include "coding_agent/AgentSession.hpp"
#include "coding_agent/tui/InteractiveMode.hpp"
#include <cch/coding_agent/AgentConfigDir.hpp>
#include <cch/coding_agent/Settings.hpp>
#include <cch/tui/ProcessTerminal.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>

#include <algorithm>
#include <cctype>
#include <exception>
#include <filesystem>
#include <future>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <variant>

namespace cch::cli {
namespace {

/// pi `readPipedStdin`: read all of piped stdin and trim it; empty (or
/// whitespace-only) content is absent and contributes nothing to the initial
/// message merge.
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
    auto text = collected.str();
    const auto not_space = [](unsigned char character) {
        return !std::isspace(character);
    };
    text.erase(text.begin(), std::find_if(text.begin(), text.end(), not_space));
    text.erase(std::find_if(text.rbegin(), text.rend(), not_space).base(), text.end());
    return text;
}

void print_creation_error(
    bool is_resume_target,
    std::ostream& error_stream,
    const util::Error& error) {
    error_stream << (is_resume_target
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

/// pi main.ts session selection: the startup settings manager supplies the
/// sessionDir chain value used during session-family resolution, exactly like
/// pi's `startupSettingsManager.getSessionDir()`.
[[nodiscard]] std::optional<std::string> startup_settings_session_dir(
    const std::filesystem::path& workspace) {
    auto settings = coding_agent::SettingsManager::create(
        workspace, coding_agent::agent_config_dir(),
        /* project_trusted */ false);
    return settings.settings().session_dir;
}

[[nodiscard]] int run_native_tui(
    coding_agent::AgentSession& session,
    InitialMessageResult initial,
    std::optional<std::string> model_fallback_message) {
    cch::tui::ProcessTerminal terminal;
    boost::asio::io_context io;
    auto future = boost::asio::co_spawn(
        io,
        coding_agent::tui::run_interactive_mode(
            session,
            terminal,
            coding_agent::tui::InteractiveModeConfig{
                .agent_config_directory = coding_agent::agent_config_dir(),
                .initial_prompt = initial.initial_message.empty()
                    ? std::nullopt
                    : std::optional<std::string>{std::move(initial.initial_message)},
                .initial_prompt_options = coding_agent::PromptOptions{
                    .expand_prompt_templates = true,
                    .images = std::move(initial.initial_images),
                },
                .model_fallback_message = std::move(model_fallback_message),
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
            return 1;
        }
    } catch (const std::exception& error) {
        std::cerr << "Native TUI failed: " << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "Native TUI failed: unknown exception\n";
        return 1;
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
    // pi main.ts boot order: the session-family flag guards run before any
    // session machinery, the cross-project fork prompt happens during session
    // selection (before piped stdin is read), and the `--name` guard follows
    // session selection.
    if (auto guard = session_family_guard_error(config); guard) {
        streams.error << *guard << '\n';
        return 1;
    }

    auto assembly = assemble_session_target(
        config,
        startup_settings_session_dir(config.workspace),
        streams.input,
        // pi takes over stdout outside interactive mode (output-guard.ts), so
        // the cross-project notice, fork prompt, and "Aborted." line land on
        // stderr in print mode and never pollute the one-shot stdout.
        frontend == Frontend::Interactive ? streams.output : streams.error,
        streams.error);
    if (!assembly) {
        streams.error << assembly.error().message << '\n';
        return 1;
    }
    if (assembly->aborted) {
        // pi: the declined fork printed "Aborted." during session selection.
        return 0;
    }
    if (auto name_guard = session_name_guard_error(config); name_guard) {
        streams.error << *name_guard << '\n';
        return 1;
    }

    coding_agent::runtime::AgentSessionCreationRequest request;
    // The interactive Native TUI always receives its independent User Shell
    // (ADR 0026); the one-shot print path keeps ordinary-prompt semantics
    // for leading '!' text.
    const bool is_resume_target =
        std::holds_alternative<coding_agent::ExplicitResumeSessionTarget>(
            assembly->target);
    request.provide_user_shell = frontend == Frontend::Interactive;
    request.project_trust_override = config.project_trust_override;
    request.no_skills = config.no_skills;
    request.no_prompt_templates = config.no_prompt_templates;
    request.prompt_template_paths = config.prompt_template_paths;
    request.workspace = config.workspace;
    request.session_target = std::move(assembly->target);
    request.session_name = config.name;
    request.session_dir = config.session_dir;
    request.provider = config.provider;
    request.model = config.model;
    request.models = config.models;
    request.api_key = config.api_key;

    const auto print_creation_failure = [&](const util::Error& error) {
        // pi `MissingSessionCwdError`: the resumed session's stored header
        // cwd no longer exists — the stderr error is pi's verbatim text with
        // no "could not resume session:" prefix, and the run exits 1. The
        // interactive Continue/Cancel prompt lands with the startup-TUI
        // host; until then both frontends surface this error.
        if (error.code == util::ErrorCode::MissingSessionCwd) {
            streams.error << error.message << '\n';
        } else {
            print_creation_error(is_resume_target, streams.error, error);
        }
        return 1;
    };

    // The private test seam injects the deterministic fake catalog; at most
    // one of the two call sites below executes per run, so moving `models`
    // here is safe.
    const auto create_session = [&]() {
        return models
            ? coding_agent::create_agent_session_for_testing(
                  std::move(request), std::move(models))
            : coding_agent::create_agent_session(std::move(request));
    };

    // pi main.ts: `--list-models` runs post-runtime pre-stdin and exits 0
    // after printing (the in-memory session is a means to the runtime; the
    // table prints before any diagnostics, matching pi's
    // listModels-before-reportDiagnostics order).
    if (config.list_models) {
        auto created = create_session();
        if (!created) {
            return print_creation_failure(created.error());
        }
        print_list_models(
            *created->session->model_runtime(),
            config.list_models,
            streams.output,
            streams.error);
        return 0;
    }

    // pi `readPipedStdin`: piped stdin is trimmed and only present when it
    // has content; a TTY stdin contributes nothing.
    auto piped_input = read_piped_input(
        streams.input, environment.stdin_is_terminal);
    if (!piped_input) {
        streams.error << piped_input.error().message << '\n';
        return 1;
    }

    // pi `prepareInitialMessage` + `buildInitialMessage`: piped stdin, @file
    // text, and the first positional merge into the initial prompt with no
    // separator; remaining positionals prompt sequentially afterwards.
    auto initial = build_initial_message(InitialMessageInput{
        .messages = config.messages,
        .file_arguments = config.file_arguments,
        .working_directory = config.workspace,
        .stdin_content = std::move(*piped_input),
    });
    if (!initial) {
        streams.error << "could not prepare initial prompt: "
                      << initial.error().message;
        if (!initial.error().detail.empty() &&
            initial.error().detail != initial.error().message) {
            streams.error << ": " << initial.error().detail;
        }
        streams.error << '\n';
        return 1;
    }

    auto created = create_session();
    if (!created) {
        return print_creation_failure(created.error());
    }
    print_session_diagnostics(streams.error, created->diagnostics);

    auto& session = *created->session;
    if (frontend == Frontend::Interactive) {
        // The interactive spine takes one initial prompt; the remaining
        // positionals join the space-separated initial prompt exactly like
        // the pre-print-alignment entry chain.
        std::string interactive_text = initial->initial_message;
        for (const auto& message : initial->remaining_messages) {
            interactive_text += ' ';
            interactive_text += message;
        }
        initial->initial_message = std::move(interactive_text);
        initial->remaining_messages.clear();
        return run_native_tui(
            session,
            std::move(*initial),
            std::move(created->model_fallback_message));
    }

    return run_print_mode(
        session,
        PrintModeConfig{
            .output = streams.output,
            .error = streams.error,
        },
        PrintModePlan{
            .initial_message = std::move(initial->initial_message),
            .messages = std::move(initial->remaining_messages),
            .initial_prompt_options = coding_agent::PromptOptions{
                .expand_prompt_templates = true,
                .images = std::move(initial->initial_images),
            },
        });
}

[[nodiscard]] int run_cli_entry(
    int argc,
    char** argv,
    CliStreams streams,
    CliRuntimeOptions options) {
    auto parsed = parse_args(argc, argv);
    if (!parsed) {
        // Parse errors carry the full help text in detail. Unified exit
        // codes: usage errors exit 1 like pi.
        const auto& error = parsed.error();
        streams.error << (error.detail.empty() ? error.message : error.detail) << '\n';
        return 1;
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
        return 1;
    } else {
        return run_async_cli(
            config, *frontend, streams, environment, std::move(options.models));
    }
}

} // namespace cch::cli
