#pragma once

#include "CliRenderer.hpp"

#include <ostream>

namespace cch::cli {

/// Text-mode presentation: human-readable event rendering and command results
/// on the output stream and prompt failures on the error stream. Terminal
/// control is opt-in so non-TTY output never gains implicit ANSI.
class TextCliRenderer final : public CliRenderer {
public:
    TextCliRenderer(
        std::ostream& output,
        std::ostream& error,
        bool allow_terminal_control = true);

    [[nodiscard]] util::ExpectedVoid on_session_start(
        const harness::session::SessionMetadata& metadata) override;
    [[nodiscard]] util::ExpectedVoid on_event(
        const agent::AgentLifecycleEvent& event) override;
    [[nodiscard]] util::ExpectedVoid on_command_result(
        std::string_view input,
        std::string_view display_text) override;
    void on_prompt_error(std::string_view message) override;

private:
    std::ostream& output_;
    std::ostream& error_;
    bool allow_terminal_control_{false};
};

} // namespace cch::cli
