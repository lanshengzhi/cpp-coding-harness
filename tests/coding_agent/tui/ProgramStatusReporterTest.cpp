// Native TUI program-status reporter state machine through the terminal
// contract (#1123 TStatus, pi `ProgramStatusReporter`). The frozen whole-product
// observation is fixtures/pi-v1.3.0/audit-1051/program-status.json (revision
// 71023aec1546); the cases below mirror its reporting order: idle, blocked,
// idle, working, done, working, error — with pike's released app name and
// unnamed sessions (no done/working message in the frozen unnamed-session
// observation).

#include "coding_agent/tui/ProgramStatusReporter.hpp"
#include "support/ProtocolRecordingTerminal.hpp"

#include <cch/tui/ProgramStatus.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <vector>

namespace {

using cch::tui::ProgramStatus;

[[nodiscard]] cch::ai::MessageVariant assistant_end(
        cch::ai::AssistantStopReason reason, std::optional<std::string> error_message = std::nullopt) {
    cch::ai::AssistantMessage message;
    message.stop_reason = reason;
    message.error_message = std::move(error_message);
    message.content.emplace_back(cch::ai::text_content("secret assistant output"));
    return cch::ai::MessageVariant{std::move(message)};
}

/// The reporter plus the terminal-contract recording, one started VirtualTerminal
/// so the terminal accepts the reports.
struct ReporterFixture {
    cch::tui::VirtualTerminal terminal{{.columns = 80, .rows = 24}};
    cch::tests::ProtocolRecordingTerminal recording{terminal};
    cch::coding_agent::tui::ProgramStatusReporter reporter{recording, "pike"};

    ReporterFixture() {
        REQUIRE(recording.start([](std::string) -> cch::support::ExpectedVoid { return {}; },
                [](cch::tui::TerminalDimensions) -> cch::support::ExpectedVoid { return {}; }));
    }

    [[nodiscard]] std::vector<ProgramStatus> reports() const {
        std::vector<ProgramStatus> statuses;
        for (const auto& op : recording.ops()) {
            if (op.kind == cch::tests::ProtocolRecordingTerminal::OpKind::SetProgramStatus) {
                statuses.push_back(op.program_status);
            }
        }
        return statuses;
    }

    [[nodiscard]] ProgramStatus last() const {
        const auto statuses = reports();
        REQUIRE_FALSE(statuses.empty());
        return statuses.back();
    }
};

[[nodiscard]] ProgramStatus idle_status() { return ProgramStatus{.state = ProgramStatus::State::Idle, .app = "pike"}; }

[[nodiscard]] ProgramStatus working_status() {
    return ProgramStatus{.state = ProgramStatus::State::Working, .app = "pike"};
}

[[nodiscard]] ProgramStatus done_status() { return ProgramStatus{.state = ProgramStatus::State::Done, .app = "pike"}; }

} // namespace

TEST_CASE("Program status reporter reports idle, working during a run, and done once it settles",
        "[coding_agent][tui][program-status][issue1123][spec]") {
    ReporterFixture fixture;
    fixture.reporter.reset();
    REQUIRE(fixture.last() == idle_status());

    fixture.reporter.handle_agent_start();
    REQUIRE(fixture.last() == working_status());

    fixture.reporter.handle_message_end(assistant_end(cch::ai::AssistantStopReason::ToolUse));
    fixture.reporter.handle_message_end(assistant_end(cch::ai::AssistantStopReason::Stop));
    CHECK(fixture.last() == working_status());

    fixture.reporter.handle_settled(false);
    CHECK(fixture.last() == done_status());
    for (const auto& status : fixture.reports()) {
        if (status.message) CHECK(*status.message != "secret assistant output");
    }
}

TEST_CASE("Program status reporter reports only the outcome of the run",
        "[coding_agent][tui][program-status][issue1123][spec]") {
    ReporterFixture fixture;
    // A retried error is replaced by its successful retry.
    fixture.reporter.handle_agent_start();
    fixture.reporter.handle_message_end(assistant_end(cch::ai::AssistantStopReason::Error, "overloaded"));
    fixture.reporter.handle_message_end(assistant_end(cch::ai::AssistantStopReason::Stop));
    fixture.reporter.handle_settled(false);
    CHECK(fixture.last() == done_status());

    // The final error keeps only its first line.
    fixture.reporter.handle_agent_start();
    fixture.reporter.handle_message_end(
            assistant_end(cch::ai::AssistantStopReason::Error, "Invalid API key\n{details}"));
    fixture.reporter.handle_settled(false);
    CHECK(fixture.last() == ProgramStatus{
                                    .state = ProgramStatus::State::Error,
                                    .app = "pike",
                                    .message = "Invalid API key",
                            });

    // An aborted run reports idle when it settles; the Session's abort is
    // visible either through the aborted response...
    fixture.reporter.handle_agent_start();
    fixture.reporter.handle_message_end(assistant_end(cch::ai::AssistantStopReason::Aborted));
    fixture.reporter.handle_settled(false);
    CHECK(fixture.last() == idle_status());

    // ...or through the abort request for a run whose last response was fine
    // (an interrupt during a tool call).
    fixture.reporter.handle_agent_start();
    fixture.reporter.handle_message_end(assistant_end(cch::ai::AssistantStopReason::Stop));
    fixture.reporter.handle_settled(true);
    CHECK(fixture.last() == idle_status());
}

TEST_CASE("Program status reporter reports a failed recovery compaction as the run error",
        "[coding_agent][tui][program-status][issue1123][spec]") {
    ReporterFixture fixture;
    fixture.reporter.handle_agent_start();
    fixture.reporter.handle_message_end(assistant_end(cch::ai::AssistantStopReason::Length));
    fixture.reporter.handle_compaction_start();
    fixture.reporter.handle_compaction_end(false, std::string{"Compaction failed\nstack"}, "overflow");
    fixture.reporter.handle_settled(false);
    CHECK(fixture.last() == ProgramStatus{
                                    .state = ProgramStatus::State::Error,
                                    .app = "pike",
                                    .message = "Compaction failed",
                            });

    // A later successful response replaces the recovery error.
    fixture.reporter.handle_agent_start();
    fixture.reporter.handle_compaction_start();
    fixture.reporter.handle_compaction_end(false, std::string{"Compaction failed"}, "threshold");
    fixture.reporter.handle_message_end(assistant_end(cch::ai::AssistantStopReason::Stop));
    fixture.reporter.handle_settled(false);
    CHECK(fixture.last() == done_status());
}

TEST_CASE("Program status reporter reports compaction inside a run and a manual compaction result",
        "[coding_agent][tui][program-status][issue1123][spec]") {
    ReporterFixture fixture;
    fixture.reporter.handle_agent_start();
    fixture.reporter.handle_compaction_start();
    CHECK(fixture.last() == ProgramStatus{
                                    .state = ProgramStatus::State::Working,
                                    .app = "pike",
                                    .message = "Compacting context",
                            });
    fixture.reporter.handle_compaction_end(false, std::nullopt, "threshold");
    CHECK(fixture.last() == working_status());
    fixture.reporter.handle_message_end(assistant_end(cch::ai::AssistantStopReason::Stop));
    fixture.reporter.handle_settled(false);
    CHECK(fixture.last() == done_status());

    // A manual compaction outside a run reports its own outcome.
    fixture.reporter.handle_compaction_start();
    fixture.reporter.handle_compaction_end(false, std::nullopt, "manual");
    CHECK(fixture.last() == done_status());
    fixture.reporter.handle_compaction_start();
    fixture.reporter.handle_compaction_end(false, std::string{"No model"}, "manual");
    CHECK(fixture.last() == ProgramStatus{
                                    .state = ProgramStatus::State::Error,
                                    .app = "pike",
                                    .message = "No model",
                            });
    fixture.reporter.handle_compaction_start();
    fixture.reporter.handle_compaction_end(true, std::nullopt, "manual");
    CHECK(fixture.last() == idle_status());
}

TEST_CASE("Program status reporter reports the most recent open dialog and the underlying state",
        "[coding_agent][tui][program-status][issue1123][spec]") {
    using cch::coding_agent::tui::BlockedProgramStatus;
    ReporterFixture fixture;
    fixture.reporter.handle_agent_start();
    fixture.reporter.set_blocked("extension-selector",
            BlockedProgramStatus{.kind = ProgramStatus::Kind::Permission, .message = "Allow bash?"});
    fixture.reporter.set_blocked(
            "login", BlockedProgramStatus{.kind = ProgramStatus::Kind::Auth, .message = "Log in to Anthropic"});
    CHECK(fixture.last() == ProgramStatus{
                                    .state = ProgramStatus::State::Blocked,
                                    .app = "pike",
                                    .kind = ProgramStatus::Kind::Auth,
                                    .message = "Log in to Anthropic",
                            });

    // The run settles while the selector is still open: the dialog stays
    // reported.
    fixture.reporter.set_blocked("login", std::nullopt);
    fixture.reporter.handle_message_end(assistant_end(cch::ai::AssistantStopReason::Stop));
    fixture.reporter.handle_settled(false);
    CHECK(fixture.last() == ProgramStatus{
                                    .state = ProgramStatus::State::Blocked,
                                    .app = "pike",
                                    .kind = ProgramStatus::Kind::Permission,
                                    .message = "Allow bash?",
                            });

    // Reopening a source replaces its dialog instead of stacking a second one.
    fixture.reporter.set_blocked(
            "extension-selector", BlockedProgramStatus{.kind = ProgramStatus::Kind::Question, .message = "Pick one"});
    CHECK(fixture.last() == ProgramStatus{
                                    .state = ProgramStatus::State::Blocked,
                                    .app = "pike",
                                    .kind = ProgramStatus::Kind::Question,
                                    .message = "Pick one",
                            });
    fixture.reporter.set_blocked("extension-selector", std::nullopt);
    CHECK(fixture.last() == done_status());
}

TEST_CASE("Program status reporter sends each status once", "[coding_agent][tui][program-status][issue1123][spec]") {
    ReporterFixture fixture;
    fixture.reporter.handle_agent_start();
    fixture.reporter.handle_agent_start();
    fixture.reporter.handle_message_end(assistant_end(cch::ai::AssistantStopReason::ToolUse));
    CHECK(fixture.reports().size() == 1);

    fixture.reporter.handle_settled(false);
    CHECK(fixture.reports().size() == 2);
}

TEST_CASE("Program status reporter returns to idle when the session is replaced",
        "[coding_agent][tui][program-status][issue1123][spec]") {
    ReporterFixture fixture;
    fixture.reporter.handle_agent_start();
    fixture.reporter.handle_message_end(assistant_end(cch::ai::AssistantStopReason::Stop));
    fixture.reporter.handle_settled(false);
    REQUIRE(fixture.last() == done_status());

    fixture.reporter.reset();
    CHECK(fixture.last() == idle_status());
}
