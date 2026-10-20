#pragma once

#include <cch/tui/ProgramStatus.hpp>

#include <cch/ai/Message.hpp>

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::tui {
class Terminal;
} // namespace cch::tui

namespace cch::coding_agent::tui {

/// What a blocked program waits for (pi `BlockedStatus`).
struct BlockedProgramStatus {
    cch::tui::ProgramStatus::Kind kind{cch::tui::ProgramStatus::Kind::Question};
    std::string message;
};

/// Reports interactive-mode state to the terminal (OSC 7501, pi
/// `ProgramStatusReporter`): `working` during agent runs and compaction,
/// `blocked` while a dialog waits for the user, then `done`, `error`, or `idle`
/// once the run settles. Messages are limited to dialog titles and the first
/// line of errors; prompts and assistant output are never reported.
class ProgramStatusReporter final {
public:
    /// The terminal must outlive the reporter. `session_name` reads the
    /// current Session (pi `getSessionName`); an empty function reports an
    /// unnamed session.
    ///
    /// Lifetime contract (CODING_STANDARDS.md 7.5): the getter borrows the
    /// owning object, so it is invoked only while that owner is alive. The
    /// reporter is a member of the owner, so the callback can never outlive
    /// its referent, and the owner must not invoke it after its own
    /// destruction. The getter is called from the owner's serialized domain
    /// (the interactive engine's own executor), never concurrently.
    ProgramStatusReporter(cch::tui::Terminal& terminal,
            std::string app,
            std::move_only_function<std::optional<std::string>()> session_name = {});

    /// pi AgentSessionEvent `agent_start`: a run is active until it settles.
    void handle_agent_start();
    /// pi AgentSessionEvent `message_end`: the latest assistant response
    /// decides the run outcome, so a retried error is replaced by its
    /// successful retry. Non-assistant messages do not report.
    void handle_message_end(const cch::ai::MessageVariant& message);
    void handle_compaction_start();
    /// pi AgentSessionEvent `compaction_end`: settle the compaction override
    /// and, for a manual compaction outside a run, the resting status.
    void handle_compaction_end(bool aborted, const std::optional<std::string>& error_message, std::string_view reason);
    /// pi AgentSessionEvent `agent_settled`: the run's outcome becomes the
    /// resting status, and an aborted run returns to idle. `aborted` carries
    /// the Session's abort request for the run (a run can be aborted before it
    /// reports an aborted response).
    void handle_settled(bool aborted);

    /// Report `blocked` for a dialog until it is cleared with `nullopt`.
    /// Reopening a source replaces it; the most recent open dialog is reported.
    void set_blocked(std::string source, std::optional<BlockedProgramStatus> status);

    /// Forget the previous session's run, for example after switching sessions.
    void reset();
    /// Re-report the current status after the session name changed (pi
    /// `session_info_changed`).
    void refresh();

private:
    void report();
    [[nodiscard]] cch::tui::ProgramStatus current_status();

    cch::tui::Terminal& terminal_;
    std::string app_;
    /// pi `getSessionName`: reads the owning object's current Session.
    std::move_only_function<std::optional<std::string>()> session_name_;
    bool run_active_{false};
    /// The run's latest response was aborted, so it returns to idle when it
    /// settles even though the Session's prompt resolves successfully (pi
    /// `agent_settled` `aborted`, observed through the response).
    bool run_aborted_{false};
    bool compacting_{false};
    /// Outcome of the current run, reported once it settles.
    cch::tui::ProgramStatus run_result_{.state = cch::tui::ProgramStatus::State::Done};
    /// Status while no run is active.
    cch::tui::ProgramStatus resting_status_{.state = cch::tui::ProgramStatus::State::Idle};
    /// Open dialogs by source, in the order they opened; the most recent one
    /// is reported.
    std::vector<std::pair<std::string, BlockedProgramStatus>> blocked_;
    std::optional<cch::tui::ProgramStatus> last_report_{std::nullopt};
};

} // namespace cch::coding_agent::tui
