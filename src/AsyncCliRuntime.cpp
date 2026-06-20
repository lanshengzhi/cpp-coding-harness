#include "AsyncCliRuntime.hpp"

#include "../include/cch/agent/AgentLoop.hpp"
#include "../include/cch/ai/Content.hpp"
#include "coding_agent/runtime/EventPrinter.hpp"
#include "coding_agent/runtime/JsonEventPrinter.hpp"
#include "coding_agent/runtime/RuntimeServices.hpp"
#include "coding_agent/runtime/SessionLifecycle.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>

#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace cch::cli {
namespace {

[[nodiscard]] bool is_json_mode(OutputMode mode) {
    return mode == OutputMode::Json;
}

[[nodiscard]] std::string terminal_code_for_loop_error(const std::string& message) {
    if (message == "max turns exceeded") {
        return "max_turns_exceeded";
    }
    return "runtime_error";
}

[[nodiscard]] std::string text_error_for_loop_error(const std::string& message) {
    return message == "max turns exceeded" ? "max_turns_exceeded" : message;
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

    auto services = coding_agent::runtime::make_runtime_services(coding_agent::runtime::RuntimeServicesConfig{
        workspace,
        config.enable_bash,
        provider_name,
        config.model,
        config.base_url,
        config.api_key_env,
    });
    if (!services) {
        if (json_mode && json_printer) {
            (void)print_json_terminal(*json_printer, false, "runtime_service_failed", "could not create runtime services");
        } else {
            std::cerr << "could not create runtime services: " << services.error().message << ": " << services.error().detail << '\n';
        }
        return 2;
    }

    agent::AsyncAgentLoop loop(*services->client, std::move(services->tools), agent::AsyncAgentOptions{config.max_turns, config.model});

    auto run_prompt = [&](const std::string& prompt) -> bool {
        boost::asio::io_context io;
        std::optional<boost::asio::io_context> print_io;
        std::optional<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> print_work;
        std::optional<std::jthread> print_thread;
        if (!json_mode) {
            print_io.emplace();
            print_work.emplace(boost::asio::make_work_guard(*print_io));
            print_thread.emplace([&]() { print_io->run(); });
        }

        std::optional<util::Expected<agent::AsyncAgentRunResult>> result;
        const auto previous_size = history.size();
        boost::asio::co_spawn(
            io,
            [&]() -> boost::asio::awaitable<void> {
                result = co_await loop.continue_with(history, prompt, [&](const agent::AgentLifecycleEvent& event) -> util::ExpectedVoid {
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
                co_return;
            },
            boost::asio::detached);
        io.run();
        if (!json_mode) {
            print_work->reset();
            print_thread->join();
        }
        std::cout.flush();
        if (!result || !*result) {
            const auto message = result ? (*result).error().message : std::string{"async loop did not finish"};
            if (json_mode) {
                (void)print_json_terminal(*json_printer, false, terminal_code_for_loop_error(message), text_error_for_loop_error(message));
            } else {
                std::cerr << "loop failed: " << text_error_for_loop_error(message) << '\n';
            }
            return false;
        }
        history = (*result)->context.messages;
        for (std::size_t index = previous_size; index < history.size(); ++index) {
            if (auto appended = store.append(history[index]); !appended) {
                if (json_mode) {
                    (void)print_json_terminal(*json_printer, false, "session_persist_failed", "could not persist session entry");
                } else {
                    std::cerr << "could not persist session entry: " << appended.error().message << ": " << appended.error().detail << '\n';
                }
                return false;
            }
        }
        if (json_mode) {
            return print_json_terminal(*json_printer, true, "completed");
        }
        if (const auto* final_message = std::get_if<ai::AssistantMessage>(&history.back())) {
            std::cout << ai::text_from_assistant_content(final_message->content) << '\n';
        }
        return true;
    };

    if (config.repl) {
        std::string line;
        while (std::cout << "> " && std::getline(std::cin, line)) {
            if (line == "exit" || line == "quit") break;
            if (!line.empty() && !run_prompt(line)) return 1;
        }
        return 0;
    }

    return run_prompt(config.prompt) ? 0 : 1;
}

} // namespace cch::cli
