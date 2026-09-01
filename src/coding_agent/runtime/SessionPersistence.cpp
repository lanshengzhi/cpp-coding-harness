#include "SessionPersistence.hpp"

#include <cch/agent/harness/session/SessionStore.hpp>
#include <cch/ai/Content.hpp>
#include <cch/support/AsyncResult.hpp>

#include "support/AsyncResultBridge.hpp"
#include "agent/harness/RuntimeRoot.hpp"

#include <cstddef>
#include <deque>
#include <exception>
#include <mutex>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace cch::coding_agent::runtime {

/// Conservative per-admitted-operation byte charge covering the encoded
/// Session Entry line and retained operation state, mirroring the execution
/// environment's admission overhead (ADR 0040 admission/overload).
inline constexpr std::size_t kAdmittedOperationOverheadBytes{4096};

struct SessionPersistence::PendingOp {
    harness::RuntimeTarget::Admission admission;
    ai::MessageVariant message;
};

struct SessionPersistence::State {
    std::shared_ptr<harness::session::SessionStore> store;
    std::shared_ptr<harness::RuntimeTarget> target;
    std::mutex mutex;
    bool in_flight{false};
    std::deque<PendingOp> queue;
    std::optional<support::Error> first_failure;
    std::vector<support::AsyncCompletion<void, support::Error>> drain_waiters;
};

namespace {

[[nodiscard]] std::size_t content_bytes(const ai::Content& content) {
    return std::visit(
        [](const auto& block) -> std::size_t {
            using Block = std::decay_t<decltype(block)>;
            if constexpr (std::is_same_v<Block, ai::TextContent>) {
                return block.text.size();
            } else if constexpr (std::is_same_v<Block, ai::ThinkingContent>) {
                return block.thinking.size();
            } else {
                return block.data.size();
            }
        },
        content);
}

[[nodiscard]] std::size_t assistant_content_bytes(const ai::AssistantContent& content) {
    return std::visit(
        [](const auto& block) -> std::size_t {
            using Block = std::decay_t<decltype(block)>;
            if constexpr (std::is_same_v<Block, ai::TextContent>) {
                return block.text.size();
            } else if constexpr (std::is_same_v<Block, ai::ThinkingContent>) {
                return block.thinking.size();
            } else {
                return block.raw_arguments.size();
            }
        },
        content);
}

/// Conservative encoded-size estimate for the admission byte charge: the
/// persisted text/image payload sizes. Only the three incrementally
/// persisted message kinds carry meaningful payload; the fixed per-operation
/// overhead covers the remaining entry fields.
[[nodiscard]] std::size_t estimate_message_bytes(const ai::MessageVariant& message) {
    return std::visit(
        [](const auto& variant) -> std::size_t {
            using Message = std::decay_t<decltype(variant)>;
            std::size_t bytes = 0;
            if constexpr (std::is_same_v<Message, ai::UserMessage>) {
                if (const auto* text = std::get_if<std::string>(&variant.content)) {
                    bytes += text->size();
                } else {
                    for (const auto& block : std::get<std::vector<ai::Content>>(
                             variant.content)) {
                        bytes += content_bytes(block);
                    }
                }
            } else if constexpr (std::is_same_v<Message, ai::AssistantMessage>) {
                for (const auto& block : variant.content) {
                    bytes += assistant_content_bytes(block);
                }
            } else if constexpr (std::is_same_v<Message, ai::ToolResultMessage>) {
                for (const auto& block : variant.content) {
                    bytes += content_bytes(block);
                }
            }
            return bytes;
        },
        message);
}

[[nodiscard]] support::Error busy_error(const char* detail) {
    return support::make_error(
        support::ErrorCode::Busy,
        "session persistence could not be scheduled",
        detail);
}

} // namespace

/// The admission always completes exactly once — even when the worker queue
/// is already stopping — so the target mailbox sequence can never stall
/// (ADR 0040). Requires `state->mutex` to be held.
void SessionPersistence::start_operation_locked(
    const std::shared_ptr<State>& state,
    PendingOp op) noexcept {
    state->in_flight = true;
    // Keep the pending operation in shared ownership so the worker task can
    // complete its admission without the post_worker call ever observing a
    // moved-from admission (mirror the filesystem operation state in
    // AsyncFileSystemOperations).
    auto shared = std::make_shared<PendingOp>(std::move(op));
    const bool queued = shared->admission.post_worker(
        [state, shared]() mutable noexcept {
            support::ExpectedVoid outcome;
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
            try {
#endif
                outcome = state->store->append(shared->message);
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
            } catch (const std::exception& error) {
                outcome = std::unexpected(support::make_error(
                    support::ErrorCode::Session,
                    "session persistence append failed",
                    error.what()));
            } catch (...) {
                outcome = std::unexpected(support::make_error(
                    support::ErrorCode::Session,
                    "session persistence append failed"));
            }
#endif
            std::move(shared->admission).complete(
                [state, outcome = std::move(outcome)]() mutable noexcept {
                    deliver_result(state, std::move(outcome));
                });
        });
    if (!queued) {
        std::move(shared->admission).complete([state]() mutable noexcept {
            deliver_result(
                state,
                std::unexpected(busy_error("runtime worker queue is stopping")));
        });
    }
}

/// Mailbox delivery on the RuntimeTarget's loop: latch the first failure,
/// start the next queued append, or mark the chain idle and release drain
/// waiters (fired outside the lock).
void SessionPersistence::deliver_result(
    const std::shared_ptr<State>& state,
    support::ExpectedVoid outcome) noexcept {
    std::vector<support::AsyncCompletion<void, support::Error>> waiters;
    {
        std::lock_guard lock(state->mutex);
        if (!outcome && !state->first_failure) {
            state->first_failure = outcome.error();
        }
        if (!state->queue.empty()) {
            auto next = std::move(state->queue.front());
            state->queue.pop_front();
            start_operation_locked(state, std::move(next));
        } else {
            state->in_flight = false;
            waiters.swap(state->drain_waiters);
        }
    }
    for (auto& waiter : waiters) {
        std::move(waiter)({});
    }
}

SessionPersistence::SessionPersistence(
    std::shared_ptr<harness::session::SessionStore> store,
    std::shared_ptr<harness::RuntimeTarget> target)
    : state_(std::make_shared<State>()) {
    state_->store = std::move(store);
    state_->target = std::move(target);
}

SessionPersistence::~SessionPersistence() = default;

support::ExpectedVoid SessionPersistence::submit_message_append(
    ai::MessageVariant message) {
    const auto byte_charge =
        estimate_message_bytes(message) + kAdmittedOperationOverheadBytes;
    std::lock_guard lock(state_->mutex);
    if (state_->first_failure) {
        return std::unexpected(*state_->first_failure);
    }
    auto admission = state_->target->try_admit_reserved(byte_charge);
    if (!admission) {
        auto error = busy_error("runtime admission capacity is saturated");
        state_->first_failure = error;
        return std::unexpected(std::move(error));
    }
    PendingOp op{
        .admission = std::move(*admission),
        .message = std::move(message),
    };
    if (state_->in_flight) {
        state_->queue.push_back(std::move(op));
        return {};
    }
    start_operation_locked(state_, std::move(op));
    return {};
}

boost::asio::awaitable<void> SessionPersistence::drain() {
    for (;;) {
        {
            std::lock_guard lock(state_->mutex);
            if (!state_->in_flight && state_->queue.empty()) {
                co_return;
            }
        }
        // Cross-executor resumption and abandonment are the designed
        // AsyncResult semantics (ADR 0040 / AsyncResultBridge).
        (void)co_await support::detail::await_async_result(
                support::AsyncResult<void>{support::AsyncProducer<void, support::Error>{
                        [state = state_](support::AsyncCompletion<void, support::Error> completion) mutable noexcept {
                            bool fire_now = false;
                            {
                                std::lock_guard lock(state->mutex);
                                if (!state->in_flight && state->queue.empty()) {
                                    fire_now = true;
                                } else {
                                    state->drain_waiters.push_back(std::move(completion));
                                }
                            }
                            if (fire_now) {
                                std::move(completion)({});
                            }
                        }}});
    }
}

std::optional<support::Error> SessionPersistence::failure() const {
    std::lock_guard lock(state_->mutex);
    return state_->first_failure;
}

} // namespace cch::coding_agent::runtime
