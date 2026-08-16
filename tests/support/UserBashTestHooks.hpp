#pragma once

#include <cch/ai/Message.hpp>
#include "coding_agent/AgentSession.hpp"
#include <cch/support/Error.hpp>
#include "coding_agent/runtime/AgentSessionInteractiveAccess.hpp"
#include "coding_agent/runtime/UserBash.hpp"
#include "support/PumpUntil.hpp"

#include <catch2/catch_test_macros.hpp>
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

using PromptResult = std::optional<support::ExpectedVoid>;
using BashResult =
    std::optional<support::Expected<coding_agent::runtime::UserBashCompletion>>;

/// Assert a spawned prompt/bash completes, pumping the loop until its
/// completion handler posts back. Completions cross Runtime worker and
/// mailbox hops, so plain drain_ready can go idle while a completion is
/// still in flight (PumpUntil.hpp); a fixed drain-then-assert sequence is a
/// load-sensitive race (the issue-87/88 flake class).
template <typename Slot>
inline void require_completed(boost::asio::io_context& io, const Slot& slot) {
    REQUIRE(pump_until(io, [&slot] { return slot.has_value(); }));
}

inline void spawn_prompt(
    boost::asio::io_context& io,
    coding_agent::AgentSession& session,
    std::string text,
    PromptResult& slot) {
    boost::asio::co_spawn(
        io,
        session.prompt(std::move(text)),
        [&slot](std::exception_ptr exception, support::ExpectedVoid result) {
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
                return support::ExpectedVoid{};
            }),
        [&slot](
            std::exception_ptr exception,
            support::Expected<coding_agent::runtime::UserBashCompletion> result) {
            REQUIRE(exception == nullptr);
            slot.emplace(std::move(result));
        });
}

} // namespace cch::tests
