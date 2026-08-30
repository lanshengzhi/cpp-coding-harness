// P14 end-to-end: the tree overlay on pi's default double-Escape trigger
// (500 ms window, empty editor, `doubleEscapeAction` default "tree" — the
// settings field stays out of the subset), the tree selector rendering and
// navigation through the `navigateTree` runtime (leaf/active-path semantics,
// editor pre-fill, the `Navigated to selected point` / `Already at this
// point` / `No entries in session` statuses), the `shift+l` label editing
// with the persisted `label` entry, and the `app.message.copy` clipboard
// flow with pi's statuses — driven through the VirtualTerminal against a
// session over a temp Agent Config Directory with dummy-only models.json
// values. No live credentials, no network validation.

#include "coding_agent/AgentSession.hpp"
#include "coding_agent/tui/InteractiveMode.hpp"
#include "coding_agent/tui/InteractiveSessionRun.hpp"
#include "coding_agent/tui/TestTuiActionSink.hpp"
#include "support/EnvVarGuard.hpp"
#include "support/PumpUntil.hpp"
#include "support/TempWorkspace.hpp"

#include <cch/agent/harness/session/SessionStore.hpp>
#include <cch/agent/harness/session/SessionTree.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include <cch/support/Error.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

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
/// drives runtime creation deterministically.
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
    }

    /// Write a resumed session file (header + messages) at `path`.
    void write_session(
        const std::filesystem::path& path,
        std::vector<std::string> user_messages,
        std::string session_id = "boot-session") {
        auto created = harness::session::SessionStore::create_new(
            path,
            {
                .session_id = session_id,
                .created_at = "2026-07-05T00:00:00Z",
                .workspace = workspace.path().string(),
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
/// session-replacement surface (pi `createRuntime`). The recorder shares its
/// state with the closed action sink, so it stays valid while the run lives.
[[nodiscard]] std::unique_ptr<coding_agent::AgentSession> boot(
    Fixture& fixture,
    Running& running,
    std::shared_ptr<coding_agent::tui::testing::ActionSinkRecorder> actions =
        std::make_shared<coding_agent::tui::testing::ActionSinkRecorder>()) {
    coding_agent::runtime::AgentSessionCreationRequest request;
    request.session_facts.no_skills = true;
    request.session_facts.no_prompt_templates = true;
    request.workspace = fixture.workspace.path();
    request.session_target =
        coding_agent::ExplicitOpenOrCreateSessionTarget{fixture.session_file};
    auto created = coding_agent::create_agent_session(std::move(request));
    REQUIRE(created.has_value());

    actions->replace_session =
        [](coding_agent::runtime::AgentSessionCreationRequest request)
        -> support::Expected<coding_agent::CreateAgentSessionResult> {
            request.session_facts.no_skills = true;
            request.session_facts.no_prompt_templates = true;
            return coding_agent::create_agent_session(std::move(request));
        };

    auto run = coding_agent::tui::InteractiveSessionRunBuilder{}
        .with_session(*created->session)
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
    return std::move(created->session);
}

/// Press Escape twice (with the decoder flush the lone-ESC path needs) to
/// open the tree overlay through the 500 ms double-escape window.
void double_escape(Running& running) {
    REQUIRE(running.terminal.inject_input("\x1b"));
    drain_ready(running.io);
    REQUIRE(running.terminal.inject_input(""));
    drain_ready(running.io);
    REQUIRE(running.terminal.inject_input("\x1b"));
    drain_ready(running.io);
    REQUIRE(running.terminal.inject_input(""));
    drain_ready(running.io);
}

} // namespace

TEST_CASE(
    "double-escape with an empty editor opens the session tree overlay",
    "[coding_agent][tui][tree-selector][e2e][issue410]") {
    Fixture fixture;
    fixture.write_session(fixture.session_file, {"user-0", "user-1"});
    Running running;
    auto session = boot(fixture, running);

    double_escape(running);
    const auto screen = visible_screen(running.terminal);
    CHECK(screen.find("  Session Tree") != std::string::npos);
    CHECK(screen.find("Type to search:") != std::string::npos);
    CHECK(screen.find("user: user-0") != std::string::npos);
    CHECK(screen.find("user: user-1") != std::string::npos);

    // A single Escape cancels the overlay back to the main screen.
    REQUIRE(running.terminal.inject_input("\x1b"));
    drain_ready(running.io);
    REQUIRE(running.terminal.inject_input(""));
    drain_ready(running.io);
    CHECK(visible_screen(running.terminal).find("Session Tree") == std::string::npos);

    REQUIRE(running.terminal.inject_input("\x04"));
    drain_ready(running.io);
    REQUIRE(running.run_result);
    CHECK(*running.run_result);
}

TEST_CASE(
    "tree navigation switches the active path, pre-fills the editor, and reports the pi status",
    "[coding_agent][tui][tree-selector][e2e][issue410]") {
    Fixture fixture;
    fixture.write_session(fixture.session_file, {"user-0", "user-1", "user-2"});
    Running running;
    auto session = boot(fixture, running);
    REQUIRE(session->message_count() == 5);

    double_escape(running);
    // The leaf (the restored thinking entry) is hidden in the default view,
    // so the selection sits on the last message (user-2); move up to the
    // root user message and confirm.
    REQUIRE(running.terminal.inject_input("\x1b[A"));
    drain_ready(running.io);
    REQUIRE(running.terminal.inject_input("\x1b[A"));
    drain_ready(running.io);
    REQUIRE(running.terminal.inject_input("\x1b[A"));
    drain_ready(running.io);
    REQUIRE(running.terminal.inject_input("\x1b[A"));
    drain_ready(running.io);
    REQUIRE(running.terminal.inject_input("\r"));
    drain_ready(running.io);

    const auto screen = visible_screen(running.terminal);
    // The overlay closed, the status reported, and the editor pre-filled
    // with the navigated-to message's text.
    CHECK(screen.find("Navigated to selected point") != std::string::npos);
    CHECK(screen.find("user-0") != std::string::npos);
    // The live context was rebuilt to the root position (before the first
    // entry): no assistant replies remain in the transcript.
    CHECK(screen.find("assistant reply") == std::string::npos);
    CHECK(session->message_count() == 0);

    // The editor carries the pre-filled text: clear it (ctrl+c) before the
    // empty-editor exit (ctrl+d).
    REQUIRE(running.terminal.inject_input("\x03"));
    drain_ready(running.io);
    REQUIRE(running.terminal.inject_input("\x04"));
    drain_ready(running.io);
    REQUIRE(running.run_result);
    CHECK(*running.run_result);
}

TEST_CASE(
    "selecting the current leaf reports Already at this point",
    "[coding_agent][tui][tree-selector][e2e][issue410]") {
    Fixture fixture;
    fixture.write_session(fixture.session_file, {"user-0", "user-1"});
    Running running;
    auto session = boot(fixture, running);

    // The resumed leaf is the restored thinking entry (hidden in the
    // default view), so the visible selection is the last message. Navigate
    // to it first (making the leaf visible), then re-open and select the
    // now-current leaf.
    // The resumed leaf (the restored thinking entry) is hidden, so the
    // visible selection is the last message (user-1). Navigate to it first:
    // the user-message target moves the leaf to its parent (assistant
    // reply 0) and pre-fills the editor with its text.
    double_escape(running);
    REQUIRE(running.terminal.inject_input("\r"));
    drain_ready(running.io);
    auto screen = visible_screen(running.terminal);
    CHECK(screen.find("Navigated to selected point") != std::string::npos);

    // Clear the pre-filled editor (ctrl+c), re-open the tree: the selection
    // now sits on the visible leaf.
    REQUIRE(running.terminal.inject_input("\x03"));
    drain_ready(running.io);
    double_escape(running);
    REQUIRE(running.terminal.inject_input("\r"));
    drain_ready(running.io);
    screen = visible_screen(running.terminal);
    CHECK(screen.find("Already at this point") != std::string::npos);
    // Nothing moved since the navigation (the live context still carries the
    // root-to-leaf path through assistant reply 0).
    CHECK(session->message_count() == 2);

    REQUIRE(running.terminal.inject_input("\x04"));
    drain_ready(running.io);
    REQUIRE(running.run_result);
    CHECK(*running.run_result);
}

TEST_CASE(
    "a fresh in-memory session opens the tree on its initial thinking entry",
    "[coding_agent][tui][tree-selector][e2e][issue491]") {
    Fixture fixture;
    // An in-memory session (no file, no messages). pi's createAgentSession
    // appends the initial thinking-level change to every new session, so the
    // tree is never entry-less for a created session; the selector opens on
    // that one entry (hidden by the default filter) instead of reporting an
    // empty session.
    Running running;
    coding_agent::runtime::AgentSessionCreationRequest request;
    request.session_facts.no_skills = true;
    request.session_facts.no_prompt_templates = true;
    request.workspace = fixture.workspace.path();
    request.session_target = coding_agent::InMemorySessionTarget{};
    auto created = coding_agent::create_agent_session(std::move(request));
    REQUIRE(created.has_value());

    coding_agent::tui::testing::ActionSinkRecorder actions;
    actions.replace_session =
        [](coding_agent::runtime::AgentSessionCreationRequest request)
        -> support::Expected<coding_agent::CreateAgentSessionResult> {
            request.session_facts.no_skills = true;
            request.session_facts.no_prompt_templates = true;
            return coding_agent::create_agent_session(std::move(request));
        };
    auto run = coding_agent::tui::InteractiveSessionRunBuilder{}
        .with_session(*created->session)
        .with_agent_config_directory(fixture.agent_dir.path())
        .with_action_sink(actions.make_sink())
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

    double_escape(running);
    const auto screen = visible_screen(running.terminal);
    CHECK(screen.find("Session Tree") != std::string::npos);
    CHECK(screen.find("No entries in session") == std::string::npos);

    // Dismiss the selector (Escape + the decoder flush the lone-ESC path
    // needs), then Ctrl-D quits.
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
    "shift+l edits a label and persists the label entry",
    "[coding_agent][tui][tree-selector][e2e][issue410]") {
    Fixture fixture;
    fixture.write_session(fixture.session_file, {"user-0", "user-1"});
    Running running;
    auto session = boot(fixture, running);

    double_escape(running);
    // The selection sits on the last message (user-1); move up to the first
    // user message and press shift+l (the plain capital letter on the wire).
    REQUIRE(running.terminal.inject_input("\x1b[A"));
    drain_ready(running.io);
    REQUIRE(running.terminal.inject_input("\x1b[A"));
    drain_ready(running.io);
    REQUIRE(running.terminal.inject_input("L"));
    drain_ready(running.io);
    auto screen = visible_screen(running.terminal);
    CHECK(screen.find("Label (empty to remove):") != std::string::npos);

    REQUIRE(running.terminal.inject_input("reviewed"));
    drain_ready(running.io);
    REQUIRE(running.terminal.inject_input("\r"));
    drain_ready(running.io);

    // The tree re-shows the committed label.
    screen = visible_screen(running.terminal);
    CHECK(screen.find("[reviewed]") != std::string::npos);

    // The label entry landed in the session file targeting the first user
    // message.
    auto loaded = harness::session::SessionStore::load(fixture.session_file);
    REQUIRE(loaded.has_value());
    harness::session::SessionTree tree(std::move(*loaded));
    std::optional<std::string> first_user_id;
    for (const auto& entry : tree.entries()) {
        if (entry.kind == harness::session::SessionEntryKind::Message &&
            entry.message.has_value()) {
            const auto* user = std::get_if<ai::UserMessage>(&*entry.message);
            if (user != nullptr && !first_user_id) {
                first_user_id = entry.entry_id;
            }
        }
    }
    REQUIRE(first_user_id.has_value());
    const auto label = tree.get_label(*first_user_id);
    REQUIRE(label.has_value());
    CHECK(*label == "reviewed");

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
    "tree copy reports pi statuses through the clipboard writer",
    "[coding_agent][tui][tree-selector][e2e][issue410]") {
    Fixture fixture;
    fixture.write_session(fixture.session_file, {"user-0", "user-1"});
    Running running;
    auto actions = std::make_shared<coding_agent::tui::testing::ActionSinkRecorder>();
    auto session = boot(fixture, running, actions);

    double_escape(running);
    // Copy the selected entry (the last user message: the leaf — the
    // restored thinking entry — is hidden, so the selection sits on it).
    REQUIRE(running.terminal.inject_input("\x18")); // Ctrl+X
    drain_ready(running.io);
    auto screen = visible_screen(running.terminal);
    CHECK(screen.find("Copied selected message to clipboard") != std::string::npos);
    REQUIRE(actions->write_clipboard.size() == 1);
    CHECK(actions->write_clipboard[0].text == "user-1");

    // Escape, reopen, and copy an entry with no copyable text (the current
    // leaf is the assistant message; a settings entry hides behind the all
    // filter — the model-change entry carries no text).
    REQUIRE(running.terminal.inject_input("\x1b"));
    drain_ready(running.io);
    REQUIRE(running.terminal.inject_input(""));
    drain_ready(running.io);
    double_escape(running);
    REQUIRE(running.terminal.inject_input("\x01")); // Ctrl+A (all entries)
    drain_ready(running.io);
    // Move to the thinking entry below the last message and copy it: a
    // settings entry reports "no text to copy".
    REQUIRE(running.terminal.inject_input("\x1b[B"));
    drain_ready(running.io);
    REQUIRE(running.terminal.inject_input("\x18"));
    drain_ready(running.io);
    screen = visible_screen(running.terminal);
    CHECK(screen.find("Selected entry has no text to copy") != std::string::npos);

    REQUIRE(running.terminal.inject_input("\x1b"));
    drain_ready(running.io);
    REQUIRE(running.terminal.inject_input(""));
    drain_ready(running.io);
    REQUIRE(running.terminal.inject_input("\x04"));
    drain_ready(running.io);
    REQUIRE(running.run_result);
    CHECK(*running.run_result);
}
