// P13 end-to-end: the in-session session selector (threaded list, scopes,
// rename/delete, path toggle, search), the resume flow with the pi statuses
// and the missing-cwd Continue/Cancel prompt, and the fork flow with the
// user-message selector, `selectedText` editor pre-fill, and the verbatim pi
// strings — driven through the VirtualTerminal against a session over a temp
// Agent Config Directory with dummy-only models.json values. No live
// credentials, no network validation.

#include "coding_agent/AgentSession.hpp"
#include "coding_agent/runtime/SessionFactory.hpp"
#include "coding_agent/tui/InteractiveMode.hpp"
#include "coding_agent/tui/InteractiveSessionRun.hpp"
#include "coding_agent/tui/TestTuiActionSink.hpp"
#include "support/EnvVarGuard.hpp"
#include "support/PumpUntil.hpp"
#include "support/TempWorkspace.hpp"

#include "agent/harness/RuntimeRoot.hpp"
#include <cch/agent/harness/session/SessionStore.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include <cch/support/Error.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>

using namespace cch;
using tests::drain_ready;

namespace {

constexpr std::string_view kKeyedModels = R"({
  "providers": {
    "alpha": {
      "baseUrl": "https://alpha.example/v1",
      "api": "openai-responses",
      "apiKey": "dummy-alpha-key",
      "models": [{"id": "alpha-1", "name": "Alpha Reasoning", "reasoning": true}]
    }
  }
})";

/// One isolated assembly fixture: a temp workspace for the session files and
/// a temp Agent Config Directory (`PI_CODING_AGENT_DIR`) whose models.json
/// and keybindings.json drive runtime creation and the unbound
/// `app.session.*` triggers deterministically.
struct Fixture {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace agent_dir;
    tests::EnvVarGuard dir_guard{"PI_CODING_AGENT_DIR"};
    tests::EnvVarGuard home_guard{"HOME"};
    tests::EnvVarGuard kimi_guard{"KIMI_API_KEY"};
    std::filesystem::path session_file;

    Fixture() {
        dir_guard.set(agent_dir.path().string());
        home_guard.set(workspace.path().string());
        kimi_guard.unset();
        session_file = workspace.path() / "session.jsonl";
        std::ofstream models(agent_dir.path() / "models.json", std::ios::binary);
        models << kKeyedModels;
        // The `app.session.*` main-editor actions are recognized-but-unbound
        // (pi defaultKeys []); a user-assigned keybinding triggers the flow.
        std::ofstream keybindings(agent_dir.path() / "keybindings.json", std::ios::binary);
        keybindings << R"({"app.session.resume":"f6","app.session.fork":"f7","app.session.new":"f8"})";
    }

    /// Write a resumed session file (header + messages with the full wire
    /// fields) at `path`; `cwd` overrides the header workspace.
    void write_session(
        const std::filesystem::path& path,
        std::vector<std::string> user_messages,
        std::optional<std::string> header_cwd = std::nullopt,
        std::string session_id = "other-session") {
        auto created = harness::session::SessionStore::create_new(
            path,
            {
                .session_id = session_id,
                .created_at = "2026-07-05T00:00:00Z",
                .workspace = header_cwd.value_or(workspace.path().string()),
                .provider = "alpha",
                .model = "alpha-1",
            });
        REQUIRE(created.has_value());
        std::size_t index = 0;
        for (const auto& text : user_messages) {
            auto user = ai::user_text_message(text);
            user.timestamp = 1'750'000'000'000 + static_cast<ai::TimestampMs>(index * 2);
            REQUIRE(created->append(ai::MessageVariant{user}).has_value());
            if (index + 1 < user_messages.size()) {
                auto assistant = ai::assistant_text_message(
                    "assistant reply " + std::to_string(index));
                assistant.api = "openai-responses";
                assistant.provider = "alpha";
                assistant.model = "alpha-1";
                assistant.timestamp =
                    1'750'000'000'001 + static_cast<ai::TimestampMs>(index * 2);
                REQUIRE(created->append(ai::MessageVariant{assistant}).has_value());
            }
            ++index;
        }
    }
};

struct Running {
    // Terminal first: it must outlive the io_context, whose shutdown destroys
    // the interactive-mode coroutine frame (and its Tui) last.
    tui::VirtualTerminal terminal{tui::VirtualTerminalOptions{.columns = 100, .rows = 40}};
    boost::asio::io_context io;
    std::optional<support::ExpectedVoid> run_result;
};

[[nodiscard]] std::string visible_screen(const tui::VirtualTerminal& terminal) {
    std::string text;
    for (const auto& line : terminal.screen()) {
        text.append(line);
        text.push_back('\n');
    }
    return text;
}

/// Boot the interactive mode against the fixture session with the real
/// session-factory replacement surface (pi `createRuntime`) routed through
/// the asynchronous Session replacement test adapter (issue #580): the
/// resume, new-session, and fork flows reach AsyncSessionReplacementSink
/// exclusively, and tests wait on the seam completion counter before
/// asserting the post-replacement screen.
[[nodiscard]] std::unique_ptr<coding_agent::AgentSession> boot(
    Fixture& fixture,
    Running& running,
    const std::shared_ptr<coding_agent::tui::testing::ActionSinkRecorder>& actions) {
    coding_agent::runtime::AgentSessionCreationRequest request;
    request.session_facts.no_skills = true;
    request.session_facts.no_prompt_templates = true;
    request.workspace = fixture.workspace.path();
    request.session_target =
        coding_agent::ExplicitOpenOrCreateSessionTarget{fixture.session_file};
    auto created = coding_agent::create_agent_session(std::move(request));
    REQUIRE(created.has_value());

    // The replacement Runtime Root shares the interactive loop: assembly
    // work and the install continuation are both pumped by the test.
    auto runtime_io = std::shared_ptr<boost::asio::io_context>(
        &running.io, [](boost::asio::io_context*) {});
    auto runtime_root = std::make_shared<harness::RuntimeRoot>(
        std::move(runtime_io), harness::RuntimeLimits{});
    actions->replace_session_async =
        [runtime_root](std::size_t /* action_generation */,
            coding_agent::runtime::AgentSessionCreationRequest request,
            std::stop_token stop_token)
        -> support::AsyncResult<coding_agent::CreateAgentSessionResult> {
            request.session_facts.no_skills = true;
            request.session_facts.no_prompt_templates = true;
            request.execution_runtime_target = runtime_root->make_target();
            return coding_agent::create_agent_session_async(
                std::move(request), std::nullopt,
                coding_agent::runtime::AssemblyOverrides{
                    .model_runtime = nullptr, .models = nullptr, .user_shell = nullptr},
                stop_token);
        };

    auto run = coding_agent::tui::InteractiveSessionRunBuilder{}
        .with_session(*created->session)
        .with_agent_config_directory(fixture.agent_dir.path())
        .with_action_sink(actions->make_sink())
        .with_async_session_replacement_sink(actions->make_async_session_replacement_sink())
        .with_runtime_root(std::move(runtime_root))
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
    return std::move(created->session);
}

/// Pump until the asynchronous replacement completed at the seam, then
/// drain: the engine's installation continuation is queued on the same loop
/// by the time the count is observed (issue #579).
void wait_replacement(
    Running& running,
    const std::shared_ptr<coding_agent::tui::testing::ActionSinkRecorder>& actions,
    std::size_t completions) {
    REQUIRE(tests::pump_until(running.io, [&] {
        return actions->replacement_completions.load(std::memory_order_acquire) >= completions;
    }));
    drain_ready(running.io);
}

} // namespace

TEST_CASE(
    "session selector lists sessions and resumes the chosen one with the pi status",
    "[coding_agent][tui][session-selector][e2e][issue409]") {
    Fixture fixture;
    // The resume target: written before boot so the boot session is newest.
    fixture.write_session(
        fixture.workspace.path() / "other.jsonl", {"hello from other"});
    Running running;
    auto actions = std::make_shared<coding_agent::tui::testing::ActionSinkRecorder>();
    auto session = boot(fixture, running, actions);

    // alt+r opens the session selector (user-assigned keybinding on the
    // recognized-but-unbound app.session.resume).
    REQUIRE(running.terminal.inject_input("\x1b[17~"));
    drain_ready(running.io);
    auto screen = visible_screen(running.terminal);
    CHECK(screen.find("Resume Session (Current Folder)") != std::string::npos);
    CHECK(screen.find("◉ Current Folder") != std::string::npos);
    CHECK(screen.find("hello from other") != std::string::npos);
    CHECK(screen.find("re:<pattern> regex") != std::string::npos);

    // The current boot session (no messages) is newest, so the target sits
    // one row below: Down + Enter resumes it through the asynchronous
    // replacement adapter.
    REQUIRE(running.terminal.inject_input("\x1b[B"));
    drain_ready(running.io);
    REQUIRE(running.terminal.inject_input("\r"));
    wait_replacement(running, actions, 1);
    REQUIRE(actions->replace_sessions.size() == 1);
    CHECK(actions->replace_sessions[0].target == "explicit-resume");
    screen = visible_screen(running.terminal);
    CHECK(screen.find("Resumed session") != std::string::npos);
    CHECK(screen.find("hello from other") != std::string::npos);

    REQUIRE(running.terminal.inject_input("\x04"));
    drain_ready(running.io);
    REQUIRE(running.run_result);
    CHECK(*running.run_result);
}

TEST_CASE(
    "session selector cancels on Escape and the current session is marked",
    "[coding_agent][tui][session-selector][e2e][issue409]") {
    Fixture fixture;
    fixture.write_session(
        fixture.workspace.path() / "other.jsonl", {"hello from other"});
    Running running;
    auto actions = std::make_shared<coding_agent::tui::testing::ActionSinkRecorder>();
    auto session = boot(fixture, running, actions);

    REQUIRE(running.terminal.inject_input("\x1b[17~"));
    drain_ready(running.io);
    auto screen = visible_screen(running.terminal);
    // The boot session row shows its (no messages) placeholder.
    CHECK(screen.find("(no messages)") != std::string::npos);

    REQUIRE(running.terminal.inject_input("\x1b"));
    drain_ready(running.io);
    REQUIRE(running.terminal.inject_input(""));
    drain_ready(running.io);
    screen = visible_screen(running.terminal);
    CHECK(screen.find("Resume Session (Current Folder)") == std::string::npos);

    REQUIRE(running.terminal.inject_input("\x04"));
    drain_ready(running.io);
    REQUIRE(running.run_result);
    CHECK(*running.run_result);
}

TEST_CASE(
    "session selector search, named filter, and scope toggle work through the input seam",
    "[coding_agent][tui][session-selector][e2e][issue409]") {
    Fixture fixture;
    fixture.write_session(
        fixture.workspace.path() / "other.jsonl",
        {"fix the parser bug"},
        std::nullopt,
        "other-session");
    fixture.write_session(
        fixture.workspace.path() / "named.jsonl",
        {"refactor the loader"},
        std::nullopt,
        "named-session");
    Running running;
    auto actions = std::make_shared<coding_agent::tui::testing::ActionSinkRecorder>();
    auto session = boot(fixture, running, actions);

    REQUIRE(running.terminal.inject_input("\x1b[17~"));
    drain_ready(running.io);
    auto screen = visible_screen(running.terminal);
    CHECK(screen.find("fix the parser bug") != std::string::npos);
    CHECK(screen.find("refactor the loader") != std::string::npos);

    // Type a search: only the matching row remains.
    REQUIRE(running.terminal.inject_input("parser"));
    drain_ready(running.io);
    screen = visible_screen(running.terminal);
    CHECK(screen.find("fix the parser bug") != std::string::npos);
    CHECK(screen.find("refactor the loader") == std::string::npos);

    // Ctrl+n toggles the named filter: no session has a name, so the list
    // empties with pi's empty message.
    REQUIRE(running.terminal.inject_input("\x0e"));
    drain_ready(running.io);
    screen = visible_screen(running.terminal);
    CHECK(screen.find("No named sessions in current folder") != std::string::npos);

    // Ctrl+n back to all, then Escape closes the selector (with the flush
    // the decoder needs to finish a lone ESC).
    REQUIRE(running.terminal.inject_input("\x0e"));
    drain_ready(running.io);
    REQUIRE(running.terminal.inject_input("\x1b"));
    drain_ready(running.io);
    REQUIRE(running.terminal.inject_input(""));
    drain_ready(running.io);

    REQUIRE(running.terminal.inject_input("\x04"));
    drain_ready(running.io);
    REQUIRE(running.run_result);
    CHECK(*running.run_result);
}

TEST_CASE(
    "session selector renames a session through the inline input",
    "[coding_agent][tui][session-selector][e2e][issue409]") {
    Fixture fixture;
    const auto other = fixture.workspace.path() / "other.jsonl";
    fixture.write_session(other, {"hello from other"});
    Running running;
    auto actions = std::make_shared<coding_agent::tui::testing::ActionSinkRecorder>();
    auto session = boot(fixture, running, actions);

    REQUIRE(running.terminal.inject_input("\x1b[17~"));
    drain_ready(running.io);

    // Down to the other session, Ctrl+r enters rename mode.
    REQUIRE(running.terminal.inject_input("\x1b[B"));
    drain_ready(running.io);
    REQUIRE(running.terminal.inject_input("\x12"));
    drain_ready(running.io);
    auto screen = visible_screen(running.terminal);
    CHECK(screen.find("Rename Session") != std::string::npos);

    REQUIRE(running.terminal.inject_input("My Session"));
    drain_ready(running.io);
    REQUIRE(running.terminal.inject_input("\r"));
    drain_ready(running.io);

    // The session_info entry landed in the file.
    auto loaded = harness::session::SessionStore::load(other);
    REQUIRE(loaded.has_value());
    harness::session::SessionTree tree(std::move(*loaded));
    const auto name = tree.get_session_name();
    REQUIRE(name.has_value());
    CHECK(*name == "My Session");

    // The list refreshed with the name (warning color renders the text).
    screen = visible_screen(running.terminal);
    CHECK(screen.find("My Session") != std::string::npos);

    REQUIRE(running.terminal.inject_input("\x1b"));
    drain_ready(running.io);
    REQUIRE(running.terminal.inject_input(""));
    drain_ready(running.io);
    REQUIRE(running.terminal.inject_input("\x04"));
    drain_ready(running.io);
    REQUIRE(running.run_result);
    CHECK(*running.run_result);
}

TEST_CASE(
    "session selector deletes a session after confirmation",
    "[coding_agent][tui][session-selector][e2e][issue409]") {
    Fixture fixture;
    const auto other = fixture.workspace.path() / "other.jsonl";
    fixture.write_session(other, {"hello from other"});
    Running running;
    auto actions = std::make_shared<coding_agent::tui::testing::ActionSinkRecorder>();
    auto session = boot(fixture, running, actions);

    REQUIRE(running.terminal.inject_input("\x1b[17~"));
    drain_ready(running.io);
    REQUIRE(running.terminal.inject_input("\x1b[B"));
    drain_ready(running.io);
    REQUIRE(running.terminal.inject_input("\x04"));
    drain_ready(running.io);
    auto screen = visible_screen(running.terminal);
    CHECK(screen.find("Delete session?") != std::string::npos);

    REQUIRE(running.terminal.inject_input("\r"));
    drain_ready(running.io);
    screen = visible_screen(running.terminal);
    CHECK(screen.find("Session deleted") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(other));

    REQUIRE(running.terminal.inject_input("\x1b"));
    drain_ready(running.io);
    REQUIRE(running.terminal.inject_input(""));
    drain_ready(running.io);
    REQUIRE(running.terminal.inject_input("\x04"));
    drain_ready(running.io);
    REQUIRE(running.run_result);
    CHECK(*running.run_result);
}

TEST_CASE(
    "fork flow pre-fills selectedText, switches sessions, and reports the pi status",
    "[coding_agent][tui][session-selector][e2e][issue409]") {
    Fixture fixture;
    // The boot session itself carries three user turns.
    fixture.write_session(
        fixture.session_file,
        {"user-0", "user-1", "user-2"},
        std::nullopt,
        "boot-session");
    Running running;
    auto actions = std::make_shared<coding_agent::tui::testing::ActionSinkRecorder>();
    auto session = boot(fixture, running, actions);
    REQUIRE(session->message_count() == 5);

    // alt+f opens the user-message selector with the last message
    // preselected (pi showUserMessageSelector).
    REQUIRE(running.terminal.inject_input("\x1b[18~"));
    drain_ready(running.io);
    auto screen = visible_screen(running.terminal);
    CHECK(screen.find("Fork from Message") != std::string::npos);
    CHECK(screen.find("Message 3 of 3") != std::string::npos);

    // Enter forks before the last user message through the asynchronous
    // replacement adapter: a branched session with the
    // first two turns, the editor pre-filled with "user-2", and the pi
    // status.
    REQUIRE(running.terminal.inject_input("\r"));
    wait_replacement(running, actions, 1);
    REQUIRE(actions->replace_sessions.size() == 1);
    CHECK(actions->replace_sessions[0].target == "explicit-open-or-create");
    screen = visible_screen(running.terminal);
    CHECK(screen.find("Forked to new session") != std::string::npos);
    CHECK(screen.find("user-2") != std::string::npos);

    // A second session file appeared in the session directory.
    std::size_t jsonl_files = 0;
    for (const auto& entry :
         std::filesystem::directory_iterator(fixture.workspace.path())) {
        if (entry.path().extension() == ".jsonl") ++jsonl_files;
    }
    CHECK(jsonl_files == 2);

    // The branched file carries the prefix history and the parent pointer.
    std::filesystem::path branched;
    for (const auto& entry :
         std::filesystem::directory_iterator(fixture.workspace.path())) {
        if (entry.path() != fixture.session_file) branched = entry.path();
    }
    REQUIRE(!branched.empty());
    auto loaded = harness::session::SessionStore::load(branched);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->metadata.parent_session.has_value());
    CHECK(*loaded->metadata.parent_session == fixture.session_file);
    auto resumed = harness::session::resume_session(branched);
    REQUIRE(resumed.has_value());
    REQUIRE(resumed->history.size() == 4);

    // The editor pre-fills "user-2", so Ctrl+C clears it (app.clear) and
    // Ctrl+D exits (app.exit requires an empty editor).
    REQUIRE(running.terminal.inject_input("\x03"));
    drain_ready(running.io);
    REQUIRE(running.terminal.inject_input("\x04"));
    drain_ready(running.io);
    REQUIRE(running.run_result);
    CHECK(*running.run_result);
}

TEST_CASE(
    "fork flow reports no messages to fork from on an empty session",
    "[coding_agent][tui][session-selector][e2e][issue409]") {
    Fixture fixture;
    Running running;
    auto actions = std::make_shared<coding_agent::tui::testing::ActionSinkRecorder>();
    auto session = boot(fixture, running, actions);

    REQUIRE(running.terminal.inject_input("\x1b[18~"));
    drain_ready(running.io);
    auto screen = visible_screen(running.terminal);
    CHECK(screen.find("No messages to fork from") != std::string::npos);
    // No selector opened.
    CHECK(screen.find("Fork from Message") == std::string::npos);

    REQUIRE(running.terminal.inject_input("\x04"));
    drain_ready(running.io);
    REQUIRE(running.run_result);
    CHECK(*running.run_result);
}

TEST_CASE(
    "resuming a session whose stored cwd is gone prompts and resumes in the current cwd",
    "[coding_agent][tui][session-selector][e2e][issue409]") {
    Fixture fixture;
    const auto missing_cwd = fixture.workspace.path() / "vanished";
    fixture.write_session(
        fixture.workspace.path() / "other.jsonl",
        {"hello from the vanished project"},
        missing_cwd.string(),
        "vanished-session");
    Running running;
    auto actions = std::make_shared<coding_agent::tui::testing::ActionSinkRecorder>();
    auto session = boot(fixture, running, actions);

    REQUIRE(running.terminal.inject_input("\x1b[17~"));
    drain_ready(running.io);
    // The vanished-cwd session's header cwd filters it out of the current
    // scope; Tab switches to the All scope (pi listAll).
    REQUIRE(running.terminal.inject_input("\t"));
    drain_ready(running.io);
    REQUIRE(running.terminal.inject_input("\x1b[B"));
    drain_ready(running.io);
    REQUIRE(running.terminal.inject_input("\r"));
    drain_ready(running.io);
    // The Continue/Cancel prompt with pi's verbatim text.
    auto screen = visible_screen(running.terminal);
    CHECK(screen.find("Session cwd not found") != std::string::npos);
    CHECK(screen.find("cwd from session file does not exist") != std::string::npos);
    CHECK(screen.find(missing_cwd.string()) != std::string::npos);
    CHECK(screen.find("continue in current cwd") != std::string::npos);

    // "Yes" continues in the current cwd: the resume completes through
    // the asynchronous replacement adapter.
    REQUIRE(running.terminal.inject_input("\r"));
    wait_replacement(running, actions, 1);
    REQUIRE(actions->replace_sessions.size() == 1);
    CHECK(actions->replace_sessions[0].target == "explicit-resume");
    screen = visible_screen(running.terminal);
    CHECK(screen.find("Resumed session in current cwd") != std::string::npos);
    CHECK(screen.find("hello from the vanished project") != std::string::npos);

    REQUIRE(running.terminal.inject_input("\x04"));
    drain_ready(running.io);
    REQUIRE(running.run_result);
    CHECK(*running.run_result);
}

TEST_CASE(
    "declining the missing-cwd prompt reports Resume cancelled",
    "[coding_agent][tui][session-selector][e2e][issue409]") {
    Fixture fixture;
    const auto missing_cwd = fixture.workspace.path() / "vanished";
    fixture.write_session(
        fixture.workspace.path() / "other.jsonl",
        {"hello from the vanished project"},
        missing_cwd.string(),
        "vanished-session");
    Running running;
    auto actions = std::make_shared<coding_agent::tui::testing::ActionSinkRecorder>();
    auto session = boot(fixture, running, actions);

    REQUIRE(running.terminal.inject_input("\x1b[17~"));
    drain_ready(running.io);
    REQUIRE(running.terminal.inject_input("\t"));
    drain_ready(running.io);
    REQUIRE(running.terminal.inject_input("\x1b[B"));
    drain_ready(running.io);
    REQUIRE(running.terminal.inject_input("\r"));
    drain_ready(running.io);
    auto screen = visible_screen(running.terminal);
    CHECK(screen.find("Session cwd not found") != std::string::npos);

    // Down to "No" + Enter cancels the resume before any replacement is
    // requested.
    REQUIRE(running.terminal.inject_input("\x1b[B"));
    drain_ready(running.io);
    REQUIRE(running.terminal.inject_input("\r"));
    drain_ready(running.io);
    screen = visible_screen(running.terminal);
    CHECK(screen.find("Resume cancelled") != std::string::npos);
    CHECK(actions->replace_sessions.empty());

    REQUIRE(running.terminal.inject_input("\x04"));
    drain_ready(running.io);
    REQUIRE(running.run_result);
    CHECK(*running.run_result);
}

TEST_CASE(
    "app.session.new starts a fresh session with the pi chat line",
    "[coding_agent][tui][session-selector][e2e][issue409]") {
    Fixture fixture;
    fixture.write_session(fixture.session_file, {"old turn"}, std::nullopt, "boot-session");
    Running running;
    auto actions = std::make_shared<coding_agent::tui::testing::ActionSinkRecorder>();
    auto session = boot(fixture, running, actions);
    REQUIRE(session->message_count() == 1);

    REQUIRE(running.terminal.inject_input("\x1b[19~"));
    wait_replacement(running, actions, 1);
    REQUIRE(actions->replace_sessions.size() == 1);
    CHECK(actions->replace_sessions[0].target == "default-persisted");
    auto screen = visible_screen(running.terminal);
    CHECK(screen.find("New session started") != std::string::npos);
    CHECK(screen.find("old turn") == std::string::npos);

    REQUIRE(running.terminal.inject_input("\x04"));
    drain_ready(running.io);
    REQUIRE(running.run_result);
    CHECK(*running.run_result);
}
