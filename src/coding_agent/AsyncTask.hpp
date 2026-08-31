#pragma once

#include <cch/support/AsyncResult.hpp>

#include <coroutine>
#include <expected>
#include <exception>
#include <memory>
#include <optional>
#include <utility>

namespace cch::coding_agent::detail {

/// Small coroutine implementation detail for composing Owner Interface
/// AsyncResults. The public loader contracts expose only support::AsyncResult;
/// no coroutine, executor, or scheduler type crosses the Owner boundary.
template <typename T, typename E>
class AsyncTask {
public:
    using completion_type = support::AsyncCompletion<T, E>;

    struct State {
        std::coroutine_handle<> handle;
        completion_type completion;

        ~State() {
            if (handle) {
                handle.destroy();
            }
        }
    };

    struct promise_type {
        std::shared_ptr<State> state;
        std::optional<std::expected<T, E>> outcome;

        [[nodiscard]] AsyncTask get_return_object() {
            auto state = std::make_shared<State>();
            const auto handle = std::coroutine_handle<promise_type>::from_promise(*this);
            state->handle = handle;
            this->state = state;
            return AsyncTask{std::move(state)};
        }

        [[nodiscard]] static std::suspend_always initial_suspend() noexcept {
            return {};
        }

        struct FinalAwaiter {
            [[nodiscard]] static bool await_ready() noexcept { return false; }
            void await_suspend(std::coroutine_handle<promise_type> handle) noexcept {
                auto state = handle.promise().state;
                auto completion = std::move(state->completion);
                auto outcome = std::move(*handle.promise().outcome);
                state->handle = {};
                handle.promise().state.reset();
                if (completion) {
                    completion(std::move(outcome));
                }
                handle.destroy();
            }
            static void await_resume() noexcept {}
        };

        [[nodiscard]] static FinalAwaiter final_suspend() noexcept { return {}; }

        static void unhandled_exception() noexcept { std::terminate(); }

        void return_value(std::expected<T, E> value) noexcept {
            outcome.emplace(std::move(value));
        }

        void return_value(std::unexpected<E> value) noexcept {
            outcome.emplace(std::move(value));
        }

        void return_value(T value) noexcept {
            outcome.emplace(std::move(value));
        }
    };

    AsyncTask(AsyncTask&&) noexcept = default;
    AsyncTask& operator=(AsyncTask&&) noexcept = default;
    AsyncTask(const AsyncTask&) = delete;
    AsyncTask& operator=(const AsyncTask&) = delete;

    void start(completion_type completion) noexcept {
        auto state = std::move(state_);
        state->completion = std::move(completion);
        state->handle.resume();
    }

private:
    explicit AsyncTask(std::shared_ptr<State> state) noexcept
        : state_(std::move(state)) {}

    std::shared_ptr<State> state_;
};

template <typename T, typename E>
[[nodiscard]] support::AsyncResult<T, E> to_async_result(
    AsyncTask<T, E> task) {
    return support::AsyncResult<T, E>{
        support::AsyncProducer<T, E>{
            [task = std::move(task)](
                typename AsyncTask<T, E>::completion_type completion) mutable noexcept {
                task.start(std::move(completion));
            }}};
}

} // namespace cch::coding_agent::detail
