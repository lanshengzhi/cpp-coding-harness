// Whole-product OSC 7501 program status through the native TUI public entry
// (#1123 TStatus): `run_interactive_mode` over a real Session, the scripted
// deterministic Provider, and a recording terminal. The expected report order
// replays fixtures/pi-v1.3.0/audit-1051/program-status.json (revision
// 71023aec1546): idle, blocked(kind=question), idle, working, done, working,
// error — with pike's app name and unnamed sessions. The blocked case uses the
// boot project-trust selector, the only dialog pike exposes at this seam; the
// frozen observation used an extension dialog (documented difference, see the
// issue evidence).
//
// The recordings prove the terminal-contract reports and their order. The raw
// bytes of the same reports are pinned in tests/tui/ProcessTerminalProgramStatusTest.cpp
// and the dated pseudo-terminal capture of the installed binary.

#include "coding_agent/tui/InteractiveMode.hpp"
#include "coding_agent/tui/InteractiveSessionRun.hpp"
#include "coding_agent/tui/TestTuiActionSink.hpp"

#include "support/AgentRootFixture.hpp"
#include "support/EnvVarGuard.hpp"
#include "support/ModelsFixture.hpp"
#include "support/ProtocolRecordingTerminal.hpp"
#include "support/PumpUntil.hpp"
#include "support/RuntimeFixture.hpp"
#include "support/RuntimeLoopDriver.hpp"
#include "support/ScriptedRuntimeFixture.hpp"
#include "support/TempWorkspace.hpp"

#include "coding_agent/environment/RuntimeRoot.hpp"

#include <cch/ai/Content.hpp>
#include <cch/ai/InferenceFailure.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include "coding_agent/AgentSession.hpp"
#include "coding_agent/ModelRuntimeTestSupport.hpp"
#include "coding_agent/runtime/SessionFactory.hpp"

#include <catch2/catch_test_macros.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace cch;
using tests::drain_ready;
using tests::pump_until;

namespace {

/// Serialize every recorded status through the Owner's formatter. The expected
/// values below are frozen wire literals from
/// fixtures/pi-v1.3.0/audit-1051/program-status.json (unnamed) and
/// fixtures/pi-v1.3.0/audit-1123/named-session-program-status.json, never built
/// by the same formatter.
[[nodiscard]] std::vector<std::string> reported_wire(const tests::ProtocolRecordingTerminal& terminal) {
    std::vector<std::string> wires;
    for (const auto& op : terminal.ops()) {
        if (op.kind == tests::ProtocolRecordingTerminal::OpKind::SetProgramStatus) {
            wires.push_back(tui::format_program_status(op.program_status));
        }
    }
    return wires;
}

[[nodiscard]] bool reported_wire_contains(const tests::ProtocolRecordingTerminal& terminal, std::string_view wire) {
    const auto wires = reported_wire(terminal);
    return std::find(wires.begin(), wires.end(), wire) != wires.end();
}

constexpr std::string_view kIdleWire = "\x1b]7501;state=idle:app=pike\x1b\\";
constexpr std::string_view kWorkingWire = "\x1b]7501;state=working:app=pike\x1b\\";
constexpr std::string_view kDoneWire = "\x1b]7501;state=done:app=pike\x1b\\";
constexpr std::string_view kErrorWire = "\x1b]7501;state=error:app=pike:msg=SW52YWxpZCBBUEkga2V5\x1b\\";
constexpr std::string_view kBlockedTrustWire =
        "\x1b]7501;state=blocked:app=pike:kind=question:msg=VHJ1c3QgcHJvamVjdCBmb2xkZXI/\x1b\\";
constexpr std::string_view kNamedWorkingWire = "\x1b]7501;state=working:app=pike:msg=cmV2aWV3LXNlc3Npb24=\x1b\\";
constexpr std::string_view kNamedDoneWire = "\x1b]7501;state=done:app=pike:msg=cmV2aWV3LXNlc3Npb24=\x1b\\";
constexpr std::string_view kRenamedAfterDoneWire = "\x1b]7501;state=done:app=pike:msg=cmVuYW1lZC1zZXNzaW9u\x1b\\";
constexpr std::string_view kRenamedWhileWorkingWire =
        "\x1b]7501;state=working:app=pike:msg=cmVuYW1lZC13aGlsZS13b3JraW5n\x1b\\";

/// The most recent report, or a default status when none was recorded.
[[nodiscard]] tui::ProgramStatus last_status(const tests::ProtocolRecordingTerminal& terminal) {
    tui::ProgramStatus status;
    for (const auto& op : terminal.ops()) {
        if (op.kind == tests::ProtocolRecordingTerminal::OpKind::SetProgramStatus) status = op.program_status;
    }
    return status;
}

[[nodiscard]] auto make_run(coding_agent::AgentSession& session, const std::filesystem::path& config_dir = {}) {
    return coding_agent::tui::InteractiveSessionRunBuilder{}
            .with_session(session)
            .with_agent_config_directory(config_dir)
            .build();
}

/// A live Session over the deterministic scripted Provider, entering the
/// interactive mode through the same builder the CLI boot uses.
struct SessionProbe {
    tests::TempWorkspace workspace;
    tests::TempWorkspace config;
    // HOME points at the temporary config so the trust store and settings are
    // hermetic; declared before the runtime and session so they are destroyed
    // first.
    tests::EnvVarGuard home_guard{"HOME"};
    tests::ScriptedRuntimeFixture scripted;
    tests::RuntimeFixture runtime;
    /// When set, boot a persisted session so `/name` has a `session_info`
    /// surface (`InMemorySessionTarget` drops the name).
    std::filesystem::path persisted_session;
    std::unique_ptr<coding_agent::AgentSession> session;
    /// Services the Session's RuntimeRoot loop while the interactive mode runs
    /// on the test's io_context; declared after the fixture it borrows.
    tests::RuntimeLoopDriver runtime_driver{runtime};

    void boot() {
        home_guard.set(config.path().string());
        coding_agent::runtime::AgentSessionCreationRequest request;
        request.workspace = workspace.path();
        request.session_target = persisted_session.empty()
                                         ? coding_agent::SessionTarget{coding_agent::InMemorySessionTarget{}}
                                         : coding_agent::SessionTarget{
                                                   coding_agent::ExplicitOpenOrCreateSessionTarget{persisted_session}};
        request.session_facts.model = "fake/fake-model";
        request.session_facts.no_skills = true;
        request.session_facts.no_prompt_templates = true;
        request.execution_runtime_target = runtime.make_target();
        request.model_runtime = scripted.runtime;
        coding_agent::isolate_magpie_gateway(request);
        auto created = runtime.run(coding_agent::create_agent_session_async(std::move(request), std::nullopt, {}));
        REQUIRE(created);
        session = std::move(created->session);
    }
};

/// The recording terminal in front of a 100x30 VirtualTerminal, running the
/// public interactive entry point.
struct InteractiveProbe {
    tui::VirtualTerminal terminal{{.columns = 100, .rows = 30}};
    tests::ProtocolRecordingTerminal recording{terminal};
    boost::asio::io_context io;
    std::optional<support::ExpectedVoid> run_result;

    void start(coding_agent::AgentSession& session, const std::filesystem::path& config_dir) {
        boost::asio::co_spawn(io,
                coding_agent::tui::run_interactive_mode(recording, make_run(session, config_dir)),
                [this](std::exception_ptr exception, support::ExpectedVoid result) {
                    CHECK(exception == nullptr);
                    run_result.emplace(std::move(result));
                });
        drain_ready(io);
    }

    [[nodiscard]] std::string screen() const {
        std::string text;
        for (const auto& line : terminal.screen()) {
            text.append(line);
            text.push_back('\n');
        }
        return text;
    }

    void type(std::string_view text) {
        REQUIRE(terminal.inject_input(std::string{text}));
        // A lone ESC stays an unresolved fragment until the decoder flushes;
        // every submission here is complete, so resolve it immediately.
        REQUIRE(terminal.flush_input());
        drain_ready(io);
    }

    void exit() {
        type("\x04");
        REQUIRE(pump_until(io, [this] { return run_result.has_value(); }));
        CHECK(*run_result);
    }
};

} // namespace

TEST_CASE("Native TUI reports idle, working, and done for a normal prompt",
        "[coding_agent][tui][program-status][issue1123][spec]") {
    SessionProbe session_fixture;
    session_fixture.boot();
    // Gate the model turn so the in-progress report is observable before the
    // outcome replaces it.
    session_fixture.scripted.control->gate_at = 0;
    session_fixture.scripted.control->responses.push_back(ai::assistant_text_message("status answer"));

    InteractiveProbe probe;
    probe.start(*session_fixture.session, session_fixture.config.path());
    REQUIRE(pump_until(probe.io,
            [&] { return reported_wire(probe.recording) == std::vector<std::string>{std::string{kIdleWire}}; }));

    probe.type("status prompt\r");
    REQUIRE(pump_until(probe.io, [&] { return reported_wire_contains(probe.recording, kWorkingWire); }));
    // The run is held open: working stays reported and no outcome leaks early.
    CHECK(reported_wire(probe.recording) ==
            std::vector<std::string>{std::string{kIdleWire}, std::string{kWorkingWire}});

    session_fixture.scripted.control->release();
    REQUIRE(pump_until(probe.io, [&] { return reported_wire_contains(probe.recording, kDoneWire); }));
    CHECK(reported_wire(probe.recording) ==
            std::vector<std::string>{std::string{kIdleWire}, std::string{kWorkingWire}, std::string{kDoneWire}});
    // The resting status stays done while the prompt waits for the next turn.
    REQUIRE(pump_until(probe.io, [&] { return probe.screen().find("status answer") != std::string::npos; }));
    CHECK(reported_wire(probe.recording).back() == kDoneWire);

    probe.exit();
    // The TUI reports nothing after the settled outcome; removing the report
    // is the terminal contract's job on stop.
    CHECK(reported_wire(probe.recording) ==
            std::vector<std::string>{std::string{kIdleWire}, std::string{kWorkingWire}, std::string{kDoneWire}});
}

TEST_CASE("Native TUI reports a failed run's error first line instead of done",
        "[coding_agent][tui][program-status][issue1123][spec]") {
    SessionProbe session_fixture;
    session_fixture.boot();
    auto failure = ai::assistant_text_message("");
    failure.stop_reason = ai::AssistantStopReason::Error;
    failure.error_message = "Invalid API key\n{details}";
    session_fixture.scripted.control->responses.push_back(std::move(failure));
    // Unauthorized is not retryable, so the first terminal settles the run.
    session_fixture.scripted.control->failure_kinds.push_back(ai::InferenceFailureKind::Unauthorized);

    InteractiveProbe probe;
    probe.start(*session_fixture.session, session_fixture.config.path());
    REQUIRE(pump_until(probe.io,
            [&] { return reported_wire(probe.recording) == std::vector<std::string>{std::string{kIdleWire}}; }));

    probe.type("error prompt\r");
    REQUIRE(pump_until(probe.io, [&] { return reported_wire_contains(probe.recording, kErrorWire); }));
    const auto statuses = reported_wire(probe.recording);
    REQUIRE(statuses.size() == 3);
    CHECK(statuses[0] == kIdleWire);
    CHECK(statuses[1] == kWorkingWire);
    // Only the first line is reported, and the failure never becomes done.
    CHECK(statuses[2] == kErrorWire);
    CHECK(std::count(statuses.begin(), statuses.end(), kDoneWire) == 0);

    probe.exit();
}

TEST_CASE("Native TUI returns to idle when a run is interrupted, then reports the next run",
        "[coding_agent][tui][program-status][issue1123][spec]") {
    SessionProbe session_fixture;
    session_fixture.boot();
    session_fixture.scripted.control->gate_at = 0;
    session_fixture.scripted.control->gated_response = "after cancel";

    InteractiveProbe probe;
    probe.start(*session_fixture.session, session_fixture.config.path());
    REQUIRE(pump_until(probe.io,
            [&] { return reported_wire(probe.recording) == std::vector<std::string>{std::string{kIdleWire}}; }));

    probe.type("interrupt me\r");
    REQUIRE(pump_until(probe.io, [&] { return reported_wire_contains(probe.recording, kWorkingWire); }));
    // Interrupt the held run: the prompt settles aborted, so the status
    // returns to idle instead of reporting the interrupted turn as done.
    probe.type("\x1b");
    REQUIRE(pump_until(probe.io, [&] {
        return reported_wire(probe.recording) ==
               std::vector<std::string>{std::string{kIdleWire}, std::string{kWorkingWire}, std::string{kIdleWire}};
    }));

    // The next turn runs normally: a new working report follows the idle one,
    // so the interrupted outcome cannot be mistaken for this turn's result.
    probe.type("after cancel\r");
    REQUIRE(pump_until(probe.io, [&] {
        return reported_wire(probe.recording) == std::vector<std::string>{std::string{kIdleWire},
                                                         std::string{kWorkingWire},
                                                         std::string{kIdleWire},
                                                         std::string{kWorkingWire},
                                                         std::string{kDoneWire}};
    }));

    probe.exit();
}

TEST_CASE("Native TUI reports the persisted session name while working and done",
        "[coding_agent][tui][program-status][issue1123][spec]") {
    SessionProbe session_fixture;
    session_fixture.persisted_session = session_fixture.workspace.path() / "named-session.jsonl";
    session_fixture.boot();
    REQUIRE(session_fixture.session->session_name() == std::nullopt);
    session_fixture.scripted.control->gate_at = 0;
    session_fixture.scripted.control->responses.push_back(ai::assistant_text_message("named answer"));

    InteractiveProbe probe;
    probe.start(*session_fixture.session, session_fixture.config.path());
    REQUIRE(pump_until(probe.io,
            [&] { return reported_wire(probe.recording) == std::vector<std::string>{std::string{kIdleWire}}; }));

    // `/name` persists the name before the run starts; the working report
    // carries it as the frozen named-session message did.
    probe.type("/name review-session\r");
    drain_ready(probe.io);
    REQUIRE(session_fixture.session->session_name() == std::optional<std::string>{"review-session"});
    probe.type("named prompt\r");
    REQUIRE(pump_until(probe.io, [&] { return reported_wire_contains(probe.recording, kNamedWorkingWire); }));
    CHECK(reported_wire(probe.recording) ==
            std::vector<std::string>{std::string{kIdleWire}, std::string{kNamedWorkingWire}});

    session_fixture.scripted.control->release();
    REQUIRE(pump_until(probe.io, [&] { return reported_wire_contains(probe.recording, kNamedDoneWire); }));

    // A rename after the run refreshes the resting done report.
    probe.type("/name renamed-session\r");
    REQUIRE(pump_until(probe.io, [&] { return reported_wire(probe.recording).back() == kRenamedAfterDoneWire; }));

    // A rename while the next run is working refreshes the working report.
    session_fixture.scripted.control->gate_at = 1;
    session_fixture.scripted.control->responses.push_back(ai::assistant_text_message("renamed answer"));
    probe.type("renamed prompt\r");
    REQUIRE(pump_until(probe.io, [&] { return reported_wire_contains(probe.recording, kNamedDoneWire); }));
    probe.type("/name renamed-while-working\r");
    REQUIRE(pump_until(probe.io, [&] { return reported_wire_contains(probe.recording, kRenamedWhileWorkingWire); }));
    session_fixture.scripted.control->release();
    REQUIRE(pump_until(probe.io, [&] {
        return reported_wire(probe.recording).back() ==
               "\x1b]7501;state=done:app=pike:msg=cmVuYW1lZC13aGlsZS13b3JraW5n\x1b\\";
    }));

    probe.exit();
}

TEST_CASE("Native TUI reports the boot project-trust question as the blocked status",
        "[coding_agent][tui][program-status][issue1123][spec]") {
    tests::TempWorkspace workspace;
    const tests::EnvVarGuard home_guard{"HOME", workspace.path().string()};
    const auto agent_dir = tests::agent_root_under_home(workspace.path());
    std::filesystem::create_directories(agent_dir);
    // A trust-requiring project resource makes the boot ask the trust question.
    auto resource = workspace.path() / ".pi" / "skills" / "README.md";
    std::filesystem::create_directories(resource.parent_path());
    std::ofstream(resource, std::ios::binary | std::ios::trunc) << "project skill marker";

    tests::RuntimeFixture runtime;
    tests::RuntimeLoopDriver runtime_driver(runtime);
    const auto models = tests::make_scripted_fake_models();
    coding_agent::tui::testing::ActionSinkRecorder recorder;
    boost::asio::io_context io;
    std::optional<support::ExpectedVoid> run_result;
    recorder.replace_session_async =
            [models, runtime_target = runtime.make_target()](std::size_t /* action_generation */,
                    coding_agent::runtime::AgentSessionCreationRequest request,
                    std::stop_token stop_token) -> support::AsyncResult<coding_agent::CreateAgentSessionResult> {
        request.provide_user_shell = true;
        request.execution_runtime_target = runtime_target;
        return coding_agent::create_agent_session_async(
                std::move(request), std::nullopt, tests::cli_fake_overrides(models), stop_token);
    };

    coding_agent::runtime::AgentSessionCreationRequest boot_request;
    boot_request.workspace = workspace.path();
    boot_request.session_target = coding_agent::InMemorySessionTarget{};
    boot_request.session_facts.no_prompt_templates = true;

    tui::VirtualTerminal terminal{{.columns = 120, .rows = 30}};
    tests::ProtocolRecordingTerminal recording{terminal};
    auto runtime_io = std::shared_ptr<boost::asio::io_context>(&io, [](boost::asio::io_context*) {});
    auto runtime_root = std::make_shared<harness::RuntimeRoot>(std::move(runtime_io), harness::RuntimeLimits{});
    auto run = std::move(coding_agent::tui::InteractiveSessionRunBuilder{})
                       .with_agent_config_directory(agent_dir)
                       .with_action_sink(recorder.make_sink())
                       .with_async_session_replacement_sink(recorder.make_async_session_replacement_sink())
                       .with_defer_boot(std::move(boot_request))
                       .with_runtime_root(std::move(runtime_root))
                       .build();
    boost::asio::co_spawn(io,
            coding_agent::tui::run_interactive_mode(recording, std::move(run)),
            [&](std::exception_ptr exception, support::ExpectedVoid result) {
                CHECK(exception == nullptr);
                run_result.emplace(std::move(result));
            });
    drain_ready(io);

    // The startup idle report precedes the dialog block, exactly as the frozen
    // observation recorded idle then blocked.
    REQUIRE(pump_until(io, [&] {
        return reported_wire(recording) ==
               std::vector<std::string>{std::string{kIdleWire}, std::string{kBlockedTrustWire}};
    }));
    const auto blocked_status = last_status(recording);
    CHECK(blocked_status.app == "pike");
    CHECK(blocked_status.state == tui::ProgramStatus::State::Blocked);
    CHECK(blocked_status.kind == tui::ProgramStatus::Kind::Question);

    // Answering the question clears the block and the boot continues; the
    // resting status is reported again.
    REQUIRE(terminal.inject_input("\r"));
    REQUIRE(pump_until(io, [&] {
        return recorder.replacement_completions.load(std::memory_order_acquire) >= 1 &&
               reported_wire(recording) == std::vector<std::string>{std::string{kIdleWire},
                                                   std::string{kBlockedTrustWire},
                                                   std::string{kIdleWire}};
    }));

    REQUIRE(terminal.inject_input("\x04"));
    REQUIRE(pump_until(io, [&] { return run_result.has_value(); }));
    CHECK(*run_result);
}
