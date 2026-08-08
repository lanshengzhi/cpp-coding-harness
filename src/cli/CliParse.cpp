#include "CliParse.hpp"

#include <CLI/CLI.hpp>

#include <cctype>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>

namespace cch::cli {
namespace {

#ifndef CCH_PROJECT_VERSION
#define CCH_PROJECT_VERSION "0.0.0"
#endif

constexpr std::string_view kProjectVersion = CCH_PROJECT_VERSION;

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

/// pi's short spellings, matched exactly against whole argv tokens (pi args.ts
/// hand parser matches the full token before any short-flag bundling). The
/// multi-character shorts (-na/-ns/-np/-nc) cannot coexist with the
/// single-character -n/-a/-p shorts in a CLI11 table, so every pi short is
/// normalized to its long spelling before CLI11 parses.
[[nodiscard]] std::string expand_pi_short(std::string_view token) {
    if (token == "-p") return "--print";
    if (token == "-a") return "--approve";
    if (token == "-na") return "--no-approve";
    if (token == "-r") return "--resume";
    if (token == "-c") return "--continue";
    if (token == "-n") return "--name";
    if (token == "-ns") return "--no-skills";
    if (token == "-np") return "--no-prompt-templates";
    if (token == "-nc") return "--no-context-files";
    if (token == "-h") return "--help";
    if (token == "-v") return "--version";
    return std::string{token};
}

/// pi `--list-models [search]`: the token after the flag is the fuzzy search
/// pattern only when it is not a flag and not an `@file` argument (pi args.ts
/// exact-match semantics). The intent is extracted here so CLI11 never sees the
/// flag (its optional-value handling would consume a following `@file`); a bare
/// flag records an empty search.
struct NormalizedArgv {
    std::vector<std::string> tokens;
    /// has_value() when --list-models appeared; empty string is the bare flag.
    std::optional<std::string> list_models;
};

[[nodiscard]] NormalizedArgv normalize_argv(
    const std::vector<std::string>& raw) {
    NormalizedArgv normalized;
    normalized.tokens.reserve(raw.size());
    bool options_ended = false;
    for (std::size_t index = 0; index < raw.size(); ++index) {
        const auto& token = raw[index];
        if (options_ended) {
            // Tokens after `--` are positionals verbatim (no short expansion,
            // no flag extraction).
            normalized.tokens.push_back(token);
            continue;
        }
        if (token == "--") {
            options_ended = true;
            normalized.tokens.push_back(token);
            continue;
        }
        if (token == "--list-models" || token.starts_with("--list-models=")) {
            std::string search;
            if (const auto equals = token.find('='); equals != std::string::npos) {
                search = token.substr(equals + 1);
            } else if (index + 1 < raw.size() &&
                       !raw[index + 1].starts_with('-') &&
                       !raw[index + 1].starts_with('@')) {
                search = raw[++index];
            }
            normalized.list_models = std::move(search);
            continue;
        }
        normalized.tokens.push_back(expand_pi_short(token));
    }
    return normalized;
}

[[nodiscard]] cch::util::ExpectedVoid validate_output_mode(
    const std::string& mode_text) {
    // The internal OutputMode reduces to the text default (pi): only
    // `--mode text` parses; the removed json/rpc values are hard-rejected so
    // no removed surface is ever accepted-but-ignored.
    if (mode_text == "text") {
        return {};
    }
    if (mode_text == "json" || mode_text == "rpc") {
        return std::unexpected(cli_error(
            "--mode " + mode_text + " was removed; only --mode text is supported"));
    }
    return std::unexpected(cli_error("unsupported --mode: " + mode_text));
}

/// The pi-aligned option surface documented in help: pi's spellings and shorts
/// for every kept flag, with the C++ binary's own identity.
[[nodiscard]] std::string help_text() {
    return std::string{
        "cch - C++ coding-agent harness with read, bash, edit, write tools\n"
        "\n"
        "Usage:\n"
        "  cpp_harness [options] [@files...] [messages...]\n"
        "\n"
        "Options:\n"
        "  --provider <name>              Provider name\n"
        "  --model <pattern>              Model pattern or ID (supports \"provider/id\"\n"
        "                                 and optional \":<thinking>\")\n"
        "  --models <patterns>            Comma-separated model patterns for cycling\n"
        "  --api-key <key>                API key (in-memory runtime override, never\n"
        "                                 persisted; requires an explicit model)\n"
        "  --thinking <level>             Set thinking level: off, minimal, low, medium,\n"
        "                                 high, xhigh, max\n"
        "  --print, -p                    Non-interactive mode: process prompt and exit\n"
        "  --mode <mode>                  Output mode: text (default)\n"
        "  --continue, -c                 Continue previous session\n"
        "  --resume, -r                   Select a session to resume\n"
        "  --session <path|id>            Use specific session file or partial UUID\n"
        "  --session-id <id>              Use exact project session ID, creating it if\n"
        "                                 missing\n"
        "  --fork <path|id>               Fork specific session file or partial UUID\n"
        "                                 into a new session\n"
        "  --session-dir <dir>            Directory for session storage and lookup\n"
        "  --no-session                   Don't save session (ephemeral)\n"
        "  --name, -n <name>              Set session display name\n"
        "  --approve, -a                  Trust project-local files for this run\n"
        "  --no-approve, -na              Ignore project-local files for this run\n"
        "  --system-prompt <text>         System prompt (default: coding assistant\n"
        "                                 prompt)\n"
        "  --append-system-prompt <text>  Append text or file contents to the system\n"
        "                                 prompt (can be used multiple times)\n"
        "  --skill <path>                 Load a skill file or directory (can be used\n"
        "                                 multiple times)\n"
        "  --no-skills, -ns               Disable skills discovery and loading\n"
        "  --prompt-template <path>       Load a prompt template file or directory (can\n"
        "                                 be used multiple times)\n"
        "  --no-prompt-templates, -np     Disable prompt template discovery and loading\n"
        "  --theme <path>                 Load a theme file or directory (can be used\n"
        "                                 multiple times)\n"
        "  --no-themes                    Disable theme discovery and loading\n"
        "  --no-context-files, -nc        Disable AGENTS.md and CLAUDE.md discovery and\n"
        "                                 loading\n"
        "  --list-models [search]         List available models (with optional fuzzy\n"
        "                                 search)\n"
        "  --help, -h                     Show this help\n"
        "  --version, -v                  Show version number\n"
        "\n"
        "Sessions: without --session/--resume/--continue/--fork/--no-session, a new\n"
        "session persists under the agent config directory's workspace-keyed sessions\n"
        "root (~/.pi/agent/sessions/<workspace-key>/, root override:\n"
        "PI_CODING_AGENT_DIR). Automatic-directory overrides (highest first):\n"
        "--session-dir, PI_CODING_AGENT_SESSION_DIR, settings.json sessionDir;\n"
        "relative values resolve against the workspace. Explicit paths may live\n"
        "anywhere; --no-session runs in memory without a transcript.\n"
        "Frontend: --mode text keeps automatic frontend selection (interactive TUI on\n"
        "a terminal; --print or a non-TTY stream selects one-shot text).\n"
        "Safety: prompts, file contents, and command outputs may be sent to the\n"
        "configured provider. Sessions are local sensitive transcripts even after\n"
        "secret-looking text is redacted.\n"};
}

} // namespace

std::string_view project_version() {
    return kProjectVersion;
}

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

    std::vector<std::string> raw_args;
    raw_args.reserve(static_cast<std::size_t>(argc > 0 ? argc : 0));
    // argv[0] is the program name (CLI11's parse_char_t reads it as the app
    // name); only the remaining tokens are normalized.
    if (argc > 0) {
        raw_args.emplace_back(argv[0]);
    }
    for (int index = 1; index < argc; ++index) {
        raw_args.emplace_back(argv[index]);
    }
    auto normalized = normalize_argv(raw_args);
    std::vector<char*> normalized_argv;
    normalized_argv.reserve(normalized.tokens.size());
    for (auto& argument : normalized.tokens) {
        normalized_argv.push_back(argument.data());
    }
    const int normalized_argc = static_cast<int>(normalized_argv.size());

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
    std::string thinking_text;
    std::string session_id_text;
    std::string fork_text;
    std::string name_text;
    std::string system_prompt_text;
    std::string mode_text{"text"};

    CLI::App app{"C++ coding-agent harness", "cpp-harness"};
    app.set_help_flag("--help", "Print this help message and exit");

    app.add_flag("--version", config.version, "Print the version and exit");
    app.add_flag("--print", config.print, "Process one prompt and exit without the Native TUI");
    auto* approve_option = app.add_flag("--approve", approve_project, "Trust project resources for this run");
    auto* no_approve_option = app.add_flag("--no-approve", no_approve_project, "Do not trust project resources for this run");
    approve_option->excludes(no_approve_option);
    no_approve_option->excludes(approve_option);
    app.add_flag("--no-skills", config.no_skills, "Disable project-local skills for this run");
    app.add_flag("--no-prompt-templates", config.no_prompt_templates, "Disable all prompt template loading");
    app.add_flag("--no-context-files", config.no_context_files, "Disable AGENTS.md and CLAUDE.md discovery and loading");
    app.add_flag("--no-themes", config.no_themes, "Disable theme discovery and loading");
    app.add_option("--prompt-template", config.prompt_template_paths, "Load a prompt template file or directory (repeatable)")
        ->expected(1, -1)
        ->allow_extra_args(false);
    app.add_option("--skill", config.skills, "Load a skill file or directory (repeatable)")
        ->expected(1, -1)
        ->allow_extra_args(false);
    app.add_option("--theme", config.themes, "Load a theme file or directory (repeatable)")
        ->expected(1, -1)
        ->allow_extra_args(false);
    auto* session_option = app.add_option("--session", session_text, "Create a new JSONL session at an explicit path");
    auto* resume_option = app.add_option("--resume", resume_text, "Resume and append to an existing JSONL session");
    session_option->excludes(resume_option);
    resume_option->excludes(session_option);
    auto* no_session_option = app.add_flag("--no-session", "Run the session in memory without persisting a transcript");
    auto* session_dir_option = app.add_option("--session-dir", session_dir_text,
        "Directory for automatic session storage (overrides PI_CODING_AGENT_SESSION_DIR and settings.json sessionDir)");
    auto* continue_option = app.add_flag("--continue", config.continue_session, "Continue the most recent session");
    auto* session_id_option = app.add_option("--session-id", session_id_text, "Use exact project session ID, creating it if missing");
    auto* fork_option = app.add_option("--fork", fork_text, "Fork a session file or partial UUID into a new session");
    auto* name_option = app.add_option("--name", name_text, "Set session display name");
    auto* provider_option = app.add_option("--provider", provider_text, "Provider name");
    auto* model_option = app.add_option("--model", model_text, "Model pattern or ID");
    auto* models_option = app.add_option("--models", models_text, "Comma-separated model patterns for cycling");
    auto* api_key_option = app.add_option("--api-key", api_key_text, "API key (in-memory runtime override, never persisted)");
    auto* thinking_option = app.add_option("--thinking", thinking_text, "Set thinking level: off, minimal, low, medium, high, xhigh, max");
    auto* system_prompt_option = app.add_option("--system-prompt", system_prompt_text, "System prompt (default: coding assistant prompt)");
    auto* append_system_prompt_option = app.add_option("--append-system-prompt", config.append_system_prompt,
        "Append text or file contents to the system prompt (repeatable)")
        ->expected(1, -1)
        ->allow_extra_args(false);
    auto* mode_option = app.add_option(
        "--mode",
        mode_text,
        "Output mode: text (default)")
        ->default_str("text");
    app.add_option("prompt", prompt_parts, "Prompt")->expected(0, -1);

    try {
        app.parse(normalized_argc, normalized_argv.data());
    } catch (const CLI::CallForHelp&) {
        config.help = true;
        config.help_text = help_text();
        return config;
    } catch (const CLI::ParseError& error) {
        const auto message = normalize_parse_error(error);
        return std::unexpected(cch::util::make_error(
            cch::util::ErrorCode::Validation,
            message,
            message + "\n\n" + help_text()));
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
    if (thinking_option->count() > 0) {
        config.thinking = thinking_text;
    }
    if (session_id_option->count() > 0) {
        config.session_id = session_id_text;
    }
    if (fork_option->count() > 0) {
        config.fork = fork_text;
    }
    if (name_option->count() > 0) {
        config.name = name_text;
    }
    if (normalized.list_models) {
        config.list_models = std::move(*normalized.list_models);
    }
    if (system_prompt_option->count() > 0) {
        config.system_prompt = system_prompt_text;
    }
    if (approve_option->count() > 0) {
        config.project_trust_override = true;
    } else if (no_approve_option->count() > 0) {
        config.project_trust_override = false;
    }

    if (mode_option->count() > 0) {
        if (auto parsed_mode = validate_output_mode(mode_text); !parsed_mode) {
            return std::unexpected(std::move(parsed_mode.error()));
        }
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
    return config;
}

} // namespace cch::cli
