#pragma once

#include "coding_agent/AgentSession.hpp"

#include <boost/asio/awaitable.hpp>

#include <functional>
#include <memory>
#include <string>

namespace cch::coding_agent::detail {

/// Private access for coordinating adapter acknowledgement with the
/// AgentSession preflight boundary without widening the session handle contract.
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
        std::vector<ai::ImageContent> images,
        bool expand_prompt_templates,
        std::move_only_function<util::ExpectedVoid()> on_preflight_accepted);

    [[nodiscard]] static util::ExpectedVoid prompt_blocking(
        AgentSession& session,
        std::string text,
        std::vector<ai::ImageContent> images,
        bool expand_prompt_templates,
        std::move_only_function<util::ExpectedVoid()> on_preflight_accepted);

    /// Manual compaction (pi `AgentSession.compact`). Same impl_ copying
    /// contract as prompt(): the returned lazy awaitable survives moving or
    /// destroying the public handle before its first co_await.
    [[nodiscard]] static boost::asio::awaitable<util::Expected<CompactionResult>>
    compact(
        AgentSession& session,
        std::string custom_instructions);

    /// Runtime model switch (pi `AgentSession.setModel`). Same impl_ copying
    /// contract as prompt().
    [[nodiscard]] static boost::asio::awaitable<util::ExpectedVoid> set_model(
        AgentSession& session,
        ai::Model model);
    /// Blocking facade driving the async path on a temporary executor.
    [[nodiscard]] static util::ExpectedVoid set_model_blocking(
        AgentSession& session,
        ai::Model model);

    /// Runtime model cycle (pi `AgentSession.cycleModel`). Same impl_
    /// copying contract as prompt().
    [[nodiscard]] static boost::asio::awaitable<
        util::Expected<std::optional<ModelCycleResult>>>
    cycle_model(AgentSession& session, std::string direction);
    /// Blocking facade driving the async path on a temporary executor.
    [[nodiscard]] static util::Expected<std::optional<ModelCycleResult>>
    cycle_model_blocking(AgentSession& session, std::string direction);

private:
    [[nodiscard]] static boost::asio::awaitable<util::ExpectedVoid> prompt_impl(
        std::shared_ptr<AgentSession::Impl> impl,
        std::string text,
        std::vector<ai::ImageContent> images,
        bool expand_prompt_templates,
        std::move_only_function<util::ExpectedVoid()> on_preflight_accepted);

    [[nodiscard]] static boost::asio::awaitable<util::Expected<CompactionResult>>
    compact_impl(
        std::shared_ptr<AgentSession::Impl> impl,
        std::string custom_instructions);

    [[nodiscard]] static boost::asio::awaitable<util::ExpectedVoid> set_model_impl(
        std::shared_ptr<AgentSession::Impl> impl,
        ai::Model model);

    [[nodiscard]] static boost::asio::awaitable<
        util::Expected<std::optional<ModelCycleResult>>>
    cycle_model_impl(
        std::shared_ptr<AgentSession::Impl> impl,
        std::string direction);
};

} // namespace cch::coding_agent::detail
