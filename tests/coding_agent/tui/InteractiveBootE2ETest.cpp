// Interactive-boot-through-an-agent-turn E2E (issue #399): VirtualTerminal-
// driven boot, a fake ModelRuntime scripted turn, and CLI-level goldens of
// the boot and post-turn screens. The goldens live under
// `fixtures/pi-coding-agent/e2e/` and are byte-compared like the pi-tui
// screen-state gate; P24 wraps the bundle documentation around them.
//
// Coverage per the ticket's acceptance criteria:
// - boot renders pi's main-screen composition (header hints only, no logo;
//   chat; pending-messages; status; editor; footer) and the initial Agent
//   Session snapshot before focusing input;
// - a focused-editor submission starts an ordinary prompt; the assistant
//   streams in the pi shape (thinking blocks, error/abort notices) with
//   OSC 133 zones; user messages render in the pi box shape;
// - tool calls render through the tool-execution surface with the edit
//   `diff` renderer;
// - `app.interrupt` follows pi's precedence with the stale-generation guard.

#include "coding_agent/tui/InteractiveMode.hpp"
#include "support/FakeModelRuntime.hpp"
#include "support/ModelsFixture.hpp"
#include "support/TempWorkspace.hpp"

#include <cch/agent/harness/session/SessionStore.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include "coding_agent/AgentSession.hpp"
#include "coding_agent/runtime/SessionFactory.hpp"
#include "support/ExpectedMacros.hpp"

#include "ai/providers/FakeProvider.hpp"

#include <cch/support/Error.hpp>
#include <catch2/catch_test_macros.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace cch;

namespace {

void drain_ready(boost::asio::io_context& io) {
    if (io.stopped()) io.restart();
    while (io.poll() != 0) {
    }
}

[[nodiscard]] std::string visible_screen(const tui::VirtualTerminal& terminal) {
    std::string text;
    for (const auto& line : terminal.screen()) {
        text.append(line);
        text.push_back('\n');
    }
    return text;
}

[[nodiscard]] std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

[[nodiscard]] std::filesystem::path golden_path(std::string_view name) {
    return std::filesystem::path{CCH_SOURCE_DIR} / "fixtures/pi-coding-agent/e2e" / name;
}

/// Writes the screen to the committed golden when CCH_CAPTURE_GOLDENS=1.
void capture_golden(std::string_view name, const std::string& screen) {
    if (std::getenv("CCH_CAPTURE_GOLDENS") == nullptr) return;
    const auto path = golden_path(name);
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream output{path, std::ios::binary};
    output << screen;
}

[[nodiscard]] bool any_output_line_has(
    const tui::VirtualTerminal& terminal,
    std::string_view needle) {
    for (const auto& line : terminal.output()) {
        if (line.find(needle) != std::string::npos) return true;
    }
    return false;
}


/// One resumed session with a fixed workspace file the scripted edit tool
/// modifies. The session carries the injected fake ModelRuntime. The
/// workspace lives at the deterministic `e2e_workspace_path()` so the
/// CLI-level goldens (which render the footer's pwd line) stay byte-stable.
struct E2eSession {
    std::filesystem::path workspace;
    tests::TempWorkspace config;
    std::shared_ptr<coding_agent::ModelRuntime> runtime;
    std::unique_ptr<coding_agent::AgentSession> session;
};

/// Deterministic workspace path for the CLI-level goldens: the footer's pwd
/// line renders the workspace, so a random temp path would make the goldens
/// nondeterministic. The fixture removes and recreates the directory at
/// boot, so a crashed prior run cannot leak stale state.
[[nodiscard]] std::filesystem::path e2e_workspace_path() {
    std::error_code error;
    // Fixed base: the committed goldens render the footer's pwd line, so a
    // TMPDIR-isolated temp path would make them byte-unstable.
    const auto base = std::filesystem::path{"/tmp"};
    const auto path = base / "cpp-harness-e2e-workspace";
    std::filesystem::remove_all(path, error);
    error.clear();
    std::filesystem::create_directories(path, error);
    REQUIRE(!error);
    return path;
}

[[nodiscard]] std::unique_ptr<E2eSession> make_e2e_session(
    std::shared_ptr<coding_agent::ModelRuntime> runtime) {
    auto fixture = std::make_unique<E2eSession>();
    fixture->workspace = e2e_workspace_path();
    {
        std::ofstream notes(fixture->workspace / "notes.txt", std::ios::binary);
        notes << "alpha\n";
    }

    const auto session_file = fixture->workspace / "e2e-session.jsonl";
    auto store = harness::session::SessionStore::create_new(
        session_file,
        {
            .session_id = "e2e-session",
            .created_at = "2026-08-10T00:00:00Z",
            .workspace = fixture->workspace,
            .provider = "fake",
            .model = "fake-model",
        });
    REQUIRE(store);

    ai::UserMessage user;
    user.timestamp = 1'700'000'000'000;
    user.content = std::vector<ai::Content>{
        ai::text_content("Resume request: check the notes file."),
    };
    REQUIRE(store->append(ai::MessageVariant{user}));

    ai::AssistantMessage assistant;
    assistant.provider = "fake";
    assistant.api = "fake";
    assistant.model = "fake-model";
    assistant.stop_reason = ai::AssistantStopReason::Stop;
    assistant.timestamp = 1'700'000'000'001;
    assistant.content.emplace_back(ai::text_content(
        "Resumed reply: the notes file is ready."));
    REQUIRE(store->append(ai::MessageVariant{assistant}));

    fixture->runtime = std::move(runtime);

    coding_agent::runtime::AgentSessionCreationRequest request;
    request.session_target = coding_agent::ExplicitResumeSessionTarget{session_file};
    request.workspace = fixture->workspace;
    request.execution_runtime_target = tests::detail::fixture_runtime_target();
    request.session_facts.no_skills = true;
    request.session_facts.no_prompt_templates = true;
    request.model_runtime = fixture->runtime;
    auto created = coding_agent::create_agent_session_for_testing(
        std::move(request), ai::providers::make_scripted_fake_models());
    REQUIRE(created);
    fixture->session = std::move(created->session);
    return fixture;
}

/// The scripted turn for the golden: thinking, a plan, an edit call (its
/// result renders through the diff renderer), then the final answer.
[[nodiscard]] std::shared_ptr<tests::FakeModelRuntime> scripted_turn_runtime() {
    auto runtime = std::make_shared<tests::FakeModelRuntime>();
    ai::AssistantMessage turn;
    turn.provider = "fake";
    turn.api = "fake";
    turn.model = "fake-model";
    turn.stop_reason = ai::AssistantStopReason::ToolUse;
    turn.content.emplace_back(ai::thinking_content(
        "Check the notes file content, then update the line."));
    turn.content.emplace_back(ai::text_content(
        "I will edit notes.txt to replace the placeholder line."));
    turn.content.emplace_back(ai::tool_call_content(
        "e2e-edit-1",
        "edit",
        R"({"path":"notes.txt","edits":[{"oldText":"alpha","newText":"beta"}]})"));
    runtime->responses.push_back(std::move(turn));

    ai::AssistantMessage final_answer;
    final_answer.provider = "fake";
    final_answer.api = "fake";
    final_answer.model = "fake-model";
    final_answer.stop_reason = ai::AssistantStopReason::Stop;
    final_answer.content.emplace_back(ai::text_content(
        "Done: the notes file now says beta."));
    runtime->responses.push_back(std::move(final_answer));
    return runtime;
}



} // namespace

TEST_CASE(
    "E2E: interactive boot renders pi's main-screen composition and the initial snapshot",
    "[coding_agent][tui][e2e][issue399]") {
    auto fixture = make_e2e_session(scripted_turn_runtime());

    tui::VirtualTerminal terminal({.columns = 72, .rows = 24});
    boost::asio::io_context io;
    std::optional<support::ExpectedVoid> run_result;
    boost::asio::co_spawn(
        io,
        coding_agent::tui::run_interactive_mode(
            *fixture->session,
            terminal,
            {.agent_config_directory = fixture->config.path()}),
        [&](std::exception_ptr exception, support::ExpectedVoid result) {
            CHECK(exception == nullptr);
            run_result.emplace(std::move(result));
        });
    drain_ready(io);

    const auto screen = visible_screen(terminal);
    capture_golden("boot.txt", screen);

    // The committed CLI-level boot golden.
    const auto expected = read_text_file(golden_path("boot.txt"));
    CHECK(screen == expected);

    // Main-screen composition: header keybinding hints only, no logo; the
    // initial snapshot renders before input focus.
    CHECK(screen.find("interrupt") != std::string::npos);
    CHECK(screen.find("Resume request: check the notes file.") != std::string::npos);
    CHECK(screen.find("Resumed reply: the notes file is ready.") != std::string::npos);

    REQUIRE(terminal.inject_input("\x04"));
    drain_ready(io);
    REQUIRE(run_result);
    CHECK(*run_result);
}

TEST_CASE(
    "E2E: a focused-editor submission streams a fake-runtime turn in the pi shape",
    "[coding_agent][tui][e2e][issue399]") {
    auto fixture = make_e2e_session(scripted_turn_runtime());
    auto* runtime = static_cast<tests::FakeModelRuntime*>(fixture->runtime.get());

    // 25 rows keep the typed user message visible below the two-line compact
    // header (the loaded-resources notice, #418); at 24 rows the extra header
    // row would scroll it off.
    tui::VirtualTerminal terminal({.columns = 72, .rows = 25});
    boost::asio::io_context io;
    std::optional<support::ExpectedVoid> run_result;
    boost::asio::co_spawn(
        io,
        coding_agent::tui::run_interactive_mode(
            *fixture->session,
            terminal,
            {.agent_config_directory = fixture->config.path()}),
        [&](std::exception_ptr exception, support::ExpectedVoid result) {
            CHECK(exception == nullptr);
            run_result.emplace(std::move(result));
        });
    drain_ready(io);

    REQUIRE(terminal.inject_input("continue the work\r"));
    for (int attempt = 0; attempt < 100 && runtime->calls.size() != 2; ++attempt) {
        drain_ready(io);
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }

    REQUIRE(runtime->calls.size() == 2);
    const auto screen = visible_screen(terminal);
    capture_golden("turn.txt", screen);

    // The committed CLI-level post-turn golden.
    const auto expected = read_text_file(golden_path("turn.txt"));
    CHECK(screen == expected);

    // The scripted turn rendered in the pi shape: thinking blocks, the
    // assistant text, the edit tool-execution block with the diff renderer,
    // and the final answer.
    CHECK(screen.find("Check the notes file content") != std::string::npos);
    CHECK(screen.find("I will edit notes.txt") != std::string::npos);
    CHECK(screen.find("edit notes.txt") != std::string::npos);
    CHECK(screen.find("-1 alpha") != std::string::npos);
    CHECK(screen.find("+1 beta") != std::string::npos);
    CHECK(screen.find("Done: the notes file now says beta.") != std::string::npos);
    // The user message renders in the pi box shape (no "You:" label).
    CHECK(screen.find("You: continue the work") == std::string::npos);
    CHECK(screen.find("continue the work") != std::string::npos);

    // OSC 133 zones wrap the rendered user and assistant blocks: an A-zone
    // line precedes the block and the B/C-zone line follows its last line.
    // (The write sequence is frame-timing dependent — the status indicator
    // animation interleaves renders — so the zones are asserted by ordering
    // in the output stream, and the committed screen golden stays the
    // byte-level gate.)
    const auto& output = terminal.output();
    const auto user_line = std::find_if(output.begin(), output.end(), [](const auto& line) {
        return line.find("continue the work") != std::string::npos;
    });
    REQUIRE(user_line != output.end());
    const auto user_zone_a = std::find_if(output.begin(), user_line, [](const auto& line) {
        return line.find("\x1b]133;A\x07") != std::string::npos;
    });
    CHECK(user_zone_a != user_line);
    const auto user_zone_bc = std::find_if(user_line, output.end(), [](const auto& line) {
        return line.find("\x1b]133;B\x07\x1b]133;C\x07") != std::string::npos;
    });
    CHECK(user_zone_bc != output.end());
    // The assistant message that carries the tool call renders without the
    // zones (pi suppresses them while tool components render separately); the
    // final answer message carries them.
    const auto first_final_line = std::find_if(output.begin(), output.end(), [](const auto& line) {
        return line.find("Done: the notes file now says beta.") != std::string::npos;
    });
    REQUIRE(first_final_line != output.end());
    CHECK((first_final_line->find("\x1b]133;A\x07") != std::string::npos ||
        (first_final_line != output.begin() &&
         (first_final_line - 1)->find("\x1b]133;A\x07") != std::string::npos)));
    CHECK(any_output_line_has(terminal, "\x1b]133;B\x07\x1b]133;C\x07"));

    // The edit tool applied the replacement in the workspace.
    CHECK(read_text_file(fixture->workspace / "notes.txt") == "beta\n");

    REQUIRE(terminal.inject_input("\x04"));
    drain_ready(io);
    REQUIRE(run_result);
    CHECK(*run_result);
}

TEST_CASE(
    "E2E: app.interrupt aborts the active Agent run with the stale-generation guard",
    "[coding_agent][tui][e2e][issue399]") {
    auto gated = std::make_shared<tests::FakeModelRuntime>();
    gated->gate_at = 0;
    gated->emit_partial_before_gate = true;
    auto fixture = make_e2e_session(gated);

    tui::VirtualTerminal terminal({.columns = 72, .rows = 24});
    boost::asio::io_context io;
    std::optional<support::ExpectedVoid> run_result;
    boost::asio::co_spawn(
        io,
        coding_agent::tui::run_interactive_mode(
            *fixture->session,
            terminal,
            {.agent_config_directory = fixture->config.path()}),
        [&](std::exception_ptr exception, support::ExpectedVoid result) {
            CHECK(exception == nullptr);
            run_result.emplace(std::move(result));
        });
    drain_ready(io);

    // Esc while idle with an empty editor is a no-op (no pending Bash, no
    // active run): the editor stays focused and the session keeps running.
    // The empty inject flushes the input decoder's pending ESC.
    REQUIRE(terminal.inject_input("\x1b"));
    REQUIRE(terminal.inject_input(""));
    drain_ready(io);
    CHECK(gated->calls.empty());

    // A focused-editor submission starts the run; Esc aborts it while the
    // stream is active (pi's app.interrupt precedence: active Agent run
    // first, then running User Bash, then pending User Bash submission).
    REQUIRE(terminal.inject_input("start the run\r"));
    drain_ready(io);
    REQUIRE(gated->calls.size() == 1);
    REQUIRE(terminal.inject_input("\x1b"));
    REQUIRE(terminal.inject_input(""));
    drain_ready(io);
    const auto screen = visible_screen(terminal);
    CHECK(screen.find("Operation aborted") != std::string::npos);

    // The aborted run is quiescent: a fresh submission starts a new turn;
    // releasing the gate lets it complete.
    REQUIRE(terminal.inject_input("retry\r"));
    drain_ready(io);
    CHECK(gated->calls.size() == 2);
    gated->release();
    drain_ready(io);
    CHECK(visible_screen(terminal).find("gated done") != std::string::npos);

    REQUIRE(terminal.inject_input("\x04"));
    drain_ready(io);
    REQUIRE(run_result);
    CHECK(*run_result);
}

TEST_CASE(
    "E2E: the model fallback message renders as a boot warning line",
    "[coding_agent][tui][e2e][issue404]") {
    auto fixture = make_e2e_session(scripted_turn_runtime());

    tui::VirtualTerminal terminal({.columns = 72, .rows = 24});
    boost::asio::io_context io;
    std::optional<support::ExpectedVoid> run_result;
    boost::asio::co_spawn(
        io,
        coding_agent::tui::run_interactive_mode(
            *fixture->session,
            terminal,
            {.agent_config_directory = fixture->config.path(),
             .model_fallback_message =
                 "Could not restore model deepseek/deepseek-v4-flash. "
                 "Using openai-codex/gpt-5.5"}),
        [&](std::exception_ptr exception, support::ExpectedVoid result) {
            CHECK(exception == nullptr);
            run_result.emplace(std::move(result));
        });
    drain_ready(io);

    // pi interactive-mode.ts `showWarning`: `Warning: <modelFallbackMessage>`
    // renders in the chat container before the initial prompt (wrapped at
    // the terminal width like pi's Text component).
    const auto screen = visible_screen(terminal);
    CHECK(screen.find(
              "Warning: Could not restore model deepseek/deepseek-v4-flash.") !=
          std::string::npos);
    CHECK(screen.find("openai-codex/gpt-5.5") != std::string::npos);
    // The resumed history still renders below the warning.
    CHECK(screen.find("Resume request: check the notes file.") != std::string::npos);

    REQUIRE(terminal.inject_input("\x04"));
    drain_ready(io);
    REQUIRE(run_result);
    CHECK(*run_result);
}
