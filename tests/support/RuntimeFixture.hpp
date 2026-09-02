#pragma once

#include "agent/harness/RuntimeRoot.hpp"
#include "support/AsyncResultBridge.hpp"
#include "support/PumpUntil.hpp"

#include <catch2/catch_test_macros.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <expected>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace cch::tests {

/// Teardown observations exposed by RuntimeFixture. RuntimeRoot::close()
/// performs admission stop, queued-worker drain, worker join, and work-guard
/// release as one operation, so the worker-joined observation is recorded when
/// that call returns. The final loop drain is separate because the caller owns
/// the Runtime loop.
enum class RuntimeTeardownEvent {
    SessionCloseRequested,
    SessionsQuiesced,
    SessionsDestroyed,
    RuntimeClose,
    RuntimeWorkersJoined,
    RuntimeLoopDrained,
};

/// Test-only owner for one isolated Runtime loop and RuntimeRoot. The fixture
/// is deliberately driven by the calling test thread: RuntimeRoot owns the
/// worker pool and this class owns/pumps the interaction loop. Every fixture
/// constructs its own loop and root; no process-global Runtime state is used.
///
/// Sessions created with a fixture target may be transferred to
/// adopt_session(). The fixture then requests Session Close, pumps until every
/// adopted Session reports closed, destroys those Sessions, and only then
/// closes the RuntimeRoot. This is the strongest ordering the existing
/// synchronous AgentSession::close() request and is_open() observation expose.
class RuntimeFixture final {
public:
    explicit RuntimeFixture(
        harness::RuntimeLimits limits = {},
        std::chrono::milliseconds wait_budget = std::chrono::milliseconds{5000})
        : loop_(std::make_shared<boost::asio::io_context>()),
          root_(loop_, limits),
          wait_budget_(wait_budget) {
        teardown_events_.reserve(6);
    }

    RuntimeFixture(RuntimeFixture&&) = delete;
    RuntimeFixture& operator=(RuntimeFixture&&) = delete;
    ~RuntimeFixture() { close(); }
    RuntimeFixture(const RuntimeFixture&) = delete;
    RuntimeFixture& operator=(const RuntimeFixture&) = delete;

    /// Create a target backed by this fixture's RuntimeRoot. Each target has
    /// its own ordered mailbox while all targets share this fixture's loop and
    /// worker pool.
    [[nodiscard]] std::shared_ptr<harness::RuntimeTarget> make_target() const {
        return root_.make_target();
    }

    /// Consume one production AsyncResult on this fixture's loop and return
    /// its exact std::expected<T, E> terminal outcome. A stalled operation is
    /// reported as a test failure after wait_budget_; timeout is not translated
    /// into a production error alternative.
    template <typename T, typename E>
    [[nodiscard]] std::expected<T, E> run(support::AsyncResult<T, E> operation) {
        loop_->restart();

        struct RunState final {
            std::optional<std::expected<T, E>> outcome;
            std::atomic<bool> done{false};
        };
        const auto state = std::make_shared<RunState>();

        boost::asio::co_spawn(
            *loop_,
            [operation = std::move(operation), state]() mutable -> boost::asio::awaitable<void> {
                state->outcome.emplace(
                    co_await support::detail::await_async_result(std::move(operation)));
                state->done.store(true, std::memory_order_release);
                co_return;
            },
            boost::asio::detached);

        const bool completed = pump_until(*loop_, state->done, wait_budget_);
        REQUIRE(completed);
        REQUIRE(state->outcome.has_value());
        return std::move(*state->outcome);
    }

    /// Transfer ownership of a Session-like object to this fixture. The type
    /// must provide close() and is_open(); no production Session interface is
    /// introduced or required by the generic Runtime harness. The adopted
    /// object is closed and destroyed before RuntimeRoot::close().
    template <typename Session>
    [[nodiscard]] Session& adopt_session(std::unique_ptr<Session> session) {
        if (closed_ || !session) {
            std::terminate();
        }
        auto holder = std::make_unique<SessionHolder<Session>>(std::move(session));
        Session& reference = holder->session();
        sessions_.push_back(std::move(holder));
        return reference;
    }

    /// Close adopted Sessions in place, wait for their observable closed state,
    /// destroy them, then perform RuntimeRoot Close and the final loop drain.
    /// RuntimeRoot::close() remains the authority for worker drain and join;
    /// this wrapper only supplies the Session-before-Runtime ordering and its
    /// test-visible record.
    void close() noexcept {
        if (closed_) {
            return;
        }

        if (!sessions_.empty()) {
            teardown_events_.push_back(RuntimeTeardownEvent::SessionCloseRequested);
            for (auto iterator = sessions_.rbegin(); iterator != sessions_.rend(); ++iterator) {
                (*iterator)->close();
            }

            const bool sessions_closed = pump_until(
                *loop_,
                [this] {
                    return std::all_of(
                        sessions_.begin(), sessions_.end(), [](const auto& session) {
                            return !session->is_open();
                        });
                },
                wait_budget_);
            REQUIRE(sessions_closed);
            teardown_events_.push_back(RuntimeTeardownEvent::SessionsQuiesced);

            // Session Close may post owned-filesystem cleanup to the fixture
            // loop after its public closed state becomes observable. Drain it
            // while every adopted Session and the RuntimeRoot are still alive.
            drain_ready(*loop_, std::chrono::milliseconds{2}, wait_budget_);
            sessions_.clear();
            teardown_events_.push_back(RuntimeTeardownEvent::SessionsDestroyed);
        }

        teardown_events_.push_back(RuntimeTeardownEvent::RuntimeClose);
        root_.close();
        // RuntimeRoot::close() returns only after it has stopped admission,
        // drained queued worker tasks, joined workers, and released its loop
        // work guard.
        teardown_events_.push_back(RuntimeTeardownEvent::RuntimeWorkersJoined);
        drain_ready(*loop_, std::chrono::milliseconds{2}, wait_budget_);
        teardown_events_.push_back(RuntimeTeardownEvent::RuntimeLoopDrained);
        closed_ = true;
    }

    [[nodiscard]] bool closed() const noexcept { return closed_; }

    [[nodiscard]] const std::vector<RuntimeTeardownEvent>& teardown_events() const noexcept {
        return teardown_events_;
    }

private:
    class SessionHolderBase {
    public:
        virtual ~SessionHolderBase() = default;
        virtual void close() noexcept = 0;
        [[nodiscard]] virtual bool is_open() const noexcept = 0;
    };

    template <typename Session>
    class SessionHolder final : public SessionHolderBase {
    public:
        explicit SessionHolder(std::unique_ptr<Session> session)
            : session_(std::move(session)) {}

        void close() noexcept override { session_->close(); }

        [[nodiscard]] bool is_open() const noexcept override {
            return session_->is_open();
        }

        [[nodiscard]] Session& session() noexcept { return *session_; }

    private:
        std::unique_ptr<Session> session_;
    };

    std::shared_ptr<boost::asio::io_context> loop_;
    harness::RuntimeRoot root_;
    std::chrono::milliseconds wait_budget_;
    std::vector<std::unique_ptr<SessionHolderBase>> sessions_;
    std::vector<RuntimeTeardownEvent> teardown_events_;
    bool closed_{false};
};

} // namespace cch::tests
