#include "AsyncCliRuntime.hpp"

#include "../include/cch/ai/Content.hpp"
#include "../include/cch/coding_agent/Config.hpp"
#include "coding_agent/runtime/AgentSessionRunner.hpp"
#include "coding_agent/runtime/EventPrinter.hpp"
#include "coding_agent/runtime/JsonEventPrinter.hpp"
#include "coding_agent/runtime/RpcMode.hpp"
#include "coding_agent/runtime/RuntimeServices.hpp"
#include "coding_agent/runtime/SessionLifecycle.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>

#include <cstdlib>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace cch::cli {
namespace {

[[nodiscard]] bool is_json_mode(OutputMode mode) {
    return mode == OutputMode::Json;
}

[[nodiscard]] bool is_rpc_mode(OutputMode mode) {
    return mode == OutputMode::Rpc;
}

bool print_json_terminal(coding_agent::runtime::JsonEventPrinter& printer, bool success, std::string code, std::string message = {}) {
    if (auto printed = printer.print_terminal(success, std::move(code), std::move(message)); !printed) {
        std::cerr << "event printer failed: " << printed.error().message << '\n';
        return false;
    }
    std::cout.flush();
    return true;
}

} // namespace

int run_async_cli(const AsyncCliRuntimeConfig& config) {
    const auto json_mode = is_json_mode(config.output_mode);
    const auto provider_name = std::string{config.fake ? "fake" : "openai-compatible"};
    auto opened_session = coding_agent::runtime::open_session(coding_agent::runtime::SessionOpenRequest{
        config.session_path,
        config.resume_path,
        config.workspace,
        config.workspace_explicit,
        config.session_id,
        config.created_at,
        provider_name,
        config.model,
    });
    if (!opened_session) {
        if (opened_session.error().message == "resume workspace does not match session metadata") {
            std::cerr << opened_session.error().detail << '\n';
        } else if (!config.resume_path.empty()) {
            std::cerr << "could not resume session: " << opened_session.error().message << ": " << opened_session.error().detail << '\n';
        } else {
            std::cerr << "could not create session: " << opened_session.error().message << ": " << opened_session.error().detail << '\n';
        }
        return 2;
    }

    auto workspace = opened_session->workspace;
    auto history = std::move(opened_session->history);
    auto store = std::move(opened_session->store);

    std::optional<coding_agent::runtime::JsonEventPrinter> json_printer;
    if (json_mode) {
        json_printer.emplace(std::cout);
        if (auto printed = json_printer->print_session_header(store.metadata()); !printed) {
            std::cerr << "event printer failed: " << printed.error().message << '\n';
            return 2;
        }
    }

    // Load config file defaults; CLI values take precedence.
    const char* home = std::getenv("HOME");
    std::string config_path = home ? std::string(home) + "/.cpp-harness/config.json" : "";
    auto config_data = coding_agent::ConfigLoader::load(config_path);
    if (!config_data) {
        std::cerr << "warning: could not load config: " << config_data.error().message << '\n';
        config_data = coding_agent::ConfigData{};
    }

    static const std::map<std::string, std::string> default_model{
        {"openai-compatible", "gpt-4.1-mini"},
        {"fake", "fake-model"},
    };
    auto provider_default_model = [&](const std::string& provider) -> std::string {
        auto it = default_model.find(provider);
        return it != default_model.end() ? it->second : "gpt-4.1-mini";
    };

    std::string resolved_provider = provider_name;
    if (config_data->provider && resolved_provider == "openai-compatible" && !config.fake) {
        resolved_provider = *config_data->provider;
    }

    std::string resolved_model = config.model;
    if (resolved_model.empty() && opened_session->stored_model) {
        resolved_model = *opened_session->stored_model;
    }
    if (resolved_model.empty() && config_data->model) resolved_model = *config_data->model;
    if (resolved_model.empty()) resolved_model = provider_default_model(resolved_provider);

    std::string resolved_base_url = config.base_url;
    if (resolved_base_url.empty() && config_data->base_url) resolved_base_url = *config_data->base_url;
    if (resolved_base_url.empty()) resolved_base_url = "https://api.openai.com";

    std::string resolved_api_key_env = config.api_key_env;
    if (resolved_api_key_env.empty() && config_data->api_key_env) {
        resolved_api_key_env = config_data->api_key_env->front();
        auto resolved_key = coding_agent::ConfigLoader::resolve_api_key(*config_data->api_key_env);
        if (resolved_key) {
            for (const auto& env_name : *config_data->api_key_env) {
                const char* val = std::getenv(env_name.c_str());
                if (val && val[0] != '\0') {
                    resolved_api_key_env = env_name;
                    break;
                }
            }
        }
    }
    if (resolved_api_key_env.empty()) resolved_api_key_env = "OPENAI_API_KEY";

    auto services = coding_agent::runtime::make_runtime_services(coding_agent::runtime::RuntimeServicesConfig{
        workspace,
        config.enable_bash,
        resolved_provider,
        resolved_model,
        resolved_base_url,
        resolved_api_key_env,
    });
    if (!services) {
        if (json_mode && json_printer) {
            (void)print_json_terminal(*json_printer, false, "runtime_service_failed", "could not create runtime services");
        } else {
            std::cerr << "could not create runtime services: " << services.error().message << ": " << services.error().detail << '\n';
        }
        return 2;
    }

    coding_agent::CommandRegistry command_registry;
    register_builtin_commands(command_registry);

    coding_agent::runtime::AgentSessionRunner runner(
        *services->client,
        std::move(services->tools),
        agent::AsyncAgentOptions{config.max_turns, resolved_model},
        {} /* templates — empty until U3 */,
        &command_registry);

    if (is_rpc_mode(config.output_mode)) {
        return coding_agent::runtime::run_rpc_mode(coding_agent::runtime::RpcModeConfig{
            std::cin,
            std::cout,
            history,
            store,
            runner,
            resolved_provider,
            resolved_model,
            workspace,
        });
    }

    auto run_prompt = [&](const std::string& prompt) -> bool {
        std::optional<boost::asio::io_context> print_io;
        std::optional<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> print_work;
        std::optional<std::jthread> print_thread;
        if (!json_mode) {
            print_io.emplace();
            print_work.emplace(boost::asio::make_work_guard(*print_io));
            print_thread.emplace([&]() { print_io->run(); });
        }

        auto result = runner.run_prompt(
            history,
            store,
            prompt,
            [&](const agent::AgentLifecycleEvent& event) -> util::ExpectedVoid {
                if (json_mode) {
                    return json_printer->print_agent_event(event);
                }
                try {
                    boost::asio::post(*print_io, [event]() {
                        try {
                            coding_agent::runtime::print_agent_event(event, std::cout);
                        } catch (const std::exception& e) {
                            std::cerr << "event printer failed: " << e.what() << '\n';
                        }
                    });
                    return util::ExpectedVoid{};
                } catch (const std::exception& e) {
                    return std::unexpected(util::make_error(util::ErrorCode::Tool, "event printer failed", e.what()));
                }
            });

        if (!json_mode) {
            print_work->reset();
            print_thread->join();
        }
        std::cout.flush();
        if (!result.success) {
            if (json_mode) {
                (void)print_json_terminal(*json_printer, false, result.code, result.message);
            } else if (result.code == "session_persist_failed") {
                std::cerr << result.message << '\n';
            } else {
                std::cerr << "loop failed: " << result.message << '\n';
            }
            return false;
        }
        if (json_mode) {
            return print_json_terminal(*json_printer, true, result.code);
        }
        if (!history.empty()) {
            if (const auto* final_message = std::get_if<ai::AssistantMessage>(&history.back())) {
                std::cout << ai::text_from_assistant_content(final_message->content) << '\n';
            }
        }
        return true;
    };

    if (config.repl) {
        std::string line;
        while (std::cout << "> " && std::getline(std::cin, line)) {
            if (line == "exit" || line == "quit") break;
            if (line.empty()) continue;

            // Intercept slash-commands and shell passthrough
            if (!line.empty() && (line[0] == '/' || line[0] == '!')) {
                coding_agent::CommandContext cmd_ctx{
                    .session_id = store.metadata().session_id,
                    .workspace_path = store.metadata().workspace.string(),
                    .provider = resolved_provider,
                    .model = resolved_model,
                    .message_count = history.size(),
                };
                auto processed = coding_agent::process_prompt(line, {} /* templates */, command_registry, cmd_ctx);
                if (processed.command_handled) {
                    if (processed.display_text) {
                        std::cout << *processed.display_text << '\n';
                    }
                    if (processed.shutdown_requested) {
                        return 0;
                    }
                    continue; // back to REPL prompt
                }
                // Template expanded — pass expanded text to run_prompt
                line = std::move(processed.expanded_prompt);
            }

            if (!run_prompt(line)) return 1;
        }
        return 0;
    }

    return run_prompt(config.prompt) ? 0 : 1;
}

} // namespace cch::cli
