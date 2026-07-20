#pragma once

#include "CliRenderer.hpp"

#include "coding_agent/runtime/JsonEventPrinter.hpp"

#include <ostream>

namespace cch::cli {

/// Direct-JSON presentation: a session header record followed by JSONL event
/// records. Frontend command outcomes are not AgentSession events, so command
/// results are suppressed rather than emitted as synthetic protocol records.
class JsonCliRenderer final : public CliRenderer {
public:
    JsonCliRenderer(std::ostream& output, std::ostream& error);

    [[nodiscard]] util::ExpectedVoid on_session_start(
        const harness::session::SessionMetadata& metadata) override;
    [[nodiscard]] util::ExpectedVoid on_event(
        const agent::AgentLifecycleEvent& event) override;
    [[nodiscard]] util::ExpectedVoid on_command_result(
        std::string_view input,
        std::string_view display_text) override;
    void on_prompt_error(std::string_view message) override;

private:
    coding_agent::runtime::JsonEventPrinter printer_;
    std::ostream& error_;
};

} // namespace cch::cli
