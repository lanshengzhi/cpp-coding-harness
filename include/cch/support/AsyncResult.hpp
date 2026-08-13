#pragma once

#include <cch/support/Error.hpp>

#include <atomic>
#include <coroutine>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <variant>

namespace cch::support {

/// Terminal delivery callback for a pending operation. Move-only and
/// `noexcept`: the producer delivers exactly one `std::expected<T, E>` and a
/// throwing or duplicate completion is an invariant violation (ADR 0040
/// §AsyncResult). A stored callback's copyability says nothing about the
/// lifetime of its referents (CODING_STANDARDS.md §6.2).
template <typename T, typename E>
using AsyncCompletion = std::move_only_function<void(std::expected<T, E>) noexcept>;

/// Producer of a pending operation. Invoked exactly once at initiation with
/// the terminal completion; after initiation returns it owns everything it
/// needs. Initiation is `noexcept`.
template <typename T, typename E>
using AsyncProducer = std::move_only_function<void(AsyncCompletion<T, E>) noexcept>;

/// Lazy, move-only, single-consumption operation result (ADR 0040 §AsyncResult).
///
/// One shape represents both completion paths:
///   * a ready value constructed from `std::expected<T, E>` completes inline
///     with no allocation or coroutine suspension;
///   * a pending operation constructed from an `AsyncProducer` may allocate and
///     completes once, delivering at most one terminal `std::expected<T, E>`.
///
/// A result is consumed exactly once, either by move-only callback `start` or
/// by move-only `co_await`. Duplicate, moved-from, or reused consumption; a
/// producer that completes twice or is empty; and a completion that throws all
/// call `std::terminate` in Debug and Release.
///
/// `AsyncResult` owns no execution machinery: it introduces no event loop,
/// thread pool, event bus, timeout, retry, stream, or cancellation outcome.
/// Cancellation is supplied explicitly by the producer with `std::stop_token`
/// and resolved into the operation's own terminal outcome.
template <typename T, typename E = Error>
class AsyncResult {
public:
    using completion_type = AsyncCompletion<T, E>;
    using producer_type = AsyncProducer<T, E>;

    /// Ready path: completes inline, no allocation.
    explicit AsyncResult(std::expected<T, E> ready) : state_(std::in_place_index<1>, std::move(ready)) {}

    /// Pending path: owns the producer. The producer must be non-empty.
    explicit AsyncResult(producer_type producer) : state_(std::in_place_index<2>, std::move(producer)) {}

    // A moved-from result is reset to the consumed `monostate`, so consuming a
    // moved-from result is distinguishable from a valid one and terminates.
    AsyncResult(AsyncResult&& other) noexcept(
        std::is_nothrow_move_constructible_v<
            std::variant<std::monostate, std::expected<T, E>, producer_type>>)
        : state_(std::move(other.state_)) {
        other.state_ = std::monostate{};
    }
    AsyncResult& operator=(AsyncResult&& other) noexcept(
        std::is_nothrow_move_assignable_v<
            std::variant<std::monostate, std::expected<T, E>, producer_type>>) {
        if (this != &other) {
            state_ = std::move(other.state_);
            other.state_ = std::monostate{};
        }
        return *this;
    }
    ~AsyncResult() = default;

    AsyncResult(const AsyncResult&) = delete;
    AsyncResult& operator=(const AsyncResult&) = delete;

    /// Consume by move-only callback. Ready values complete inline; a pending
    /// value initiates the producer, which owns `completion` thereafter.
    void start(completion_type completion) noexcept;

    /// Consume by move-only `co_await`. `await_resume()` yields the terminal
    /// `std::expected<T, E>`; a ready value does not suspend.
    auto operator co_await() && noexcept;

private:
    // Shared control block between a suspended awaiter and the producer's
    // completion callback. Heap-allocated only for the pending path; a ready
    // value never creates one. `completed` preserves the producer's
    // at-most-once contract; `abandoned` is set by the awaiter's destructor
    // once the coroutine frame is gone, so a late first completion is accepted
    // and discarded rather than resuming a dead frame (ADR 0040).
    struct PendingState {
        std::coroutine_handle<> handle;
        std::atomic<bool> completed{false};
        std::atomic<bool> abandoned{false};
        std::optional<std::expected<T, E>> result;
    };

    // monostate = consumed/moved-from; index 1 = ready value; index 2 = producer.
    std::variant<std::monostate, std::expected<T, E>, producer_type> state_;
};

template <typename T, typename E>
void AsyncResult<T, E>::start(completion_type completion) noexcept {
    using state_type = std::variant<std::monostate, std::expected<T, E>, producer_type>;
    auto state = std::exchange(state_, state_type{std::monostate{}});

    if (auto* ready = std::get_if<std::expected<T, E>>(&state)) {
        completion(std::move(*ready));
        return;
    }
    if (auto* producer = std::get_if<producer_type>(&state)) {
        if (!*producer) {
            std::terminate(); // empty producer is an invariant violation
        }
        // Wrap the completion to enforce the producer's at-most-once contract
        // on the callback path as well. Pending state may allocate.
        auto delivered = std::make_shared<std::atomic<bool>>(false);
        completion_type guarded =
            [flag = delivered, done = std::move(completion)](std::expected<T, E> outcome) mutable noexcept {
                if (flag->exchange(true, std::memory_order_acq_rel)) {
                    std::terminate(); // duplicate completion: at-most-once violation
                }
                done(std::move(outcome));
            };
        std::move(*producer)(std::move(guarded));
        return;
    }
    std::terminate(); // consumed, moved-from, or reused
}

template <typename T, typename E>
auto AsyncResult<T, E>::operator co_await() && noexcept {
    struct Awaiter {
        std::variant<std::monostate, std::expected<T, E>, producer_type> state;
        std::optional<std::expected<T, E>> ready;
        std::shared_ptr<PendingState> pending;

        explicit Awaiter(std::variant<std::monostate, std::expected<T, E>, producer_type> value)
            : state(std::move(value)) {}

        bool await_ready() noexcept {
            if (std::holds_alternative<std::expected<T, E>>(state)) {
                ready = std::move(std::get<std::expected<T, E>>(state));
                state = std::monostate{};
                return true;
            }
            return false;
        }

        void await_suspend(std::coroutine_handle<> handle) noexcept {
            auto state = std::exchange(
                this->state,
                std::variant<std::monostate, std::expected<T, E>, producer_type>{std::monostate{}});
            auto* producer = std::get_if<producer_type>(&state);
            if (producer == nullptr || !*producer) {
                std::terminate(); // consumed, moved-from, or empty producer
            }

            pending = std::make_shared<PendingState>();
            pending->handle = handle;

            completion_type completion = [slot = pending](std::expected<T, E> outcome) noexcept {
                const bool first = !slot->completed.exchange(true, std::memory_order_acq_rel);
                if (!first) {
                    std::terminate(); // duplicate completion: at-most-once violation
                }
                if (slot->abandoned.load(std::memory_order_acquire)) {
                    return; // late first completion after abandonment is discarded safely
                }
                slot->result = std::move(outcome);
                slot->handle.resume();
            };

            std::move(*producer)(std::move(completion));
        }

        std::expected<T, E> await_resume() noexcept {
            if (pending != nullptr) {
                if (!pending->result.has_value()) {
                    std::terminate(); // resumed before completion is an invariant violation
                }
                return std::move(*pending->result);
            }
            return std::move(*ready);
        }

        ~Awaiter() {
            if (pending != nullptr) {
                // The frame is gone; a later first completion must not resume it.
                pending->abandoned.store(true, std::memory_order_release);
            }
        }

        Awaiter(const Awaiter&) = delete;
        Awaiter& operator=(const Awaiter&) = delete;
        Awaiter(Awaiter&&) = default;
        Awaiter& operator=(Awaiter&&) = default;
    };

    return Awaiter(std::move(state_));
}

} // namespace cch::support
