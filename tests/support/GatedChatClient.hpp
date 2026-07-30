#pragma once

#include <cch/ai/ChatClient.hpp>
#include <cch/ai/Content.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <chrono>
#include <format>
#include <optional>
#include <vector>

namespace cch::tests {

/// Drain every ready handler without blocking on gated (pending) work.
inline void drain_ready(boost::asio::io_context& io) {
    if (io.stopped()) io.restart();
    while (io.poll() != 0) {
    }
}

/// Gates every provider request until release(); each later request re-arms
/// the gate so steering/follow-up continuations stay observable.
class GatedChatClient final : public ai::StreamingChatClient {
public:
    [[nodiscard]] boost::asio::awaitable<util::Expected<ai::AssistantMessage>> stream(
        const ai::StreamChatRequest& request,
        ai::AssistantEventSink) override {
        requests.push_back(request);
        const auto turn = requests.size();

        const auto executor = co_await boost::asio::this_coro::executor;
        gate_.emplace(executor);
        gate_->expires_at(std::chrono::steady_clock::time_point::max());
        boost::system::error_code error;
        co_await gate_->async_wait(
            boost::asio::redirect_error(boost::asio::use_awaitable, error));
        gate_.reset();

        auto response = ai::assistant_text_message(std::format("turn {}", turn));
        response.provider = "gated-fake";
        response.api = "fake";
        response.model = request.model->id;
        response.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        co_return response;
    }

    void release() {
        if (gate_) (void)gate_->cancel();
    }

    std::vector<ai::StreamChatRequest> requests;

private:
    std::optional<boost::asio::steady_timer> gate_;
};

} // namespace cch::tests
