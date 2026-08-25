#pragma once

#include "coding_agent/runtime/AsyncUserShell.hpp"
#include "support/AsyncResultBridge.hpp"
#include "support/ReleaseGate.hpp"

#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <chrono>
#include <cstddef>
#include <exception>
#include <memory>
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
        std::optional<support::Error> infrastructure_failure;
        bool gated{false};
    };

    void enqueue(Execution execution) {
        executions_.push_back(std::move(execution));
    }

    void release() {
        gate_.release();
    }

    [[nodiscard]] support::AsyncResult<coding_agent::runtime::UserShellResult> execute(
        std::string command,
        coding_agent::runtime::UserShellUpdateSink update_sink,
        std::stop_token stop_token) override {
        return support::detail::make_async_result(
                [this, command = std::move(command), update_sink = std::move(update_sink), stop_token]() mutable
                        -> boost::asio::awaitable<support::Expected<coding_agent::runtime::UserShellResult>> {
                    commands.push_back(std::move(command));
                    const auto index = next_execution_++;
                    if (index >= executions_.size()) {
                        co_return std::unexpected(support::make_error(
                                support::ErrorCode::Process, "fake User Shell has no queued execution"));
                    }

                    auto& execution = executions_[index];
                    for (const auto& update : execution.updates) {
                        if (auto delivered = update_sink(update); !delivered) {
                            co_return std::unexpected(delivered.error());
                        }
                    }
                    ++started_count;

                    if (execution.gated) {
                        std::stop_callback cancellation{stop_token, [this] {
                                                            ++cancellation_request_count;
                                                            gate_.interrupt();
                                                        }};
                        // The stop check must precede wait(): an interrupt
                        // delivered before the gate is armed does not linger
                        // (ReleaseGate contract).
                        if (!stop_token.stop_requested()) {
                            co_await gate_.wait();
                        }
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

    // Observable state lives in shared storage: the Session uniquely owns the
    // shell and releases it when Close finalizes, so a test that asserts
    // post-close behavior holds a `counters()` copy instead of dereferencing
    // the released fake (ASan, issue #473).
    struct Counters {
        std::vector<std::string> commands;
        std::size_t started_count{0};
        std::size_t cancellation_request_count{0};
    };

    FakeUserShell()
        : counters_(std::make_shared<Counters>()),
          commands(counters_->commands),
          started_count(counters_->started_count),
          cancellation_request_count(counters_->cancellation_request_count) {}

    [[nodiscard]] std::shared_ptr<const Counters> counters() const { return counters_; }

private:
    // Declared before the reference members below so they bind to live
    // storage (members initialize in declaration order).
    std::shared_ptr<Counters> counters_;

public:
    // Non-owning references bound to `counters_` storage, which is shared and
    // outlives the shell (CODING_STANDARDS.md §7.5); they keep the pre-#473
    // field spelling source-compatible. Storing references deletes the move
    // ctor/assignment — the fake is always held by unique_ptr.
    std::vector<std::string>& commands;
    std::size_t& started_count;
    std::size_t& cancellation_request_count;

private:
    std::vector<Execution> executions_;
    std::size_t next_execution_{0};
    ReleaseGate gate_;
};

} // namespace cch::tests
