#pragma once

#include <cch/ai/Message.hpp>
#include <cch/ai/StreamEvent.hpp>
#include <cch/support/Error.hpp>
#include <cch/support/JsonValue.hpp>

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace cch::ai::api {

using JsonObject = support::JsonValue::object_t;

/// One open Responses output slot, keyed by the provider's output_index.
struct Slot {
    enum class Kind {
        Thinking,
        Text,
        ToolCall,
    };

    Kind kind{Kind::Text};
    std::size_t content_index{};
    std::string partial_arguments{};
};

/// Responses slot lifecycle shared with the event processor: index
/// extraction, block creation, delta append, and block finalization.

[[nodiscard]] std::optional<std::size_t> output_index(const JsonObject& event);

[[nodiscard]] support::ExpectedVoid create_slot(std::size_t index,
        const JsonObject& item,
        std::map<std::size_t, Slot>& slots,
        AssistantMessage& assistant,
        AssistantEventSink& sink);

[[nodiscard]] support::ExpectedVoid append_delta(const JsonObject& event,
        std::string_view type,
        std::map<std::size_t, Slot>& slots,
        AssistantMessage& assistant,
        AssistantEventSink& sink);

[[nodiscard]] support::ExpectedVoid append_reasoning_separator(const JsonObject& event,
        std::map<std::size_t, Slot>& slots,
        AssistantMessage& assistant,
        AssistantEventSink& sink);

[[nodiscard]] support::ExpectedVoid finish_argument_stream(const JsonObject& event,
        std::map<std::size_t, Slot>& slots,
        AssistantMessage& assistant,
        AssistantEventSink& sink);

[[nodiscard]] support::ExpectedVoid finish_output_item(const JsonObject& event,
        std::map<std::size_t, Slot>& slots,
        AssistantMessage& assistant,
        AssistantEventSink& sink);

} // namespace cch::ai::api
