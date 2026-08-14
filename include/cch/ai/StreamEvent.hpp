#pragma once

#include "Content.hpp"
#include "Message.hpp"
#include "Usage.hpp"

#include <cch/support/Error.hpp>

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <variant>

namespace cch::ai {

struct AssistantStartEvent {
    AssistantMessage partial{};
};

struct TextStartEvent {
    std::size_t content_index{};
    AssistantMessage partial{};
};

struct TextDeltaEvent {
    std::size_t content_index{};
    std::string delta{};
    AssistantMessage partial{};
};

struct TextEndEvent {
    std::size_t content_index{};
    std::string content{};
    AssistantMessage partial{};
};

struct ThinkingStartEvent {
    std::size_t content_index{};
    AssistantMessage partial{};
};

struct ThinkingDeltaEvent {
    std::size_t content_index{};
    std::string delta{};
    AssistantMessage partial{};
};

struct ThinkingEndEvent {
    std::size_t content_index{};
    std::string content{};
    AssistantMessage partial{};
};

struct ToolCallStartEvent {
    std::size_t content_index{};
    AssistantMessage partial{};
};

struct ToolCallDeltaEvent {
    std::size_t content_index{};
    std::string delta{};
    AssistantMessage partial{};
};

struct ToolCallEndEvent {
    std::size_t content_index{};
    ToolCallContent tool_call{};
    AssistantMessage partial{};
};

struct AssistantDoneEvent {
    AssistantStopReason reason{AssistantStopReason::Stop};
    AssistantMessage message{};
};

struct AssistantErrorEvent {
    AssistantStopReason reason{AssistantStopReason::Error};
    AssistantMessage error{};
    /// Models-category enrichment for request/runtime failures. Provider
    /// terminal events may omit it when no Models category applies.
    std::optional<cch::support::Error> failure{std::nullopt};
};

using AssistantStreamEvent = std::variant<
    AssistantStartEvent,
    TextStartEvent,
    TextDeltaEvent,
    TextEndEvent,
    ThinkingStartEvent,
    ThinkingDeltaEvent,
    ThinkingEndEvent,
    ToolCallStartEvent,
    ToolCallDeltaEvent,
    ToolCallEndEvent,
    AssistantDoneEvent,
    AssistantErrorEvent>;

using AssistantEventSink = std::move_only_function<cch::support::ExpectedVoid(const AssistantStreamEvent&)>;

} // namespace cch::ai
