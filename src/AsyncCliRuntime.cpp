#include "AsyncCliRuntime.hpp"

#include "../include/cch/agent/AgentLoop.hpp"
#include "coding_agent/runtime/EventPrinter.hpp"
#include "coding_agent/runtime/RuntimeServices.hpp"
#include "coding_agent/runtime/SessionLifecycle.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>

#include <iostream>
#include <optional>
#include <string>
#include <utility>

namespace cch::cli {

int run_async_cli(const AsyncCliRuntimeConfig& config) {
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

    auto services = coding_agent::runtime::make_runtime_services(coding_agent::runtime::RuntimeServicesConfig{
        workspace,
        config.enable_bash,
        provider_name,
        config.model,
        config.base_url,
        config.api_key_env,
    });
    if (!services) {
        std::cerr << "could not create runtime services: " << services.error().message << ": " << services.error().detail << '\n';
        return 2;
    }

    agent::AsyncAgentLoop loop(*services->client, std::move(services->tools), agent::AsyncAgentOptions{config.max_turns, config.model});

    auto run_prompt = [&](const std::string& prompt) -> bool {
        boost::asio::io_context io;
        std::optional<util::Expected<agent::AsyncAgentRunResult>> result;
        const auto previous_size = history.size();
        boost::asio::co_spawn(
            io,
            [&]() -> boost::asio::awaitable<void> {
                result = co_await loop.continue_with(history, prompt, [](const agent::AgentLifecycleEvent& event) {
                    coding_agent::runtime::print_agent_event(event, std::cout);
                    return util::ExpectedVoid{};
                });
                co_return;
            },
            boost::asio::detached);
        io.run();
        if (!result || !*result) {
            const auto message = result ? (*result).error().message : std::string{"async loop did not finish"};
            std::cerr << "loop failed: " << (message == "max turns exceeded" ? "max_turns_exceeded" : message) << '\n';
            return false;
        }
        history = (*result)->context.messages;
        for (std::size_t index = previous_size; index < history.size(); ++index) {
            if (auto appended = store.append(history[index]); !appended) {
                std::cerr << "could not persist session entry: " << appended.error().message << ": " << appended.error().detail << '\n';
                return false;
            }
        }
        const auto& final_message = std::get<ai::AssistantMessage>(history.back());
        std::cout << coding_agent::runtime::text_from_content(final_message.content) << '\n';
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
