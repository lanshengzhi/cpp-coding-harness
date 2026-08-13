#pragma once

#include <cch/agent/AgentTool.hpp>
#include <cch/ai/Tool.hpp>
#include <cch/util/Error.hpp>

#include "ai/AsyncResultBridge.hpp"

#include <boost/asio/co_spawn.hpp>

#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace cch::tests {

/// Build a passive `agent::Tool` whose execute operation co_spawns `body` (a
/// fresh awaitable per call) onto the consuming coroutine's executor (the
/// Agent loop's serialized domain) and bridges the terminal outcome into the
/// returned `AsyncResult`. `body` accepts the invocation, the stop token, and
/// the cumulative Tool Update sink (ignored by non-streaming fakes).
/// Coroutine starts are serialized so the shared move-only body is never
/// invoked concurrently; coroutine creation is cheap and non-blocking.
template <typename Body>
[[nodiscard]] agent::Tool make_fake_tool(
    ai::Tool definition,
    agent::ToolConcurrency concurrency,
    Body body,
    std::optional<std::string> prompt_snippet = std::nullopt,
    std::vector<std::string> prompt_guidelines = {}) {
    auto shared_body = std::make_shared<Body>(std::move(body));
    auto body_mutex = std::make_shared<std::mutex>();

    agent::Tool tool;
    tool.definition = std::move(definition);
    tool.concurrency = concurrency;
    tool.prompt_snippet = std::move(prompt_snippet);
    tool.prompt_guidelines = std::move(prompt_guidelines);
    tool.execute = [body_mutex, shared_body](
        agent::ToolInvocation invocation,
        std::stop_token stop_token,
        agent::ToolUpdateSink update_sink) -> agent::ToolExecuteResult {
        return agent::ToolExecuteResult{
            [body_mutex, shared_body,
             invocation = std::move(invocation),
             stop_token = std::move(stop_token),
             update_sink = std::move(update_sink)](
                agent::ToolExecuteResult::completion_type completion) mutable noexcept {
                auto executor = cch::ai::detail::t_initiating_executor;
                if (!executor) {
                    completion(std::unexpected(util::make_error(
                        util::ErrorCode::Tool, "tool execution has no initiating executor")));
                    return;
                }
                try {
                    boost::asio::awaitable<util::Expected<agent::AsyncToolExecutionResult>> coro;
                    {
                        std::lock_guard lock(*body_mutex);
                        coro = (*shared_body)(
                            std::move(invocation),
                            std::move(stop_token),
                            std::move(update_sink));
                    }
                    boost::asio::co_spawn(
                        executor,
                        std::move(coro),
                        [completion = std::move(completion)](
                            std::exception_ptr eptr,
                            util::Expected<agent::AsyncToolExecutionResult> result) mutable noexcept {
                            if (eptr) {
                                completion(std::unexpected(util::make_error(
                                    util::ErrorCode::Tool, "tool execution failed")));
                            } else {
                                completion(std::move(result));
                            }
                        });
                } catch (...) {
                    completion(std::unexpected(util::make_error(
                        util::ErrorCode::Tool, "tool execution failed")));
                }
            }};
    };
    return tool;
}

} // namespace cch::tests
