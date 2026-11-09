#pragma once

#include <boost/asio/awaitable.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <type_traits>
#include <utility>
#include <vector>

namespace cch::codemode::detail {

// A witness covers participants and registered stop callbacks, including the
// destruction of callback-owned captures. It does not cover terminal delivery
// or work transferred outside this accounting domain. Abandoned participants
// deliberately stay pending. Calls on the same Participant require serialization.
class RemoteRetirement {
    struct Waiter {
        explicit Waiter(boost::asio::any_io_executor executor) : channel(std::move(executor), 1) {}
        // Capacity one: the publisher uses try_send, so a duplicate notification is dropped.
        boost::asio::experimental::concurrent_channel<void(boost::system::error_code, bool)> channel;
    };

    struct Publication {
        bool published = false;
        std::vector<std::weak_ptr<Waiter>> waiters;
    };

    struct State {
        std::mutex mutex;
        std::stop_source stop;
        std::promise<void> completion;
        std::shared_future<void> future = completion.get_future().share();
        std::size_t active = 0;
        bool open = true;
        bool published = false;
        std::size_t delivery_failures = 0;
#ifdef CCH_REMOTE_RETIREMENT_TESTING
        // Test seam: runs immediately before each post, outside the mutex.
        void (*before_post)() = nullptr;
#endif
        std::vector<std::weak_ptr<Waiter>> waiters;

        // Called with mutex held; detached waiters are notified after unlocking.
        [[nodiscard]] Publication publish_if_ready() {
            if (open || active != 0 || published) return {};
            published = true;
            return {true, std::move(waiters)};
        }

        // Neither promise completion, executor nor injection hook runs under mutex. The
        // publication latch in publish_if_ready makes delivery happen at most once.
        void notify(Publication pending) {
            if (!pending.published) return;
            completion.set_value();
            for (auto& weak : pending.waiters) {
                if (auto waiter = weak.lock()) {
#if defined(__cpp_exceptions)
                    try {
#endif
#ifdef CCH_REMOTE_RETIREMENT_TESTING
                        if (before_post) before_post();
#endif
                        boost::asio::post(waiter->channel.get_executor(), [weak] {
                            if (auto current = weak.lock()) {
                                (void)current->channel.try_send(boost::system::error_code{}, true);
                            }
                        });
#if defined(__cpp_exceptions)
                    } catch (...) {
                        std::lock_guard lock(mutex);
                        ++delivery_failures;
                    }
#endif
                }
            }
        }
    };

    struct CallbackLease {
        explicit CallbackLease(std::shared_ptr<State> value) : state(std::move(value)) {}
        CallbackLease(const CallbackLease&) = delete;
        std::shared_ptr<State> state;
        bool armed = false;
        ~CallbackLease() {
            if (!armed) return;
            Publication pending;
            {
                std::lock_guard lock(state->mutex);
                --state->active;
                pending = state->publish_if_ready();
            }
            state->notify(std::move(pending));
        }
    };

    template <typename Function> struct CallbackPayload {
        CallbackPayload(std::shared_ptr<CallbackLease> value, Function callback)
            : lease(std::move(value)), function(std::move(callback)) {}
        // Members are destroyed in reverse order: captures are gone before
        // the lease can publish completion.
        std::shared_ptr<CallbackLease> lease;
        Function function;
    };

    template <typename Function> struct CallbackInvoker {
        std::shared_ptr<CallbackPayload<Function>> payload;
        void operator()() const noexcept {
            // This local survives even if the callback destroys its own handle.
            auto keep_alive = payload;
            keep_alive->function();
        }
    };

public:
    class Observer;

    class AsyncWait {
    public:
        template <typename Rep, typename Period>
        [[nodiscard]] std::future_status wait_for(std::chrono::duration<Rep, Period> duration) const {
            return state_->future.wait_for(duration);
        }

    private:
        friend class Observer;
        explicit AsyncWait(std::shared_ptr<State> state) : state_(std::move(state)) {}
        std::shared_ptr<State> state_;
    };

    class Observer {
    public:
        [[nodiscard]] bool ready() const {
            return state_->future.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
        }

        // A failed post can leave an executor wait pending. This aggregate
        // count cannot identify it, and cancellation may itself require a
        // working executor. No bounded recovery or per-waiter error is offered.
        // A delivery failure is not a retirement witness.
        [[nodiscard]] std::size_t delivery_failures() const {
            std::lock_guard lock(state_->mutex);
            return state_->delivery_failures;
        }

        // Never blocks, including when the returned wait is discarded.
        [[nodiscard]] AsyncWait async_wait() const { return AsyncWait(state_); }

        // Posts a notification to the current Asio executor. A false result is
        // cancellation/error, never evidence of retirement. Success proves the
        // participant domain only: the caller MUST independently keep the
        // executor and borrowed resources alive until the posting tail, this
        // coroutine, its completion handler, their captures and executor-bearing tails are
        // quiescent. Observing return or entering a completion handler is not
        // such a proof (ADR 0077). This is not a complete teardown witness.
        // A wait started after publication returns true on its first resume and
        // posts nothing: true reports the state at registration, not a
        // notification this wait observed.
        [[nodiscard]] static boost::asio::awaitable<bool> async_wait_on_executor(Observer observer) {
            auto executor = co_await boost::asio::this_coro::executor;
            auto waiter = std::make_shared<Waiter>(executor);
            {
                std::lock_guard lock(observer.state_->mutex);
                if (observer.state_->published) co_return true;
                // Reclaim registrations abandoned by destroyed executors.
                std::erase_if(observer.state_->waiters, [](const auto& entry) { return entry.expired(); });
                observer.state_->waiters.emplace_back(waiter);
            }
            boost::system::error_code error;
            const bool notified = co_await waiter->channel.async_receive(
                    boost::asio::redirect_error(boost::asio::use_awaitable, error));
            {
                std::lock_guard lock(observer.state_->mutex);
                std::erase_if(observer.state_->waiters, [&](const auto& entry) {
                    auto current = entry.lock();
                    return !current || current == waiter;
                });
            }
            co_return !error && notified;
        }

        // Blocking waits must not run on a participant or callback needed to retire.
        template <typename Rep, typename Period>
        [[nodiscard]] bool wait_for(std::chrono::duration<Rep, Period> duration) const {
            return state_->future.wait_for(duration) == std::future_status::ready;
        }

    private:
        friend class RemoteRetirement;
        explicit Observer(std::shared_ptr<State> state) : state_(std::move(state)) {}
        std::shared_ptr<State> state_;
    };

    class Participant;

    template <typename Function> class Callback {
    public:
        Callback(const Callback&) = delete;
        Callback& operator=(const Callback&) = delete;
        Callback(std::shared_ptr<State> state, std::shared_ptr<CallbackPayload<Function>> payload)
            : payload_(std::move(payload)), callback_(state->stop.get_token(), CallbackInvoker<Function>{payload_}) {}

    private:
        std::shared_ptr<CallbackPayload<Function>> payload_;
        std::stop_callback<CallbackInvoker<Function>> callback_;
    };

    class Participant {
    public:
        Participant(const Participant&) = delete;
        Participant& operator=(const Participant&) = delete;
        Participant(Participant&&) noexcept = default;
        Participant& operator=(Participant&&) = delete;

        [[nodiscard]] std::optional<Participant> admit_child() {
            if (!state_) return std::nullopt;
            std::lock_guard lock(state_->mutex);
            ++state_->active;
            return Participant(state_);
        }

        [[nodiscard]] bool stop_requested() const { return state_ && state_->stop.stop_requested(); }

        // An already-stopped token invokes the function synchronously inside this
        // call, so it must not be called while holding a lock the callback takes.
        // The handle owns the registration and keeps retirement pending until
        // unregistered and every invocation and capture-destruction tail ends.
        // No raw stop_token escapes this interface. Construction exceptions
        // propagate after rollback when exceptions are enabled. In the supported
        // -fno-exceptions build, allocation failure or a throwing operation
        // terminates the process; no recovery or successful witness is promised.
        // Callback invocation is noexcept: throwing from user code terminates.
        template <typename Function> [[nodiscard]] auto on_stop(Function&& function) {
            using Stored = std::decay_t<Function>;
            auto state = state_;
            if (!state) return std::unique_ptr<Callback<Stored>>{};
            // Account before invoking user copy/move constructors: they may
            // synchronously retire this participant or throw.
            auto lease = std::make_shared<CallbackLease>(state);
            {
                std::lock_guard lock(state->mutex);
                ++state->active;
                lease->armed = true;
            }
            auto payload = std::make_shared<CallbackPayload<Stored>>(lease, std::forward<Function>(function));
            return std::make_unique<Callback<Stored>>(state, std::move(payload));
        }

        // After the call this handle holds no participant: retire_self() again is
        // a no-op and admit_child() returns nullopt. The last release publishes
        // only once admission is closed.
        void retire_self() {
            auto state = std::move(state_);
            if (!state) return;
            Publication pending;
            {
                std::lock_guard lock(state->mutex);
                --state->active;
                pending = state->publish_if_ready();
            }
            state->notify(std::move(pending));
        }

    private:
        friend class RemoteRetirement;
        explicit Participant(std::shared_ptr<State> state) : state_(std::move(state)) {}
        std::shared_ptr<State> state_;
    };

    RemoteRetirement() : state_(std::make_shared<State>()) {}
#ifdef CCH_REMOTE_RETIREMENT_TESTING
    // Test-only failure and reentrancy injection, invoked outside the mutex.
    explicit RemoteRetirement(void (*before_post)()) : RemoteRetirement() { state_->before_post = before_post; }
#endif
    RemoteRetirement(const RemoteRetirement&) = delete;
    RemoteRetirement& operator=(const RemoteRetirement&) = delete;

    [[nodiscard]] Observer observe() const { return Observer(state_); }

    [[nodiscard]] std::optional<Participant> admit() {
        std::lock_guard lock(state_->mutex);
        if (!state_->open) return std::nullopt;
        ++state_->active;
        return Participant(state_);
    }

    void close_admission() {
        auto state = state_;
        Publication pending;
        {
            std::lock_guard lock(state->mutex);
            state->open = false;
            pending = state->publish_if_ready();
        }
        state->notify(std::move(pending));
    }

    void request_stop() {
        // Keep the stop machinery alive even if its callback destroys its owner.
        auto state = state_;
        Publication pending;
        {
            std::lock_guard lock(state->mutex);
            state->open = false;
            pending = state->publish_if_ready();
        }
        state->notify(std::move(pending));
        state->stop.request_stop();
    }

private:
    std::shared_ptr<State> state_;
};

// A result-only producer has no retirement witness.
template <typename Result> struct RemoteOperation {
    Result result;
    std::optional<RemoteRetirement::Observer> retirement;
};

} // namespace cch::codemode::detail
