#include "agent/AgentLoop.hpp"
#include "ai/providers/BoostBeastHttpTransport.hpp"
#include "ai/providers/OpenAIChatClient.hpp"
#include "harness/session/JsonlSessionStore.hpp"
#include "tools/Tools.hpp"
#include "AsyncCliRuntime.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
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
    bool async_stack{false};
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

void print_help(std::ostream& out) {
    out << "cpp-harness [options] [prompt]\n"
        << "\nOptions:\n"
        << "  --fake                    Use deterministic fake provider (no network)\n"
        << "  --async                   Run the experimental coroutine/Glaze stack\n"
        << "  --repl                    Read prompts interactively until exit/quit\n"
        << "  --workspace <path>        Workspace boundary for tools (default: cwd)\n"
        << "  --session <path>          Create a new JSONL session at path\n"
        << "  --resume <path>           Resume and append to an existing JSONL session\n"
        << "  --max-turns <n>           Maximum model turns per prompt\n"
        << "  --enable-bash             Allow model-requested bash commands\n"
        << "  --model <name>            Provider model name\n"
        << "  --base-url <url>          OpenAI-compatible base URL\n"
        << "  --api-key-env <name>      Environment variable containing API key\n"
        << "\nSafety: prompts, file contents, and command outputs may be sent to the configured provider.\n"
        << "Sessions are local sensitive transcripts even after secret-looking text is redacted.\n";
}

std::string join_prompt(const std::vector<std::string>& parts, std::size_t start) {
    std::ostringstream out;
    for (std::size_t i = start; i < parts.size(); ++i) {
        if (i > start) {
            out << ' ';
        }
        out << parts[i];
    }
    return out.str();
}

cch::util::Result<CliConfig> parse_args(int argc, char** argv) {
    CliConfig config;
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& arg = args[i];
        auto need_value = [&](const std::string& name) -> cch::util::Result<std::string> {
            if (i + 1 >= args.size()) {
                return cch::util::Result<std::string>::failure(name + " requires a value");
            }
            return cch::util::Result<std::string>::success(args[++i]);
        };
        if (arg == "--help" || arg == "-h") {
            config.help = true;
        } else if (arg == "--fake") {
            config.fake = true;
        } else if (arg == "--async") {
            config.async_stack = true;
        } else if (arg == "--repl") {
            config.repl = true;
        } else if (arg == "--enable-bash") {
            config.enable_bash = true;
        } else if (arg == "--workspace") {
            auto value = need_value(arg);
            if (!value) return cch::util::Result<CliConfig>::failure(value.error());
            config.workspace = value.value();
            config.workspace_explicit = true;
        } else if (arg == "--session") {
            auto value = need_value(arg);
            if (!value) return cch::util::Result<CliConfig>::failure(value.error());
            config.session_path = value.value();
        } else if (arg == "--resume") {
            auto value = need_value(arg);
            if (!value) return cch::util::Result<CliConfig>::failure(value.error());
            config.resume_path = value.value();
        } else if (arg == "--max-turns") {
            auto value = need_value(arg);
            if (!value) return cch::util::Result<CliConfig>::failure(value.error());
            try {
                std::size_t consumed = 0;
                config.max_turns = std::stoi(value.value(), &consumed);
                if (consumed != value.value().size()) {
                    return cch::util::Result<CliConfig>::failure("--max-turns must be an integer");
                }
            } catch (const std::exception&) {
                return cch::util::Result<CliConfig>::failure("--max-turns must be an integer");
            }
        } else if (arg == "--model") {
            auto value = need_value(arg);
            if (!value) return cch::util::Result<CliConfig>::failure(value.error());
            config.model = value.value();
        } else if (arg == "--base-url") {
            auto value = need_value(arg);
            if (!value) return cch::util::Result<CliConfig>::failure(value.error());
            config.base_url = value.value();
        } else if (arg == "--api-key-env") {
            auto value = need_value(arg);
            if (!value) return cch::util::Result<CliConfig>::failure(value.error());
            config.api_key_env = value.value();
        } else if (!arg.empty() && arg[0] == '-') {
            return cch::util::Result<CliConfig>::failure("unknown option: " + arg);
        } else {
            config.prompt = join_prompt(args, i);
            break;
        }
    }
    if (config.max_turns <= 0) {
        return cch::util::Result<CliConfig>::failure("--max-turns must be positive");
    }
    if (!config.repl && config.prompt.empty() && !config.help) {
        return cch::util::Result<CliConfig>::failure("prompt is required unless --repl is used");
    }
    return cch::util::Result<CliConfig>::success(std::move(config));
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

class ScriptedFakeClient final : public cch::ai::ChatClient {
public:
    cch::util::Result<cch::ai::ChatResponse> complete(const cch::ai::ChatRequest& request) override {
        cch::ai::ChatResponse response;
        cch::agent::Message assistant;
        assistant.role = cch::agent::Role::Assistant;
        if (!request.context.messages.empty()) {
            auto last = cch::agent::message_from_ai(request.context.messages.back());
            if (last.role == cch::agent::Role::Tool) {
                assistant.content = "fake observed: " + last.content;
                response.assistant_message = cch::agent::to_ai_message(assistant);
                return cch::util::Result<cch::ai::ChatResponse>::success(response);
            }
        }
        std::string prompt;
        for (auto it = request.context.messages.rbegin(); it != request.context.messages.rend(); ++it) {
            auto message = cch::agent::message_from_ai(*it);
            if (message.role == cch::agent::Role::User) {
                prompt = message.content;
                break;
            }
        }
        if (prompt.rfind("read ", 0) == 0) {
            cch::agent::ToolCall call;
            call.id = "fake-read-1";
            call.name = "read_file";
            call.arguments = {{"path", prompt.substr(5)}};
            call.raw_arguments = boost::json::serialize(call.arguments);
            assistant.content = "reading " + prompt.substr(5);
            assistant.tool_calls.push_back(std::move(call));
            response.assistant_message = cch::agent::to_ai_message(assistant);
            response.stop_reason = cch::ai::StopReason::ToolUse;
            return cch::util::Result<cch::ai::ChatResponse>::success(response);
        }
        if (prompt.rfind("bash ", 0) == 0) {
            cch::agent::ToolCall call;
            call.id = "fake-bash-1";
            call.name = "bash";
            call.arguments = {{"command", prompt.substr(5)}};
            call.raw_arguments = boost::json::serialize(call.arguments);
            assistant.content = "running bash";
            assistant.tool_calls.push_back(std::move(call));
            response.assistant_message = cch::agent::to_ai_message(assistant);
            response.stop_reason = cch::ai::StopReason::ToolUse;
            return cch::util::Result<cch::ai::ChatResponse>::success(response);
        }
        assistant.content = "fake: " + prompt;
        response.assistant_message = cch::agent::to_ai_message(assistant);
        return cch::util::Result<cch::ai::ChatResponse>::success(response);
    }
};

void print_event(const cch::agent::LoopEvent& event) {
    if (event.type == "model_request") std::cout << "[model-request] " << event.detail << '\n';
    else if (event.type == "assistant") std::cout << "[assistant] " << event.detail << '\n';
    else if (event.type == "tool_call") std::cout << "[tool-call] " << event.detail << '\n';
    else if (event.type == "tool_success") std::cout << "[tool-success] " << event.detail << '\n';
    else if (event.type == "tool_error") std::cout << "[tool-error] " << event.detail << '\n';
    else if (event.type == "provider_error") std::cout << "[provider-error] " << event.detail << '\n';
    else if (event.type == "max_turns") std::cout << "[max-turns] " << event.detail << '\n';
    else if (event.type == "completed") std::cout << "[completed] " << event.detail << '\n';
}

cch::util::Result<void> validate_workspace(const std::filesystem::path& workspace) {
    std::error_code ec;
    if (!std::filesystem::exists(workspace, ec) || !std::filesystem::is_directory(workspace, ec)) {
        return cch::util::Result<void>::failure("invalid workspace path: " + workspace.string());
    }
    return cch::util::Result<void>::success();
}

std::filesystem::path canonical_workspace(const std::filesystem::path& workspace) {
    std::error_code ec;
    auto canonical = std::filesystem::weakly_canonical(workspace, ec);
    return ec ? workspace.lexically_normal() : canonical;
}

bool same_workspace(const std::filesystem::path& first, const std::filesystem::path& second) {
    std::error_code first_ec;
    std::error_code second_ec;
    auto first_canonical = std::filesystem::weakly_canonical(first, first_ec);
    auto second_canonical = std::filesystem::weakly_canonical(second, second_ec);
    if (first_ec || second_ec) {
        return first.lexically_normal() == second.lexically_normal();
    }
    return first_canonical == second_canonical;
}

cch::util::Result<void> validate_config_before_model(const CliConfig& config) {
    std::error_code ec;
    if (!config.fake) {
        const char* key = std::getenv(config.api_key_env.c_str());
        if (key == nullptr || *key == '\0') {
            return cch::util::Result<void>::failure("missing API key; set " + config.api_key_env + " before real-provider mode");
        }
    }
    if (!config.resume_path.empty() && !config.session_path.empty()) {
        return cch::util::Result<void>::failure("use either --session or --resume, not both");
    }
    if (!config.session_path.empty() && std::filesystem::exists(config.session_path, ec)) {
        return cch::util::Result<void>::failure("session file already exists; use --resume to append");
    }
    return cch::util::Result<void>::success();
}

} // namespace

int main(int argc, char** argv) {
    auto parsed = parse_args(argc, argv);
    if (!parsed) {
        std::cerr << parsed.error() << "\n\n";
        print_help(std::cerr);
        return 2;
    }
    CliConfig config = std::move(parsed.value());
    if (config.help) {
        print_help(std::cout);
        return 0;
    }
    auto validation = validate_config_before_model(config);
    if (!validation) {
        std::cerr << validation.error() << '\n';
        return 2;
    }

    std::vector<cch::agent::Message> history;
    cch::harness::session::JsonlSessionStore store;
    if (!config.resume_path.empty()) {
        auto loaded = cch::harness::session::JsonlSessionStore::load(config.resume_path);
        if (!loaded) {
            std::cerr << "could not resume session: " << loaded.error() << '\n';
            return 2;
        }
        if (!loaded.value().metadata.workspace.empty()) {
            if (config.workspace_explicit) {
                if (!same_workspace(config.workspace, loaded.value().metadata.workspace)) {
                    std::cerr << "resume workspace does not match session metadata; omit --workspace to use "
                              << loaded.value().metadata.workspace << " or start a new session\n";
                    return 2;
                }
            } else {
                config.workspace = loaded.value().metadata.workspace;
            }
        }
        auto workspace_validation = validate_workspace(config.workspace);
        if (!workspace_validation) {
            std::cerr << workspace_validation.error() << '\n';
            return 2;
        }
        config.workspace = canonical_workspace(config.workspace);
        history = loaded.value().messages;
        auto opened = cch::harness::session::JsonlSessionStore::open_existing(config.resume_path);
        if (!opened) {
            std::cerr << "could not open session for append: " << opened.error() << '\n';
            return 2;
        }
        store = std::move(opened.value());
    } else {
        auto workspace_validation = validate_workspace(config.workspace);
        if (!workspace_validation) {
            std::cerr << workspace_validation.error() << '\n';
            return 2;
        }
        config.workspace = canonical_workspace(config.workspace);
        auto path = config.session_path.empty() ? default_session_path() : config.session_path;
        cch::harness::session::SessionMetadata metadata{timestamp_for_path(), timestamp_for_path(), config.workspace, config.fake ? "fake" : "openai-compatible", config.model};
        auto created = cch::harness::session::JsonlSessionStore::create_new(path, metadata);
        if (!created) {
            std::cerr << "could not create session: " << created.error() << '\n';
            return 2;
        }
        store = std::move(created.value());
    }

    if (config.async_stack) {
        return cch::cli::run_async_cli(cch::cli::AsyncCliRuntimeConfig{
            config.fake,
            config.repl,
            config.enable_bash,
            config.max_turns,
            config.workspace,
            config.model,
            config.base_url,
            config.api_key_env,
            config.prompt,
        });
    }

    std::unique_ptr<cch::ai::ChatClient> client;
    if (config.fake) {
        client = std::make_unique<ScriptedFakeClient>();
    } else {
        cch::ai::providers::OpenAIConfig provider;
        provider.base_url = config.base_url;
        provider.api_key_env = config.api_key_env;
        provider.model = config.model;
        client = std::make_unique<cch::ai::providers::OpenAIChatClient>(std::make_shared<cch::ai::providers::BoostBeastHttpTransport>(), provider);
    }

    cch::agent::ToolRegistry registry;
    cch::tools::add_all_tools(registry);
    cch::agent::LoopOptions options;
    options.workspace = config.workspace;
    options.model = config.model;
    options.max_turns = config.max_turns;
    options.bash_enabled = config.enable_bash;
    options.secret_environment_names.push_back(config.api_key_env);
    options.on_event = print_event;
    options.on_message = [&](const cch::agent::Message& message) { return store.append(message); };

    cch::agent::AgentLoop loop(*client, std::move(registry), options);
    auto run_prompt = [&](const std::string& prompt) -> bool {
        auto result = loop.continue_with(history, prompt);
        if (!result) {
            std::cerr << "loop failed: " << result.error() << '\n';
            return false;
        }
        history = result.value().messages;
        std::cout << result.value().final_text << '\n';
        return true;
    };

    if (config.repl) {
        std::string line;
        while (std::cout << "> " && std::getline(std::cin, line)) {
            if (line == "exit" || line == "quit") {
                break;
            }
            if (line.empty()) {
                continue;
            }
            if (!run_prompt(line)) {
                return 1;
            }
        }
        return 0;
    }

    return run_prompt(config.prompt) ? 0 : 1;
}
