#include "CliParse.hpp"

#include <CLI/CLI.hpp>

#include <sstream>
#include <string>
#include <system_error>
#include <utility>

namespace cch::cli {
namespace {

cch::util::Error cli_error(std::string message) {
    return cch::util::make_error(cch::util::ErrorCode::Validation, message, message);
}

std::string join_prompt(const std::vector<std::string>& parts) {
    std::ostringstream out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) {
            out << ' ';
        }
        out << parts[i];
    }
    return out.str();
}

std::string normalize_parse_error(const CLI::ParseError& error) {
    const std::string name = error.get_name();
    const std::string message = error.what();

    if (name == "ExcludesError") {
        return "use either --session or --resume, not both";
    }

    const std::string singular = "The following argument was not expected: ";
    const std::string plural = "The following arguments were not expected: ";
    const std::size_t prefix_size = message.starts_with(singular) ? singular.size()
        : (message.starts_with(plural) ? plural.size() : std::string::npos);
    if (prefix_size != std::string::npos) {
        const auto unexpected = message.substr(prefix_size);
        if (!unexpected.empty() && unexpected.front() == '-') {
            return "unknown option: " + unexpected;
        }
    }

    return message;
}

[[nodiscard]] cch::util::Expected<OutputMode> parse_output_mode(const std::string& mode_text) {
    if (mode_text == "text") {
        return OutputMode::Text;
    }
    if (mode_text == "json") {
        return OutputMode::Json;
    }
    if (mode_text == "rpc") {
        return OutputMode::Rpc;
    }
    return std::unexpected(cli_error("unsupported --mode: " + mode_text));
}

} // namespace

cch::util::Expected<CliConfig> parse_args(int argc, char** argv) {
    CliConfig config;
    std::error_code cwd_ec;
    config.workspace = std::filesystem::current_path(cwd_ec);
    if (cwd_ec) {
        return std::unexpected(cch::util::make_error(
            cch::util::ErrorCode::Validation,
            "could not determine the current working directory",
            "the default workspace is unavailable: " + cwd_ec.message()));
    }
    std::string workspace_text = config.workspace.string();
    std::string session_text;
    std::string resume_text;
    std::string session_dir_text;
    std::vector<std::string> prompt_parts;
    bool approve_project = false;
    bool no_approve_project = false;
    std::string provider_text;
    std::string model_text;
    std::string models_text;
    std::string api_key_text;
    std::string mode_text{"text"};

    CLI::App app{"C++ coding-agent harness", "cpp-harness"};
    app.footer(
        "Sessions: without --session/--resume/--no-session, a new session persists\n"
        "under the agent config directory's workspace-keyed sessions root\n"
        "(~/.pi/agent/sessions/<workspace-key>/, root override:\n"
        "PI_CODING_AGENT_DIR). Automatic-directory overrides (highest first):\n"
        "--session-dir, CCH_CODING_AGENT_SESSION_DIR, settings.json sessionDir;\n"
        "relative values resolve against the final workspace. Explicit paths may\n"
        "live anywhere; --no-session runs in memory without a transcript.\n"
        "Frontend: --mode rpc/json wins; otherwise --print or a non-TTY stream\n"
        "selects one-shot text. Interactive Linux/macOS terminals use the Native TUI;\n"
        "--mode text leaves this automatic selection unchanged.\n"
        "Safety: prompts, file contents, and command outputs may be sent to the configured provider.\n"
        "Sessions are local sensitive transcripts even after secret-looking text is redacted.");
    app.set_help_flag("-h,--help", "Print this help message and exit");

    app.add_flag("--fake", config.fake, "Use deterministic fake provider (no network)");
    app.add_flag("-p,--print", config.print, "Process one prompt and exit without the Native TUI");
    app.add_flag("--enable-bash", config.enable_bash, "Allow model-requested bash commands");
    auto* approve_option = app.add_flag("-a,--approve", approve_project, "Trust project resources for this run");
    auto* no_approve_option = app.add_flag("--no-approve", no_approve_project, "Do not trust project resources for this run");
    approve_option->excludes(no_approve_option);
    no_approve_option->excludes(approve_option);
    app.add_flag("--no-skills", config.no_skills, "Disable project-local skills for this run");
    app.add_flag("--no-prompt-templates", config.no_prompt_templates, "Disable all prompt template loading");
    app.add_option("--prompt-template", config.prompt_template_paths, "Load a prompt template file or directory (repeatable)")
        ->expected(1, -1)
        ->allow_extra_args(false);
    auto* workspace_option = app.add_option("--workspace", workspace_text, "Workspace boundary for tools (default: cwd)");
    auto* session_option = app.add_option("--session", session_text, "Create a new JSONL session at an explicit path");
    auto* resume_option = app.add_option("--resume", resume_text, "Resume and append to an existing JSONL session");
    session_option->excludes(resume_option);
    resume_option->excludes(session_option);
    auto* no_session_option = app.add_flag("--no-session", "Run the session in memory without persisting a transcript");
    auto* session_dir_option = app.add_option("--session-dir", session_dir_text,
        "Directory for automatic session storage (overrides CCH_CODING_AGENT_SESSION_DIR and settings.json sessionDir)");
    app.add_option("--max-turns", config.max_turns, "Maximum model turns per prompt (default: no cap)")
        ->check(CLI::Range(1, 64));
    auto* provider_option = app.add_option("--provider", provider_text, "Provider name");
    auto* model_option = app.add_option("--model", model_text, "Model pattern or ID");
    auto* models_option = app.add_option("--models", models_text, "Comma-separated model patterns for cycling");
    auto* api_key_option = app.add_option("--api-key", api_key_text, "API key (in-memory runtime override, never persisted)");
    auto* mode_option = app.add_option(
        "--mode",
        mode_text,
        "Protocol mode: json or rpc; text keeps automatic frontend selection")
        ->default_str("text");
    app.add_option("prompt", prompt_parts, "Prompt")->expected(0, -1);

    try {
        app.parse(argc, argv);
    } catch (const CLI::CallForHelp&) {
        config.help = true;
        config.help_text = app.help();
        return config;
    } catch (const CLI::ParseError& error) {
        const auto message = normalize_parse_error(error);
        return std::unexpected(cch::util::make_error(
            cch::util::ErrorCode::Validation,
            message,
            message + "\n\n" + app.help()));
    }

    if (workspace_option->count() > 0) {
        config.workspace_explicit = true;
        config.workspace = workspace_text;
    }
    if (no_session_option->count() > 0 && session_option->count() > 0) {
        return std::unexpected(cli_error("--no-session cannot be combined with --session"));
    }
    if (no_session_option->count() > 0 && resume_option->count() > 0) {
        return std::unexpected(cli_error("--no-session cannot be combined with --resume"));
    }
    if (session_option->count() > 0) {
        config.session_target = coding_agent::ExplicitNewSessionTarget{session_text};
    }
    if (resume_option->count() > 0) {
        config.session_target = coding_agent::ExplicitResumeSessionTarget{resume_text};
    }
    if (no_session_option->count() > 0) {
        config.session_target = coding_agent::InMemorySessionTarget{};
    }
    if (session_dir_option->count() > 0) {
        config.session_dir = session_dir_text;
    }
    if (model_option->count() > 0) {
        config.model = model_text;
    }
    if (provider_option->count() > 0) {
        config.provider = provider_text;
    }
    if (models_option->count() > 0) {
        std::vector<std::string> patterns;
        std::size_t start = 0;
        while (start <= models_text.size()) {
            const auto comma = models_text.find(',', start);
            const auto end = comma == std::string::npos ? models_text.size() : comma;
            auto pattern = models_text.substr(start, end - start);
            // Trim whitespace.
            const auto not_space = [](unsigned char character) {
                return !std::isspace(character);
            };
            pattern.erase(pattern.begin(), std::find_if(pattern.begin(), pattern.end(), not_space));
            pattern.erase(std::find_if(pattern.rbegin(), pattern.rend(), not_space).base(), pattern.end());
            if (!pattern.empty()) {
                patterns.push_back(std::move(pattern));
            }
            if (comma == std::string::npos) {
                break;
            }
            start = comma + 1;
        }
        config.models = std::move(patterns);
    }
    if (api_key_option->count() > 0) {
        config.api_key = api_key_text;
    }
    if (approve_option->count() > 0) {
        config.project_trust_override = true;
    } else if (no_approve_option->count() > 0) {
        config.project_trust_override = false;
    }

    if (mode_option->count() > 0) {
        auto parsed_mode = parse_output_mode(mode_text);
        if (!parsed_mode) {
            return std::unexpected(std::move(parsed_mode.error()));
        }
        config.output_mode = *parsed_mode;
    } else {
        config.output_mode = OutputMode::Text;
    }

    // `--api-key` requires an explicit model (pi): it cannot name a provider
    // without a model, and never applies to a resume/default selection.
    if (api_key_option->count() > 0 &&
        model_option->count() == 0 &&
        provider_option->count() == 0 &&
        models_option->count() == 0) {
        return std::unexpected(cli_error(
            "--api-key requires a model to be specified via --model, --provider/--model, or --models"));
    }

    std::vector<std::string> prompt_text_parts;
    prompt_text_parts.reserve(prompt_parts.size());
    for (auto& part : prompt_parts) {
        if (part.starts_with('@')) {
            config.file_arguments.push_back(part.substr(1));
        } else {
            prompt_text_parts.push_back(std::move(part));
        }
    }
    config.prompt = join_prompt(prompt_text_parts);
    const bool has_initial_input = !config.prompt.empty() || !config.file_arguments.empty();
    if (config.output_mode == OutputMode::Rpc && has_initial_input) {
        return std::unexpected(cli_error("--mode rpc reads prompts from stdin; positional prompt is not allowed"));
    }
    if (config.output_mode == OutputMode::Json && !has_initial_input) {
        return std::unexpected(cli_error("prompt is required for --mode json"));
    }
    return config;
}

} // namespace cch::cli
