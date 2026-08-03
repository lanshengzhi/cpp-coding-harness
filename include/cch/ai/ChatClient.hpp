#pragma once

#include "Context.hpp"
#include "Message.hpp"
#include "Model.hpp"
#include "StreamEvent.hpp"

#include <cch/util/Error.hpp>

#include <boost/asio/awaitable.hpp>

#include <stop_token>
#include <string>

namespace cch::ai {

struct StreamChatRequest {
    AiContext context{};
    /// Active prompt cancellation token. Provider implementations propagate
    /// this token through their transport and normalize accepted cancellation
    /// into one Assistant Message with stop reason `aborted`.
    std::stop_token stop_token{};
    /// Complete model identity and capabilities requested for this call (ADR
    /// 0019). Every provider request carries this concrete value; provider
    /// clients have no construction-time model fallback.
    Model model{};
};

class StreamingChatClient {
public:
    virtual ~StreamingChatClient() = default;

    /// Domain failures complete through exactly one terminal event and an
    /// agreeing AssistantMessage value. The Expected error alternative is
    /// reserved for consumer-sink or other infrastructure failure. The
    /// borrowed request must outlive the returned awaitable.
    [[nodiscard]] virtual boost::asio::awaitable<util::Expected<AssistantMessage>> stream(
        const StreamChatRequest& request,
        AssistantEventSink sink) = 0;

    /// The borrowed request must outlive the returned awaitable.
    [[nodiscard]] boost::asio::awaitable<util::Expected<AssistantMessage>> complete(
        const StreamChatRequest& request) {
        co_return co_await stream(request, [](const AssistantStreamEvent&) { return util::ExpectedVoid{}; });
    }
};

} // namespace cch::ai
