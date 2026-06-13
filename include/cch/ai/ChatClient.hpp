#pragma once

#include "Context.hpp"
#include "Message.hpp"
#include "StreamEvent.hpp"

#include "../util/Error.hpp"

#include <boost/asio/awaitable.hpp>

#include <functional>
#include <string>

namespace cch::ai {

struct StreamChatRequest {
    AiContext context;
    std::string model;
};

using AssistantEventSink = std::move_only_function<util::ExpectedVoid(const AssistantStreamEvent&)>;

class StreamingChatClient {
public:
    virtual ~StreamingChatClient() = default;

    [[nodiscard]] virtual boost::asio::awaitable<util::Expected<AssistantMessage>> stream(
        const StreamChatRequest& request,
        AssistantEventSink sink) = 0;

    [[nodiscard]] boost::asio::awaitable<util::Expected<AssistantMessage>> complete(const StreamChatRequest& request) {
        co_return co_await stream(request, [](const AssistantStreamEvent&) { return util::ExpectedVoid{}; });
    }
};

} // namespace cch::ai
