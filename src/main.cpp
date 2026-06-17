#include "AsyncCliRuntime.hpp"

#include "../include/cch/util/Error.hpp"

#include <CLI/CLI.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct CliConfig {
    bool fake{false};
    bool repl{false};
    bool enable_bash{false};
    bool help{false};
    bool workspace_explicit{false};
    int max_turns{8};
    std::filesystem::path workspace{std::filesystem::current_path()};
    std::filesystem::path session_path;
    std::filesystem::path resume_path;
    std::string model{"gpt-4.1-mini"};
    std::string base_url{"https://api.openai.com"};
    std::string api_key_env{"OPENAI_API_KEY"};
    std::string prompt;
    std::string help_text;
};

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
    const auto prefix_size = message.rfind(singular, 0) == 0 ? singular.size()
        : (message.rfind(plural, 0) == 0 ? plural.size() : std::string::npos);
    if (prefix_size != std::string::npos) {
        const auto unexpected = message.substr(prefix_size);
        if (!unexpected.empty() && unexpected.front() == '-') {
            return "unknown option: " + unexpected;
        }
    }

    return message;
}

cch::util::Expected<CliConfig> parse_args(int argc, char** argv) {
    CliConfig config;
    std::string workspace_text = config.workspace.string();
    std::string session_text;
    std::string resume_text;
    int max_turns_option = config.max_turns;
    std::vector<std::string> prompt_parts;

    CLI::App app{"C++ coding-agent harness", "cpp-harness"};
    app.footer(
        "Safety: prompts, file contents, and command outputs may be sent to the configured provider.\n"
        "Sessions are local sensitive transcripts even after secret-looking text is redacted.");
    app.set_help_flag("-h,--help", "Print this help message and exit");

    app.add_flag("--fake", config.fake, "Use deterministic fake provider (no network)");
    app.add_flag("--repl", config.repl, "Read prompts interactively until exit/quit");
    app.add_flag("--enable-bash", config.enable_bash, "Allow model-requested bash commands");
    auto* workspace_option = app.add_option("--workspace", workspace_text, "Workspace boundary for tools (default: cwd)");
    auto* session_option = app.add_option("--session", session_text, "Create a new JSONL session at path");
    auto* resume_option = app.add_option("--resume", resume_text, "Resume and append to an existing JSONL session");
    session_option->excludes(resume_option);
    resume_option->excludes(session_option);
    app.add_option("--max-turns", max_turns_option, "Maximum model turns per prompt")
        ->check(CLI::PositiveNumber);
    app.add_option("--model", config.model, "Provider model name")->capture_default_str();
    app.add_option("--base-url", config.base_url, "OpenAI-compatible base URL")->capture_default_str();
    app.add_option("--api-key-env", config.api_key_env, "Environment variable containing API key")->capture_default_str();
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
    if (session_option->count() > 0) {
        config.session_path = session_text;
    }
    if (resume_option->count() > 0) {
        config.resume_path = resume_text;
    }
    if (app.count("--max-turns") > 0) {
        config.max_turns = max_turns_option;
    }

    config.prompt = join_prompt(prompt_parts);
    if (config.max_turns <= 0) {
        return std::unexpected(cli_error("--max-turns must be positive"));
    }
    if (!config.repl && config.prompt.empty()) {
        return std::unexpected(cli_error("prompt is required unless --repl is used"));
    }
    return config;
}

std::string timestamp_for_path() {
    auto now = std::chrono::system_clock::now();
    auto seconds = std::chrono::system_clock::to_time_t(now);
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &seconds);
#else
    localtime_r(&seconds, &tm);
#endif
    std::ostringstream out;
    out << std::put_time(&tm, "%Y%m%d-%H%M%S") << '-' << std::setw(3) << std::setfill('0') << milliseconds.count();
    return out.str();
}

std::string random_suffix() {
    std::random_device device;
    std::uniform_int_distribution<unsigned int> distribution(0, 0xFFFFu);
    std::ostringstream out;
    out << std::hex << std::setw(4) << std::setfill('0') << distribution(device);
    return out.str();
}

std::filesystem::path default_session_path() {
    return std::filesystem::current_path() / ".cpp-harness" / "sessions" / (timestamp_for_path() + "-" + random_suffix() + ".jsonl");
}

cch::util::ExpectedVoid validate_workspace(const std::filesystem::path& workspace) {
    std::error_code ec;
    if (!std::filesystem::exists(workspace, ec) || !std::filesystem::is_directory(workspace, ec)) {
        return std::unexpected(cli_error("invalid workspace path: " + workspace.string()));
    }
    return {};
}

std::filesystem::path canonical_workspace(const std::filesystem::path& workspace) {
    std::error_code ec;
    auto canonical = std::filesystem::weakly_canonical(workspace, ec);
    return ec ? workspace.lexically_normal() : canonical;
}

cch::util::ExpectedVoid validate_config_before_model(const CliConfig& config) {
    std::error_code ec;
    if (!config.fake) {
        const char* key = std::getenv(config.api_key_env.c_str());
        if (key == nullptr || *key == '\0') {
            return std::unexpected(cli_error("missing API key; set " + config.api_key_env + " before real-provider mode"));
        }
    }
    if (!config.resume_path.empty() && !config.session_path.empty()) {
        return std::unexpected(cli_error("use either --session or --resume, not both"));
    }
    if (!config.session_path.empty() && std::filesystem::exists(config.session_path, ec)) {
        return std::unexpected(cli_error("session file already exists; use --resume to append"));
    }
    return {};
}

void print_error(const cch::util::Error& error) {
    std::cerr << (error.detail.empty() ? error.message : error.detail) << '\n';
}

} // namespace

int main(int argc, char** argv) {
    auto parsed = parse_args(argc, argv);
    if (!parsed) {
        print_error(parsed.error());
        return 2;
    }
    CliConfig config = std::move(*parsed);
    if (config.help) {
        std::cout << config.help_text;
        return 0;
    }
    auto validation = validate_config_before_model(config);
    if (!validation) {
        print_error(validation.error());
        return 2;
    }

    auto workspace_validation = validate_workspace(config.workspace);
    if (!workspace_validation) {
        print_error(workspace_validation.error());
        return 2;
    }
    config.workspace = canonical_workspace(config.workspace);

    return cch::cli::run_async_cli(cch::cli::AsyncCliRuntimeConfig{
        config.fake,
        config.repl,
        config.enable_bash,
        config.max_turns,
        config.workspace_explicit,
        config.workspace,
        config.session_path.empty() ? default_session_path() : config.session_path,
        config.resume_path,
        timestamp_for_path(),
        timestamp_for_path(),
        config.model,
        config.base_url,
        config.api_key_env,
        config.prompt,
    });
}
