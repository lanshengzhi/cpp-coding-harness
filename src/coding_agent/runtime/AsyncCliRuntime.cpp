#include "coding_agent/runtime/AsyncCliRuntime.hpp"

#include "cli/CliParse.hpp"
#include "cli/InitialPrompt.hpp"
#include "cli/ListModels.hpp"
#include "cli/PrintMode.hpp"
#include "cli/SessionFamily.hpp"
#include "cli/StartupTui.hpp"
#include "coding_agent/AgentSession.hpp"
#include "coding_agent/runtime/SessionFactory.hpp"
#include "coding_agent/SessionCwd.hpp"
#include "agent/harness/RuntimeRoot.hpp"
#include "coding_agent/tui/InteractiveMode.hpp"
#include "coding_agent/tui/InteractiveSessionRun.hpp"
#include "coding_agent/tui/ThemeController.hpp"
#include "support/AsyncResultBridge.hpp"
#include <cch/coding_agent/AgentConfigDir.hpp>
#include <cch/coding_agent/ModelRuntime.hpp>
#include <cch/coding_agent/Settings.hpp>
#include <cch/tui/ProcessTerminal.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <csignal>
#include <exception>
#include <filesystem>
#include <future>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace cch::cli {
namespace {

/// Scenario-measured Runtime capacities and fairness limits
/// (`docs/runtime-capacities.md`; ADR 0040 §Admission, overload, and
/// fairness). The `harness::RuntimeLimits` defaults are the measured
/// production policy; every CLI Runtime root uses the same set so
/// replacement Sessions reuse identical admission and mailbox-batch
/// behavior.
constexpr harness::RuntimeLimits kRuntimeLimits{};

/// Final application Close (ADR 0040 §Session Event Commitment and Close,
/// issue #467): with the current Session already closed and quiesced by the
/// owning mode, stop Runtime admission, drain queued worker work, and join
/// the workers, then pump the shared loop so every admitted terminal outcome
/// reaches its target mailbox before the root and the loop are destroyed.
/// RuntimeRoot::close releases the loop work guard, so the drain returns
/// once the queued work is delivered.
void close_runtime(
    const std::shared_ptr<harness::RuntimeRoot>& runtime_root,
    boost::asio::io_context& io) {
    // Idle AgentSession close posts owned-environment cleanup to the Runtime
    // loop because it cannot await from its synchronous close() seam. Start
    // those cleanup operations before RuntimeRoot stops admission; otherwise
    // AsyncLocalExecutionEnv::cleanup() is rejected as Busy and tracked
    // temporary resources leak during CLI teardown.
    if (io.stopped()) {
        io.restart();
    }
    (void)io.poll();
    runtime_root->close();
    if (io.stopped()) {
        io.restart();
    }
    while (io.poll() != 0) {
    }
}

/// pi `readPipedStdin`: read all of piped stdin and trim it; empty (or
/// whitespace-only) content is absent and contributes nothing to the initial
/// message merge.
[[nodiscard]] support::Expected<std::string> read_piped_input(
    std::istream& input,
    bool stdin_is_terminal) {
    if (stdin_is_terminal) return std::string{};

    std::ostringstream collected;
    collected << input.rdbuf();
    if (input.bad()) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Unknown,
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
    const support::Error& error) {
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

/// pi main.ts: the startup settings manager supplies the sessionDir chain
/// value used during session-family resolution and the global-scope `theme`
/// value for the boot and startup-TUI theme init, exactly like pi's
/// `startupSettingsManager` (main.ts `startupSettingsManager.getSessionDir()`
/// / `createStartupTui(settingsManager)`).
[[nodiscard]] coding_agent::SettingsManager startup_settings(
    const std::filesystem::path& workspace) {
    return coding_agent::SettingsManager::create(
        workspace, coding_agent::agent_config_dir(),
        /* project_trusted */ false);
}

/// pi `createStartupTui` options for the startup-TUI hosts (the `--resume`
/// picker and the boot missing-cwd Continue/Cancel prompt): the agent
/// config directory (keybindings.json read + boot theme load) and the raw
/// global-scope `theme` setting for the G5 controller default init.
[[nodiscard]] StartupTuiOptions startup_tui_options(
    const coding_agent::SettingsManager& settings) {
    return StartupTuiOptions{
        .agent_config_directory = coding_agent::agent_config_dir(),
        .theme_setting = settings.settings().theme,
    };
}

[[nodiscard]] int run_native_tui_boot(InitialMessageResult initial,
        const CliConfig& config,
        std::shared_ptr<ai::Models> models,
        std::shared_ptr<coding_agent::ModelRuntime> model_runtime,
        bool model_runtime_cli_fake,
        CliStreams streams,
        coding_agent::runtime::AgentSessionCreationRequest request) {
    cch::tui::ProcessTerminal terminal;
    auto io = std::make_shared<boost::asio::io_context>();
    auto runtime_root = std::make_shared<harness::RuntimeRoot>(io, kRuntimeLimits);
    // One Models runtime shared by the boot Session and every in-session
    // replacement (ADR 0029/0030, issue #466): the Runtime loop, worker
    // capacity, and model/auth resources are reused rather than reconstructed
    // for each Session, and closing one Session never releases the shared
    // Models resources the replacement Session needs. The injected test
    // runtime follows the same sharing rule; a production runtime-creation failure falls
    // back to per-Session construction so the boot reports the same
    // session-creation error it would have reported otherwise.
    std::shared_ptr<coding_agent::ModelRuntime> shared_runtime = std::move(model_runtime);
    if (!models && !shared_runtime) {
        if (auto created = coding_agent::ModelRuntime::create({}); created) {
            shared_runtime = std::move(*created);
        }
    }
    auto run = coding_agent::tui::InteractiveSessionRunBuilder{}
                       .with_session_facts(config.session_facts)
                       .with_agent_config_directory(coding_agent::agent_config_dir())
                       .with_initial_prompt(initial.initial_message.empty()
                                                    ? std::nullopt
                                                    : std::optional<std::string>{std::move(initial.initial_message)})
                       .with_initial_prompt_options(coding_agent::PromptOptions{
                               .expand_prompt_templates = true,
                               .images = std::move(initial.initial_images),
                       })
                       .with_defer_boot(std::move(request))
                       .with_runtime_root(runtime_root)
                       .with_shared_runtime(shared_runtime)
                       .with_model_runtime_cli_fake(model_runtime_cli_fake)
                       .with_models(models)
                       .with_error_stream(&streams.error)
                       .build();

    auto future = boost::asio::co_spawn(
        *io,
        coding_agent::tui::run_interactive_mode(
            terminal,
            std::move(run)),
        boost::asio::use_future);
    while (future.wait_for(std::chrono::milliseconds{0}) != std::future_status::ready) {
        (void)io->run_one();
    }

    // Final application Close (ADR 0040, issue #467): the interactive mode
    // already stopped terminal I/O and closed and quiesced the current
    // Session (its exit waits for active prompt, User Bash, and compaction
    // work to settle); now stop Runtime
    // admission, join the workers, and drain the shared loop so admitted
    // terminals — including retired Sessions' late completions — reach their
    // mailboxes before the root, the Models runtime, and the loop destruct.
    int exit_code = 0;
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    try {
#endif
        if (auto result = future.get(); !result) {
            if (!run.creation_failure_reported()) {
                std::cerr << "Native TUI failed: " << result.error().message;
                if (!result.error().detail.empty() &&
                    result.error().detail != result.error().message) {
                    std::cerr << ": " << result.error().detail;
                }
                std::cerr << '\n';
            }
            exit_code = 1;
        }
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    } catch (const std::exception& error) {
        std::cerr << "Native TUI failed: " << error.what() << '\n';
        exit_code = 1;
    } catch (...) {
        std::cerr << "Native TUI failed: unknown exception\n";
        exit_code = 1;
    }
#endif
    close_runtime(runtime_root, *io);
    return exit_code;
}

} // namespace

[[nodiscard]] int run_async_cli(const CliConfig& config,
        Frontend frontend,
        CliStreams streams,
        FrontendEnvironment environment,
        std::shared_ptr<ai::Models> models,
        std::shared_ptr<coding_agent::ModelRuntime> model_runtime,
        bool model_runtime_cli_fake,
        ResumePickerSink resume_picker) {
    // pi main.ts boot order: the session-family flag guards run before any
    // session machinery, the cross-project fork prompt happens during session
    // selection (before piped stdin is read), and the `--name` guard follows
    // session selection.
    if (auto guard = session_family_guard_error(config); guard) {
        streams.error << *guard << '\n';
        return 1;
    }

    // pi main.ts: the startup settings manager resolves the sessionDir chain
    // value and the global-scope theme for the startup-TUI hosts.
    auto settings = startup_settings(config.workspace);

    // pi main.ts `selectSession`: `--resume` opens the startup-TUI session
    // picker on its own terminal before any session machinery. The real host
    // requires TTY descriptors (pi's ProcessTerminal crashes on piped stdin
    // the same way); the in-process CLI fixture injects a scripted picker.
    if (!resume_picker) {
        const auto options = startup_tui_options(settings);
        resume_picker = [options](
                             coding_agent::tui::SessionListLoader current_loader,
                             coding_agent::tui::SessionListLoader all_loader) {
            return run_process_terminal_resume_picker(
                options, std::move(current_loader), std::move(all_loader));
        };
    }

    auto assembly = assemble_session_target(
        config,
        settings.settings().session_dir,
        streams.input,
        // pi takes over stdout outside interactive mode (output-guard.ts), so
        // the cross-project notice, fork prompt, "Aborted.", and "No session
        // selected" lines land on stderr in print mode and never pollute the
        // one-shot stdout.
        frontend == Frontend::Interactive ? streams.output : streams.error,
        streams.error,
        std::move(resume_picker));
    if (!assembly) {
        streams.error << assembly.error().message << '\n';
        return 1;
    }
    if (assembly->aborted) {
        // pi: the declined fork printed "Aborted." during session selection.
        return 0;
    }

    // pi main.ts missing-cwd recovery: `getMissingSessionCwdIssue` runs
    // right after session selection, before the `--name` guard. The
    // interactive host prompts Continue/Cancel through the startup TUI
    // before the main TUI starts (G3; pi `promptForMissingSessionCwd` →
    // `showStartupSelector`); cancel exits 0 silently (pi
    // `process.exit(0)`). The non-interactive host prints pi's verbatim
    // stderr error and exits 1 (pi main.ts `new MissingSessionCwdError(...)`
    // + exit(1)). The chosen override flows into the creation request
    // exactly like pi's `SessionManager.open(file, sessionDir, selectedCwd)`;
    // SessionFactory's creation-time check stays as the race fallback.
    std::optional<std::filesystem::path> boot_cwd_override;
    if (auto issue = missing_session_cwd_issue(*assembly, config.workspace);
        issue) {
        if (frontend == Frontend::Interactive) {
            auto prompt = run_process_terminal_missing_cwd_prompt(
                startup_tui_options(settings),
                coding_agent::format_missing_session_cwd_prompt(*issue));
            if (!prompt) {
                streams.error << prompt.error().message << '\n';
                return 1;
            }
            if (!*prompt) {
                return 0;
            }
            boot_cwd_override = issue->fallback_cwd;
        } else {
            streams.error
                << coding_agent::format_missing_session_cwd_error(*issue)
                << '\n';
            return 1;
        }
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
    request.session_facts = config.session_facts;
    request.project_trust_override = config.session_facts.project_trust_override;
    request.workspace = config.workspace;
    request.session_target = std::move(assembly->target);
    request.resume_cwd_override = boot_cwd_override;
    request.session_name = config.name;
    request.session_dir = config.session_dir;

    const auto print_creation_failure = [&](const support::Error& error) {
        // pi `MissingSessionCwdError`: the resumed session's stored header
        // cwd no longer exists — the stderr error is pi's verbatim text with
        // no "could not resume session:" prefix, and the run exits 1. The
        // missing-cwd recovery above already resolved the interactive
        // prompt and the non-interactive error; this path covers the race
        // where the header changes between the check and session creation.
        if (error.code == support::ErrorCode::MissingSessionCwd) {
            streams.error << error.message << '\n';
        } else {
            print_creation_error(is_resume_target, streams.error, error);
        }
        return 1;
    };

    // The private test seam injects the deterministic fake catalog; at most
    // one of the two call sites below executes per run, so moving `models`
    // here is safe. The non-interactive CLI owns one Runtime root for session
    // assembly and print-mode work; the interactive path creates its root in
    // `run_native_tui_boot` after this lambda is bypassed.
    std::shared_ptr<boost::asio::io_context> runtime_io;
    std::shared_ptr<harness::RuntimeRoot> runtime_root;
    const auto create_session = [&]() -> support::Expected<coding_agent::CreateAgentSessionResult> {
        runtime_io = std::make_shared<boost::asio::io_context>();
        runtime_root = std::make_shared<harness::RuntimeRoot>(runtime_io, kRuntimeLimits);
        request.execution_runtime_target = runtime_root->make_target();
        auto operation = coding_agent::create_agent_session_async(std::move(request),
                std::nullopt,
                coding_agent::runtime::AssemblyOverrides{.model_runtime = std::move(model_runtime),
                        .cli_fake = model_runtime_cli_fake,
                        .models = std::move(models),
                        .user_shell = nullptr});
        auto future = boost::asio::co_spawn(
                *runtime_io, support::detail::await_async_result(std::move(operation)), boost::asio::use_future);
        while (future.wait_for(std::chrono::milliseconds{0}) != std::future_status::ready) {
            (void)runtime_io->run_one();
        }
        return future.get();
    };

    // pi main.ts: `--list-models` runs post-runtime pre-stdin and exits 0
    // after printing (the in-memory session is a means to the runtime; the
    // table prints before any diagnostics, matching pi's
    // listModels-before-reportDiagnostics order).
    if (config.list_models) {
        auto created = create_session();
        if (!created) {
            const int exit_code = print_creation_failure(created.error());
            close_runtime(runtime_root, *runtime_io);
            return exit_code;
        }
        print_list_models(
            *created->session->model_runtime(),
            config.list_models,
            streams.output,
            streams.error);
        created->session->close();
        close_runtime(runtime_root, *runtime_io);
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

    // pi main.ts `initTheme(settingsManager.getTheme(), appMode ===
    // "interactive")`: the boot theme init runs for every mode before the
    // no-model guard and the frontend split — resolve the settings theme
    // (slash automatic-pair values read as unset) or the COLORFGBG-detected
    // default and load it with pi's silent dark fallback. The palette is
    // the one every mode boots with; the interactive controller re-inits
    // from the same settings at TUI construction (pi
    // `InteractiveThemeController`) and the startup-TUI host consumes the
    // same init.
    {
        auto boot_settings = coding_agent::SettingsManager::create(
            config.workspace,
            coding_agent::agent_config_dir(),
            /* project_trusted */ false);
        (void)coding_agent::tui::init_boot_theme(
            coding_agent::agent_config_dir(),
            boot_settings.settings().theme);
    }

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
        // pi main.ts: the interactive boot defers session creation until
        // after the boot trust prompt; the CLI passes the session factory
        // and the base request to the boot.
        return run_native_tui_boot(std::move(*initial),
                config,
                models,
                std::move(model_runtime),
                model_runtime_cli_fake,
                streams,
                std::move(request));
    }

    auto created = create_session();
    if (!created) {
        const int exit_code = print_creation_failure(created.error());
        close_runtime(runtime_root, *runtime_io);
        return exit_code;
    }
    print_session_diagnostics(streams.error, created->diagnostics);

    const int print_exit_code = run_print_mode(
        *runtime_io,
        *created->session,
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
    // Final application Close (ADR 0040, issue #467): the prompts have
    // settled (a signal already closed the Session inside print mode; the
    // idempotent request below covers the normal exit), so stop Runtime
    // admission, join the workers, and drain the shared loop — including
    // the Session's posted process-resource cleanup — before teardown.
    created->session->close();
    close_runtime(runtime_root, *runtime_io);
    return print_exit_code;
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
        return run_async_cli(config,
                *frontend,
                streams,
                environment,
                std::move(options.models),
                std::move(options.model_runtime),
                options.model_runtime_cli_fake,
                std::move(options.resume_picker));
    }
}

} // namespace cch::cli
