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
#include "coding_agent/runtime/SessionFactory.hpp"
#include "coding_agent/tui/InteractiveMode.hpp"
#include "coding_agent/tui/InteractiveSessionRun.hpp"
#include "coding_agent/tui/TestTuiActionSink.hpp"
#include "support/AsyncResultBridge.hpp"
#include "support/EnvVarGuard.hpp"
#include "support/PumpUntil.hpp"
#include "support/RuntimeFixture.hpp"
#include "support/RuntimeLoopDriver.hpp"
#include "support/ScriptedRuntimeFixture.hpp"
#include "support/TempWorkspace.hpp"

#include "agent/harness/RuntimeRoot.hpp"
#include <cch/agent/harness/session/SessionStore.hpp>
#include <cch/agent/harness/session/SessionTree.hpp>
#include <cch/ai/Content.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include <cch/support/Error.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>
#include "support/AgentRootFixture.hpp"

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
/// a temp Agent Config Directory (`HOME`) whose models.json
/// drives runtime creation deterministically.
struct Fixture {
    cch::tests::TempWorkspace workspace;
    std::filesystem::path agent_dir;
    tests::EnvVarGuard home_guard{"HOME"};
    tests::EnvVarGuard kimi_guard{"KIMI_API_KEY"};
    std::filesystem::path session_file;

    Fixture() {
        home_guard.set(workspace.path().string());
        agent_dir = tests::agent_root_under_home(workspace.path());
        std::filesystem::create_directories(agent_dir);
        agent_dir = tests::agent_root_under_home(workspace.path());
        std::filesystem::create_directories(agent_dir);
        kimi_guard.unset();
        session_file = workspace.path() / "session.jsonl";
        std::ofstream models(agent_dir / "models.json", std::ios::binary);
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
/// session-replacement surface (pi `createRuntime`) routed through the
/// asynchronous Session replacement test adapter (issue #580). The recorder
/// shares its state with the closed action sink, so it stays valid while
/// the run lives.
[[nodiscard]] std::unique_ptr<coding_agent::AgentSession> boot(
    Fixture& fixture,
    Running& running,
    std::shared_ptr<coding_agent::tui::testing::ActionSinkRecorder> actions =
        std::make_shared<coding_agent::tui::testing::ActionSinkRecorder>()) {
    coding_agent::runtime::AgentSessionCreationRequest request;
    request.session_facts.no_skills = true;
    request.session_facts.no_prompt_templates = true;
    request.workspace = fixture.workspace.path();
    request.session_target = coding_agent::ExplicitOpenOrCreateSessionTarget{fixture.session_file};

    // The replacement Runtime Root shares the interactive loop: assembly
    // work and the install continuation are both pumped by the test.
    auto runtime_io = std::shared_ptr<boost::asio::io_context>(&running.io, [](boost::asio::io_context*) {});
    auto runtime_root = std::make_shared<harness::RuntimeRoot>(std::move(runtime_io), harness::RuntimeLimits{});

    // Create the boot session through the one asynchronous Session Assembly
    // door on the interactive loop (RuntimeRoot holds a work guard on the
    // loop, so pump until creation completes rather than draining).
    request.execution_runtime_target = runtime_root->make_target();
    std::optional<support::Expected<coding_agent::CreateAgentSessionResult>> booted;
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
    return std::move(created.session);
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

namespace {

/// The compaction summarization response (pi `summarization_response`), same
/// shape as FooterStatusInteractiveTest's.
[[nodiscard]] ai::AssistantMessage summarization_response() {
    auto summary = ai::assistant_text_message("## Goal\nCompacted history summary");
    summary.provider = "fake";
    summary.api = "fake";
    summary.model = "fake-model";
    summary.timestamp =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
                    .count();
    summary.usage = ai::Usage{};
    summary.usage.input = 3000;
    summary.usage.output = 100;
    return summary;
}

/// The screen text with line breaks and padding runs collapsed to single
/// spaces, so a wrapped chat line (the long refusal sentence) can be matched
/// whole.
[[nodiscard]] std::string collapse_whitespace(std::string_view text) {
    std::string collapsed;
    bool previous_was_whitespace = false;
    for (const char character : text) {
        const bool is_whitespace = std::isspace(static_cast<unsigned char>(character)) != 0;
        if (is_whitespace) {
            if (!previous_was_whitespace) collapsed.push_back(' ');
        } else {
            collapsed.push_back(character);
        }
        previous_was_whitespace = is_whitespace;
    }
    return collapsed;
}

[[nodiscard]] std::string flatten_screen(const tui::VirtualTerminal& terminal) {
    std::string text;
    for (const auto& line : terminal.screen()) {
        for (const char c : line) {
            if (c == ' ' && !text.empty() && text.back() == ' ') continue;
            text.push_back(c);
        }
        text.push_back(' ');
    }
    return text;
}

/// A persisted session with one user/assistant pair, resumable through the
/// interactive boot with an injected scripted runtime — the streaming /
/// compaction counterpart of the plain Fixture above (same shape as
/// FooterStatusInteractiveTest's ResumedSessionFixture). The tiny
/// keepRecentTokens budget makes the small session summarizable (pi's
/// findCutPoint keeps the recent budget), so a compaction actually runs.
struct StreamingFixture {
    tests::TempWorkspace workspace;
    tests::TempWorkspace config;
    tests::EnvVarGuard home_guard{"HOME", config.path().string()};
    std::filesystem::path session_file;
    tests::ScriptedRuntimeFixture scripted;
    std::shared_ptr<coding_agent::ModelRuntime> runtime{scripted.runtime};
    tests::RuntimeFixture runtime_fixture;
    std::unique_ptr<coding_agent::AgentSession> session;

    StreamingFixture() {
        config.write(".config/pike/agent/settings.json",
                R"({"compaction": {"enabled": true, "keepRecentTokens": 1, "reserveTokens": 1}})");
    }

    void create() {
        session_file = workspace.path() / "tree-stream-session.jsonl";
        auto store = harness::session::SessionStore::create_new(session_file,
                {
                        .session_id = "tree-stream-session",
                        .created_at = "2026-08-12T00:00:00Z",
                        .workspace = workspace.path(),
                        .provider = "fake",
                        .model = "fake-model",
                });
        REQUIRE(store);
        REQUIRE(store->append(ai::MessageVariant{ai::user_text_message("resume request", 1'700'000'000'000)}));
        ai::AssistantMessage assistant;
        assistant.provider = "fake";
        assistant.api = "fake";
        assistant.model = "fake-model";
        assistant.stop_reason = ai::AssistantStopReason::Stop;
        assistant.timestamp = 1'700'000'000'001;
        assistant.content.emplace_back(ai::text_content("resumed reply text"));
        REQUIRE(store->append(ai::MessageVariant{assistant}));

        coding_agent::runtime::AgentSessionCreationRequest request;
        request.session_target = coding_agent::ExplicitResumeSessionTarget{session_file};
        request.execution_runtime_target = runtime_fixture.make_target();
        request.workspace = workspace.path();
        request.session_facts.no_skills = true;
        request.session_facts.no_prompt_templates = true;
        request.model_runtime = runtime;
        auto created =
                runtime_fixture.run(coding_agent::create_agent_session_async(std::move(request), std::nullopt, {}));
        REQUIRE(created);
        session = std::move(created->session);
    }
};

/// Boot the interactive mode over the fixture's resumed scripted-runtime
/// session (the streaming counterpart of `boot` above).
void boot_scripted(StreamingFixture& fixture,
        tui::VirtualTerminal& terminal,
        boost::asio::io_context& io,
        std::optional<support::ExpectedVoid>& run_result) {
    boost::asio::co_spawn(io,
            coding_agent::tui::run_interactive_mode(terminal,
                    coding_agent::tui::InteractiveSessionRunBuilder{}
                            .with_session(*fixture.session)
                            .with_agent_config_directory(fixture.config.path())
                            .build()),
            [&](std::exception_ptr exception, support::ExpectedVoid result) {
                CHECK(exception == nullptr);
                run_result.emplace(std::move(result));
            });
    drain_ready(io);
}

} // namespace

TEST_CASE("double-escape with an empty editor opens the session tree overlay",
        "[coding_agent][tui][tree-selector][e2e][issue410][spec]") {
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

TEST_CASE("tree navigation switches the active path, pre-fills the editor, and reports the pi status",
        "[coding_agent][tui][tree-selector][e2e][issue410][spec]") {
    Fixture fixture;
    fixture.write_session(fixture.session_file, {"user-0", "user-1", "user-2"});
    Running running;
    auto actions = std::make_shared<coding_agent::tui::testing::ActionSinkRecorder>();
    auto session = boot(fixture, running, actions);
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
    // Tree navigation is an in-place `navigate_tree` on the live Session
    // (pi `navigateTree`), not a Session replacement: the asynchronous
    // replacement adapter is never crossed.
    CHECK(actions->replace_sessions.empty());

    // The editor carries the pre-filled text: clear it (ctrl+c) before the
    // empty-editor exit (ctrl+d).
    REQUIRE(running.terminal.inject_input("\x03"));
    drain_ready(running.io);
    REQUIRE(running.terminal.inject_input("\x04"));
    drain_ready(running.io);
    REQUIRE(running.run_result);
    CHECK(*running.run_result);
}

TEST_CASE("selecting the current leaf reports Already at this point",
        "[coding_agent][tui][tree-selector][e2e][issue410][spec]") {
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

TEST_CASE("a fresh in-memory session opens the tree on its initial thinking entry",
        "[coding_agent][tui][tree-selector][e2e][issue491][spec]") {
    Fixture fixture;
    // An in-memory session (no file, no messages). pi's createAgentSession
    // appends the initial thinking-level change to every new session, so the
    // tree is never entry-less for a created session; the selector opens on
    // that one entry (hidden by the default filter) instead of reporting an
    // empty session.
    Running running;
    auto actions = std::make_shared<coding_agent::tui::testing::ActionSinkRecorder>();
    auto runtime_io = std::shared_ptr<boost::asio::io_context>(&running.io, [](boost::asio::io_context*) {});
    auto runtime_root = std::make_shared<harness::RuntimeRoot>(std::move(runtime_io), harness::RuntimeLimits{});

    coding_agent::runtime::AgentSessionCreationRequest request;
    request.session_facts.no_skills = true;
    request.session_facts.no_prompt_templates = true;
    request.workspace = fixture.workspace.path();
    request.session_target = coding_agent::InMemorySessionTarget{};
    request.execution_runtime_target = runtime_root->make_target();
    std::optional<support::Expected<coding_agent::CreateAgentSessionResult>> booted;
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

TEST_CASE("shift+l edits a label and persists the label entry",
        "[coding_agent][tui][tree-selector][e2e][issue410][spec]") {
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

TEST_CASE("tree copy reports pi statuses through the clipboard writer",
        "[coding_agent][tui][tree-selector][e2e][issue410][spec]") {
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

TEST_CASE("tree navigation while only streaming aborts the run and then navigates",
        "[coding_agent][tui][tree-selector][e2e][issue788][spec]") {
    // pi interactive-mode.ts:5461-5472: the navigation commit stops the
    // active response first (restore queued input, abort, settle) and only
    // refuses afterwards when a compaction is still in flight. With no
    // compaction running, the navigation proceeds as before this ticket.
    StreamingFixture fixture;
    fixture.create();
    tests::RuntimeLoopDriver runtime_driver(fixture.runtime_fixture);
    // Gate the first model call so the prompt is streaming when the tree
    // navigation commits.
    fixture.scripted.control->gate_at = 0;

    tui::VirtualTerminal terminal({.columns = 100, .rows = 30});
    boost::asio::io_context io;
    std::optional<support::ExpectedVoid> run_result;
    boot_scripted(fixture, terminal, io, run_result);
    const tests::RunJoinGuard join_run{io, [&] { return run_result.has_value(); }};

    REQUIRE(terminal.inject_input("hello\r"));
    REQUIRE(tests::pump_until(io, [&] { return fixture.session->is_streaming(); }));
    CHECK_FALSE(fixture.session->is_compacting());

    // /tree opens the selector over the active response; the abort-vs-refuse
    // decision happens at the navigation commit.
    REQUIRE(terminal.inject_input("/tree\r"));
    drain_ready(io);
    CHECK(flatten_screen(terminal).find("Session Tree") != std::string::npos);
    // Move the selection off the leaf (the in-flight turn has not committed
    // its trailing entries, so the last visible row is the leaf) to an
    // earlier message, then commit.
    REQUIRE(terminal.inject_input("\x1b[A"));
    drain_ready(io);
    REQUIRE(terminal.inject_input("\x1b[A"));
    drain_ready(io);
    REQUIRE(terminal.inject_input("\r"));
    REQUIRE(tests::pump_until(
            io, [&] { return flatten_screen(terminal).find("Navigated to selected point") != std::string::npos; }));
    // The streaming run was aborted first; the navigation proceeded (the
    // compaction refusal is absent).
    CHECK_FALSE(fixture.session->is_streaming());
    CHECK(flatten_screen(terminal).find(
                  "Wait for the current compaction or tree navigation to finish before navigating the session tree.") ==
            std::string::npos);

    // The editor carries pre-filled/restored text: clear it (ctrl+c) before
    // the empty-editor exit (ctrl+d).
    REQUIRE(terminal.inject_input("\x03"));
    drain_ready(io);
    REQUIRE(terminal.inject_input("\x04"));
    REQUIRE(tests::pump_until(io, [&] { return run_result.has_value(); }));
    CHECK(*run_result);
}

TEST_CASE("tree navigation while compacting refuses with pi's verbatim error and keeps the compaction UI",
        "[coding_agent][tui][tree-selector][compaction][e2e][issue788][spec]") {
    // pi interactive-mode.ts:5461-5472: a compaction in flight refuses with
    // the verbatim error while preserving the active operation's status UI.
    StreamingFixture fixture;
    fixture.create();
    tests::RuntimeLoopDriver runtime_driver(fixture.runtime_fixture);
    // Gate the summarization call so compaction remains active while the
    // selector commits.
    fixture.scripted.control->gate_at = 0;
    fixture.scripted.control->responses.push_back(summarization_response());

    tui::VirtualTerminal terminal({.columns = 100, .rows = 30});
    boost::asio::io_context io;
    std::optional<support::ExpectedVoid> run_result;
    std::optional<support::Expected<coding_agent::CompactionResult>> compact_result;
    boot_scripted(fixture, terminal, io, run_result);
    const tests::RunJoinGuard join_run{io, [&] { return run_result.has_value(); }};

    // Open the selector before starting compaction; its navigation commit can
    // exercise the refusal while the active operation remains gated.
    REQUIRE(terminal.inject_input("/tree\r"));
    REQUIRE(tests::pump_until(io, [&] { return flatten_screen(terminal).find("Session Tree") != std::string::npos; }));
    boost::asio::co_spawn(
            io,
            [&]() -> boost::asio::awaitable<void> {
                compact_result = co_await fixture.session->compact();
                co_return;
            },
            boost::asio::detached);
    REQUIRE(tests::pump_until(io, [&] {
        return fixture.session->is_compacting() && !fixture.session->is_streaming() &&
               flatten_screen(terminal).find("Compacting context... (escape to cancel)") != std::string::npos;
    }));
    CHECK_FALSE(fixture.session->is_streaming());

    // Navigate the already-open session tree during the compaction: refused
    // verbatim. Move the selection to the first visible row (repeated
    // arrow-ups clamp at the top — never the hidden thinking leaf), then commit.
    for (int up = 0; up < 8; ++up) {
        REQUIRE(terminal.inject_input("\x1b[A"));
        drain_ready(io);
    }
    REQUIRE(terminal.inject_input("\r"));
    REQUIRE(tests::pump_until(io, [&] {
        const auto screen = collapse_whitespace(flatten_screen(terminal));
        return screen.find("Wait for the current compaction or tree navigation to finish before navigating the session "
                           "tree.") != std::string::npos;
    }));
    drain_ready(io);
    auto screen = flatten_screen(terminal);
    // The navigation never ran.
    CHECK(screen.find("Navigated to selected point") == std::string::npos);
    // The compaction status UI is preserved.
    CHECK(screen.find("Compacting context... (escape to cancel)") != std::string::npos);

    fixture.scripted.control->release();
    REQUIRE(tests::pump_until(io, [&] { return compact_result.has_value(); }));
    REQUIRE(compact_result->has_value());
    // The editor may carry pre-filled/restored text: clear it (ctrl+c)
    // before the empty-editor exit (ctrl+d).
    REQUIRE(terminal.inject_input("\x03"));
    drain_ready(io);
    REQUIRE(terminal.inject_input("\x04"));
    REQUIRE(tests::pump_until(io, [&] { return run_result.has_value(); }));
    CHECK(*run_result);
}
