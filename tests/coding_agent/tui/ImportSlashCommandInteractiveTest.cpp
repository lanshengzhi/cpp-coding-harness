// E2E `/import` dispatch (#1044): pi v1.3.0 `handleImportCommand` —
//
//   1. the path argument is required; a bare `/import` reports pi's usage
//      error "Usage: /import <path.jsonl>";
//   2. a Yes/No confirmation precedes every file operation, so a declined
//      import changes nothing (no copy in the session directory, the
//      current session untouched);
//   3. the file is copied into the session directory with pi's `-1`, `-2`,
//      ... collision suffix and opened from there;
//   4. a missing file reports "Failed to import session: File not found: ...";
//   5. the imported session becomes the current session: its entries are
//      what the TUI shows, and the previous session's entries are gone.
//
// Driven through the VirtualTerminal against a temp Agent Config Directory
// with dummy-only model values. No live credentials, no network.

#include "coding_agent/AgentSession.hpp"
#include "coding_agent/runtime/SessionFactory.hpp"
#include "coding_agent/tui/InteractiveMode.hpp"
#include "coding_agent/tui/InteractiveSessionRun.hpp"
#include "coding_agent/tui/TestTuiActionSink.hpp"
#include "support/AsyncResultBridge.hpp"
#include "support/AgentRootFixture.hpp"
#include "support/EnvVarGuard.hpp"
#include "support/ModelsFixture.hpp"
#include "support/PumpUntil.hpp"
#include "support/RuntimeFixture.hpp"
#include "support/TempWorkspace.hpp"

#include "coding_agent/environment/RuntimeRoot.hpp"
#include <cch/coding_agent/session/SessionStore.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include <cch/support/Error.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

using namespace cch;

namespace {

/// The visible screen with every run of whitespace collapsed to one space:
/// a status line longer than the terminal width wraps, and the wrapped
/// halves must still match the text the test looks for.
[[nodiscard]] std::string visible_screen(const tui::VirtualTerminal& terminal) {
    std::string text;
    for (const auto& line : terminal.screen()) {
        text.append(line);
        text.push_back('\n');
    }
    std::string collapsed;
    collapsed.reserve(text.size());
    bool pending_space = false;
    for (const char character : text) {
        if (std::isspace(static_cast<unsigned char>(character)) != 0) {
            pending_space = !collapsed.empty();
            continue;
        }
        if (pending_space) {
            collapsed.push_back(' ');
            pending_space = false;
        }
        collapsed.push_back(character);
    }
    return collapsed;
}

/// One isolated assembly fixture: a temp workspace for the session files and
/// a temp Agent Config Directory (`HOME`) whose sessions root holds the
/// imported copies.
struct Fixture {
    tests::TempWorkspace workspace;
    std::filesystem::path agent_dir;
    tests::EnvVarGuard home_guard{"HOME"};
    std::filesystem::path session_file;
    std::filesystem::path session_dir;

    Fixture() {
        home_guard.set(workspace.path().string());
        agent_dir = tests::agent_root_under_home(workspace.path());
        std::filesystem::create_directories(agent_dir);
        // The current session lives in the Agent Config Directory's sessions
        // root; the import copies its source file beside it.
        session_dir = agent_dir / "sessions" / "workspace-key";
        std::filesystem::create_directories(session_dir);
        session_file = session_dir / "current.jsonl";
    }

    /// Write a session file (header + messages) at `path`.
    void write_session(const std::filesystem::path& path,
            std::vector<std::string> user_messages,
            std::optional<std::string> header_cwd = std::nullopt,
            std::string session_id = "imported-session") {
        std::filesystem::create_directories(path.parent_path());
        auto created = harness::session::SessionStore::create_new(path,
                {
                        .session_id = session_id,
                        .created_at = "2026-07-05T00:00:00Z",
                        .workspace = header_cwd.value_or(workspace.path().string()),
                        .provider = "fake",
                        .model = "fake-model",
                });
        REQUIRE(created.has_value());
        std::size_t index = 0;
        for (const auto& text : user_messages) {
            auto user = ai::user_text_message(text);
            user.timestamp = 1'750'000'000'000 + static_cast<ai::TimestampMs>(index * 2);
            REQUIRE(created->append(ai::MessageVariant{user}).has_value());
            if (index + 1 < user_messages.size()) {
                auto assistant = ai::assistant_text_message("imported assistant reply " + std::to_string(index));
                assistant.api = "scripted-fake";
                assistant.provider = "fake";
                assistant.model = "fake-model";
                assistant.timestamp = 1'750'000'000'001 + static_cast<ai::TimestampMs>(index * 2);
                REQUIRE(created->append(ai::MessageVariant{assistant}).has_value());
            }
            ++index;
        }
    }
};

struct Running {
    // Terminal first: it must outlive the io_context, whose shutdown destroys
    // the interactive-mode coroutine frame (and its Tui) last.
    tui::VirtualTerminal terminal{tui::VirtualTerminalOptions{.columns = 120, .rows = 40}};
    boost::asio::io_context io;
    std::optional<support::ExpectedVoid> run_result;
};

/// Boot the interactive mode against the fixture session with the real
/// session-factory replacement surface (pi `createRuntime`) routed through
/// the asynchronous Session replacement test adapter.
[[nodiscard]] std::unique_ptr<coding_agent::AgentSession> boot(Fixture& fixture,
        Running& running,
        const std::shared_ptr<coding_agent::tui::testing::ActionSinkRecorder>& actions) {
    coding_agent::runtime::AgentSessionCreationRequest request;
    request.session_facts.no_skills = true;
    request.session_facts.no_prompt_templates = true;
    request.workspace = fixture.workspace.path();
    request.session_target = coding_agent::ExplicitOpenOrCreateSessionTarget{fixture.session_file};
    request.request_model = tests::scripted_request_model("fake", "fake-model");

    auto runtime_io = std::shared_ptr<boost::asio::io_context>(&running.io, [](boost::asio::io_context*) {});
    auto runtime_root = std::make_shared<harness::RuntimeRoot>(std::move(runtime_io), harness::RuntimeLimits{});
    request.execution_runtime_target = runtime_root->make_target();

    std::optional<support::Expected<coding_agent::CreateAgentSessionResult>> booted;
    coding_agent::isolate_magpie_gateway(request);
    boost::asio::co_spawn(running.io,
            support::detail::await_async_result(coding_agent::create_agent_session_async(std::move(request))),
            [&](std::exception_ptr exception, support::Expected<coding_agent::CreateAgentSessionResult> created) {
                CHECK(exception == nullptr);
                booted.emplace(std::move(created));
            });
    REQUIRE(tests::pump_until(running.io, [&] { return booted.has_value(); }));
    REQUIRE(booted->has_value());
    auto created = std::move(**booted);

    actions->replace_session_async =
            [runtime_root](std::size_t /* action_generation */,
                    coding_agent::runtime::AgentSessionCreationRequest request,
                    std::stop_token stop_token) -> support::AsyncResult<coding_agent::CreateAgentSessionResult> {
        request.session_facts.no_skills = true;
        request.session_facts.no_prompt_templates = true;
        request.execution_runtime_target = runtime_root->make_target();
        coding_agent::isolate_magpie_gateway(request);
        return coding_agent::create_agent_session_async(std::move(request),
                std::nullopt,
                coding_agent::runtime::AssemblyOverrides{.model_runtime = nullptr, .user_shell = nullptr},
                stop_token);
    };

    auto run = coding_agent::tui::InteractiveSessionRunBuilder{}
                       .with_session(*created.session)
                       .with_agent_config_directory(fixture.agent_dir)
                       .with_action_sink(actions->make_sink())
                       .with_async_session_replacement_sink(actions->make_async_session_replacement_sink())
                       .with_runtime_root(std::move(runtime_root))
                       .build();

    boost::asio::co_spawn(running.io,
            coding_agent::tui::run_interactive_mode(running.terminal, std::move(run)),
            [&](std::exception_ptr exception, support::ExpectedVoid result) {
                CHECK(exception == nullptr);
                running.run_result.emplace(std::move(result));
            });
    tests::drain_ready(running.io);
    return std::move(created.session);
}

/// Pump until the asynchronous replacement completed at the seam, then
/// drain: the engine's installation continuation is queued on the same loop
/// by the time the count is observed.
void wait_replacement(Running& running,
        const std::shared_ptr<coding_agent::tui::testing::ActionSinkRecorder>& actions,
        std::size_t completions) {
    REQUIRE(tests::pump_until(running.io,
            [&] { return actions->replacement_completions.load(std::memory_order_acquire) >= completions; }));
    tests::drain_ready(running.io);
}

/// Pump until the screen shows `text`.
void wait_for_screen(Running& running, std::string_view text) {
    REQUIRE(tests::pump_until(
            running.io, [&] { return visible_screen(running.terminal).find(text) != std::string::npos; }));
}

/// The pi confirmation prompt's Yes/No selector: "Yes" is the first row, so
/// Enter confirms it and Down + Enter declines it.
void confirm_import(Running& running, bool accept) {
    REQUIRE(tests::pump_until(
            running.io, [&] { return visible_screen(running.terminal).find("Import session") != std::string::npos; }));
    tests::drain_ready(running.io);
    if (!accept) {
        REQUIRE(running.terminal.inject_input("\x1b[B"));
        tests::drain_ready(running.io);
    }
    REQUIRE(running.terminal.inject_input("\r"));
    tests::drain_ready(running.io);
}

void finish_interactive(Running& running) {
    REQUIRE(running.terminal.inject_input("\x04"));
    REQUIRE(tests::pump_until(
            running.io, [&running] { return running.run_result.has_value(); }, std::chrono::seconds{10}));
    REQUIRE(running.run_result);
    CHECK(*running.run_result);
}

} // namespace

TEST_CASE("/import without a path reports pi's usage error", "[coding_agent][tui][import][e2e][issue1044][spec]") {
    Fixture fixture;
    Running running;
    auto actions = std::make_shared<coding_agent::tui::testing::ActionSinkRecorder>();
    auto session = boot(fixture, running, actions);

    REQUIRE(running.terminal.inject_input("/import\r"));
    wait_for_screen(running, "Usage: /import <path.jsonl>");
    // No replacement was requested and no confirmation was shown.
    CHECK(actions->replace_sessions.empty());

    finish_interactive(running);
}

TEST_CASE("/import replaces the current session with the imported one",
        "[coding_agent][tui][import][e2e][issue1044][spec]") {
    Fixture fixture;
    // The current session carries its own entry; the imported file carries
    // another one.
    fixture.write_session(fixture.session_file, {"current session entry"});
    const auto import_source = fixture.workspace.path() / "incoming.jsonl";
    fixture.write_session(import_source, {"imported session entry"});
    Running running;
    auto actions = std::make_shared<coding_agent::tui::testing::ActionSinkRecorder>();
    auto session = boot(fixture, running, actions);

    REQUIRE(running.terminal.inject_input("/import " + import_source.string() + "\r"));
    confirm_import(running, /*accept=*/true);
    wait_replacement(running, actions, 1);
    wait_for_screen(running, "Session imported from: " + import_source.string());

    // The imported session is the current one: its entry is what the chat
    // shows, and the previous session's entry is gone.
    auto screen = visible_screen(running.terminal);
    CHECK(screen.find("imported session entry") != std::string::npos);
    CHECK(screen.find("current session entry") == std::string::npos);

    // The file was copied into the session directory.
    CHECK(std::filesystem::exists(fixture.session_dir / "incoming.jsonl"));

    finish_interactive(running);
}

TEST_CASE("a declined import confirmation changes nothing", "[coding_agent][tui][import][e2e][issue1044][spec]") {
    Fixture fixture;
    fixture.write_session(fixture.session_file, {"current session entry"});
    const auto import_source = fixture.workspace.path() / "incoming.jsonl";
    fixture.write_session(import_source, {"imported session entry"});
    Running running;
    auto actions = std::make_shared<coding_agent::tui::testing::ActionSinkRecorder>();
    auto session = boot(fixture, running, actions);

    REQUIRE(running.terminal.inject_input("/import " + import_source.string() + "\r"));
    confirm_import(running, /*accept=*/false);
    wait_for_screen(running, "Import cancelled");

    // Nothing was copied into the session directory, and no replacement was
    // requested: the current session is untouched.
    CHECK_FALSE(std::filesystem::exists(fixture.session_dir / "incoming.jsonl"));
    CHECK(actions->replace_sessions.empty());
    auto screen = visible_screen(running.terminal);
    CHECK(screen.find("imported session entry") == std::string::npos);
    CHECK(screen.find("current session entry") != std::string::npos);

    finish_interactive(running);
}

TEST_CASE("an import that collides with a stored session file lands beside it",
        "[coding_agent][tui][import][e2e][issue1044][spec]") {
    Fixture fixture;
    fixture.write_session(fixture.session_file, {"current session entry"});
    // A file with the same base name already sits in the session directory.
    fixture.write_session(fixture.session_dir / "incoming.jsonl", {"stored session entry"});
    const auto import_source = fixture.workspace.path() / "incoming.jsonl";
    fixture.write_session(import_source, {"imported session entry"});
    Running running;
    auto actions = std::make_shared<coding_agent::tui::testing::ActionSinkRecorder>();
    auto session = boot(fixture, running, actions);

    REQUIRE(running.terminal.inject_input("/import " + import_source.string() + "\r"));
    confirm_import(running, /*accept=*/true);
    wait_replacement(running, actions, 1);
    wait_for_screen(running, "Session imported from: " + import_source.string());

    // pi's collision suffix: the copy lands as `incoming-1.jsonl`, and the
    // stored file is not overwritten.
    CHECK(std::filesystem::exists(fixture.session_dir / "incoming-1.jsonl"));
    std::ifstream stored(fixture.session_dir / "incoming.jsonl", std::ios::binary);
    const std::string stored_text{std::istreambuf_iterator<char>(stored), std::istreambuf_iterator<char>()};
    CHECK(stored_text.find("stored session entry") != std::string::npos);
    CHECK(stored_text.find("imported session entry") == std::string::npos);

    // The imported session is the current one.
    auto screen = visible_screen(running.terminal);
    CHECK(screen.find("imported session entry") != std::string::npos);
    CHECK(screen.find("stored session entry") == std::string::npos);

    finish_interactive(running);
}

TEST_CASE("/import on a missing file reports pi's error and keeps the current session",
        "[coding_agent][tui][import][e2e][issue1044][spec]") {
    Fixture fixture;
    fixture.write_session(fixture.session_file, {"current session entry"});
    const auto missing = fixture.workspace.path() / "not-here.jsonl";
    Running running;
    auto actions = std::make_shared<coding_agent::tui::testing::ActionSinkRecorder>();
    auto session = boot(fixture, running, actions);

    REQUIRE(running.terminal.inject_input("/import " + missing.string() + "\r"));
    confirm_import(running, /*accept=*/true);
    wait_for_screen(running, "Failed to import session: File not found: " + missing.string());

    // No replacement was requested and the current session still shows its
    // own entry.
    CHECK(actions->replace_sessions.empty());
    auto screen = visible_screen(running.terminal);
    CHECK(screen.find("current session entry") != std::string::npos);

    finish_interactive(running);
}
