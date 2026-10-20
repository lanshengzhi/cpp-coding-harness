#include "ProgramStatusReporter.hpp"

#include <cch/tui/Terminal.hpp>

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>

namespace cch::coding_agent::tui {
namespace {

/// First line of an error, trimmed, with pi's "Error" fallback when nothing
/// remains (pi `firstLine`).
[[nodiscard]] std::string first_line(const std::optional<std::string>& text) {
    auto line = text.value_or(std::string{});
    const auto newline = line.find_first_of("\r\n");
    if (newline != std::string::npos) line.resize(newline);
    const auto first = line.find_first_not_of(" \t\v\f");
    if (first == std::string::npos) return "Error";
    const auto last = line.find_last_not_of(" \t\v\f");
    return line.substr(first, last - first + 1);
}

} // namespace

ProgramStatusReporter::ProgramStatusReporter(cch::tui::Terminal& terminal, std::string app)
    : terminal_(terminal), app_(std::move(app)) {}

void ProgramStatusReporter::handle_agent_start() {
    run_active_ = true;
    run_aborted_ = false;
    run_result_ = cch::tui::ProgramStatus{.state = cch::tui::ProgramStatus::State::Done};
    report();
}

void ProgramStatusReporter::handle_message_end(const cch::ai::MessageVariant& message) {
    const auto* assistant = std::get_if<cch::ai::AssistantMessage>(&message);
    if (assistant == nullptr) return;
    if (assistant->stop_reason == cch::ai::AssistantStopReason::Aborted) run_aborted_ = true;
    run_result_ = assistant->stop_reason == cch::ai::AssistantStopReason::Error
        ? cch::tui::ProgramStatus{
                  .state = cch::tui::ProgramStatus::State::Error,
                  .message = first_line(assistant->error_message),
          }
        : cch::tui::ProgramStatus{.state = cch::tui::ProgramStatus::State::Done};
    report();
}

void ProgramStatusReporter::handle_compaction_start() {
    compacting_ = true;
    report();
}

void ProgramStatusReporter::handle_compaction_end(
        bool aborted, const std::optional<std::string>& error_message, std::string_view reason) {
    compacting_ = false;
    if (run_active_) {
        // A failed recovery compaction ends the run unless a later response
        // succeeds; an aborted recovery leaves the run's outcome idle.
        if (aborted) {
            run_result_ = cch::tui::ProgramStatus{.state = cch::tui::ProgramStatus::State::Idle};
        } else if (error_message) {
            run_result_ = cch::tui::ProgramStatus{
                    .state = cch::tui::ProgramStatus::State::Error,
                    .message = first_line(error_message),
            };
        }
    } else if (aborted) {
        resting_status_ = cch::tui::ProgramStatus{.state = cch::tui::ProgramStatus::State::Idle};
    } else if (reason == "manual") {
        resting_status_ = error_message ? cch::tui::ProgramStatus{
                                                  .state = cch::tui::ProgramStatus::State::Error,
                                                  .message = first_line(error_message),
                                          }
                                        : cch::tui::ProgramStatus{
                                                  .state = cch::tui::ProgramStatus::State::Done,
                                          };
    }
    report();
}

void ProgramStatusReporter::handle_settled(bool aborted) {
    const bool run_aborted = aborted || run_aborted_;
    run_active_ = false;
    run_aborted_ = false;
    resting_status_ =
            run_aborted ? cch::tui::ProgramStatus{.state = cch::tui::ProgramStatus::State::Idle} : run_result_;
    report();
}

void ProgramStatusReporter::set_blocked(std::string source, std::optional<BlockedProgramStatus> status) {
    const auto found = std::find_if(
            blocked_.begin(), blocked_.end(), [&source](const auto& entry) { return entry.first == source; });
    if (found != blocked_.end()) blocked_.erase(found);
    if (status) blocked_.emplace_back(std::move(source), std::move(*status));
    report();
}

void ProgramStatusReporter::reset() {
    run_active_ = false;
    run_aborted_ = false;
    compacting_ = false;
    run_result_ = cch::tui::ProgramStatus{.state = cch::tui::ProgramStatus::State::Done};
    resting_status_ = cch::tui::ProgramStatus{.state = cch::tui::ProgramStatus::State::Idle};
    report();
}

void ProgramStatusReporter::report() {
    auto status = current_status();
    status.app = app_;
    if (last_report_ && *last_report_ == status) return;
    last_report_ = status;
    (void)terminal_.set_program_status(status);
}

cch::tui::ProgramStatus ProgramStatusReporter::current_status() const {
    if (!blocked_.empty()) {
        return cch::tui::ProgramStatus{
                .state = cch::tui::ProgramStatus::State::Blocked,
                .kind = blocked_.back().second.kind,
                .message = blocked_.back().second.message,
        };
    }
    if (compacting_) {
        return cch::tui::ProgramStatus{
                .state = cch::tui::ProgramStatus::State::Working,
                .message = "Compacting context",
        };
    }
    // pi names the session in working and done reports; pike sessions are
    // unnamed, so the message stays absent (the frozen unnamed-session
    // observation).
    return run_active_ ? cch::tui::ProgramStatus{.state = cch::tui::ProgramStatus::State::Working} : resting_status_;
}

} // namespace cch::coding_agent::tui
