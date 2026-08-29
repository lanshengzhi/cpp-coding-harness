#pragma once

#include "support/ModelsFixture.hpp"
#include "support/ReleaseGate.hpp"
#include "ai/ModelStreamBridge.hpp"
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
class GatedChatProvider final : public ScriptedProvider {
public:
    GatedChatProvider() : ScriptedProvider("sdk-host") {}

    [[nodiscard]] ai::ModelStream stream(
            ai::Model model, ai::AiContext context, coding_agent::ModelRuntimeTestStreamOptions options) override {
        return ai::detail::make_model_stream(
            [this,
             model = std::move(model),
             context = std::move(context),
             options = std::move(options)](
                ai::AssistantEventSink)
                -> boost::asio::awaitable<support::Expected<ai::AssistantMessage>> {
                requests.push_back(RecordedProviderRequest{model, context, options});
                const auto turn = requests.size();

                co_await gate_.wait();

                auto response = ai::assistant_text_message(std::format("turn {}", turn));
                response.provider = "gated-fake";
                response.api = "fake";
                response.model = model.id;
                response.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                co_return response;
            });
    }

    void release() {
        gate_.release();
    }

    std::vector<RecordedProviderRequest> requests;

private:
    ReleaseGate gate_;
};

} // namespace cch::tests
