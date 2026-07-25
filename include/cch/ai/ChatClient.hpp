#pragma once

#include "Context.hpp"
#include "Message.hpp"
#include "Model.hpp"
#include "StreamEvent.hpp"

#include "../util/Error.hpp"

#include <boost/asio/awaitable.hpp>

#include <functional>
#include <optional>
#include <string>

namespace cch::ai {

struct StreamChatRequest {
    AiContext context;
    /// Model identity requested for this call (ADR 0019). Precedence rule:
    /// when present, this value is authoritative; when std::nullopt, the
    /// provider client's configured Model applies. A call with neither is
    /// rejected as a validation error.
    std::optional<Model> model;
};

using AssistantEventSink = std::move_only_function<util::ExpectedVoid(const AssistantStreamEvent&)>;

class StreamingChatClient {
public:
    virtual ~StreamingChatClient() = default;

    /// Provider-call acceptance boundary:
    ///
    /// Configuration, required-capability, static-request validation, and
    /// request-serialization failures detected before runtime transport begins
    /// reject the call through the error alternative. A rejected call must not
    /// start transport work or emit an AssistantStreamEvent.
    ///
    /// The call is accepted after those checks, immediately before runtime
    /// transport begins. After that point, provider, transport, protocol,
    /// malformed-response, premature-end, and cancellation failures are completed
    /// assistant outcomes: emit exactly one terminal
    /// event and return its final AssistantMessage through the value alternative.
    /// Successful outcomes use AssistantDoneEvent. Error and aborted outcomes
    /// use AssistantErrorEvent, whose message and stop reason must agree with the
    /// returned value.
    ///
    /// A failure reported by the consumer-owned sink remains an explicit
    /// infrastructure error. It is not normalized into an assistant outcome.
    [[nodiscard]] virtual boost::asio::awaitable<util::Expected<AssistantMessage>> stream(
        const StreamChatRequest& request,
        AssistantEventSink sink) = 0;

    [[nodiscard]] boost::asio::awaitable<util::Expected<AssistantMessage>> complete(const StreamChatRequest& request) {
        co_return co_await stream(request, [](const AssistantStreamEvent&) { return util::ExpectedVoid{}; });
    }
};

} // namespace cch::ai
