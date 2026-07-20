#include "TextCliRenderer.hpp"

#include "coding_agent/runtime/EventPrinter.hpp"

namespace cch::cli {

TextCliRenderer::TextCliRenderer(std::ostream& output, std::ostream& error)
    : output_(output), error_(error) {}

util::ExpectedVoid TextCliRenderer::on_session_start(
    const harness::session::SessionMetadata& /*metadata*/) {
    return {};
}

util::ExpectedVoid TextCliRenderer::on_event(const agent::AgentLifecycleEvent& event) {
    coding_agent::runtime::print_agent_event(event, output_);
    return {};
}

util::ExpectedVoid TextCliRenderer::on_command_result(
    std::string_view input,
    std::string_view display_text) {
    if (input == "/clear") {
        output_ << "\033[2J\033[H";
        output_.flush();
        if (!output_) {
            return std::unexpected(util::make_error(
                util::ErrorCode::Unknown,
                "failed to clear terminal"));
        }
        return {};
    }
    if (!display_text.empty()) {
        output_ << display_text << '\n';
    }
    return {};
}

void TextCliRenderer::on_prompt_error(std::string_view message) {
    error_ << "loop failed: " << message << '\n';
}

} // namespace cch::cli
