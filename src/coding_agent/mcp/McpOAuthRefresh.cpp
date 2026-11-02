// pi's shared MCP OAuth refresh lifecycle (ticket #1189). The request-time
// token resolver starts at most one refresh per server/URL; every waiter
// observes the operation only until its own cancellation fires, while the
// operation itself keeps running so a rotated refresh token is persisted.
// `settled()` is pi's provider `settled()`.

#include "coding_agent/mcp/McpOAuthRefresh.hpp"

#include "support/AsyncResultBridge.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/experimental/channel.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/error_code.hpp>

#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <utility>
#include <vector>

namespace cch::coding_agent::mcp {
namespace {

using WakeChannel = boost::asio::experimental::channel<void(boost::system::error_code)>;

/// One request's wait on the shared refresh: a buffered wake channel, so a
/// completion or cancellation that arrives before the receive is retained.
struct RefreshWaiter {
    explicit RefreshWaiter(boost::asio::any_io_executor executor) : wake(std::move(executor), 1) {}

    WakeChannel wake;
};

struct RefreshSlot {
    bool done{false};
    std::optional<support::Expected<cch::mcp::OAuthCredential>> result;
    std::vector<std::shared_ptr<RefreshWaiter>> waiters;
};

[[nodiscard]] support::Error cancelled_error() {
    return support::make_error(support::ErrorCode::Cancelled, "Login cancelled");
}

} // namespace

struct McpOAuthRefresh::Impl {
    std::mutex mutex;
    std::shared_ptr<RefreshSlot> current;

    /// Wait for `slot` (or `stop_token`). Returns false when the waiter was
    /// cancelled; the slot keeps running.
    [[nodiscard]] boost::asio::awaitable<bool> wait_for_slot(const std::shared_ptr<RefreshSlot>& slot,
            std::stop_token stop_token,
            boost::asio::any_io_executor executor);
};

boost::asio::awaitable<bool> McpOAuthRefresh::Impl::wait_for_slot(
        const std::shared_ptr<RefreshSlot>& slot, std::stop_token stop_token, boost::asio::any_io_executor executor) {
    for (;;) {
        {
            std::scoped_lock lock(mutex);
            if (slot->done) {
                co_return true;
            }
        }
        auto waiter = std::make_shared<RefreshWaiter>(executor);
        {
            std::scoped_lock lock(mutex);
            if (slot->done) {
                co_return true;
            }
            slot->waiters.push_back(waiter);
        }
        std::stop_callback cancel{stop_token, [waiter, executor] {
                                      boost::asio::post(executor, [waiter] {
                                          waiter->wake.try_send(boost::asio::error::operation_aborted);
                                      });
                                  }};
        if (stop_token.stop_requested()) {
            co_return false;
        }
        boost::system::error_code receive_error;
        co_await waiter->wake.async_receive(boost::asio::redirect_error(boost::asio::use_awaitable, receive_error));
        if (stop_token.stop_requested()) {
            co_return false;
        }
    }
}

McpOAuthRefresh::McpOAuthRefresh() : impl_(std::make_shared<Impl>()) {}

McpOAuthRefresh::~McpOAuthRefresh() = default;

bool McpOAuthRefresh::in_flight() const noexcept {
    std::scoped_lock lock(impl_->mutex);
    return static_cast<bool>(impl_->current);
}

support::AsyncResult<std::optional<cch::mcp::OAuthCredential>> McpOAuthRefresh::refresh(
        Operation operation, std::stop_token stop_token) {
    return support::detail::make_async_result(
            [self = shared_from_this(), operation = std::move(operation), stop_token]() mutable
                    -> boost::asio::awaitable<support::Expected<std::optional<cch::mcp::OAuthCredential>>> {
                auto executor = co_await boost::asio::this_coro::executor;
                std::shared_ptr<RefreshSlot> slot;
                bool start = false;
                {
                    std::scoped_lock lock(self->impl_->mutex);
                    slot = self->impl_->current;
                    if (!slot) {
                        slot = std::make_shared<RefreshSlot>();
                        self->impl_->current = slot;
                        start = true;
                    }
                }
                if (start) {
                    // Start outside the coordinator lock: the operation may
                    // complete inline and its completion re-enters the lock.
                    // The refresh owns itself, not the waiter: a cancelled
                    // request must not abort the token exchange.
                    boost::asio::co_spawn(
                            executor,
                            [self, slot, operation = std::move(operation)]() mutable -> boost::asio::awaitable<void> {
                                auto outcome = co_await operation();
                                std::vector<std::shared_ptr<RefreshWaiter>> waiters;
                                {
                                    std::scoped_lock lock(self->impl_->mutex);
                                    slot->done = true;
                                    slot->result = std::move(outcome);
                                    waiters = std::move(slot->waiters);
                                    if (self->impl_->current == slot) {
                                        self->impl_->current.reset();
                                    }
                                }
                                for (const auto& waiter : waiters) {
                                    waiter->wake.try_send(boost::system::error_code{});
                                }
                            },
                            boost::asio::detached);
                }
                if (stop_token.stop_requested()) {
                    co_return std::unexpected(cancelled_error());
                }
                const bool completed = co_await self->impl_->wait_for_slot(slot, stop_token, executor);
                if (!completed) {
                    co_return std::unexpected(cancelled_error());
                }
                std::optional<support::Expected<cch::mcp::OAuthCredential>> result;
                {
                    std::scoped_lock lock(self->impl_->mutex);
                    result = slot->result;
                }
                if (!result.has_value()) {
                    co_return std::unexpected(
                            support::make_error(support::ErrorCode::Process, "MCP OAuth refresh produced no result"));
                }
                if (!result->has_value()) {
                    co_return std::unexpected(std::move(result->error()));
                }
                co_return std::optional<cch::mcp::OAuthCredential>{**result};
            });
}

support::AsyncResult<void> McpOAuthRefresh::settled() {
    return support::detail::make_async_result(
            [self = shared_from_this()]() -> boost::asio::awaitable<support::ExpectedVoid> {
                auto executor = co_await boost::asio::this_coro::executor;
                std::shared_ptr<RefreshSlot> slot;
                {
                    std::scoped_lock lock(self->impl_->mutex);
                    slot = self->impl_->current;
                }
                if (slot) {
                    (void)co_await self->impl_->wait_for_slot(slot, {}, executor);
                }
                co_return support::ExpectedVoid{};
            });
}

} // namespace cch::coding_agent::mcp
