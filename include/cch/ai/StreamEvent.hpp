#pragma once

#include "Content.hpp"
#include "Message.hpp"
#include "Usage.hpp"

#include <cstddef>
#include <string>
#include <variant>

namespace cch::ai {

struct AssistantStartEvent {
    AssistantMessage partial;
};

struct TextStartEvent {
    std::size_t content_index{};
    AssistantMessage partial;
};

struct TextDeltaEvent {
    std::size_t content_index{};
    std::string delta;
    AssistantMessage partial;
};

struct TextEndEvent {
    std::size_t content_index{};
    std::string content;
    AssistantMessage partial;
};

struct ThinkingStartEvent {
    std::size_t content_index{};
    AssistantMessage partial;
};

struct ThinkingDeltaEvent {
    std::size_t content_index{};
    std::string delta;
    AssistantMessage partial;
};

struct ThinkingEndEvent {
    std::size_t content_index{};
    std::string content;
    AssistantMessage partial;
};

struct ToolCallStartEvent {
    std::size_t content_index{};
    AssistantMessage partial;
};

struct ToolCallDeltaEvent {
    std::size_t content_index{};
    std::string delta;
    AssistantMessage partial;
};

struct ToolCallEndEvent {
    std::size_t content_index{};
    ToolCallContent tool_call;
    AssistantMessage partial;
};

struct AssistantDoneEvent {
    AssistantStopReason reason{AssistantStopReason::Stop};
    AssistantMessage message;
};

struct AssistantErrorEvent {
    AssistantStopReason reason{AssistantStopReason::Error};
    AssistantMessage error;
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

} // namespace cch::ai
