#pragma once

#include <cch/ai/Message.hpp>
#include <cch/coding_agent/Sdk.hpp>
#include <cch/util/Error.hpp>
#include "coding_agent/runtime/AgentSessionInteractiveAccess.hpp"
#include "coding_agent/runtime/UserBash.hpp"

#include "../../third_party/catch2/catch_test_macros.hpp"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace cch::tests {

[[nodiscard]] inline std::size_t bash_message_count(
    const std::vector<ai::MessageVariant>& messages) {
    std::size_t count = 0;
    for (const auto& message : messages) {
        if (std::holds_alternative<ai::BashExecutionMessage>(message)) ++count;
    }
    return count;
}

using PromptResult = std::optional<util::ExpectedVoid>;
using BashResult =
    std::optional<util::Expected<coding_agent::runtime::UserBashCompletion>>;

inline void spawn_prompt(
    boost::asio::io_context& io,
    coding_agent::AgentSession& session,
    std::string text,
    PromptResult& slot) {
    boost::asio::co_spawn(
        io,
        session.prompt(std::move(text)),
        [&slot](std::exception_ptr exception, util::ExpectedVoid result) {
            REQUIRE(exception == nullptr);
            slot.emplace(std::move(result));
        });
}

inline void spawn_bash(
    boost::asio::io_context& io,
    coding_agent::AgentSession& session,
    std::string command,
    BashResult& slot) {
    boost::asio::co_spawn(
        io,
        coding_agent::detail::AgentSessionInteractiveAccess::run_user_bash(
            session,
            std::move(command),
            false,
            [](const coding_agent::runtime::UserBashProgress&) {
                return util::ExpectedVoid{};
            }),
        [&slot](
            std::exception_ptr exception,
            util::Expected<coding_agent::runtime::UserBashCompletion> result) {
            REQUIRE(exception == nullptr);
            slot.emplace(std::move(result));
        });
}

} // namespace cch::tests
