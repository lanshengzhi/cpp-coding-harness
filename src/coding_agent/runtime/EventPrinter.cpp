#include "EventPrinter.hpp"

#include <ostream>
#include <variant>

namespace cch::coding_agent::runtime {

std::string text_from_content(const std::vector<ai::Content>& content) {
    std::string text;
    for (const auto& block : content) {
        if (const auto* text_block = std::get_if<ai::TextContent>(&block)) {
            text += text_block->text;
        }
    }
    return text;
}

void print_agent_event(const agent::AgentLifecycleEvent& event, std::ostream& out) {
    if (const auto* turn = std::get_if<agent::TurnStartEvent>(&event)) {
        out << "[model-request] turn " << turn->turn << '\n';
    } else if (const auto* update = std::get_if<agent::MessageUpdateEvent>(&event)) {
        out << "[assistant] " << update->delta << '\n';
    } else if (const auto* start = std::get_if<agent::ToolExecutionStartEvent>(&event)) {
        out << "[tool-call] " << start->tool_name << '#' << start->tool_call_id << '\n';
    } else if (const auto* end = std::get_if<agent::ToolExecutionEndEvent>(&event)) {
        out << (end->is_error ? "[tool-error] " : "[tool-success] ") << end->tool_call_id << '\n';
        if (end->is_error && !end->content.empty()) {
            out << end->content << '\n';
        }
    } else if (const auto* done = std::get_if<agent::AgentEndEvent>(&event)) {
        if (done->success) {
            out << "[completed] " << done->reason << '\n';
        } else if (done->reason == "max turns exceeded") {
            out << "[max-turns] max_turns_exceeded\n";
        } else {
            out << "[provider-error] " << done->reason << '\n';
        }
    }
}

} // namespace cch::coding_agent::runtime
