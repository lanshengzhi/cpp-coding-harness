#include "CliParse.hpp"

#include <CLI/CLI.hpp>

#include <sstream>
#include <string>
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
    const std::size_t prefix_size = message.rfind(singular, 0) == 0 ? singular.size()
        : (message.rfind(plural, 0) == 0 ? plural.size() : std::string::npos);
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
    std::string workspace_text = config.workspace.string();
    std::string session_text;
    std::string resume_text;
    int max_turns_option = config.max_turns;
    std::vector<std::string> prompt_parts;
    bool approve_project = false;
    bool no_approve_project = false;
    std::string model_text;
    std::string base_url_text;
    std::string api_key_env_text;
    std::string auth_text;
    std::string mode_text{"text"};

    CLI::App app{"C++ coding-agent harness", "cpp-harness"};
    app.footer(
        "Sessions: without --session/--resume/--no-session, a new session persists\n"
        "under the agent config directory's workspace-keyed sessions root\n"
        "(~/.cpp-harness/agent/sessions/<workspace-key>/, root override:\n"
        "CCH_CODING_AGENT_DIR). Explicit paths may live anywhere; --no-session runs\n"
        "in memory without a transcript.\n"
        "Safety: prompts, file contents, and command outputs may be sent to the configured provider.\n"
        "Sessions are local sensitive transcripts even after secret-looking text is redacted.");
    app.set_help_flag("-h,--help", "Print this help message and exit");

    app.add_flag("--fake", config.fake, "Use deterministic fake provider (no network)");
    app.add_flag("--repl", config.repl, "Read prompts interactively until exit/quit");
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
    app.add_option("--max-turns", max_turns_option, "Maximum model turns per prompt")
        ->check(CLI::Range(1, 64));
    auto* model_option = app.add_option("--model", model_text, "Provider model name")->default_str("gpt-4.1-mini");
    auto* base_url_option = app.add_option("--base-url", base_url_text, "OpenAI-compatible base URL")
        ->default_str("https://api.openai.com");
    auto* api_key_env_option = app.add_option("--api-key-env", api_key_env_text, "Environment variable containing API key")
        ->default_str("OPENAI_API_KEY");
    app.add_option("--auth", auth_text, "Auth provider name in ~/.cpp-harness/agent/auth.json");
    auto* mode_option = app.add_option("--mode", mode_text, "Output mode: text, json, or rpc")->default_str("text");
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
    if (app.count("--max-turns") > 0) {
        config.max_turns = max_turns_option;
    }
    if (model_option->count() > 0) {
        config.provider_overrides.model = model_text;
    }
    if (base_url_option->count() > 0) {
        config.provider_overrides.base_url = base_url_text;
    }
    if (api_key_env_option->count() > 0) {
        config.provider_overrides.api_key_env = api_key_env_text;
    }
    if (app.count("--auth") > 0) {
        config.provider_overrides.auth = auth_text;
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

    config.prompt = join_prompt(prompt_parts);
    if (config.output_mode == OutputMode::Json && config.repl) {
        return std::unexpected(cli_error("--mode json cannot be combined with --repl"));
    }
    if (config.output_mode == OutputMode::Rpc && config.repl) {
        return std::unexpected(cli_error("--mode rpc cannot be combined with --repl"));
    }
    if (config.output_mode == OutputMode::Rpc && !config.prompt.empty()) {
        return std::unexpected(cli_error("--mode rpc reads prompts from stdin; positional prompt is not allowed"));
    }
    if (config.output_mode == OutputMode::Text && !config.repl && config.prompt.empty()) {
        // No prompt in text mode defaults to an interactive REPL, matching pi's no-argument behavior.
        config.repl = true;
    }
    if (config.output_mode == OutputMode::Json && !config.repl && config.prompt.empty()) {
        return std::unexpected(cli_error("prompt is required for --mode json"));
    }
    return config;
}

} // namespace cch::cli
