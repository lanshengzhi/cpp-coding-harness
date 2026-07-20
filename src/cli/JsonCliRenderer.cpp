#include "JsonCliRenderer.hpp"

namespace cch::cli {

JsonCliRenderer::JsonCliRenderer(std::ostream& output, std::ostream& error)
    : printer_(output), error_(error) {}

util::ExpectedVoid JsonCliRenderer::on_session_start(
    const harness::session::SessionMetadata& metadata) {
    return printer_.print_session_header(metadata);
}

util::ExpectedVoid JsonCliRenderer::on_event(const agent::AgentLifecycleEvent& event) {
    return printer_.print_agent_event(event);
}

util::ExpectedVoid JsonCliRenderer::on_command_result(
    std::string_view /*input*/,
    std::string_view /*display_text*/) {
    // Frontend command outcomes are not AgentSession events. JSON mode
    // therefore emits no synthetic protocol record for them.
    return {};
}

void JsonCliRenderer::on_prompt_error(std::string_view message) {
    error_ << "loop failed: " << message << '\n';
}

} // namespace cch::cli
