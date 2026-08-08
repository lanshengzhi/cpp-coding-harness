#pragma once

#include "../../include/cch/agent/AgentEvent.hpp"
#include "../../include/cch/harness/session/SessionEntry.hpp"
#include "../../include/cch/util/Error.hpp"

#include <string_view>

namespace cch::cli {

/// Presentation seam for one-shot CLI output. OneShotCliFrontend owns command
/// dispatch and run outcomes; a CliRenderer owns how session start, Agent
/// events, command results, and prompt failures are presented. The current
/// implementation is TextCliRenderer (the removed JSON renderer is gone with
/// the JSON CLI mode, ADR 0036).
class CliRenderer {
public:
    virtual ~CliRenderer() = default;

    /// Present the start of an Agent Session (e.g. a JSON session header
    /// record). Text mode presents nothing.
    [[nodiscard]] virtual util::ExpectedVoid on_session_start(
        const harness::session::SessionMetadata& metadata) = 0;

    /// Present one agent lifecycle event.
    [[nodiscard]] virtual util::ExpectedVoid on_event(
        const agent::AgentLifecycleEvent& event) = 0;

    /// Present a frontend command result. `input` is the raw command line
    /// (text mode special-cases /clear); JSON mode suppresses command output.
    [[nodiscard]] virtual util::ExpectedVoid on_command_result(
        std::string_view input,
        std::string_view display_text) = 0;

    /// Present a failed prompt.
    virtual void on_prompt_error(std::string_view message) = 0;
};

} // namespace cch::cli
