#include "EventPrinter.hpp"

#include "BoundedText.hpp"

#include <ostream>
#include <string>
#include <string_view>
#include <variant>

namespace cch::coding_agent::runtime {

namespace {

/// The one presented terminal diagnostic for an accepted error/aborted
/// outcome, under the same redaction and bounded-output policy as structured
/// frontend output.
[[nodiscard]] std::string terminal_diagnostic(
    const ai::AssistantMessage& assistant,
    std::string_view fallback) {
    if (assistant.error_message && !assistant.error_message->empty()) {
        return bounded_redacted(*assistant.error_message);
    }
    return std::string{fallback};
}

} // namespace

void print_agent_event(const agent::AgentLifecycleEvent& event, std::ostream& out) {
    if (std::holds_alternative<agent::TurnStartEvent>(event)) {
        out << "[model-request]\n";
    } else if (const auto* update = std::get_if<agent::MessageUpdateEvent>(&event)) {
        if (const auto* delta = std::get_if<ai::TextDeltaEvent>(&update->assistant_event)) {
            out << "[assistant] " << delta->delta << '\n';
        }
    } else if (const auto* start = std::get_if<agent::ToolExecutionStartEvent>(&event)) {
        out << "[tool-call] " << start->tool_name << '#' << start->tool_call_id << '\n';
    } else if (const auto* end = std::get_if<agent::ToolExecutionEndEvent>(&event)) {
        out << (end->is_error ? "[tool-error] " : "[tool-success] ") << end->tool_call_id << '\n';
        if (end->is_error && !end->result.content.empty()) {
            out << ai::text_from_content(end->result.content) << '\n';
        }
    } else if (const auto* end = std::get_if<agent::MessageEndEvent>(&event)) {
        if (const auto* assistant = std::get_if<ai::AssistantMessage>(&end->message)) {
            // An accepted error/aborted outcome is presented exactly once, at
            // the final Assistant Message's message_end. The prompt itself
            // completed, so this never also routes through the prompt-error
            // ("loop failed") path.
            if (assistant->stop_reason == ai::AssistantStopReason::Error) {
                out << "[error] " << terminal_diagnostic(*assistant, "unknown error") << '\n';
            } else if (assistant->stop_reason == ai::AssistantStopReason::Aborted) {
                out << "[aborted] " << terminal_diagnostic(*assistant, "operation aborted") << '\n';
            }
        }
    } else if (std::holds_alternative<agent::AgentEndEvent>(event)) {
        out << "[completed]\n";
    }
}

} // namespace cch::coding_agent::runtime
