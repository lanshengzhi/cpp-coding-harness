#include "AgentSessionRunner.hpp"

#include "../../../include/cch/ai/Content.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>

#include <optional>
#include <utility>

namespace cch::coding_agent::runtime {

std::string terminal_code_for_loop_error(const std::string& message) {
    if (message == "max turns exceeded") {
        return "max_turns_exceeded";
    }
    return "runtime_error";
}

std::string display_message_for_loop_error(const std::string& message) {
    return message == "max turns exceeded" ? "max_turns_exceeded" : message;
}

AgentSessionRunner::AgentSessionRunner(
    ai::StreamingChatClient& client,
    agent::AsyncToolRegistry registry,
    agent::AsyncAgentOptions options)
    : loop_(client, std::move(registry), std::move(options)) {}

PromptRunResult AgentSessionRunner::run_prompt(
    std::vector<ai::MessageVariant>& history,
    harness::session::JsonlSessionStore& store,
    std::string prompt,
    agent::AgentEventSink sink) {
    boost::asio::io_context io;
    std::optional<util::Expected<agent::AsyncAgentRunResult>> result;
    const auto previous_size = history.size();

    boost::asio::co_spawn(
        io,
        [&]() -> boost::asio::awaitable<void> {
            result = co_await loop_.continue_with(history, std::move(prompt), std::move(sink));
            co_return;
        },
        boost::asio::detached);
    io.run();

    if (!result || !*result) {
        const auto message = result ? (*result).error().message : std::string{"async loop did not finish"};
        return PromptRunResult{
            false,
            terminal_code_for_loop_error(message),
            display_message_for_loop_error(message),
        };
    }

    auto new_history = std::move((*result)->context.messages);
    for (std::size_t index = previous_size; index < new_history.size(); ++index) {
        if (auto appended = store.append(new_history[index]); !appended) {
            return PromptRunResult{false, "session_persist_failed", "could not persist session entry"};
        }
    }

    history = std::move(new_history);
    return PromptRunResult{true, "completed", {}};
}

} // namespace cch::coding_agent::runtime
