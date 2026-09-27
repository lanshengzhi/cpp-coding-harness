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
#include "coding_agent/tui/Theme.hpp"
#include "coding_agent/tui/InteractiveMode.hpp"
#include "coding_agent/tui/InteractiveSessionRun.hpp"
#include "coding_agent/tui/TestTuiActionSink.hpp"
#include "support/EnvVarGuard.hpp"
#include "support/Json.hpp"
#include "support/PumpUntil.hpp"
#include "support/RuntimeFixture.hpp"
#include "support/RuntimeLoopDriver.hpp"
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
#include "support/AgentRootFixture.hpp"

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
    tests::RuntimeFixture runtime;
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
    resume.execution_runtime_target = fixture->runtime.make_target();
    resume.workspace = fixture->workspace;
    resume.session_facts.no_skills = true;
    resume.session_facts.no_prompt_templates = true;
    auto created = fixture->runtime.run(coding_agent::create_agent_session_async(
            std::move(resume), std::nullopt, cch::tests::cli_fake_overrides(tests::make_scripted_fake_models())));
    REQUIRE(created);
    fixture->session = std::move(created->session);
    return fixture;
}

// ── tool-blocks: the four renderers and the fallback in one session view ────

/// The `write` arguments as one JSON document. Built through the serializer
/// rather than by hand so a content body carrying real newlines still parses:
/// the host's argument parse is exact, and a hand-escaped literal that forgets
/// one escape silently degrades to the empty-argument case.
[[nodiscard]] std::string write_arguments_json(std::string content) {
    support::JsonValue arguments{support::JsonValue::object_t{}};
    arguments.get_object().emplace("path", support::JsonValue(std::string{"generated.txt"}));
    arguments.get_object().emplace("content", support::JsonValue(std::move(content)));
    auto json = support::write_json(arguments);
    REQUIRE(json);
    return *json;
}

[[nodiscard]] std::string prefixed_lines(std::string_view prefix, std::size_t first, std::size_t last) {
    std::string text;
    for (auto line = first; line <= last; ++line) {
        if (!text.empty()) text.push_back('\n');
        text += std::string{prefix} + " " + std::to_string(line);
    }
    return text;
}

/// The foreground colour the terminal recorded for the first cell of `text`.
///
/// The rendering goldens are plain cell text, so a wrong colour that keeps the
/// same rows and the same text is invisible to every byte comparison in this
/// file. This reads the styled cell instead, which is the only place a
/// mis-coloured renderer can be caught at the screen level.
[[nodiscard]] std::string foreground_at(
        const tui::VirtualTerminal& terminal, std::string_view text, std::size_t offset = 0) {
    const auto& screen = terminal.screen();
    for (std::size_t row = 0; row < screen.size(); ++row) {
        const auto column = screen[row].find(text);
        if (column != std::string::npos) {
            REQUIRE(column + offset < terminal.cells()[row].size());
            return terminal.cells()[row][column + offset].style.fg_color;
        }
    }
    return {};
}

[[nodiscard]] bool is_bold_at(const tui::VirtualTerminal& terminal, std::string_view text, std::size_t offset = 0) {
    const auto& screen = terminal.screen();
    for (std::size_t row = 0; row < screen.size(); ++row) {
        const auto column = screen[row].find(text);
        if (column != std::string::npos) {
            REQUIRE(column + offset < terminal.cells()[row].size());
            return terminal.cells()[row][column + offset].style.bold;
        }
    }
    return false;
}

/// The leading SGR parameter list of a themed probe, i.e. the colour the
/// VirtualTerminal records for that token's foreground.
[[nodiscard]] std::string token_foreground(
        const coding_agent::tui::LiveTheme& theme, coding_agent::tui::ThemeToken token) {
    const auto styled = theme.foreground(token, "x");
    const auto begin = styled.find("\x1b[");
    REQUIRE(begin != std::string::npos);
    const auto end = styled.find('m', begin);
    REQUIRE(end != std::string::npos);
    return styled.substr(begin + 2, end - begin - 2);
}

/// A resumed session whose one assistant turn drives five tool components: a
/// collapsed `read`, a folded `bash`, a `write` whose preview is folded, an
/// `edit` carrying `details.diff`, and one tool with no registered renderer, so
/// the registry fallback's own framing is in the same view.
struct ToolBlocksSession {
    std::filesystem::path workspace;
    tests::TempWorkspace config;
    tests::RuntimeFixture runtime;
    std::unique_ptr<coding_agent::AgentSession> session;
};

[[nodiscard]] std::unique_ptr<ToolBlocksSession> make_tool_blocks_session() {
    auto fixture = std::make_unique<ToolBlocksSession>();
    fixture->workspace = rendering_workspace_path("tool-blocks");
    const auto session_file = fixture->workspace / "tool-blocks.jsonl";
    auto store = harness::session::SessionStore::create_new(session_file,
            {
                    .session_id = "tool-blocks",
                    .created_at = "2026-08-10T00:00:00Z",
                    .workspace = fixture->workspace,
                    .provider = "fake",
                    .model = "fake-model",
            });
    REQUIRE(store);

    REQUIRE(store->append(
            ai::MessageVariant{ai::user_text_message("render every tool block in one view", 1'700'000'000'000)}));

    auto content = prefixed_lines("gen", 1, 14) + "\n";

    ai::AssistantMessage assistant;
    assistant.provider = "fake";
    assistant.api = "fake";
    assistant.model = "fake-model";
    assistant.stop_reason = ai::AssistantStopReason::ToolUse;
    assistant.timestamp = 1'700'000'000'001;
    assistant.content.emplace_back(ai::text_content("One turn, five tool blocks."));
    // `offset`/`limit` on the read so the golden carries pi's line range, and
    // an unregistered `probe` so the fallback is in the same screen.
    assistant.content.emplace_back(
            ai::tool_call_content("block-read", "read", R"({"path":"notes.txt","offset":5,"limit":3})"));
    assistant.content.emplace_back(ai::tool_call_content("block-bash", "bash", R"({"command":"ls -la"})"));
    assistant.content.emplace_back(ai::tool_call_content("block-write", "write", write_arguments_json(content)));
    assistant.content.emplace_back(ai::tool_call_content(
            "block-edit", "edit", R"({"path":"notes.txt","edits":[{"oldText":"alpha","newText":"beta"}]})"));
    assistant.content.emplace_back(ai::tool_call_content("block-probe", "probe", R"({"query":"alpha","limit":2})"));
    REQUIRE(store->append(ai::MessageVariant{assistant}));

    // A collapsed successful read result renders nothing: pi `read.ts:111-115`.
    REQUIRE(store->append(ai::MessageVariant{ai::ToolResultMessage{
            .tool_call_id = "block-read",
            .tool_name = "read",
            .content = {ai::text_content("read body that stays hidden")},
            .details = std::nullopt,
            .is_error = false,
            .timestamp = 1'700'000'000'002,
    }}));

    auto details_with_diff = support::JsonValue{support::JsonValue::object_t{}};
    details_with_diff.get_object().emplace("diff", support::JsonValue(std::string{"-1 alpha\n+1 beta"}));
    REQUIRE(store->append(ai::MessageVariant{ai::ToolResultMessage{
            .tool_call_id = "block-edit",
            .tool_name = "edit",
            .content = {ai::text_content("Successfully replaced 1 block(s) in notes.txt.")},
            .details = std::move(details_with_diff),
            .is_error = false,
            .timestamp = 1'700'000'000'003,
    }}));

    REQUIRE(store->append(ai::MessageVariant{
            ai::tool_result_message("block-bash", "bash", prefixed_lines("bash", 1, 12), false, 1'700'000'000'004)}));
    REQUIRE(store->append(ai::MessageVariant{
            ai::tool_result_message("block-write", "write", "wrote 14 lines", false, 1'700'000'000'005)}));
    REQUIRE(store->append(ai::MessageVariant{
            ai::tool_result_message("block-probe", "probe", "probe answered in one line", false, 1'700'000'000'006)}));

    coding_agent::runtime::AgentSessionCreationRequest resume;
    resume.session_target = coding_agent::ExplicitResumeSessionTarget{session_file};
    resume.execution_runtime_target = fixture->runtime.make_target();
    resume.workspace = fixture->workspace;
    resume.session_facts.no_skills = true;
    resume.session_facts.no_prompt_templates = true;
    auto created = fixture->runtime.run(coding_agent::create_agent_session_async(
            std::move(resume), std::nullopt, cch::tests::cli_fake_overrides(tests::make_scripted_fake_models())));
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
    std::filesystem::path agent_dir;
    tests::RuntimeFixture runtime;
    tests::EnvVarGuard home_guard{"HOME"};
    tests::EnvVarGuard kimi_guard{"KIMI_API_KEY"};
    std::filesystem::path session_file;

    ModelFixture() {
        workspace = rendering_workspace_path("model-switch");
        home_guard.set(workspace.string());
        agent_dir = tests::agent_root_under_home(workspace);
        std::filesystem::create_directories(agent_dir);
        kimi_guard.unset();
        session_file = workspace / "model-switch.jsonl";
        std::ofstream models(agent_dir / "models.json", std::ios::binary);
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
    tests::RuntimeFixture runtime;
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
    request.execution_runtime_target = fixture->runtime.make_target();
    request.workspace = fixture->workspace;
    request.session_facts.no_skills = true;
    request.session_facts.no_prompt_templates = true;
    request.model_runtime = fixture->scripted.runtime;
    auto created = fixture->runtime.run(coding_agent::create_agent_session_async(std::move(request), std::nullopt, {}));
    REQUIRE(created);
    fixture->session = std::move(created->session);
    return fixture;
}

} // namespace

TEST_CASE("rendering golden: the full message pipeline renders in pi's shapes",
        "[coding_agent][tui][rendering][issue422][compat-pi]") {
    auto fixture = make_pipeline_session();
    tests::RuntimeLoopDriver runtime_driver(fixture->runtime);

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
    // pi `read.ts:111-115` returns the empty string for a collapsed
    // successful read result, so the pipeline's read block is its title line
    // and the result's own text is nowhere on the screen. This assertion
    // encoded the pre-renderer fallback framing; it is not weakened, it is
    // stated the other way round, and the byte comparison above plus the
    // `read saved.txt` title still pin the tool-execution component.
    CHECK(screen.find("persisted tool output") == std::string::npos);
    CHECK(screen.find(R"({"path":"saved.txt"})") == std::string::npos);
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

TEST_CASE("rendering golden: the four tool renderers and the fallback compose in one "
          "session view",
        "[coding_agent][tui][rendering][issue422][tool-renderers][issue828][compat-pi]") {
    auto fixture = make_tool_blocks_session();
    tests::RuntimeLoopDriver runtime_driver(fixture->runtime);

    // 64 rows hold the whole turn — five tool blocks, the user message and the
    // assistant text — above the status/editor/footer rows, so the golden is a
    // readable composition rather than a scrolled tail.
    tui::VirtualTerminal terminal({.columns = 80, .rows = 64});
    boost::asio::io_context io;
    std::optional<support::ExpectedVoid> run_result;
    boost::asio::co_spawn(io,
            coding_agent::tui::run_interactive_mode(terminal, make_run(*fixture->session, fixture->config.path())),
            [&](std::exception_ptr exception, support::ExpectedVoid result) {
                CHECK(exception == nullptr);
                run_result.emplace(std::move(result));
            });
    drain_ready(io);

    const auto screen = visible_screen(terminal);
    capture_golden("tool-blocks.txt", screen);

    const auto expected = read_text_file(golden_path("tool-blocks.txt"));
    CHECK(screen == expected);

    // ── what the golden cannot express: the styling ──────────────────────
    // The golden is plain cell text, so a renderer that emitted the right
    // rows in the right colours would still produce a byte-identical file.
    // A wrong token on any one of these four rows is invisible to the
    // comparison above and visible only here. This is the gap the byte
    // comparison leaves open, named and closed.
    const coding_agent::tui::LiveTheme theme(
            coding_agent::tui::select_builtin_theme(terminal.capabilities()), terminal.capabilities().color);
    const auto tool_title = token_foreground(theme, coding_agent::tui::ThemeToken::ToolTitle);
    const auto tool_output = token_foreground(theme, coding_agent::tui::ThemeToken::ToolOutput);
    const auto muted = token_foreground(theme, coding_agent::tui::ThemeToken::Muted);
    const auto removed = token_foreground(theme, coding_agent::tui::ThemeToken::ToolDiffRemoved);
    const auto added = token_foreground(theme, coding_agent::tui::ThemeToken::ToolDiffAdded);

    // 1. The read title: pi's bold `toolTitle` name. A plain-text title, or a
    //    title in the `toolOutput` grey the bodies use, fails here.
    CHECK(foreground_at(terminal, "read notes.txt:5-7") == tool_title);
    CHECK(is_bold_at(terminal, "read notes.txt:5-7"));
    // 2. The bash body: `toolOutput`, and the fold hint's count clause is
    //    `muted` with a `dim` key inside it.
    CHECK(foreground_at(terminal, "bash 8") == tool_output);
    CHECK(foreground_at(terminal, "... (7 earlier lines, ") == muted);
    // 3. The write preview body: `toolOutput` as well, from the arguments
    //    rather than from any settled result.
    CHECK(foreground_at(terminal, "gen 1") == tool_output);
    CHECK(foreground_at(terminal, "... (4 more lines, ") == muted);
    // 4. The edit diff: two distinct tokens, one per row, and neither row
    //    carrying the other's. One colour for the whole diff renders the same
    //    two rows of text and is what the golden cannot see.
    CHECK(foreground_at(terminal, "alpha") == removed);
    CHECK(foreground_at(terminal, "beta") == added);
    CHECK(foreground_at(terminal, "alpha") != added);
    CHECK(foreground_at(terminal, "beta") != removed);
    CHECK(foreground_at(terminal, "alpha") != tool_output);
    CHECK(foreground_at(terminal, "beta") != tool_output);
    // The tokens are genuinely distinguishable in this theme, so the four
    // checks above discriminate rather than comparing one colour to itself.
    CHECK(tool_title != tool_output);
    CHECK(removed != added);

    // ── what the golden does express: the five blocks compose ────────────
    // The whole (padded) screen row carrying `text`, or the empty string.
    // Rows are byte-compared by the golden above; these assertions say which
    // block each visible line belongs to, so a per-renderer case that only ever
    // ran in isolation cannot hide a composition regression.
    const auto row_of = [&screen](std::string_view text) -> std::string {
        const auto at = screen.find(text);
        if (at == std::string::npos) return {};
        const auto previous_break = screen.rfind('\n', at);
        const auto next_break = screen.find('\n', at);
        const auto begin = previous_break == std::string::npos ? 0 : previous_break + 1;
        const auto end = next_break == std::string::npos ? screen.size() : next_break;
        return screen.substr(begin, end - begin);
    };
    const auto read_row = row_of("read notes.txt:5-7");
    const auto bash_row = row_of("$ ls -la");
    const auto write_row = row_of("write generated.txt");
    const auto edit_row = row_of("edit notes.txt");
    const auto probe_row = row_of("probe");
    REQUIRE(!read_row.empty());
    REQUIRE(!bash_row.empty());
    REQUIRE(!write_row.empty());
    REQUIRE(!edit_row.empty());
    REQUIRE(!probe_row.empty());
    // The read title is the whole collapsed block: the body's own text is on
    // the screen nowhere, not merely off the rows this case looks at.
    CHECK(screen.find("read body that stays hidden") == std::string::npos);
    // The bash fold keeps the tail and drops the head, and the two folds in
    // this view (`bash` and `write`) do not interfere.
    CHECK(row_of("bash 8").find("bash 8") != std::string::npos);
    CHECK(screen.find("bash 1 ") == std::string::npos);
    CHECK(screen.find("bash 7\n") == std::string::npos);
    CHECK(screen.find("gen 11") == std::string::npos);
    CHECK(screen.find("gen 14") == std::string::npos);
    // The write result text never reaches the screen: pi's write success
    // renders nothing, and the content is on screen exactly once.
    CHECK(screen.find("wrote 14 lines") == std::string::npos);
    CHECK(screen.find("Successfully replaced") == std::string::npos);
    // The fallback's own framing is in the same view: the bold name, the
    // blank line, and the two-space-indented argument JSON. None of the four
    // registered renderers prints an argument dump.
    CHECK(!row_of("  \"query\": \"alpha\"").empty());
    CHECK(!row_of("  \"limit\": 2").empty());
    CHECK(screen.find(R"({"query":"alpha","limit":2})") == std::string::npos);
    CHECK(!row_of("probe answered in one line").empty());
    // Source order: pi renders the assistant message and then the tool
    // components in call order.
    CHECK(screen.find(read_row) < screen.find(bash_row));
    CHECK(screen.find(bash_row) < screen.find(write_row));
    CHECK(screen.find(write_row) < screen.find(edit_row));
    CHECK(screen.find(edit_row) < screen.find(probe_row));

    REQUIRE(terminal.inject_input("\x04"));
    drain_ready(io);
    REQUIRE(run_result);
    CHECK(*run_result);
}

TEST_CASE("rendering golden: Ctrl+L model selector switches the model with the pi "
          "status",
        "[coding_agent][tui][rendering][issue422][compat-pi]") {
    ModelFixture fixture;
    Running running;

    coding_agent::runtime::AgentSessionCreationRequest request;
    request.session_facts.no_skills = true;
    request.session_facts.no_prompt_templates = true;
    request.execution_runtime_target = fixture.runtime.make_target();
    request.workspace = fixture.workspace;
    request.session_target =
        coding_agent::ExplicitOpenOrCreateSessionTarget{fixture.session_file};
    auto created = fixture.runtime.run(coding_agent::create_agent_session_async(std::move(request), std::nullopt, {}));
    REQUIRE(created.has_value());
    auto* session = created->session.get();
    REQUIRE(session->model() == "alpha-1");
    tests::RuntimeLoopDriver runtime_driver(fixture.runtime);

    boost::asio::co_spawn(running.io,
            coding_agent::tui::run_interactive_mode(running.terminal, make_run(*created->session, fixture.agent_dir)),
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
    // The selection crosses runtime worker hops and two render posts; wait
    // on the painted outcome itself (never a bare drain or bare state
    // poll) before capturing the settled screen.
    REQUIRE(tests::pump_until(running.io, [&] {
        const auto settled = visible_screen(running.terminal);
        return settled.find("Model: beta-1") != std::string::npos &&
               settled.find("Only showing models") == std::string::npos;
    }));
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

TEST_CASE("rendering golden: the fork flow's user-message selector overlay",
        "[coding_agent][tui][rendering][issue422][compat-pi]") {
    auto workspace = rendering_workspace_path("fork");
    tests::EnvVarGuard home_guard{"HOME"};
    tests::EnvVarGuard kimi_guard{"KIMI_API_KEY"};
    home_guard.set(workspace.string());
    const auto agent_dir = tests::agent_root_under_home(workspace);
    std::filesystem::create_directories(agent_dir);
    kimi_guard.unset();
    const auto session_file = workspace / "fork-session.jsonl";
    {
        std::ofstream models(agent_dir / "models.json", std::ios::binary);
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
        std::ofstream keybindings(agent_dir / "keybindings.json", std::ios::binary);
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
    tests::RuntimeFixture runtime;

    coding_agent::runtime::AgentSessionCreationRequest request;
    request.session_facts.no_skills = true;
    request.session_facts.no_prompt_templates = true;
    request.workspace = workspace;
    request.execution_runtime_target = runtime.make_target();
    request.session_target =
        coding_agent::ExplicitOpenOrCreateSessionTarget{session_file};
    auto created = runtime.run(coding_agent::create_agent_session_async(std::move(request), std::nullopt, {}));
    REQUIRE(created.has_value());
    REQUIRE(created->session->message_count() == 5);
    tests::RuntimeLoopDriver runtime_driver(runtime);

    coding_agent::tui::testing::ActionSinkRecorder recorder;

    boost::asio::co_spawn(running.io,
            coding_agent::tui::run_interactive_mode(
                    running.terminal, make_run(*created->session, agent_dir, recorder.make_sink())),
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

TEST_CASE("rendering golden: app.interrupt aborts the active run and renders the "
          "aborted entry",
        "[coding_agent][tui][rendering][issue422][compat-pi]") {
    tests::ScriptedRuntimeFixture gated;
    gated.control->gate_at = 0;
    gated.control->emit_partial_before_gate = true;
    auto fixture = make_interrupt_session(std::move(gated));
    tests::RuntimeLoopDriver runtime_driver(fixture->runtime);

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
    // The interrupt crosses runtime worker hops and clears the spinner; wait
    // on the painted outcome itself before capturing the settled screen.
    REQUIRE(tests::pump_until(io, [&] {
        const auto s = visible_screen(terminal);
        return s.find("Operation aborted") != std::string::npos && s.find("Working") == std::string::npos;
    }));

    const auto screen = visible_screen(terminal);
    capture_golden("interrupt.txt", screen);

    const auto expected = read_text_file(golden_path("interrupt.txt"));
    CHECK(screen == expected);

    CHECK(screen.find("Operation aborted") != std::string::npos);

    REQUIRE(terminal.inject_input("\x04"));
    REQUIRE(tests::pump_until(io, [&] { return run_result.has_value(); }));
    CHECK(*run_result);
}
