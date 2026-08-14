#pragma once

#include "coding_agent/runtime/AsyncUserShell.hpp"
#include "ai/AsyncResultBridge.hpp"

#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <chrono>
#include <cstddef>
#include <exception>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace cch::tests {

class FakeUserShell final : public coding_agent::runtime::AsyncUserShell {
public:
    struct Execution {
        std::vector<std::string> updates;
        coding_agent::runtime::UserShellResult result;
        std::optional<util::Error> infrastructure_failure;
        bool gated{false};
    };

    void enqueue(Execution execution) {
        executions_.push_back(std::move(execution));
    }

    void release() {
        if (gate_) {
            gate_->expires_at(std::chrono::steady_clock::time_point::min());
        }
    }

    [[nodiscard]] support::AsyncResult<coding_agent::runtime::UserShellResult> execute(
        std::string command,
        coding_agent::runtime::UserShellUpdateSink update_sink,
        std::stop_token stop_token) override {
        return ai::detail::make_async_result(
            [this,
             command = std::move(command),
             update_sink = std::move(update_sink),
             stop_token]() mutable
            -> boost::asio::awaitable<util::Expected<coding_agent::runtime::UserShellResult>> {
                commands.push_back(std::move(command));
                const auto index = next_execution_++;
                if (index >= executions_.size()) {
                    co_return std::unexpected(util::make_error(
                        util::ErrorCode::Process,
                        "fake User Shell has no queued execution"));
                }

                auto& execution = executions_[index];
                for (const auto& update : execution.updates) {
                    if (auto delivered = update_sink(update); !delivered) {
                        co_return std::unexpected(delivered.error());
                    }
                }
                ++started_count;

                if (execution.gated) {
                    const auto executor = co_await boost::asio::this_coro::executor;
                    gate_.emplace(executor);
                    gate_->expires_at(std::chrono::steady_clock::time_point::max());
                    std::stop_callback cancellation{stop_token, [this] {
                        ++cancellation_request_count;
                        if (gate_) {
                            try {
                                (void)gate_->cancel();
                            } catch (...) {
                            }
                        }
                    }};
                    boost::system::error_code error;
                    if (!stop_token.stop_requested()) {
                        co_await gate_->async_wait(
                            boost::asio::redirect_error(boost::asio::use_awaitable, error));
                    }
                    gate_.reset();
                    if (stop_token.stop_requested()) {
                        auto cancelled = execution.result;
                        cancelled.exit_code.reset();
                        cancelled.cancelled = true;
                        co_return cancelled;
                    }
                }

                if (execution.infrastructure_failure) {
                    co_return std::unexpected(*execution.infrastructure_failure);
                }
                co_return execution.result;
            });
    }

    std::vector<std::string> commands;
    std::size_t started_count{0};
    std::size_t cancellation_request_count{0};

private:
    std::vector<Execution> executions_;
    std::size_t next_execution_{0};
    std::optional<boost::asio::steady_timer> gate_;
};

} // namespace cch::tests
