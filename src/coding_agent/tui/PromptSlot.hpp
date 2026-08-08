#pragma once

#include <cch/util/Error.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <boost/asio/post.hpp>

#include <atomic>
#include <memory>
#include <string>

namespace cch::coding_agent::tui {

/// One awaiting user-prompt result (pi's extension prompt slot): first
/// resolution wins, posted to the consumer executor so the channel is only
/// ever touched from one thread. The login dialog and the session
/// missing-cwd prompt share it.
struct PromptSlot : public std::enable_shared_from_this<PromptSlot> {
    explicit PromptSlot(boost::asio::any_io_executor executor)
        : executor(std::move(executor)), channel(this->executor, 1) {}

    /// First resolution wins; the send is posted to the consumer executor so
    /// the channel is only ever touched from one thread.
    void resolve(util::Expected<std::string> value) {
        if (resolved_.exchange(true)) return;
        const auto self = shared_from_this();
        boost::asio::post(executor, [self, value = std::move(value)]() mutable {
            self->channel.try_send(boost::system::error_code{}, std::move(value));
        });
    }

    boost::asio::any_io_executor executor;
    boost::asio::experimental::concurrent_channel<
        void(boost::system::error_code, util::Expected<std::string>)>
        channel;

private:
    std::atomic<bool> resolved_{false};
};

} // namespace cch::coding_agent::tui
