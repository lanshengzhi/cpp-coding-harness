// P26 (#422): interactive/rendering goldens — C++-side screen goldens pinning
// the boot, the full message pipeline, and key flows through the
// VirtualTerminal seam with deterministic dimensions/environment. The screens
// live under `fixtures/pi-coding-agent/rendering/` and are byte-compared here
// like the e2e CLI-level goldens (pi keeps no golden renders; the
// interactive/rendering surface is C++-side only per the G6 record, #394).
//
// Coverage:
// - `message-pipeline.txt` — one deterministic screen rendering the whole
//   message/execution pipeline in pi's shapes: the compaction-summary message,
//   user message, assistant thinking/text, tool-execution with the diff/
//   result rendering, bash-execution block, custom and branch-summary
//   messages.
// - `model-switch.txt` — a key flow: Ctrl+L model selector → Enter switches
//   the session model, with the `Model:` status and the footer model.
// - `fork.txt` — a key flow: the in-session fork user-message selector
//   overlay (pi showUserMessageSelector).
// - `interrupt.txt` — a key flow: `app.interrupt` aborts the active run and
//   the aborted assistant entry renders.
//
// The workspace lives at the deterministic `cpp-harness-rendering-<name>`
// temp path (recreated at boot) so the footer's pwd line stays byte-stable;
// the environment is pinned by the gate capture sidecar. Regenerate from the
// frozen checkout with `CCH_CAPTURE_GOLDENS=1 ./build/cch_tests_coding_agent_interactive
// "[issue422]"` (or the gate sidecar, which then byte-verifies).

#include "coding_agent/AgentSession.hpp"
#include "coding_agent/runtime/SessionFactory.hpp"
#include "coding_agent/tui/InteractiveMode.hpp"
#include "coding_agent/tui/InteractiveSessionRun.hpp"
#include "coding_agent/tui/TestTuiActionSink.hpp"
#include "support/EnvVarGuard.hpp"
#include "support/PumpUntil.hpp"
#include "support/ScriptedRuntimeFixture.hpp"
#include "support/TempWorkspace.hpp"

#include <cch/agent/harness/session/SessionStore.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include "support/ExpectedMacros.hpp"

#include <cch/support/Error.hpp>
#include <catch2/catch_test_macros.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace cch;
using tests::drain_ready;

namespace {

[[nodiscard]] auto make_run(
    coding_agent::AgentSession& session,
    const std::filesystem::path& config_dir = {},
    coding_agent::tui::TuiActionSink sink = nullptr) {
    return coding_agent::tui::InteractiveSessionRunBuilder{}
        .with_session(session)
        .with_agent_config_directory(config_dir)
        .with_action_sink(std::move(sink))
        .build();
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
    return std::filesystem::path{CCH_SOURCE_DIR} /
           "fixtures/pi-coding-agent/rendering" / name;
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

/// Deterministic workspace path for the rendering goldens: the footer's pwd
/// line renders the workspace, so a random temp path would make the goldens
/// nondeterministic. Removed and recreated at boot per golden name.
[[nodiscard]] std::filesystem::path rendering_workspace_path(
    std::string_view name) {
    std::error_code error;
    // Fixed base: the committed goldens render the footer's pwd line, so a
    // TMPDIR-isolated temp path would make them byte-unstable.
    const auto base = std::filesystem::path{"/tmp"};
    const auto path = base / ("cpp-harness-rendering-" + std::string{name});
    std::filesystem::remove_all(path, error);
    error.clear();
    std::filesystem::create_directories(path, error);
    REQUIRE(!error);
    return path;
}

// ── message-pipeline: the full message/execution pipeline history ──────────

/// Builds a persisted session whose history exercises every message-pipeline
/// component (compaction-summary, user, assistant thinking/text, tool
/// execution, bash execution, custom, branch-summary) and resumes it through
/// the session runtime.
struct PipelineSession {
    std::filesystem::path workspace;
    tests::TempWorkspace config;
    std::unique_ptr<coding_agent::AgentSession> session;
};

[[nodiscard]] std::unique_ptr<PipelineSession> make_pipeline_session() {
    auto fixture = std::make_unique<PipelineSession>();
    fixture->workspace = rendering_workspace_path("message-pipeline");
    const auto session_file = fixture->workspace / "message-pipeline.jsonl";
    auto store = harness::session::SessionStore::create_new(
        session_file,
        {
            .session_id = "message-pipeline",
            .created_at = "2026-08-10T00:00:00Z",
            .workspace = fixture->workspace,
            .provider = "fake",
            .model = "fake-model",
        });
    REQUIRE(store);

    REQUIRE(store->append(ai::MessageVariant{
        ai::user_text_message("before compaction", 1'700'000'000'000)}));
    REQUIRE(store->append(ai::MessageVariant{
        ai::user_text_message("resume request", 1'700'000'000'001)}));
    auto loaded = harness::session::SessionStore::load(session_file);
    REQUIRE(loaded);
    REQUIRE(loaded->entries.size() >= 3);
    const auto kept_entry_id = loaded->entries[2].entry_id;
    REQUIRE(store->append_compaction(
        std::nullopt,
        harness::session::CompactionEntryValue{
            .summary = "compacted persisted context",
            .first_kept_entry_id = kept_entry_id,
            .tokens_before = 1200,
        }));

    ai::AssistantMessage assistant;
    assistant.provider = "fake";
    assistant.api = "fake";
    assistant.model = "fake-model";
    assistant.stop_reason = ai::AssistantStopReason::ToolUse;
    assistant.timestamp = 1'700'000'000'002;
    assistant.content.emplace_back(ai::thinking_content(
        "inspect the saved state\n"
        "compare the active path\n"
        "THINKING END"));
    assistant.content.emplace_back(
        ai::text_content("I will read the persisted file."));
    assistant.content.emplace_back(ai::tool_call_content(
        "pipeline-call-1",
        "read",
        R"({"path":"saved.txt"})"));
    assistant.content.emplace_back(
        ai::text_content("I will continue after the tool."));
    REQUIRE(store->append(ai::MessageVariant{assistant}));

    REQUIRE(store->append(ai::MessageVariant{ai::tool_result_message(
        "pipeline-call-1",
        "read",
        "persisted tool output",
        false,
        1'700'000'000'003)}));

    ai::BashExecutionMessage bash;
    bash.command = "ls -la";
    bash.output = "alpha\nbeta\n";
    bash.exit_code = 0;
    bash.timestamp = 1'700'000'000'004;
    REQUIRE(store->append(ai::MessageVariant{bash}));

    ai::CustomMessage custom;
    custom.custom_type = "notice";
    custom.content.emplace_back(ai::text_content("custom persisted content"));
    custom.timestamp = 1'700'000'000'005;
    REQUIRE(store->append(ai::MessageVariant{custom}));

    REQUIRE(store->append(ai::MessageVariant{ai::BranchSummaryMessage{
        .summary = "abandoned branch context",
        .from_id = "branch-entry",
        .timestamp = 1'700'000'000'006,
    }}));

    coding_agent::runtime::AgentSessionCreationRequest resume;
    resume.session_target =
        coding_agent::ExplicitResumeSessionTarget{session_file};
    resume.execution_runtime_target = tests::detail::fixture_runtime_target();
    resume.workspace = fixture->workspace;
    resume.session_facts.no_skills = true;
    resume.session_facts.no_prompt_templates = true;
    auto created =
            coding_agent::create_agent_session_for_testing(std::move(resume), tests::make_scripted_fake_models());
    REQUIRE(created);
    fixture->session = std::move(created->session);
    return fixture;
}

// ── model-switch: two keyed providers in a deterministic Agent Config Dir ───

constexpr std::string_view kReasoningAndPlainKeyed = R"({
  "providers": {
    "alpha": {
      "baseUrl": "https://alpha.example/v1",
      "api": "openai-responses",
      "apiKey": "dummy-alpha-key",
      "models": [{"id": "alpha-1", "name": "Alpha Reasoning", "reasoning": true}]
    },
    "beta": {
      "baseUrl": "https://beta.example/v1",
      "api": "openai-responses",
      "apiKey": "dummy-beta-key",
      "models": [{"id": "beta-1", "name": "Beta Plain", "reasoning": false}]
    }
  }
})";

struct ModelFixture {
    std::filesystem::path workspace;
    tests::TempWorkspace agent_dir;
    tests::EnvVarGuard dir_guard{"PI_CODING_AGENT_DIR"};
    tests::EnvVarGuard home_guard{"HOME"};
    tests::EnvVarGuard kimi_guard{"KIMI_API_KEY"};
    std::filesystem::path session_file;

    ModelFixture() {
        workspace = rendering_workspace_path("model-switch");
        dir_guard.set(agent_dir.path().string());
        home_guard.set(workspace.string());
        kimi_guard.unset();
        session_file = workspace / "model-switch.jsonl";
        std::ofstream models(agent_dir.path() / "models.json", std::ios::binary);
        models << kReasoningAndPlainKeyed;
    }
};

struct Running {
    // Terminal first: it must outlive the io_context, whose shutdown destroys
    // the interactive-mode coroutine frame (and its Tui) last.
    explicit Running(
        tui::VirtualTerminalOptions options = {
            .columns = 72, .rows = 24})
        : terminal{std::move(options)} {}

    tui::VirtualTerminal terminal;
    boost::asio::io_context io;
    std::optional<support::ExpectedVoid> run_result;
};

// ── interrupt: a gated scripted Provider for the app.interrupt leg ─────────

/// One resumed session (the e2e shape) for the interrupt golden.
struct InterruptSession {
    explicit InterruptSession(tests::ScriptedRuntimeFixture scripted_runtime) : scripted(std::move(scripted_runtime)) {}

    std::filesystem::path workspace;
    tests::TempWorkspace config;
    tests::ScriptedRuntimeFixture scripted;
    std::unique_ptr<coding_agent::AgentSession> session;
};

[[nodiscard]] std::unique_ptr<InterruptSession> make_interrupt_session(tests::ScriptedRuntimeFixture scripted_runtime) {
    auto fixture = std::make_unique<InterruptSession>(std::move(scripted_runtime));
    fixture->workspace = rendering_workspace_path("interrupt");

    const auto session_file = fixture->workspace / "interrupt-session.jsonl";
    auto store = harness::session::SessionStore::create_new(
        session_file,
        {
            .session_id = "interrupt-session",
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
    assistant.content.emplace_back(
        ai::text_content("Resumed reply: the notes file is ready."));
    REQUIRE(store->append(ai::MessageVariant{assistant}));

    coding_agent::runtime::AgentSessionCreationRequest request;
    request.session_target = coding_agent::ExplicitResumeSessionTarget{session_file};
    request.execution_runtime_target = tests::detail::fixture_runtime_target();
    request.workspace = fixture->workspace;
    request.session_facts.no_skills = true;
    request.session_facts.no_prompt_templates = true;
    request.model_runtime = fixture->scripted.runtime;
    auto created = coding_agent::create_agent_session(std::move(request));
    REQUIRE(created);
    fixture->session = std::move(created->session);
    return fixture;
}

} // namespace

TEST_CASE(
    "rendering golden: the full message pipeline renders in pi's shapes",
    "[coding_agent][tui][rendering][issue422]") {
    auto fixture = make_pipeline_session();

    // 52 rows keep the whole pipeline (compaction summary through branch
    // summary) in the chat viewport above the status/editor/footer rows.
    tui::VirtualTerminal terminal({.columns = 72, .rows = 52});
    boost::asio::io_context io;
    std::optional<support::ExpectedVoid> run_result;
    boost::asio::co_spawn(
        io,
        coding_agent::tui::run_interactive_mode(
            terminal,
            make_run(*fixture->session, fixture->config.path())),
        [&](std::exception_ptr exception, support::ExpectedVoid result) {
            CHECK(exception == nullptr);
            run_result.emplace(std::move(result));
        });
    drain_ready(io);

    const auto screen = visible_screen(terminal);
    capture_golden("message-pipeline.txt", screen);

    const auto expected = read_text_file(golden_path("message-pipeline.txt"));
    CHECK(screen == expected);

    // Every pipeline component renders: compaction summary, user message,
    // assistant thinking + text, tool execution, bash execution, custom and
    // branch summaries.
    CHECK(screen.find("Compacted from 1,200 tokens") != std::string::npos);
    CHECK(screen.find("resume request") != std::string::npos);
    CHECK(screen.find("inspect the saved state") != std::string::npos);
    CHECK(screen.find("I will read the persisted file.") != std::string::npos);
    CHECK(screen.find("read saved.txt") != std::string::npos);
    CHECK(screen.find("persisted tool output") != std::string::npos);
    CHECK(screen.find("$ ls -la") != std::string::npos);
    CHECK(screen.find("alpha") != std::string::npos);
    CHECK(screen.find("beta") != std::string::npos);
    CHECK(screen.find("[notice]") != std::string::npos);
    CHECK(screen.find("custom persisted content") != std::string::npos);
    CHECK(screen.find("[branch]") != std::string::npos);
    // Collapsed branch-summary label line (pi branch-summary-message.ts); the
    // summary body renders only when expanded.
    CHECK(screen.find("Branch summary") != std::string::npos);

    REQUIRE(terminal.inject_input("\x04"));
    drain_ready(io);
    REQUIRE(run_result);
    CHECK(*run_result);
}

TEST_CASE(
    "rendering golden: Ctrl+L model selector switches the model with the pi "
    "status",
    "[coding_agent][tui][rendering][issue422]") {
    ModelFixture fixture;
    Running running;

    coding_agent::runtime::AgentSessionCreationRequest request;
    request.session_facts.no_skills = true;
    request.session_facts.no_prompt_templates = true;
    request.workspace = fixture.workspace;
    request.session_target =
        coding_agent::ExplicitOpenOrCreateSessionTarget{fixture.session_file};
    auto created = coding_agent::create_agent_session(std::move(request));
    REQUIRE(created.has_value());
    auto* session = created->session.get();
    REQUIRE(session->model() == "alpha-1");

    boost::asio::co_spawn(
        running.io,
        coding_agent::tui::run_interactive_mode(
            running.terminal,
            make_run(*created->session, fixture.agent_dir.path())),
        [&](std::exception_ptr exception, support::ExpectedVoid result) {
            CHECK(exception == nullptr);
            running.run_result.emplace(std::move(result));
        });
    drain_ready(running.io);

    // Ctrl+L opens the selector; Down + Enter selects beta-1 (pi
    // app.model.select).
    REQUIRE(running.terminal.inject_input("\x0c"));
    drain_ready(running.io);
    REQUIRE(running.terminal.inject_input("\x1b[B"));
    drain_ready(running.io);
    REQUIRE(running.terminal.inject_input("\r"));
    drain_ready(running.io);

    const auto screen = visible_screen(running.terminal);
    capture_golden("model-switch.txt", screen);

    const auto expected = read_text_file(golden_path("model-switch.txt"));
    CHECK(screen == expected);

    // The switch landed: the Model status and the footer model.
    CHECK(screen.find("Model: beta-1") != std::string::npos);
    CHECK(session->snapshot().agent_state.model.id == "beta-1");

    REQUIRE(running.terminal.inject_input("\x04"));
    drain_ready(running.io);
    REQUIRE(running.run_result);
    CHECK(*running.run_result);
}

TEST_CASE(
    "rendering golden: the fork flow's user-message selector overlay",
    "[coding_agent][tui][rendering][issue422]") {
    auto workspace = rendering_workspace_path("fork");
    tests::TempWorkspace agent_dir;
    tests::EnvVarGuard dir_guard{"PI_CODING_AGENT_DIR"};
    tests::EnvVarGuard home_guard{"HOME"};
    tests::EnvVarGuard kimi_guard{"KIMI_API_KEY"};
    dir_guard.set(agent_dir.path().string());
    home_guard.set(workspace.string());
    kimi_guard.unset();
    const auto session_file = workspace / "fork-session.jsonl";
    {
        std::ofstream models(agent_dir.path() / "models.json", std::ios::binary);
        models << R"({
  "providers": {
    "alpha": {
      "baseUrl": "https://alpha.example/v1",
      "api": "openai-responses",
      "apiKey": "dummy-alpha-key",
      "models": [{"id": "alpha-1", "name": "Alpha Reasoning", "reasoning": true}]
    }
  }
})";
        // The `app.session.fork` main-editor action is recognized-but-unbound
        // (pi defaultKeys []); a user-assigned keybinding triggers the flow.
        std::ofstream keybindings(
            agent_dir.path() / "keybindings.json", std::ios::binary);
        keybindings << R"({"app.session.fork":"f7"})";
    }
    auto store = harness::session::SessionStore::create_new(
        session_file,
        {
            .session_id = "fork-session",
            .created_at = "2026-08-10T00:00:00Z",
            .workspace = workspace,
            .provider = "alpha",
            .model = "alpha-1",
        });
    REQUIRE(store);
    for (std::size_t index = 0; index < 3; ++index) {
        auto user = ai::user_text_message("user-" + std::to_string(index));
        user.timestamp = 1'750'000'000'000 + static_cast<ai::TimestampMs>(index * 2);
        REQUIRE(store->append(ai::MessageVariant{user}).has_value());
        if (index + 1 < 3) {
            auto assistant = ai::assistant_text_message(
                "assistant reply " + std::to_string(index));
            assistant.api = "openai-responses";
            assistant.provider = "alpha";
            assistant.model = "alpha-1";
            assistant.timestamp =
                1'750'000'000'001 + static_cast<ai::TimestampMs>(index * 2);
            REQUIRE(store->append(ai::MessageVariant{assistant}).has_value());
        }
    }

    // 100 columns (the selector-test convention): the F7 function-key
    // dispatch needs the wider terminal (a pre-existing cch_tui input
    // behavior below ~90 columns), and the overlay renders fully.
    Running running{{.columns = 100, .rows = 24}};

    coding_agent::runtime::AgentSessionCreationRequest request;
    request.session_facts.no_skills = true;
    request.session_facts.no_prompt_templates = true;
    request.workspace = workspace;
    request.session_target =
        coding_agent::ExplicitOpenOrCreateSessionTarget{session_file};
    auto created = coding_agent::create_agent_session(std::move(request));
    REQUIRE(created.has_value());
    REQUIRE(created->session->message_count() == 5);

    coding_agent::tui::testing::ActionSinkRecorder recorder;
    recorder.replace_session =
        [](coding_agent::runtime::AgentSessionCreationRequest request)
        -> support::Expected<coding_agent::CreateAgentSessionResult> {
            request.session_facts.no_skills = true;
            request.session_facts.no_prompt_templates = true;
            return coding_agent::create_agent_session(std::move(request));
        };

    boost::asio::co_spawn(
        running.io,
        coding_agent::tui::run_interactive_mode(
            running.terminal,
            make_run(*created->session, agent_dir.path(), recorder.make_sink())),
        [&](std::exception_ptr exception, support::ExpectedVoid result) {
            CHECK(exception == nullptr);
            running.run_result.emplace(std::move(result));
        });
    drain_ready(running.io);

    // f7 opens the user-message selector with the last message preselected
    // (pi showUserMessageSelector).
    REQUIRE(running.terminal.inject_input("\x1b[18~"));
    drain_ready(running.io);

    const auto screen = visible_screen(running.terminal);
    capture_golden("fork.txt", screen);

    const auto expected = read_text_file(golden_path("fork.txt"));
    CHECK(screen == expected);

    CHECK(screen.find("Fork from Message") != std::string::npos);
    CHECK(screen.find("Message 3 of 3") != std::string::npos);

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
    "rendering golden: app.interrupt aborts the active run and renders the "
    "aborted entry",
    "[coding_agent][tui][rendering][issue422]") {
    tests::ScriptedRuntimeFixture gated;
    gated.control->gate_at = 0;
    gated.control->emit_partial_before_gate = true;
    auto fixture = make_interrupt_session(std::move(gated));

    tui::VirtualTerminal terminal({.columns = 72, .rows = 24});
    boost::asio::io_context io;
    std::optional<support::ExpectedVoid> run_result;
    boost::asio::co_spawn(
        io,
        coding_agent::tui::run_interactive_mode(
            terminal,
            make_run(*fixture->session, fixture->config.path())),
        [&](std::exception_ptr exception, support::ExpectedVoid result) {
            CHECK(exception == nullptr);
            run_result.emplace(std::move(result));
        });
    drain_ready(io);

    // A focused-editor submission starts the run; Esc aborts it while the
    // stream is active (pi's app.interrupt precedence).
    REQUIRE(terminal.inject_input("start the run\r"));
    drain_ready(io);
    REQUIRE(fixture->scripted.control->calls.size() == 1);
    REQUIRE(terminal.inject_input("\x1b"));
    REQUIRE(terminal.inject_input(""));
    drain_ready(io);

    const auto screen = visible_screen(terminal);
    capture_golden("interrupt.txt", screen);

    const auto expected = read_text_file(golden_path("interrupt.txt"));
    CHECK(screen == expected);

    CHECK(screen.find("Operation aborted") != std::string::npos);

    REQUIRE(terminal.inject_input("\x04"));
    drain_ready(io);
    REQUIRE(run_result);
    CHECK(*run_result);
}
