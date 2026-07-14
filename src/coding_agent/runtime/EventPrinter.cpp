#include "EventPrinter.hpp"

#include <ostream>
#include <variant>

namespace cch::coding_agent::runtime {

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
    } else if (std::holds_alternative<agent::AgentEndEvent>(event)) {
        out << "[completed]\n";
    }
}

} // namespace cch::coding_agent::runtime
