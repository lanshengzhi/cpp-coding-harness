#pragma once

#include "../../../include/cch/coding_agent/Sdk.hpp"

#include <boost/asio/awaitable.hpp>

#include <functional>
#include <memory>
#include <string>

namespace cch::coding_agent::detail {

/// Private access for coordinating adapter acknowledgement with the
/// AgentSession preflight boundary without widening the public SDK contract.
///
/// prompt is deliberately an ordinary function (no coroutine keywords): the
/// session impl_ copy enters the prompt_impl frame synchronously at the call,
/// so a host may move or destroy the public session handle before the first
/// co_await without invalidating the returned lazy awaitable.
class AgentSessionPromptAccess {
public:
    [[nodiscard]] static boost::asio::awaitable<util::ExpectedVoid> prompt(
        AgentSession& session,
        std::string text,
        bool expand_prompt_templates,
        std::move_only_function<util::ExpectedVoid()> on_preflight_accepted);

    [[nodiscard]] static util::ExpectedVoid prompt_blocking(
        AgentSession& session,
        std::string text,
        bool expand_prompt_templates,
        std::move_only_function<util::ExpectedVoid()> on_preflight_accepted);

private:
    [[nodiscard]] static boost::asio::awaitable<util::ExpectedVoid> prompt_impl(
        std::shared_ptr<AgentSession::Impl> impl,
        std::string text,
        bool expand_prompt_templates,
        std::move_only_function<util::ExpectedVoid()> on_preflight_accepted);
};

} // namespace cch::coding_agent::detail
