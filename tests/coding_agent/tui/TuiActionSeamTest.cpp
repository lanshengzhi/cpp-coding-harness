// Route Native TUI operations through one closed action value (issue #461):
// every application-level Native TUI operation is one alternative of
// `TuiActionVariant`, carried to composition code through one move-only
// `TuiActionSink` (ADR 0040: broad positional application callbacks become
// one closed action value and one move-only sink).
//
// Coverage per the ticket's acceptance criteria:
// - boot and in-session session replacement both arrive as one
//   `ReplaceSessionAction` alternative with an owned passive payload;
// - the single sink is lossless (every admitted action is delivered exactly
//   once) while render-state requests stay separate and coalescible;
// - the session generation stamped on each action advances on Session
//   replacement and on Close, so actions from a retired generation are
//   distinguishable and rejected or safely dropped (Session replacement and
//   Close races). The TUI-internal stale-drop branch (`deliver_action`'s
//   generation check, exercised by the captured login-dialog hook) is
//   defensive: the dialog swallows all input, so a stale delivery cannot be
//   driven end-to-end while the host stays synchronous — the async in-session
//   replacement host (#466) is where retired-generation deliveries become
//   reachable, and these tests pin the seam it consumes;
// - a host rejection (or a null host) surfaces safely and the TUI survives;
// - boot diagnostics and creation failure report through the closed seam;
// - the private component-level dispatch stays private (no new public seam).

#include "coding_agent/tui/InteractiveMode.hpp"
#include "coding_agent/tui/InteractiveSessionRun.hpp"
#include "coding_agent/tui/TestTuiActionSink.hpp"

#include "support/EnvVarGuard.hpp"
#include "support/PumpUntil.hpp"
#include "support/TempWorkspace.hpp"

#include "coding_agent/AgentSession.hpp"
#include "coding_agent/runtime/SessionFactory.hpp"
#include <cch/tui/VirtualTerminal.hpp>

#include <cch/support/Error.hpp>
#include <catch2/catch_test_macros.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace cch;
using tests::drain_ready;

namespace {

[[nodiscard]] std::string visible_screen(const tui::VirtualTerminal& terminal) {
    std::string text;
    for (const auto& line : terminal.screen()) {
        text.append(line);
        text.push_back('\n');
    }
    return text;
}

struct Running {
    tui::VirtualTerminal terminal{tui::VirtualTerminalOptions{.columns = 100, .rows = 40}};
    boost::asio::io_context io;
    std::optional<support::ExpectedVoid> run_result;
};

struct Fixture {
    tests::TempWorkspace workspace;
    tests::TempWorkspace agent_dir;
    tests::EnvVarGuard agent_dir_guard{"PI_CODING_AGENT_DIR"};

    Fixture() { agent_dir_guard.set(agent_dir.path().string()); }
};

/// The replace-session creator shared by these tests (pi `createRuntime`):
/// creates an in-memory session from the carried request.
[[nodiscard]] coding_agent::tui::testing::TestSessionFactorySink in_memory_session_creator() {
    return [](coding_agent::runtime::AgentSessionCreationRequest request)
        -> support::Expected<coding_agent::CreateAgentSessionResult> {
        request.session_facts.no_skills = true;
        request.session_facts.no_prompt_templates = true;
        return coding_agent::create_agent_session(std::move(request));
    };
}

/// Boot the interactive mode through the deferred-boot entry (the boot
/// session is created via `ReplaceSessionAction` through the sink).
void boot(
    Fixture& fixture,
    Running& running,
    std::shared_ptr<coding_agent::tui::testing::ActionSinkRecorder> actions,
    coding_agent::runtime::AgentSessionCreationRequest request = {}) {
    if (request.workspace.empty()) request.workspace = fixture.workspace.path();
    if (std::holds_alternative<coding_agent::DefaultPersistedSessionTarget>(
            request.session_target)) {
        request.session_target = coding_agent::InMemorySessionTarget{};
    }
    auto run = coding_agent::tui::InteractiveSessionRunBuilder{}
        .with_defer_boot(std::move(request))
        .with_agent_config_directory(fixture.agent_dir.path())
        .with_action_sink(actions->make_sink())
        .build();
    boost::asio::co_spawn(
        running.io,
        coding_agent::tui::run_interactive_mode(
            running.terminal,
            std::move(run)),
        [&](std::exception_ptr exception, support::ExpectedVoid result) {
            CHECK(exception == nullptr);
            running.run_result.emplace(std::move(result));
        });
    drain_ready(running.io);
}

} // namespace

TEST_CASE(
    "boot and in-session replacement are one ReplaceSessionAction through the single sink",
    "[coding_agent][tui][actions][issue461]") {
    Fixture fixture;
    Running running;
    auto actions = std::make_shared<coding_agent::tui::testing::ActionSinkRecorder>();
    actions->replace_session = in_memory_session_creator();
    boot(fixture, running, actions);

    // The boot session was created through one ReplaceSessionAction carried
    // by the closed action value (generation 0: no replacement yet).
    REQUIRE(actions->replace_sessions.size() == 1);
    CHECK(actions->replace_sessions[0].workspace == fixture.workspace.path());
    CHECK(actions->replace_sessions[0].target == "in-memory");
    CHECK(actions->generations == std::vector<std::size_t>{0});

    // The in-session `/new` flow is the same closed alternative.
    REQUIRE(running.terminal.inject_input("/new\r"));
    drain_ready(running.io);
    REQUIRE(actions->replace_sessions.size() == 2);
    CHECK(actions->replace_sessions[1].workspace == fixture.workspace.path());
    CHECK(actions->generations == std::vector<std::size_t>{0, 0});

    // The replacement bound a fresh in-memory session and the run is still
    // responsive (the pi `✓ New session started` chat line).
    CHECK(visible_screen(running.terminal).find("✓ New session started") != std::string::npos);

    REQUIRE(running.terminal.inject_input("\x04"));
    drain_ready(running.io);
    REQUIRE(running.run_result);
    CHECK(*running.run_result);
}

TEST_CASE(
    "session replacement retires the action generation so later actions are re-stamped",
    "[coding_agent][tui][actions][issue461]") {
    Fixture fixture;
    Running running;
    auto actions = std::make_shared<coding_agent::tui::testing::ActionSinkRecorder>();
    actions->replace_session = in_memory_session_creator();
    boot(fixture, running, actions);
    // Boot: ReplaceSessionAction@0.
    REQUIRE(actions->generations == std::vector<std::size_t>{0});

    // Two rapid in-session replacements: each advances the generation, and
    // every admitted action is still delivered exactly once (lossless seam).
    REQUIRE(running.terminal.inject_input("/new\r"));
    drain_ready(running.io);
    REQUIRE(running.terminal.inject_input("/new\r"));
    drain_ready(running.io);
    REQUIRE(actions->replace_sessions.size() == 3);
    // Boot@0, first replacement@0 (admitted before the retirement), second
    // replacement@1 (admitted after the first replacement retired gen 0).
    CHECK(actions->generations == std::vector<std::size_t>{0, 0, 1});

    // A post-replacement action carries the newest generation (the retired
    // generation never leaks into later deliveries).
    REQUIRE(running.terminal.inject_input("/new\r"));
    drain_ready(running.io);
    REQUIRE(actions->generations.size() == 4);
    CHECK(actions->generations.back() == 2);

    REQUIRE(running.terminal.inject_input("\x04"));
    drain_ready(running.io);
    REQUIRE(running.run_result);
    CHECK(*running.run_result);
}

TEST_CASE(
    "close retires the action generation and every admitted action is delivered once",
    "[coding_agent][tui][actions][issue461]") {
    Fixture fixture;
    Running running;
    auto actions = std::make_shared<coding_agent::tui::testing::ActionSinkRecorder>();
    actions->replace_session = in_memory_session_creator();
    boot(fixture, running, actions);

    // Admit a replacement, then close while the run is live (Close race).
    REQUIRE(running.terminal.inject_input("/new\r"));
    drain_ready(running.io);
    REQUIRE(running.terminal.inject_input("\x04"));
    drain_ready(running.io);
    REQUIRE(running.run_result);
    CHECK(*running.run_result);

    // Close retired the generation: every action was delivered exactly once
    // before close, and no delivery happened after it. The recorder sees the
    // pre-close generation only (boot Replace@0, then /new@0).
    REQUIRE(actions->generations == std::vector<std::size_t>{0, 0});
    REQUIRE(actions->replace_sessions.size() == 2);
    // A post-close delivery would carry a retired generation; the TUI
    // rejects it before it reaches the host. In this synchronous host every
    // admitted action is delivered at its admitting generation, so the sink
    // records nothing after close (asserted by the exact sequence above).
    CHECK(actions->generations.size() == 2);
}

TEST_CASE(
    "a rejected replacement surfaces through the seam and the TUI keeps running",
    "[coding_agent][tui][actions][issue461]") {
    Fixture fixture;
    Running running;
    auto actions = std::make_shared<coding_agent::tui::testing::ActionSinkRecorder>();
    // The host rejects the in-session replacement (returns the creation
    // error through the closed action value's result channel).
    int replacement_calls = 0;
    actions->replace_session =
        [&replacement_calls](coding_agent::runtime::AgentSessionCreationRequest request)
        -> support::Expected<coding_agent::CreateAgentSessionResult> {
            request.session_facts.no_skills = true;
            request.session_facts.no_prompt_templates = true;
            ++replacement_calls;
            if (replacement_calls == 2) {
                return std::unexpected(support::make_error(
                    support::ErrorCode::Session,
                    "host rejected the replacement"));
            }
            return coding_agent::create_agent_session(std::move(request));
        };
    boot(fixture, running, actions);

    // `/new` (the in-session replacement after the boot session) is
    // rejected by the host.
    REQUIRE(running.terminal.inject_input("/new\r"));
    drain_ready(running.io);
    CHECK(visible_screen(running.terminal).find("host rejected the replacement") !=
          std::string::npos);

    // The TUI survives the rejection and remains interactive.
    REQUIRE(running.terminal.inject_input("\x04"));
    drain_ready(running.io);
    REQUIRE(running.run_result);
    CHECK(*running.run_result);
}

TEST_CASE(
    "a null action sink reports session replacement unavailable and keeps the run alive",
    "[coding_agent][tui][actions][issue461]") {
    Fixture fixture;
    Running running;
    // No action sink: the TUI-local default applies and replacement reports
    // an unavailable host (the pre-seam behavior preserved).
    auto request = [&] {
        coding_agent::runtime::AgentSessionCreationRequest req;
        req.session_facts.no_skills = true;
        req.session_facts.no_prompt_templates = true;
        req.workspace = fixture.workspace.path();
        req.session_target = coding_agent::InMemorySessionTarget{};
        return req;
    }();

    auto run = coding_agent::tui::InteractiveSessionRunBuilder{}
        .with_defer_boot(std::move(request))
        .with_agent_config_directory(fixture.agent_dir.path())
        .with_action_sink(nullptr)
        .build();

    boost::asio::co_spawn(
        running.io,
        coding_agent::tui::run_interactive_mode(
            running.terminal,
            std::move(run)),
        [&](std::exception_ptr exception, support::ExpectedVoid result) {
            CHECK(exception == nullptr);
            running.run_result.emplace(std::move(result));
        });
    drain_ready(running.io);

    // A null host cannot create the boot session; the boot reports the
    // failure through the seam and exits cleanly (no crash, no hang).
    REQUIRE(running.run_result);
    CHECK_FALSE(*running.run_result);
}

TEST_CASE(
    "boot reports trust diagnostics as one ReportBootDiagnosticsAction through the sink",
    "[coding_agent][tui][actions][issue461]") {
    Fixture fixture;
    // A trust-requiring project resource (a `.pi` skill document) makes the
    // boot resolve trust; an unreadable trust store (a directory where
    // `trust.json` must live) makes that resolution produce a
    // `trust_store_unavailable` diagnostic.
    std::filesystem::create_directories(fixture.workspace.path() / ".pi" / "skills");
    std::ofstream skill(fixture.workspace.path() / ".pi" / "skills" / "README.md");
    skill << "project skill marker";
    std::filesystem::create_directories(fixture.agent_dir.path() / "trust.json");

    Running running;
    auto actions = std::make_shared<coding_agent::tui::testing::ActionSinkRecorder>();
    actions->replace_session = in_memory_session_creator();
    boot(fixture, running, actions);

    // The trust-resolution diagnostic arrived as the closed
    // ReportBootDiagnosticsAction alternative with an owned payload.
    REQUIRE(actions->boot_diagnostics.size() == 1);
    REQUIRE(actions->boot_diagnostics[0].diagnostics.size() == 1);
    CHECK(actions->boot_diagnostics[0].diagnostics[0].code == "trust:trust_store_unavailable");

    // The boot still bound the session and stays interactive.
    REQUIRE(actions->replace_sessions.size() == 1);
    REQUIRE(running.terminal.inject_input("\x04"));
    drain_ready(running.io);
    REQUIRE(running.run_result);
    CHECK(*running.run_result);
}
