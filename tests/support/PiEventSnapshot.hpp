#pragma once

#include "JsonCompare.hpp"
#include "PiFixture.hpp"
#include "support/Json.hpp"

#include <cch/ai/StreamEvent.hpp>
#include <cch/ai/Usage.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace cch::tests {

/// Canonical projection of the C++ assistant stream events into the frozen
/// full-payload TS snapshot shape (issue #370). The projection rules match
/// `fixtures/pi-ai/capture/capture-ts-events.mts` and are documented in
/// `fixtures/pi-ai/README.md`:
///   - wall-clock timestamps (message and diagnostic) project to 0;
///   - diagnostic `error.stack` is dropped (machine-specific);
///   - pi streaming-parser scratch fields never appear here because the C++
///     adapters keep scratch state outside the message value;
///   - `textSignature`/`thinkingSignature` strings holding a JSON object or
///     array are re-serialized with recursively sorted keys (pi keeps
///     insertion order; the C++ surface serializes sorted maps);
///   - numbers compare through `json_mismatch` with a 1e-9 relative + 1e-12
///     absolute tolerance (V8 vs C++ IEEE-754 evaluation order).

[[nodiscard]] inline std::string canonical_signature_string(
    const std::string& signature) {
    const auto parsed = support::read_json(signature);
    if (!parsed) {
        return signature;
    }
    if (!parsed->holds<support::JsonValue::object_t>() &&
        !parsed->holds<support::JsonValue::array_t>()) {
        return signature;
    }
    auto canonical = support::write_json(*parsed);
    if (!canonical) {
        return signature;
    }
    return std::move(*canonical);
}

[[nodiscard]] inline support::JsonValue project_snapshot_content(
    const ai::TextContent& content) {
    support::JsonValue::object_t object{
        {"type", "text"},
        {"text", content.text},
    };
    if (content.text_signature) {
        object.emplace("textSignature", canonical_signature_string(*content.text_signature));
    }
    return support::JsonValue{std::move(object)};
}

[[nodiscard]] inline support::JsonValue project_snapshot_content(
    const ai::ThinkingContent& content) {
    support::JsonValue::object_t object{
        {"type", "thinking"},
        {"thinking", content.thinking},
    };
    if (content.thinking_signature) {
        object.emplace(
            "thinkingSignature",
            canonical_signature_string(*content.thinking_signature));
    }
    if (content.redacted) {
        object.emplace("redacted", true);
    }
    return support::JsonValue{std::move(object)};
}

[[nodiscard]] inline support::JsonValue project_snapshot_tool_call(
    const ai::ToolCallContent& content) {
    support::JsonValue::object_t object{
        {"type", "toolCall"},
        {"id", content.id},
        {"name", content.name},
    };
    if (content.arguments) {
        object.emplace("arguments", *content.arguments);
    }
    if (content.thought_signature) {
        object.emplace("thoughtSignature", *content.thought_signature);
    }
    return support::JsonValue{std::move(object)};
}

[[nodiscard]] inline support::JsonValue project_snapshot_content(
    const ai::AssistantContent& content) {
    // Dispatch explicitly: a generic-lambda call to the overload set would let
    // `ToolCallContent` bind to the `AssistantContent` overload through the
    // variant's converting constructor and recurse forever.
    return std::visit(
        [](const auto& concrete) -> support::JsonValue {
            using Content = std::decay_t<decltype(concrete)>;
            if constexpr (std::is_same_v<Content, ai::TextContent>) {
                return project_snapshot_content(concrete);
            } else if constexpr (std::is_same_v<Content, ai::ThinkingContent>) {
                return project_snapshot_content(concrete);
            } else {
                return project_snapshot_tool_call(concrete);
            }
        },
        content);
}

[[nodiscard]] inline support::JsonValue project_snapshot_usage(const ai::Usage& usage) {
    support::JsonValue::object_t object{
        {"input", static_cast<double>(usage.input)},
        {"output", static_cast<double>(usage.output)},
        {"cacheRead", static_cast<double>(usage.cache_read)},
        {"cacheWrite", static_cast<double>(usage.cache_write)},
    };
    if (usage.cache_write_1h) {
        object.emplace("cacheWrite1h", static_cast<double>(*usage.cache_write_1h));
    }
    if (usage.reasoning) {
        object.emplace("reasoning", static_cast<double>(*usage.reasoning));
    }
    object.emplace("totalTokens", static_cast<double>(usage.total_tokens));
    object.emplace(
        "cost",
        support::JsonValue::object_t{
            {"input", usage.cost.input},
            {"output", usage.cost.output},
            {"cacheRead", usage.cost.cache_read},
            {"cacheWrite", usage.cost.cache_write},
            {"total", usage.cost.total},
        });
    return support::JsonValue{std::move(object)};
}

[[nodiscard]] inline support::JsonValue project_snapshot_diagnostics(
    const std::vector<ai::DiagnosticEntry>& diagnostics) {
    support::JsonValue::array_t entries;
    entries.reserve(diagnostics.size());
    for (const auto& entry : diagnostics) {
        support::JsonValue::object_t object{
            {"type", entry.type},
            {"timestamp", 0.0},
        };
        if (entry.error) {
            support::JsonValue::object_t error{
                {"message", entry.error->message},
            };
            if (entry.error->name) {
                error.emplace("name", *entry.error->name);
            }
            if (entry.error->code) {
                error.emplace("code", *entry.error->code);
            }
            object.emplace("error", std::move(error));
        }
        if (entry.details) {
            object.emplace("details", *entry.details);
        }
        entries.push_back(support::JsonValue{std::move(object)});
    }
    return support::JsonValue{std::move(entries)};
}

[[nodiscard]] inline support::JsonValue project_snapshot_message(
    const ai::AssistantMessage& message) {
    support::JsonValue::array_t content;
    content.reserve(message.content.size());
    for (const auto& block : message.content) {
        content.push_back(project_snapshot_content(block));
    }
    support::JsonValue::object_t object{
        {"role", "assistant"},
        {"content", std::move(content)},
        {"api", message.api},
        {"provider", message.provider},
        {"model", message.model},
        {"usage", project_snapshot_usage(message.usage)},
        {"stopReason", ai::stop_reason_to_string(message.stop_reason)},
        {"timestamp", 0.0},
    };
    if (message.response_model) {
        object.emplace("responseModel", *message.response_model);
    }
    if (message.response_id) {
        object.emplace("responseId", *message.response_id);
    }
    if (message.diagnostics) {
        object.emplace("diagnostics", project_snapshot_diagnostics(*message.diagnostics));
    }
    if (message.raw_stop_reason) {
        object.emplace("rawStopReason", *message.raw_stop_reason);
    }
    if (message.error_message) {
        object.emplace("errorMessage", *message.error_message);
    }
    return support::JsonValue{std::move(object)};
}

namespace detail {

template <typename Event>
[[nodiscard]] inline support::JsonValue project_indexed_event(
    std::string_view type,
    const Event& event) {
    support::JsonValue::object_t object{
        {"type", std::string{type}},
        {"contentIndex", static_cast<double>(event.content_index)},
        {"partial", project_snapshot_message(event.partial)},
    };
    return support::JsonValue{std::move(object)};
}

} // namespace detail

[[nodiscard]] inline support::JsonValue project_snapshot_event(
    const ai::AssistantStreamEvent& event) {
    return std::visit(
        [](const auto& concrete) -> support::JsonValue {
            using Event = std::decay_t<decltype(concrete)>;
            if constexpr (std::is_same_v<Event, ai::AssistantStartEvent>) {
                return support::JsonValue{support::JsonValue::object_t{
                    {"type", "start"},
                    {"partial", project_snapshot_message(concrete.partial)},
                }};
            } else if constexpr (std::is_same_v<Event, ai::TextStartEvent>) {
                return detail::project_indexed_event("text_start", concrete);
            } else if constexpr (std::is_same_v<Event, ai::TextDeltaEvent>) {
                auto projected = detail::project_indexed_event("text_delta", concrete);
                projected.get_object().emplace("delta", concrete.delta);
                return projected;
            } else if constexpr (std::is_same_v<Event, ai::TextEndEvent>) {
                auto projected = detail::project_indexed_event("text_end", concrete);
                projected.get_object().emplace("content", concrete.content);
                return projected;
            } else if constexpr (std::is_same_v<Event, ai::ThinkingStartEvent>) {
                return detail::project_indexed_event("thinking_start", concrete);
            } else if constexpr (std::is_same_v<Event, ai::ThinkingDeltaEvent>) {
                auto projected = detail::project_indexed_event("thinking_delta", concrete);
                projected.get_object().emplace("delta", concrete.delta);
                return projected;
            } else if constexpr (std::is_same_v<Event, ai::ThinkingEndEvent>) {
                auto projected = detail::project_indexed_event("thinking_end", concrete);
                projected.get_object().emplace("content", concrete.content);
                return projected;
            } else if constexpr (std::is_same_v<Event, ai::ToolCallStartEvent>) {
                return detail::project_indexed_event("toolcall_start", concrete);
            } else if constexpr (std::is_same_v<Event, ai::ToolCallDeltaEvent>) {
                auto projected = detail::project_indexed_event("toolcall_delta", concrete);
                projected.get_object().emplace("delta", concrete.delta);
                return projected;
            } else if constexpr (std::is_same_v<Event, ai::ToolCallEndEvent>) {
                auto projected = detail::project_indexed_event("toolcall_end", concrete);
                projected.get_object().emplace("toolCall", project_snapshot_tool_call(concrete.tool_call));
                return projected;
            } else if constexpr (std::is_same_v<Event, ai::AssistantDoneEvent>) {
                return support::JsonValue{support::JsonValue::object_t{
                    {"type", "done"},
                    {"reason", ai::stop_reason_to_string(concrete.reason)},
                    {"message", project_snapshot_message(concrete.message)},
                }};
            } else {
                return support::JsonValue{support::JsonValue::object_t{
                    {"type", "error"},
                    {"reason", ai::stop_reason_to_string(concrete.reason)},
                    {"error", project_snapshot_message(concrete.error)},
                }};
            }
        },
        event);
}

[[nodiscard]] inline support::JsonValue project_snapshot_events(
    const std::vector<ai::AssistantStreamEvent>& events) {
    support::JsonValue::array_t projected;
    projected.reserve(events.size());
    for (const auto& event : events) {
        projected.push_back(project_snapshot_event(event));
    }
    return support::JsonValue{std::move(projected)};
}

/// Compares the C++ stream events against a frozen full-payload TS event
/// snapshot (fixtures/pi-ai, issue #370) through the canonical projection.
/// The caller reports the optional mismatch through its owning test framework.
[[nodiscard]] inline std::optional<std::string> pi_event_snapshot_mismatch(
    const std::vector<ai::AssistantStreamEvent>& events,
    std::string_view fixture_relative_path) {
    const auto expected = read_pi_fixture(fixture_relative_path);
    if (!expected) {
        return "could not read pi event fixture " + std::string{fixture_relative_path};
    }
    return json_mismatch(*expected, project_snapshot_events(events));
}

} // namespace cch::tests
