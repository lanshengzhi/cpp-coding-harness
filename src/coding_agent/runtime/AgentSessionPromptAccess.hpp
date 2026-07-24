#pragma once

#include "../../../include/cch/coding_agent/Sdk.hpp"

#include <boost/asio/awaitable.hpp>

#include <functional>
#include <memory>
#include <string>

namespace cch::coding_agent::detail {

/// Private access for coordinating adapter acknowledgement with the
/// AgentSession preflight boundary without widening the public SDK contract.
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
