#include "JsonEventPrinter.hpp"

#include "../../../include/cch/ai/glaze/AiJson.hpp"
#include "../../../include/cch/util/Json.hpp"

#include <algorithm>
#include <ostream>
#include <string>
#include <utility>
#include <variant>

namespace cch::coding_agent::runtime {
namespace {
constexpr int kSchemaVersion = 1;
constexpr std::size_t kMaxTextBytes = 8192;
constexpr std::size_t kMaxDiagnosticBytes = 512;

[[nodiscard]] util::Error json_event_error(std::string message, std::string detail = {}) {
    return util::make_error(util::ErrorCode::JsonSerialize, std::move(message), std::move(detail));
}

void add_common(util::JsonValue::object_t& record, int seq) {
    record.emplace("schemaVersion", util::JsonValue{kSchemaVersion});
    record.emplace("seq", util::JsonValue{seq});
}

[[nodiscard]] std::string bounded(std::string text, std::size_t max_bytes) {
    if (text.size() <= max_bytes) {
        return text;
    }
    text.resize(max_bytes);
    return text;
}

[[nodiscard]] bool was_truncated(const std::string& text, std::size_t max_bytes) {
    return text.size() > max_bytes;
}

[[nodiscard]] std::string stop_reason(ai::AssistantStopReason reason) {
    return ai::glaze::stop_reason_to_json(reason);
}

[[nodiscard]] util::JsonValue::object_t content_status(const std::string& status, const std::string& reason, std::size_t bytes) {
    util::JsonValue::object_t value;
    value.emplace("status", util::JsonValue{status});
    value.emplace("reason", util::JsonValue{reason});
    value.emplace("bytes", util::JsonValue{static_cast<double>(bytes)});
    return value;
}

} // namespace

JsonEventPrinter::JsonEventPrinter(std::ostream& out) : out_(&out) {}

int JsonEventPrinter::next_seq() { return next_seq_++; }

util::ExpectedVoid JsonEventPrinter::write_record(util::JsonValue::object_t record) {
    add_common(record, next_seq());
    auto serialized = util::write_json(util::JsonValue{std::move(record)});
    if (!serialized) {
        return std::unexpected(serialized.error());
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
    if (std::holds_alternative<agent::AgentEndEvent>(event)) {
        return {};
    }
    if (std::holds_alternative<agent::ThinkingUpdateEvent>(event) ||
        std::holds_alternative<agent::QueuedMessageStartEvent>(event) ||
        std::holds_alternative<agent::QueuedMessageEndEvent>(event) ||
        std::holds_alternative<agent::ToolCallStreamStartEvent>(event) ||
        std::holds_alternative<agent::ToolCallStreamUpdateEvent>(event) ||
        std::holds_alternative<agent::ToolCallStreamEndEvent>(event)) {
        return {};
    }

    util::JsonValue::object_t record;
    if (std::holds_alternative<agent::AgentStartEvent>(event)) {
        record.emplace("type", util::JsonValue{"agent_start"});
    } else if (const auto* turn = std::get_if<agent::TurnStartEvent>(&event)) {
        record.emplace("type", util::JsonValue{"turn_start"});
        record.emplace("turn", util::JsonValue{turn->turn});
    } else if (const auto* start = std::get_if<agent::MessageStartEvent>(&event)) {
        record.emplace("type", util::JsonValue{"message_start"});
        record.emplace("turn", util::JsonValue{start->turn});
        record.emplace("contentStatus", util::JsonValue{content_status("omitted", "unsupported_in_v1", 0)});
    } else if (const auto* update = std::get_if<agent::MessageUpdateEvent>(&event)) {
        const auto truncated = was_truncated(update->delta, kMaxTextBytes);
        util::JsonValue::object_t assistant_event;
        assistant_event.emplace("type", util::JsonValue{"text_delta"});
        assistant_event.emplace("delta", util::JsonValue{bounded(update->delta, kMaxTextBytes)});
        assistant_event.emplace("truncated", util::JsonValue{truncated});
        if (truncated) {
            assistant_event.emplace("omissionReason", util::JsonValue{"truncated"});
        }
        record.emplace("type", util::JsonValue{"message_update"});
        record.emplace("turn", util::JsonValue{update->turn});
        record.emplace("assistantMessageEvent", util::JsonValue{std::move(assistant_event)});
    } else if (const auto* end = std::get_if<agent::MessageEndEvent>(&event)) {
        record.emplace("type", util::JsonValue{"message_end"});
        record.emplace("turn", util::JsonValue{end->turn});
        record.emplace("contentStatus", util::JsonValue{content_status("omitted", "unsupported_in_v1", 0)});
    } else if (const auto* start = std::get_if<agent::ToolExecutionStartEvent>(&event)) {
        record.emplace("type", util::JsonValue{"tool_execution_start"});
        record.emplace("turn", util::JsonValue{start->turn});
        record.emplace("toolCallId", util::JsonValue{start->tool_call_id});
        record.emplace("toolName", util::JsonValue{start->tool_name});
    } else if (const auto* end = std::get_if<agent::ToolExecutionEndEvent>(&event)) {
        record.emplace("type", util::JsonValue{"tool_execution_end"});
        record.emplace("turn", util::JsonValue{end->turn});
        record.emplace("toolCallId", util::JsonValue{end->tool_call_id});
        record.emplace("toolName", util::JsonValue{end->tool_name});
        record.emplace("isError", util::JsonValue{end->is_error});
        record.emplace("contentStatus", util::JsonValue{content_status("omitted", "unsupported_in_v1", end->content.size())});
    } else if (const auto* turn = std::get_if<agent::TurnEndEvent>(&event)) {
        record.emplace("type", util::JsonValue{"turn_end"});
        record.emplace("turn", util::JsonValue{turn->turn});
        record.emplace("stopReason", util::JsonValue{stop_reason(turn->stop_reason)});
    } else {
        return {};
    }
    return write_record(std::move(record));
}

util::ExpectedVoid JsonEventPrinter::print_terminal(bool success, std::string code, std::string message) {
    util::JsonValue::object_t record;
    record.emplace("type", util::JsonValue{"runtime_terminal"});
    record.emplace("success", util::JsonValue{success});
    record.emplace("code", util::JsonValue{std::move(code)});
    if (!message.empty()) {
        const auto truncated = was_truncated(message, kMaxDiagnosticBytes);
        record.emplace("message", util::JsonValue{bounded(std::move(message), kMaxDiagnosticBytes)});
        record.emplace("messageTruncated", util::JsonValue{truncated});
    }
    return write_record(std::move(record));
}

} // namespace cch::coding_agent::runtime
