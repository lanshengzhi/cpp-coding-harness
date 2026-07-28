#include "JsonEventPrinter.hpp"

#include "BoundedText.hpp"

#include "ai/glaze/AiJson.hpp"
#include "util/Json.hpp"
#include "util/Redactor.hpp"

#include <algorithm>
#include <ostream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace cch::coding_agent::runtime {
namespace {
constexpr std::size_t kMaxCollectionItems = 256;
constexpr std::size_t kMaxJsonDepth = 32;
constexpr std::size_t kMaxRecordStringBytes = 256 * 1024;
constexpr std::size_t kMaxRecordNodes = 4096;
constexpr std::size_t kMaxSerializedRecordBytes = 512 * 1024;

struct JsonSafetyBudget {
    std::size_t string_bytes{kMaxRecordStringBytes};
    std::size_t nodes{kMaxRecordNodes};
};

[[nodiscard]] util::Error json_event_error(std::string message, std::string detail = {}) {
    return util::make_error(util::ErrorCode::JsonSerialize, std::move(message), std::move(detail));
}


[[nodiscard]] util::JsonValue safe_json_value(
    const util::JsonValue& value,
    JsonSafetyBudget& budget,
    std::size_t depth = 0) {
    if (budget.nodes == 0) {
        if (value.holds<util::JsonValue::array_t>()) return util::JsonValue{util::JsonValue::array_t{}};
        if (value.holds<util::JsonValue::object_t>()) return util::JsonValue{util::JsonValue::object_t{}};
        if (value.holds<std::string>()) return util::JsonValue{std::string{}};
        return value;
    }
    --budget.nodes;

    if (const auto* text = value.get_if<std::string>()) {
        auto safe = bounded_redacted(*text, std::min(kMaxPayloadStringBytes, budget.string_bytes));
        budget.string_bytes -= safe.size();
        return util::JsonValue{std::move(safe)};
    }
    if (const auto* values = value.get_if<util::JsonValue::array_t>()) {
        util::JsonValue::array_t safe;
        if (depth >= kMaxJsonDepth) {
            return util::JsonValue{std::move(safe)};
        }
        const auto count = std::min(values->size(), kMaxCollectionItems);
        safe.reserve(count);
        for (std::size_t index = 0; index < count && budget.nodes > 0; ++index) {
            safe.push_back(safe_json_value((*values)[index], budget, depth + 1));
        }
        return util::JsonValue{std::move(safe)};
    }
    if (const auto* object = value.get_if<util::JsonValue::object_t>()) {
        util::JsonValue::object_t safe;
        if (depth >= kMaxJsonDepth) {
            return util::JsonValue{std::move(safe)};
        }
        std::size_t count = 0;
        auto copy_fields = [&](bool containers) {
            for (const auto& [key, item] : *object) {
                const bool is_container = item.holds<util::JsonValue::array_t>() ||
                                          item.holds<util::JsonValue::object_t>();
                if (is_container != containers || count >= kMaxCollectionItems || budget.nodes == 0) {
                    continue;
                }
                ++count;
                auto safe_key = bounded_redacted(key, std::min(kMaxPayloadStringBytes, budget.string_bytes));
                budget.string_bytes -= safe_key.size();
                if (util::looks_secret_key(key)) {
                    safe.emplace(std::move(safe_key), util::JsonValue{std::string{util::kRedactionMarker}});
                } else {
                    safe.emplace(std::move(safe_key), safe_json_value(item, budget, depth + 1));
                }
            }
        };
        // Preserve discriminators and other scalar contract fields before
        // spending the shared record budget on nested payloads.
        copy_fields(false);
        copy_fields(true);
        return util::JsonValue{std::move(safe)};
    }
    return value;
}

[[nodiscard]] util::JsonValue safe_json_value(const util::JsonValue& value) {
    JsonSafetyBudget budget;
    return safe_json_value(value, budget);
}

void make_safe(ai::TextContent& block) {
    block.text = bounded_redacted(std::move(block.text));
    if (block.text_signature) {
        block.text_signature = bounded_redacted(std::move(*block.text_signature));
    }
}

void make_safe(ai::ThinkingContent& block) {
    block.thinking = bounded_redacted(std::move(block.thinking));
    if (block.thinking_signature) {
        block.thinking_signature = bounded_redacted(std::move(*block.thinking_signature));
    }
}

void make_safe(ai::ImageContent& block) {
    block.data = bounded_redacted(std::move(block.data));
    block.mime_type = bounded_redacted(std::move(block.mime_type));
}

void make_safe(ai::ToolCallContent& block) {
    block.id = bounded_redacted(std::move(block.id));
    block.name = bounded_redacted(std::move(block.name));
    if (block.arguments) {
        block.arguments = safe_json_value(*block.arguments);
    }
    block.raw_arguments = bounded_redacted(std::move(block.raw_arguments));
    if (block.thought_signature) {
        block.thought_signature = bounded_redacted(std::move(*block.thought_signature));
    }
    if (block.argument_error) {
        block.argument_error = bounded_redacted(std::move(*block.argument_error));
    }
}

void make_safe(ai::Content& content) {
    std::visit([](auto& block) { make_safe(block); }, content);
}

void make_safe(ai::AssistantContent& content) {
    std::visit([](auto& block) { make_safe(block); }, content);
}

void normalize_message_wire(util::JsonValue& message) {
    auto* object = message.get_if<util::JsonValue::object_t>();
    if (object == nullptr) {
        return;
    }
    const auto content = object->find("content");
    if (content == object->end()) {
        return;
    }
    auto* blocks = content->second.get_if<util::JsonValue::array_t>();
    if (blocks == nullptr) {
        return;
    }
    for (auto& block_value : *blocks) {
        auto* block = block_value.get_if<util::JsonValue::object_t>();
        if (block == nullptr) {
            continue;
        }
        const auto type = block->find("type");
        if (type == block->end() || !type->second.holds<std::string>() ||
            type->second.get<std::string>() != "toolCall") {
            continue;
        }
        block->erase("rawArguments");
        block->erase("argumentsValid");
        block->erase("argumentError");
        block->try_emplace("arguments", util::JsonValue{util::JsonValue::object_t{}});
    }
}

[[nodiscard]] ai::MessageVariant safe_message(const ai::MessageVariant& message) {
    auto safe = message;
    std::visit(
        [](auto& concrete) {
            using T = std::decay_t<decltype(concrete)>;
            if constexpr (std::is_same_v<T, ai::SystemMessage>) {
                concrete.content = bounded_redacted(std::move(concrete.content));
            } else if constexpr (std::is_same_v<T, ai::AssistantMessage>) {
                for (auto& block : concrete.content) {
                    make_safe(block);
                }
                concrete.api = bounded_redacted(std::move(concrete.api));
                concrete.provider = bounded_redacted(std::move(concrete.provider));
                concrete.model = bounded_redacted(std::move(concrete.model));
                if (concrete.response_model) {
                    concrete.response_model = bounded_redacted(std::move(*concrete.response_model));
                }
                if (concrete.response_id) {
                    concrete.response_id = bounded_redacted(std::move(*concrete.response_id));
                }
                if (concrete.error_message) {
                    concrete.error_message = bounded_redacted(std::move(*concrete.error_message));
                }
                if (concrete.diagnostics) {
                    for (auto& diagnostic : *concrete.diagnostics) {
                        diagnostic.type = bounded_redacted(std::move(diagnostic.type));
                        if (diagnostic.error) {
                            auto& error = *diagnostic.error;
                            if (error.name) {
                                error.name = bounded_redacted(std::move(*error.name));
                            }
                            error.message = bounded_redacted(std::move(error.message));
                            if (error.stack) {
                                error.stack = bounded_redacted(std::move(*error.stack));
                            }
                            if (error.code) {
                                error.code = bounded_redacted(std::move(*error.code));
                            }
                        }
                        if (diagnostic.details) {
                            diagnostic.details = safe_json_value(*diagnostic.details);
                        }
                    }
                }
            } else if constexpr (std::is_same_v<T, ai::BashExecutionMessage>) {
                concrete.command = bounded_redacted(std::move(concrete.command));
                concrete.output = bounded_redacted(std::move(concrete.output));
                if (concrete.full_output_path) {
                    concrete.full_output_path = bounded_redacted(std::move(*concrete.full_output_path));
                }
            } else if constexpr (std::is_same_v<T, ai::CustomMessage>) {
                concrete.custom_type = bounded_redacted(std::move(concrete.custom_type));
                for (auto& block : concrete.content) {
                    make_safe(block);
                }
                if (concrete.details) {
                    concrete.details = safe_json_value(*concrete.details);
                }
            } else if constexpr (std::is_same_v<T, ai::BranchSummaryMessage>) {
                concrete.summary = bounded_redacted(std::move(concrete.summary));
                concrete.from_id = bounded_redacted(std::move(concrete.from_id));
            } else if constexpr (std::is_same_v<T, ai::CompactionSummaryMessage>) {
                concrete.summary = bounded_redacted(std::move(concrete.summary));
            } else {
                for (auto& block : concrete.content) {
                    make_safe(block);
                }
                if constexpr (std::is_same_v<T, ai::ToolResultMessage>) {
                    concrete.tool_call_id = bounded_redacted(std::move(concrete.tool_call_id));
                    concrete.tool_name = bounded_redacted(std::move(concrete.tool_name));
                    if (concrete.details) {
                        concrete.details = safe_json_value(*concrete.details);
                    }
                }
            }
        },
        safe);
    return safe;
}

[[nodiscard]] util::Expected<util::JsonValue> message_value(const ai::MessageVariant& message) {
    auto safe = safe_message(message);
    auto serialized = ai::glaze::write_message_json(safe);
    if (!serialized) {
        return std::unexpected(serialized.error());
    }
    auto parsed = util::read_json<util::JsonValue>(*serialized);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    normalize_message_wire(*parsed);

    auto value = safe_json_value(*parsed);
    if (std::holds_alternative<ai::AssistantMessage>(safe)) {
        auto& object = value.get<util::JsonValue::object_t>();
        object.try_emplace("api", util::JsonValue{""});
        object.try_emplace("provider", util::JsonValue{""});
        object.try_emplace("model", util::JsonValue{""});
    }
    return value;
}

[[nodiscard]] util::Expected<util::JsonValue::array_t> message_values(
    const std::vector<ai::MessageVariant>& messages) {
    util::JsonValue::array_t values;
    const auto count = std::min(messages.size(), kMaxCollectionItems);
    values.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        auto value = message_value(messages[index]);
        if (!value) {
            return std::unexpected(value.error());
        }
        values.push_back(std::move(*value));
    }
    return values;
}

[[nodiscard]] util::Expected<util::JsonValue::array_t> tool_result_message_values(
    const std::vector<ai::ToolResultMessage>& messages) {
    util::JsonValue::array_t values;
    const auto count = std::min(messages.size(), kMaxCollectionItems);
    values.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        auto value = message_value(ai::MessageVariant{messages[index]});
        if (!value) {
            return std::unexpected(value.error());
        }
        values.push_back(std::move(*value));
    }
    return values;
}

[[nodiscard]] util::Expected<util::JsonValue> assistant_event_value(const ai::AssistantStreamEvent& event) {
    return std::visit(
        [](const auto& concrete) -> util::Expected<util::JsonValue> {
            using T = std::decay_t<decltype(concrete)>;
            util::JsonValue::object_t value;
            if constexpr (std::is_same_v<T, ai::AssistantStartEvent>) {
                value.emplace("type", util::JsonValue{"start"});
                auto partial = message_value(ai::MessageVariant{concrete.partial});
                if (!partial) return std::unexpected(partial.error());
                value.emplace("partial", std::move(*partial));
            } else if constexpr (
                std::is_same_v<T, ai::TextStartEvent> ||
                std::is_same_v<T, ai::ThinkingStartEvent> ||
                std::is_same_v<T, ai::ToolCallStartEvent>) {
                if constexpr (std::is_same_v<T, ai::TextStartEvent>) value.emplace("type", util::JsonValue{"text_start"});
                if constexpr (std::is_same_v<T, ai::ThinkingStartEvent>) value.emplace("type", util::JsonValue{"thinking_start"});
                if constexpr (std::is_same_v<T, ai::ToolCallStartEvent>) value.emplace("type", util::JsonValue{"toolcall_start"});
                value.emplace("contentIndex", util::JsonValue{static_cast<double>(concrete.content_index)});
                auto partial = message_value(ai::MessageVariant{concrete.partial});
                if (!partial) return std::unexpected(partial.error());
                value.emplace("partial", std::move(*partial));
            } else if constexpr (
                std::is_same_v<T, ai::TextDeltaEvent> ||
                std::is_same_v<T, ai::ThinkingDeltaEvent> ||
                std::is_same_v<T, ai::ToolCallDeltaEvent>) {
                if constexpr (std::is_same_v<T, ai::TextDeltaEvent>) value.emplace("type", util::JsonValue{"text_delta"});
                if constexpr (std::is_same_v<T, ai::ThinkingDeltaEvent>) value.emplace("type", util::JsonValue{"thinking_delta"});
                if constexpr (std::is_same_v<T, ai::ToolCallDeltaEvent>) value.emplace("type", util::JsonValue{"toolcall_delta"});
                value.emplace("contentIndex", util::JsonValue{static_cast<double>(concrete.content_index)});
                value.emplace("delta", util::JsonValue{bounded_redacted(concrete.delta)});
                auto partial = message_value(ai::MessageVariant{concrete.partial});
                if (!partial) return std::unexpected(partial.error());
                value.emplace("partial", std::move(*partial));
            } else if constexpr (
                std::is_same_v<T, ai::TextEndEvent> ||
                std::is_same_v<T, ai::ThinkingEndEvent>) {
                if constexpr (std::is_same_v<T, ai::TextEndEvent>) value.emplace("type", util::JsonValue{"text_end"});
                if constexpr (std::is_same_v<T, ai::ThinkingEndEvent>) value.emplace("type", util::JsonValue{"thinking_end"});
                value.emplace("contentIndex", util::JsonValue{static_cast<double>(concrete.content_index)});
                value.emplace("content", util::JsonValue{bounded_redacted(concrete.content)});
                auto partial = message_value(ai::MessageVariant{concrete.partial});
                if (!partial) return std::unexpected(partial.error());
                value.emplace("partial", std::move(*partial));
            } else if constexpr (std::is_same_v<T, ai::ToolCallEndEvent>) {
                value.emplace("type", util::JsonValue{"toolcall_end"});
                value.emplace("contentIndex", util::JsonValue{static_cast<double>(concrete.content_index)});
                ai::AssistantMessage holder;
                holder.content.emplace_back(concrete.tool_call);
                auto holder_value = message_value(ai::MessageVariant{holder});
                if (!holder_value) return std::unexpected(holder_value.error());
                auto& holder_object = holder_value->get<util::JsonValue::object_t>();
                auto& content = holder_object.at("content").get<util::JsonValue::array_t>();
                value.emplace("toolCall", content.front());
                auto partial = message_value(ai::MessageVariant{concrete.partial});
                if (!partial) return std::unexpected(partial.error());
                value.emplace("partial", std::move(*partial));
            } else if constexpr (std::is_same_v<T, ai::AssistantDoneEvent>) {
                value.emplace("type", util::JsonValue{"done"});
                value.emplace("reason", util::JsonValue{ai::stop_reason_to_string(concrete.reason)});
                auto message = message_value(ai::MessageVariant{concrete.message});
                if (!message) return std::unexpected(message.error());
                value.emplace("message", std::move(*message));
            } else if constexpr (std::is_same_v<T, ai::AssistantErrorEvent>) {
                value.emplace("type", util::JsonValue{"error"});
                value.emplace("reason", util::JsonValue{ai::stop_reason_to_string(concrete.reason)});
                auto error = message_value(ai::MessageVariant{concrete.error});
                if (!error) return std::unexpected(error.error());
                value.emplace("error", std::move(*error));
            }
            return util::JsonValue{std::move(value)};
        },
        event);
}

[[nodiscard]] util::Expected<util::JsonValue> tool_result_value(
    const agent::AsyncToolExecutionResult& result) {
    ai::ToolResultMessage holder;
    holder.content = result.content;
    holder.details = result.details;
    auto holder_value = message_value(ai::MessageVariant{std::move(holder)});
    if (!holder_value) {
        return std::unexpected(holder_value.error());
    }

    auto& holder_object = holder_value->get<util::JsonValue::object_t>();
    util::JsonValue::object_t value;
    value.emplace("content", std::move(holder_object.at("content")));
    if (holder_object.contains("details")) {
        value.emplace("details", std::move(holder_object.at("details")));
    } else {
        value.emplace("details", util::JsonValue{nullptr});
    }
    value.emplace("terminate", util::JsonValue{result.terminate});
    return util::JsonValue{std::move(value)};
}

} // namespace

JsonEventPrinter::JsonEventPrinter(std::ostream& out) : out_(&out) {}

util::ExpectedVoid JsonEventPrinter::write_record(util::JsonValue::object_t record) {
    JsonSafetyBudget budget;
    auto safe_record = safe_json_value(util::JsonValue{std::move(record)}, budget);
    auto serialized = util::write_json(safe_record);
    if (!serialized) {
        return std::unexpected(serialized.error());
    }
    if (serialized->size() > kMaxSerializedRecordBytes) {
        return std::unexpected(json_event_error(
            "JSON event exceeds bounded output limit",
            "serialized event exceeds maximum record bytes"));
    }
    (*out_) << *serialized << '\n';
    if (!(*out_)) {
        return std::unexpected(json_event_error("failed to write JSON event", "output stream failed"));
    }
    return {};
}

util::ExpectedVoid JsonEventPrinter::print_session_header(const harness::session::SessionMetadata& metadata) {
    util::JsonValue::object_t record;
    record.emplace("type", util::JsonValue{"session"});
    record.emplace("version", util::JsonValue{3});
    record.emplace("id", util::JsonValue{metadata.session_id});
    record.emplace("timestamp", util::JsonValue{metadata.created_at});
    record.emplace("cwd", util::JsonValue{metadata.workspace.string()});
    return write_record(std::move(record));
}

util::ExpectedVoid JsonEventPrinter::print_agent_event(const agent::AgentLifecycleEvent& event) {
    util::JsonValue::object_t record;
    if (std::holds_alternative<agent::AgentStartEvent>(event)) {
        record.emplace("type", util::JsonValue{"agent_start"});
    } else if (const auto* end = std::get_if<agent::AgentEndEvent>(&event)) {
        record.emplace("type", util::JsonValue{"agent_end"});
        auto messages = message_values(end->messages);
        if (!messages) return std::unexpected(messages.error());
        record.emplace("messages", util::JsonValue{std::move(*messages)});
    } else if (std::holds_alternative<agent::TurnStartEvent>(event)) {
        record.emplace("type", util::JsonValue{"turn_start"});
    } else if (const auto* end = std::get_if<agent::TurnEndEvent>(&event)) {
        record.emplace("type", util::JsonValue{"turn_end"});
        auto message = message_value(end->message);
        if (!message) return std::unexpected(message.error());
        record.emplace("message", std::move(*message));
        auto tool_results = tool_result_message_values(end->tool_results);
        if (!tool_results) return std::unexpected(tool_results.error());
        record.emplace("toolResults", util::JsonValue{std::move(*tool_results)});
    } else if (const auto* start = std::get_if<agent::MessageStartEvent>(&event)) {
        record.emplace("type", util::JsonValue{"message_start"});
        auto message = message_value(start->message);
        if (!message) return std::unexpected(message.error());
        record.emplace("message", std::move(*message));
    } else if (const auto* update = std::get_if<agent::MessageUpdateEvent>(&event)) {
        record.emplace("type", util::JsonValue{"message_update"});
        auto message = message_value(update->message);
        if (!message) return std::unexpected(message.error());
        record.emplace("message", std::move(*message));
        auto assistant_event = assistant_event_value(update->assistant_event);
        if (!assistant_event) return std::unexpected(assistant_event.error());
        record.emplace("assistantMessageEvent", std::move(*assistant_event));
    } else if (const auto* end = std::get_if<agent::MessageEndEvent>(&event)) {
        record.emplace("type", util::JsonValue{"message_end"});
        auto message = message_value(end->message);
        if (!message) return std::unexpected(message.error());
        record.emplace("message", std::move(*message));
    } else if (const auto* start = std::get_if<agent::ToolExecutionStartEvent>(&event)) {
        record.emplace("type", util::JsonValue{"tool_execution_start"});
        record.emplace("toolCallId", util::JsonValue{bounded_redacted(start->tool_call_id)});
        record.emplace("toolName", util::JsonValue{bounded_redacted(start->tool_name)});
        record.emplace("args", safe_json_value(start->args));
    } else if (const auto* update = std::get_if<agent::ToolExecutionUpdateEvent>(&event)) {
        record.emplace("type", util::JsonValue{"tool_execution_update"});
        record.emplace("toolCallId", util::JsonValue{bounded_redacted(update->tool_call_id)});
        record.emplace("toolName", util::JsonValue{bounded_redacted(update->tool_name)});
        record.emplace("args", safe_json_value(update->args));
        if (auto result = tool_result_value(update->partial_result); !result) {
            return std::unexpected(result.error());
        } else {
            record.emplace("partialResult", std::move(*result));
        }
    } else if (const auto* end = std::get_if<agent::ToolExecutionEndEvent>(&event)) {
        record.emplace("type", util::JsonValue{"tool_execution_end"});
        record.emplace("toolCallId", util::JsonValue{bounded_redacted(end->tool_call_id)});
        record.emplace("toolName", util::JsonValue{bounded_redacted(end->tool_name)});
        auto result = tool_result_value(end->result);
        if (!result) return std::unexpected(result.error());
        record.emplace("result", std::move(*result));
        record.emplace("isError", util::JsonValue{end->is_error});
    }
    return write_record(std::move(record));
}


} // namespace cch::coding_agent::runtime
