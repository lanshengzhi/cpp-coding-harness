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
#include <string_view>
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
};

cch::util::Error cli_error(std::string message) {
    return cch::util::make_error(cch::util::ErrorCode::Validation, message, message);
}

std::string safety_text() {
    return "Safety: prompts, file contents, and command outputs may be sent to the configured provider.\n"
           "Sessions are local sensitive transcripts even after secret-looking text is redacted.\n";
}

std::string join_prompt(const std::vector<std::string>& parts, std::size_t start = 0) {
    std::ostringstream out;
    for (std::size_t i = start; i < parts.size(); ++i) {
        if (i > start) {
            out << ' ';
        }
        out << parts[i];
    }
    return out.str();
}

bool is_value_option(std::string_view arg) {
    return arg == "--workspace" || arg == "--session" || arg == "--resume" || arg == "--max-turns" ||
           arg == "--model" || arg == "--base-url" || arg == "--api-key-env";
}

bool is_known_flag(std::string_view arg) {
    return arg == "--help" || arg == "-h" || arg == "--fake" || arg == "--repl" || arg == "--enable-bash";
}

cch::util::ExpectedVoid prevalidate_known_options(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg.empty()) {
            continue;
        }
        if (is_known_flag(arg)) {
            continue;
        }
        if (is_value_option(arg)) {
            if (i + 1 >= argc) {
                return std::unexpected(cli_error(std::string(arg) + " requires a value"));
            }
            ++i;
            continue;
        }
        if (arg[0] == '-') {
            return std::unexpected(cli_error("unknown option: " + std::string(arg)));
        }
        break;
    }
    return {};
}

void configure_app(CLI::App& app, CliConfig& config, std::string& max_turns_value, std::vector<std::string>& prompt_parts) {
    app.name("cpp-harness");
    app.description("Small experimental C++ coding-agent harness");
    app.footer(safety_text());
    app.positionals_at_end();

    app.add_flag("--fake", config.fake, "Use deterministic fake provider (no network)");
    app.add_flag("--repl", config.repl, "Read prompts interactively until exit/quit");
    app.add_option("--workspace", config.workspace, "Workspace boundary for tools (default: cwd)");
    app.add_option("--session", config.session_path, "Create a new JSONL session at path");
    app.add_option("--resume", config.resume_path, "Resume and append to an existing JSONL session");
    app.add_option("--max-turns", max_turns_value, "Maximum model turns per prompt");
    app.add_flag("--enable-bash", config.enable_bash, "Allow model-requested bash commands");
    app.add_option("--model", config.model, "Provider model name");
    app.add_option("--base-url", config.base_url, "OpenAI-compatible base URL");
    app.add_option("--api-key-env", config.api_key_env, "Environment variable containing API key");
    app.add_option("prompt", prompt_parts, "Prompt to send")->expected(0, -1);
}

void print_help(std::ostream& out) {
    CliConfig config;
    std::string max_turns_value = std::to_string(config.max_turns);
    std::vector<std::string> prompt_parts;
    CLI::App app;
    configure_app(app, config, max_turns_value, prompt_parts);
    out << app.help();
}

cch::util::Expected<CliConfig> parse_args(int argc, char** argv) {
    auto known_options = prevalidate_known_options(argc, argv);
    if (!known_options) {
        return std::unexpected(known_options.error());
    }

    CliConfig config;
    std::string max_turns_value = std::to_string(config.max_turns);
    std::vector<std::string> prompt_parts;
    CLI::App app;
    configure_app(app, config, max_turns_value, prompt_parts);

    try {
        app.parse(argc, argv);
    } catch (const CLI::CallForHelp&) {
        config.help = true;
        return config;
    } catch (const CLI::ParseError& error) {
        return std::unexpected(cli_error(error.what()));
    }

    config.workspace_explicit = app.count("--workspace") > 0;
    config.prompt = join_prompt(prompt_parts);

    try {
        std::size_t consumed = 0;
        config.max_turns = std::stoi(max_turns_value, &consumed);
        if (consumed != max_turns_value.size()) {
            return std::unexpected(cli_error("--max-turns must be an integer"));
        }
    } catch (const std::exception&) {
        return std::unexpected(cli_error("--max-turns must be an integer"));
    }

    if (config.max_turns <= 0) {
        return std::unexpected(cli_error("--max-turns must be positive"));
    }
    if (!config.repl && config.prompt.empty() && !config.help) {
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
        std::cerr << "\n";
        print_help(std::cerr);
        return 2;
    }
    CliConfig config = std::move(*parsed);
    if (config.help) {
        print_help(std::cout);
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
