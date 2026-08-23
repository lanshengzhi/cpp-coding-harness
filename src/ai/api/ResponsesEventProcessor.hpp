#pragma once

#include <cch/ai/Message.hpp>
#include <cch/ai/Model.hpp>
#include <cch/ai/StreamEvent.hpp>
#include <cch/support/Error.hpp>
#include <cch/support/JsonValue.hpp>

#include <memory>
#include <optional>
#include <string>

namespace cch::ai::api {

using JsonObject = support::JsonValue::object_t;

enum class ResponsesDialect {
    DeepSeek,
    Codex,
};

enum class ResponsesDelivery {
    Sse,
    WebSocket,
};

struct ResponsesProviderError {
    std::optional<std::string> code{std::nullopt};
    std::optional<std::string> message{std::nullopt};
};

struct ResponsesProcessOutcome {
    bool terminal{false};
    std::optional<ResponsesProviderError> provider_error{std::nullopt};
};

/// Private, synchronous Responses-family wire-event state machine. Framing,
/// transport, request policy, and terminal event commitment remain in the
/// owning adapter; this object only translates decoded JSON events into the
/// shared assistant event stream.
class ResponsesEventProcessor final {
public:
    ResponsesEventProcessor(ResponsesDialect dialect, ResponsesDelivery delivery, Model model);
    ResponsesEventProcessor(ResponsesEventProcessor&&) noexcept;
    ResponsesEventProcessor& operator=(ResponsesEventProcessor&&) noexcept;
    ~ResponsesEventProcessor();
    ResponsesEventProcessor(const ResponsesEventProcessor&) = delete;
    ResponsesEventProcessor& operator=(const ResponsesEventProcessor&) = delete;

    [[nodiscard]] support::Expected<ResponsesProcessOutcome> process(
            JsonObject event, AssistantMessage& assistant, AssistantEventSink& sink);
    [[nodiscard]] support::ExpectedVoid finish(AssistantMessage& assistant);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cch::ai::api
