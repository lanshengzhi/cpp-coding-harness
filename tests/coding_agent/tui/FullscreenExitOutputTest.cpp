// Tests for issue #1019: frozen fullscreen exit output policy after
// alternate-buffer restoration (group E20). Aligned against frozen arbiter
// pi-v1.0.4 (`7c10bd4337495ee613f2224843ecdf349b80d1df`) and evidence artifact 33.

#include "coding_agent/tui/InteractiveEngine.hpp"
#include "coding_agent/tui/InteractiveMode.hpp"
#include "coding_agent/tui/InteractiveSessionRun.hpp"
#include "coding_agent/AgentSession.hpp"
#include "coding_agent/SessionTarget.hpp"
#include "coding_agent/SessionPathPolicy.hpp"
#include "coding_agent/runtime/SessionFactory.hpp"
#include <cch/coding_agent/AgentConfigDir.hpp>
#include "support/EnvVarGuard.hpp"
#include "support/ModelsFixture.hpp"
#include "support/PiTuiEvidence.hpp"
#include "support/PumpUntil.hpp"
#include "support/RuntimeFixture.hpp"
#include "support/RuntimeLoopDriver.hpp"
#include "support/ScriptedProvider.hpp"
#include "support/TempWorkspace.hpp"

#include <cch/ai/Content.hpp>
#include <cch/coding_agent/ModelRuntime.hpp>
#include <cch/coding_agent/Settings.hpp>
#include "coding_agent/tui/ChatViewport.hpp"
#include <cch/tui/AltScreen.hpp>
#include <cch/tui/Container.hpp>
#include <cch/tui/Editor.hpp>
#include <cch/tui/Overlay.hpp>
#include <cch/tui/ProcessTerminal.hpp>
#include <cch/tui/Text.hpp>
#include <cch/tui/Tui.hpp>
#include <cch/tui/VirtualTerminal.hpp>
#include "support/PseudoTerminal.hpp"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <iostream>

#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace cch;
using tests::drain_ready;
using tests::pump_until;

namespace {

[[nodiscard]] std::string visible_screen(const tui::VirtualTerminal& terminal) {
    const auto lines = terminal.screen();
    std::string text;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        text.append(lines[i]);
        if (i + 1 < lines.size()) text.push_back('\n');
    }
    return text;
}

[[nodiscard]] std::string scrollback_text(const tui::VirtualTerminal& terminal) {
    const auto lines = terminal.scrollback();
    std::string text;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        text.append(lines[i]);
        if (i + 1 < lines.size()) text.push_back('\n');
    }
    return text;
}

[[nodiscard]] tests::ModelsSessionOptions session_options(
        const tests::TempWorkspace& workspace, std::shared_ptr<tests::ScriptedProvider> client) {
    tests::ModelsSessionOptions options;
    options.session_target = coding_agent::InMemorySessionTarget{};
    options.workspace = workspace.path();
    options.models = cch::tests::models_from_provider(std::move(client));
    return options;
}

[[nodiscard]] support::AsyncResult<coding_agent::CreateAgentSessionResult> create_session_async(
        tests::RuntimeFixture& runtime, tests::ModelsSessionOptions options) {
    auto models = std::move(options.models);
    coding_agent::runtime::AgentSessionCreationRequest request = std::move(options);
    request.execution_runtime_target = runtime.make_target();
    return coding_agent::create_agent_session_async(
            std::move(request), std::nullopt, cch::tests::cli_fake_overrides(std::move(models)));
}

[[nodiscard]] boost::asio::awaitable<support::ExpectedVoid> drive_engine(
        std::shared_ptr<coding_agent::tui::InteractiveEngine> engine, coding_agent::tui::InteractiveSessionRun run) {
    if (auto started = engine->start(std::move(run)); !started) co_return std::unexpected(started.error());
    if (auto warning = co_await engine->append_project_trust_warning_if_needed(); !warning) {
        auto finished = co_await engine->finish();
        if (!finished) co_return std::unexpected(finished.error());
        co_return std::unexpected(warning.error());
    }
    boost::system::error_code wait_error;
    if (!engine->exit_requested()) {
        co_await engine->exit_wait().async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, wait_error));
    }
    co_return co_await engine->finish();
}

class CallCountingTerminal final : public cch::tui::Terminal {
public:
    explicit CallCountingTerminal(cch::tui::VirtualTerminalOptions options) : inner_(std::move(options)) {}

    cch::support::ExpectedVoid start(
            cch::tui::TerminalInputSink input_sink, cch::tui::TerminalResizeSink resize_sink) override {
        ++start_calls;
        return inner_.start(std::move(input_sink), std::move(resize_sink));
    }

    cch::support::ExpectedVoid stop() override {
        ++stop_calls;
        return inner_.stop();
    }

    cch::tui::TerminalDimensions dimensions() const override { return inner_.dimensions(); }
    cch::tui::TerminalCapabilities capabilities() const override { return inner_.capabilities(); }
    cch::tui::TerminalModeState modes() const override { return inner_.modes(); }
    cch::support::ExpectedVoid clear_screen() override { return inner_.clear_screen(); }
    cch::support::ExpectedVoid write(std::string_view output) override { return inner_.write(output); }
    cch::support::ExpectedVoid set_cursor(cch::tui::CursorPosition position) override {
        return inner_.set_cursor(position);
    }
    cch::support::ExpectedVoid set_cursor_visible(bool visible) override { return inner_.set_cursor_visible(visible); }
    cch::support::ExpectedVoid set_alternate_screen(bool enabled) override {
        return inner_.set_alternate_screen(enabled);
    }
    cch::support::ExpectedVoid set_autowrap(bool enabled) override { return inner_.set_autowrap(enabled); }
    cch::support::ExpectedVoid set_scroll_margins(std::size_t top_row, std::size_t bottom_row) override {
        return inner_.set_scroll_margins(top_row, bottom_row);
    }
    cch::support::ExpectedVoid reset_scroll_margins() override { return inner_.reset_scroll_margins(); }
    cch::support::ExpectedVoid begin_synchronized_update() override { return inner_.begin_synchronized_update(); }
    cch::support::ExpectedVoid end_synchronized_update() override { return inner_.end_synchronized_update(); }
    cch::support::ExpectedVoid set_title(std::string_view title) override { return inner_.set_title(title); }
    cch::support::ExpectedVoid set_progress(bool active) override { return inner_.set_progress(active); }
    cch::support::ExpectedVoid drain_input(
            std::chrono::milliseconds max_ms, std::chrono::milliseconds idle_ms) override {
        return inner_.drain_input(max_ms, idle_ms);
    }
    cch::support::ExpectedVoid enter_output_only() override {
        ++enter_output_only_calls;
        return inner_.enter_output_only();
    }
    cch::support::ExpectedVoid leave_output_only() override {
        ++leave_output_only_calls;
        return inner_.leave_output_only();
    }

    cch::tui::VirtualTerminal& inner() { return inner_; }
    const cch::tui::VirtualTerminal& inner() const { return inner_; }

    std::size_t start_calls{0};
    std::size_t stop_calls{0};
    std::size_t enter_output_only_calls{0};
    std::size_t leave_output_only_calls{0};

private:
    cch::tui::VirtualTerminal inner_;
};

} // namespace

// ── 1. SettingsManager Tests ─────────────────────────────────────────────────

TEST_CASE(
        "SettingsManager parses and persists fullscreenExitOutput", "[coding_agent][settings][exit][issue1019][spec]") {
    tests::TempWorkspace temp;
    tests::EnvVarGuard agent_dir_guard{"PIKE_AGENT_DIR", temp.path().string()};

    SECTION("defaults to Transcript when unset") {
        auto manager = coding_agent::SettingsManager::create(temp.path(), temp.path());
        CHECK(manager.get_fullscreen_exit_output() == coding_agent::FullscreenExitOutput::Transcript);
    }

    SECTION("parses resume-hint from settings") {
        const auto settings_path = temp.path() / "settings.json";
        std::ofstream(settings_path) << R"({"fullscreenExitOutput": "resume-hint"})";
        auto manager = coding_agent::SettingsManager::create(temp.path(), temp.path());
        CHECK(manager.get_fullscreen_exit_output() == coding_agent::FullscreenExitOutput::ResumeHint);
    }

    SECTION("parses transcript from settings") {
        const auto settings_path = temp.path() / "settings.json";
        std::ofstream(settings_path) << R"({"fullscreenExitOutput": "transcript"})";
        auto manager = coding_agent::SettingsManager::create(temp.path(), temp.path());
        CHECK(manager.get_fullscreen_exit_output() == coding_agent::FullscreenExitOutput::Transcript);
    }

    SECTION("persists set_fullscreen_exit_output to disk and reloads") {
        {
            auto manager = coding_agent::SettingsManager::create(temp.path(), temp.path());
            REQUIRE(manager.set_fullscreen_exit_output(coding_agent::FullscreenExitOutput::ResumeHint).has_value());
            CHECK(manager.get_fullscreen_exit_output() == coding_agent::FullscreenExitOutput::ResumeHint);
        }
        {
            auto reloaded = coding_agent::SettingsManager::create(temp.path(), temp.path());
            CHECK(reloaded.get_fullscreen_exit_output() == coding_agent::FullscreenExitOutput::ResumeHint);
        }
    }
}

// ── 2. format_resume_command Matrix ──────────────────────────────────────────

TEST_CASE("Native TUI format_resume_command characterizes the frozen resume command matrix",
        "[coding_agent][tui][exit][resume_command][issue1019][spec]") {
    tests::TempWorkspace temp;
    const auto workspace = temp.path() / "workspace";
    std::filesystem::create_directories(workspace);

    const auto sessions_root = coding_agent::sessions_root_path();
    const auto default_dir = sessions_root / coding_agent::session_paths::encode_workspace_key(workspace);
    std::filesystem::create_directories(default_dir);

    const std::string session_id = "0197f6e4-4cf9-7f44-a2d8-f8f7f49ee9d3";
    const auto default_session_file = default_dir / (session_id + ".json");
    std::ofstream(default_session_file) << "{}\n";

    SECTION("default session dir produces pike --session <id>") {
        auto cmd = coding_agent::tui::format_resume_command(
                session_id, default_session_file, workspace, /*stdout_is_tty=*/true, "pike");
        REQUIRE(cmd.has_value());
        CHECK(*cmd == "pike --session " + session_id);
    }

    SECTION("default session dir with custom app name produces pi --session <id>") {
        auto cmd = coding_agent::tui::format_resume_command(
                session_id, default_session_file, workspace, /*stdout_is_tty=*/true, "pi");
        REQUIRE(cmd.has_value());
        CHECK(*cmd == "pi --session " + session_id);
    }

    SECTION("custom session dir without spaces") {
        const auto custom_dir = temp.path() / "custom_sessions";
        std::filesystem::create_directories(custom_dir);
        const auto custom_session_file = custom_dir / "custom-id-1.json";
        std::ofstream(custom_session_file) << "{}\n";

        auto cmd = coding_agent::tui::format_resume_command(
                "custom-id-1", custom_session_file, workspace, /*stdout_is_tty=*/true, "pike");
        REQUIRE(cmd.has_value());
        CHECK(*cmd == "pike --session-dir " + custom_dir.string() + " --session custom-id-1");
    }

    SECTION("custom session dir with spaces quotes the path") {
        const auto custom_dir = temp.path() / "my custom sessions dir";
        std::filesystem::create_directories(custom_dir);
        const auto custom_session_file = custom_dir / "spaces-id.json";
        std::ofstream(custom_session_file) << "{}\n";

        auto cmd = coding_agent::tui::format_resume_command(
                "spaces-id", custom_session_file, workspace, /*stdout_is_tty=*/true, "pike");
        REQUIRE(cmd.has_value());
        CHECK(*cmd == "pike --session-dir '" + custom_dir.string() + "' --session spaces-id");
    }

    SECTION("custom session dir with single quotes escapes them") {
        const auto custom_dir = temp.path() / "user's sessions";
        std::filesystem::create_directories(custom_dir);
        const auto custom_session_file = custom_dir / "quotes-id.json";
        std::ofstream(custom_session_file) << "{}\n";

        auto cmd = coding_agent::tui::format_resume_command(
                "quotes-id", custom_session_file, workspace, /*stdout_is_tty=*/true, "pike");
        REQUIRE(cmd.has_value());
        std::string expected_escaped = custom_dir.string();
        std::string replaced;
        for (char c : expected_escaped) {
            if (c == '\'')
                replaced += "'\\''";
            else
                replaced += c;
        }
        CHECK(*cmd == "pike --session-dir '" + replaced + "' --session quotes-id");
    }

    SECTION("custom session dir with shell metacharacters is strictly quoted") {
        // Frozen interactive-mode.ts:332: only /^[a-zA-Z0-9_\-./~:@]+$/ is unquoted.
        // Paths containing $, ;, (), backticks, wildcard, backslash, etc. MUST be quoted.
        const std::vector<std::string> metachar_dir_names = {"sessions;echo",
                "sessions$var",
                "sessions(1)",
                "sessions`id`",
                "sessions*star",
                "sessions&and",
                "sessions|pipe"};
        for (const auto& dir_name : metachar_dir_names) {
            const auto custom_dir = temp.path() / dir_name;
            std::filesystem::create_directories(custom_dir);
            const auto custom_session_file = custom_dir / "test.json";
            std::ofstream(custom_session_file) << "{}\n";

            auto cmd = coding_agent::tui::format_resume_command(
                    "meta-id", custom_session_file, workspace, /*stdout_is_tty=*/true, "pike");
            REQUIRE(cmd.has_value());
            CHECK(*cmd == "pike --session-dir '" + custom_dir.string() + "' --session meta-id");
        }
    }

    SECTION("custom session dir with allowed whitelist characters is unquoted") {
        // Whitelist: a-zA-Z0-9 _ - . / ~ : @
        const auto custom_dir = temp.path() / "dir-123_abc.XYZ~:@";
        std::filesystem::create_directories(custom_dir);
        const auto custom_session_file = custom_dir / "test.json";
        std::ofstream(custom_session_file) << "{}\n";

        auto cmd = coding_agent::tui::format_resume_command(
                "whitelist-id", custom_session_file, workspace, /*stdout_is_tty=*/true, "pike");
        REQUIRE(cmd.has_value());
        CHECK(*cmd == "pike --session-dir " + custom_dir.string() + " --session whitelist-id");
    }

    SECTION("non-existent session file returns nullopt") {
        const auto missing_file = default_dir / "does_not_exist.json";
        auto cmd = coding_agent::tui::format_resume_command(
                "missing", missing_file, workspace, /*stdout_is_tty=*/true, "pike");
        CHECK_FALSE(cmd.has_value());
    }

    SECTION("unpersisted session (nullopt path) returns nullopt") {
        auto cmd = coding_agent::tui::format_resume_command(
                session_id, std::nullopt, workspace, /*stdout_is_tty=*/true, "pike");
        CHECK_FALSE(cmd.has_value());
    }

    SECTION("non-TTY stdout returns nullopt") {
        auto cmd = coding_agent::tui::format_resume_command(
                session_id, default_session_file, workspace, /*stdout_is_tty=*/false, "pike");
        CHECK_FALSE(cmd.has_value());
    }

    SECTION("prefix constant matches pi chalk.dim ANSI string") {
        CHECK(coding_agent::tui::kResumeNoticePrefix == "\x1b[2mTo resume this session:\x1b[22m ");
    }
}

// ── 3. Fullscreen Exit Output Policy at Native TUI Consumers ──────────────────

TEST_CASE("Native TUI fullscreen exit output policy switches to regular and renders transcript to main buffer",
        "[coding_agent][tui][exit][fullscreen][issue1019][spec]") {
    tests::TempWorkspace temp;
    tests::EnvVarGuard agent_dir_guard{"PIKE_AGENT_DIR", temp.path().string()};
    tests::RuntimeFixture runtime;
    boost::asio::io_context io;

    tui::VirtualTerminal terminal({.columns = 80, .rows = 24});
    REQUIRE(terminal.seed_shell_content(
                            {"Existing main buffer line 1", "Existing main buffer line 2"}, {.column = 0, .row = 2})
                    .has_value());

    auto created =
            runtime.run(create_session_async(runtime, session_options(temp, tests::make_scripted_fake_provider())));
    REQUIRE(created.has_value());
    tests::RuntimeLoopDriver runtime_driver(runtime);
    auto& session = *created->session;

    auto engine = std::make_shared<coding_agent::tui::InteractiveEngine>(
            terminal, io.get_executor(), coding_agent::TuiMode::Fullscreen);

    std::ostringstream out_stream;
    auto run = coding_agent::tui::InteractiveSessionRunBuilder{}
                       .with_session(session)
                       .with_agent_config_directory(temp.path())
                       .with_tui_mode(coding_agent::TuiMode::Fullscreen)
                       .with_output_stream(&out_stream)
                       .with_stdout_is_tty(true)
                       .build();

    std::optional<support::ExpectedVoid> run_result;
    boost::asio::co_spawn(
            io, drive_engine(engine, std::move(run)), [&](std::exception_ptr ex, support::ExpectedVoid result) {
                CHECK(ex == nullptr);
                run_result.emplace(std::move(result));
            });
    drain_ready(io);
    const tests::RunJoinGuard join_run{io, [&] { return run_result.has_value(); }};

    REQUIRE(terminal.modes().alternate_screen);

    // Send user prompt and wait for assistant reply
    REQUIRE(terminal.inject_input("Hello from user prompt\r"));
    REQUIRE(pump_until(
            io, [&] { return visible_screen(terminal).find("fake: Hello from user prompt") != std::string::npos; }));

    // Request clean exit via Ctrl+D on empty prompt
    REQUIRE(terminal.inject_input("\x04"));
    REQUIRE(pump_until(io, [&] { return run_result.has_value(); }));
    REQUIRE(run_result.has_value());
    CHECK(run_result->has_value());

    // Acceptance criteria check:
    // 1. Alternate screen mode was left
    CHECK_FALSE(terminal.modes().alternate_screen);

    // 2. Transcript was transferred to main buffer: main screen or scrollback contains conversation
    const auto scrollback = scrollback_text(terminal);
    const auto screen = visible_screen(terminal);
    const std::string total_output = scrollback + "\n" + screen;

    CHECK(total_output.find("Existing main buffer line 1") != std::string::npos);
    CHECK(total_output.find("Hello from user prompt") != std::string::npos);
    CHECK(total_output.find("fake: Hello from user prompt") != std::string::npos);
}

TEST_CASE("Native TUI fullscreen exit output policy resume-hint preserves main buffer without emitting transcript",
        "[coding_agent][tui][exit][fullscreen][issue1019][spec]") {
    tests::TempWorkspace temp;
    tests::EnvVarGuard agent_dir_guard{"PIKE_AGENT_DIR", temp.path().string()};

    const auto settings_path = temp.path() / "settings.json";
    std::ofstream(settings_path) << R"({"fullscreenExitOutput": "resume-hint"})";

    tests::RuntimeFixture runtime;
    boost::asio::io_context io;

    tui::VirtualTerminal terminal({.columns = 80, .rows = 24});
    REQUIRE(terminal.seed_shell_content(
                            {"Existing main buffer line 1", "Existing main buffer line 2"}, {.column = 0, .row = 2})
                    .has_value());

    auto created =
            runtime.run(create_session_async(runtime, session_options(temp, tests::make_scripted_fake_provider())));
    REQUIRE(created.has_value());
    tests::RuntimeLoopDriver runtime_driver(runtime);
    auto& session = *created->session;

    auto engine = std::make_shared<coding_agent::tui::InteractiveEngine>(
            terminal, io.get_executor(), coding_agent::TuiMode::Fullscreen);

    std::ostringstream out_stream;
    auto run = coding_agent::tui::InteractiveSessionRunBuilder{}
                       .with_session(session)
                       .with_agent_config_directory(temp.path())
                       .with_tui_mode(coding_agent::TuiMode::Fullscreen)
                       .with_output_stream(&out_stream)
                       .with_stdout_is_tty(true)
                       .build();

    std::optional<support::ExpectedVoid> run_result;
    boost::asio::co_spawn(
            io, drive_engine(engine, std::move(run)), [&](std::exception_ptr ex, support::ExpectedVoid result) {
                CHECK(ex == nullptr);
                run_result.emplace(std::move(result));
            });
    drain_ready(io);
    const tests::RunJoinGuard join_run{io, [&] { return run_result.has_value(); }};

    REQUIRE(terminal.modes().alternate_screen);

    REQUIRE(terminal.inject_input("Secret user message\r"));
    REQUIRE(pump_until(
            io, [&] { return visible_screen(terminal).find("fake: Secret user message") != std::string::npos; }));

    REQUIRE(terminal.inject_input("\x04"));
    REQUIRE(pump_until(io, [&] { return run_result.has_value(); }));
    REQUIRE(run_result.has_value());
    CHECK(run_result->has_value());

    CHECK_FALSE(terminal.modes().alternate_screen);

    // Transcript was NOT transferred to main buffer!
    const auto scrollback = scrollback_text(terminal);
    const auto screen = visible_screen(terminal);
    const std::string total_output = scrollback + "\n" + screen;

    CHECK(total_output.find("Existing main buffer line 1") != std::string::npos);
    CHECK(total_output.find("Secret user message") == std::string::npos);
    CHECK(total_output.find("fake: Secret user message") == std::string::npos);
}

TEST_CASE("Native TUI regular mode exit retains incremental output regardless of exit policy",
        "[coding_agent][tui][exit][regular][issue1019][spec]") {
    tests::TempWorkspace temp;
    tests::EnvVarGuard agent_dir_guard{"PIKE_AGENT_DIR", temp.path().string()};

    const auto settings_path = temp.path() / "settings.json";
    std::ofstream(settings_path) << R"({"fullscreenExitOutput": "resume-hint"})";

    tests::RuntimeFixture runtime;
    boost::asio::io_context io;

    tui::VirtualTerminal terminal({.columns = 80, .rows = 24});
    REQUIRE(terminal.seed_shell_content({"Existing regular line 1"}, {.column = 0, .row = 1}).has_value());

    auto created =
            runtime.run(create_session_async(runtime, session_options(temp, tests::make_scripted_fake_provider())));
    REQUIRE(created.has_value());
    tests::RuntimeLoopDriver runtime_driver(runtime);
    auto& session = *created->session;

    auto engine = std::make_shared<coding_agent::tui::InteractiveEngine>(
            terminal, io.get_executor(), coding_agent::TuiMode::Regular);

    std::ostringstream out_stream;
    auto run = coding_agent::tui::InteractiveSessionRunBuilder{}
                       .with_session(session)
                       .with_agent_config_directory(temp.path())
                       .with_tui_mode(coding_agent::TuiMode::Regular)
                       .with_output_stream(&out_stream)
                       .with_stdout_is_tty(true)
                       .build();

    std::optional<support::ExpectedVoid> run_result;
    boost::asio::co_spawn(
            io, drive_engine(engine, std::move(run)), [&](std::exception_ptr ex, support::ExpectedVoid result) {
                CHECK(ex == nullptr);
                run_result.emplace(std::move(result));
            });
    drain_ready(io);
    const tests::RunJoinGuard join_run{io, [&] { return run_result.has_value(); }};

    CHECK_FALSE(terminal.modes().alternate_screen);

    REQUIRE(terminal.inject_input("Regular mode prompt\r"));
    REQUIRE(pump_until(
            io, [&] { return visible_screen(terminal).find("fake: Regular mode prompt") != std::string::npos; }));

    REQUIRE(terminal.inject_input("\x04"));
    REQUIRE(pump_until(io, [&] { return run_result.has_value(); }));
    REQUIRE(run_result.has_value());
    CHECK(run_result->has_value());

    CHECK_FALSE(terminal.modes().alternate_screen);

    const auto scrollback = scrollback_text(terminal);
    const auto screen = visible_screen(terminal);
    const std::string total_output = scrollback + "\n" + screen;

    CHECK(total_output.find("Existing regular line 1") != std::string::npos);
    CHECK(total_output.find("Regular mode prompt") != std::string::npos);
    CHECK(total_output.find("fake: Regular mode prompt") != std::string::npos);
}

TEST_CASE("Native TUI regular exit does not render pending mutation or advance frame count during finish",
        "[coding_agent][tui][exit][regular][pending][issue1019][spec]") {
    tests::TempWorkspace temp;
    tests::EnvVarGuard agent_dir_guard{"PIKE_AGENT_DIR", temp.path().string()};

    tests::RuntimeFixture runtime;
    boost::asio::io_context io;

    CallCountingTerminal terminal({.columns = 40, .rows = 10});
    REQUIRE(terminal.inner().seed_shell_content(
            {"[Main Banner Shell 1]", "[Main Banner Shell 2]"}, {.column = 0, .row = 2}).has_value());

    auto created =
            runtime.run(create_session_async(runtime, session_options(temp, tests::make_scripted_fake_provider())));
    REQUIRE(created.has_value());
    tests::RuntimeLoopDriver runtime_driver(runtime);
    auto& session = *created->session;

    auto engine = std::make_shared<coding_agent::tui::InteractiveEngine>(
            terminal, io.get_executor(), coding_agent::TuiMode::Regular);

    std::ostringstream out_stream;
    auto run = coding_agent::tui::InteractiveSessionRunBuilder{}
                       .with_session(session)
                       .with_agent_config_directory(temp.path())
                       .with_tui_mode(coding_agent::TuiMode::Regular)
                       .with_output_stream(&out_stream)
                       .with_stdout_is_tty(true)
                       .build();

    std::optional<support::ExpectedVoid> run_result;
    boost::asio::co_spawn(
            io, drive_engine(engine, std::move(run)), [&](std::exception_ptr ex, support::ExpectedVoid result) {
                CHECK(ex == nullptr);
                run_result.emplace(std::move(result));
            });
    drain_ready(io);
    const tests::RunJoinGuard join_run{io, [&] { return run_result.has_value(); }};

    REQUIRE(terminal.inner().inject_input("prompt text\r"));
    REQUIRE(pump_until(
            io, [&] { return visible_screen(terminal.inner()).find("fake: prompt text") != std::string::npos; }));

    // Introduce unpainted pending state via public ModalPresenter::replace_prompt_slot before requesting exit
    const std::string pending_text = "Mutation: unpainted pending response";
    static_cast<coding_agent::tui::ModalPresenter&>(*engine).replace_prompt_slot(
            std::make_shared<cch::tui::Text>(pending_text, 0, 0));

    const auto render_count_before = engine->render_count();
    const auto cursor_before = terminal.inner().cursor();

    // Request clean exit and wait for engine to finish
    engine->request_exit();
    REQUIRE(pump_until(io, [&] { return run_result.has_value(); }));
    REQUIRE(run_result.has_value());
    CHECK(run_result->has_value());

    // In frozen regular stopInteractiveTui (artifact 34 scenario 7 vs scenario 8):
    // 1. Regular exit does NOT render pending mutations during finish (render count delta is 0).
    const auto render_count_after = engine->render_count();
    CHECK(render_count_after == render_count_before);

    // 2. The pending mutation text MUST NOT be rendered to the terminal output.
    const auto scrollback = scrollback_text(terminal.inner());
    const auto screen = visible_screen(terminal.inner());
    const std::string total_output = scrollback + "\n" + screen;
    CHECK(total_output.find(pending_text) == std::string::npos);

    // 3. Stop cleanup sequence preserved (line cleanup advance + carriage return)
    CHECK_FALSE(terminal.modes().alternate_screen);
    CHECK(terminal.inner().cursor().row == cursor_before.row + 3);
}

TEST_CASE("Native TUI fullscreen exit dismisses active overlays before stop",
        "[coding_agent][tui][exit][fullscreen][overlay][issue1019][spec]") {
    tests::TempWorkspace temp;
    tests::EnvVarGuard agent_dir_guard{"PIKE_AGENT_DIR", temp.path().string()};
    tests::RuntimeFixture runtime;
    boost::asio::io_context io;

    tui::VirtualTerminal terminal({.columns = 80, .rows = 24});
    REQUIRE(terminal.seed_shell_content({"Existing line"}, {.column = 0, .row = 1}).has_value());

    auto created =
            runtime.run(create_session_async(runtime, session_options(temp, tests::make_scripted_fake_provider())));
    REQUIRE(created.has_value());
    tests::RuntimeLoopDriver runtime_driver(runtime);
    auto& session = *created->session;

    auto engine = std::make_shared<coding_agent::tui::InteractiveEngine>(
            terminal, io.get_executor(), coding_agent::TuiMode::Fullscreen);

    std::ostringstream out_stream;
    auto run = coding_agent::tui::InteractiveSessionRunBuilder{}
                       .with_session(session)
                       .with_agent_config_directory(temp.path())
                       .with_tui_mode(coding_agent::TuiMode::Fullscreen)
                       .with_output_stream(&out_stream)
                       .with_stdout_is_tty(true)
                       .build();

    std::optional<support::ExpectedVoid> run_result;
    boost::asio::co_spawn(
            io, drive_engine(engine, std::move(run)), [&](std::exception_ptr ex, support::ExpectedVoid result) {
                CHECK(ex == nullptr);
                run_result.emplace(std::move(result));
            });
    drain_ready(io);
    const tests::RunJoinGuard join_run{io, [&] { return run_result.has_value(); }};

    REQUIRE(terminal.modes().alternate_screen);

    REQUIRE(terminal.inject_input("Conversation before overlay\r"));
    REQUIRE(pump_until(io,
            [&] { return visible_screen(terminal).find("fake: Conversation before overlay") != std::string::npos; }));

    // Open settings overlay
    REQUIRE(terminal.inject_input("/settings\r"));
    drain_ready(io);
    REQUIRE(pump_until(io, [&] { return visible_screen(terminal).find("Skill commands") != std::string::npos; }));

    // Request clean exit directly on engine while overlay is open
    engine->request_exit();
    drain_ready(io);
    REQUIRE(pump_until(io, [&] { return run_result.has_value(); }));
    REQUIRE(run_result.has_value());
    CHECK(run_result->has_value());

    CHECK_FALSE(terminal.modes().alternate_screen);

    const auto scrollback = scrollback_text(terminal);
    const auto screen = visible_screen(terminal);
    const std::string total_output = scrollback + "\n" + screen;

    // Conversation is in main buffer
    CHECK(total_output.find("Conversation before overlay") != std::string::npos);
    // But Settings overlay is dismissed, not in main buffer transcript
    CHECK(total_output.find("Skill commands") == std::string::npos);
}

TEST_CASE("Native TUI fullscreen exit with empty transcript renders clean dock to main buffer",
        "[coding_agent][tui][exit][fullscreen][empty][issue1019][spec]") {
    tests::TempWorkspace temp;
    tests::EnvVarGuard agent_dir_guard{"PIKE_AGENT_DIR", temp.path().string()};
    tests::RuntimeFixture runtime;
    boost::asio::io_context io;

    tui::VirtualTerminal terminal({.columns = 80, .rows = 24});
    REQUIRE(terminal.seed_shell_content({"Pre-existing shell prompt"}, {.column = 0, .row = 1}).has_value());

    auto created =
            runtime.run(create_session_async(runtime, session_options(temp, tests::make_scripted_fake_provider())));
    REQUIRE(created.has_value());
    tests::RuntimeLoopDriver runtime_driver(runtime);
    auto& session = *created->session;

    auto engine = std::make_shared<coding_agent::tui::InteractiveEngine>(
            terminal, io.get_executor(), coding_agent::TuiMode::Fullscreen);

    std::ostringstream out_stream;
    auto run = coding_agent::tui::InteractiveSessionRunBuilder{}
                       .with_session(session)
                       .with_agent_config_directory(temp.path())
                       .with_tui_mode(coding_agent::TuiMode::Fullscreen)
                       .with_output_stream(&out_stream)
                       .with_stdout_is_tty(true)
                       .build();

    std::optional<support::ExpectedVoid> run_result;
    boost::asio::co_spawn(
            io, drive_engine(engine, std::move(run)), [&](std::exception_ptr ex, support::ExpectedVoid result) {
                CHECK(ex == nullptr);
                run_result.emplace(std::move(result));
            });
    drain_ready(io);
    const tests::RunJoinGuard join_run{io, [&] { return run_result.has_value(); }};

    REQUIRE(terminal.modes().alternate_screen);

    // Immediate clean exit with 0 messages
    REQUIRE(terminal.inject_input("\x04"));
    REQUIRE(pump_until(io, [&] { return run_result.has_value(); }));
    REQUIRE(run_result.has_value());
    CHECK(run_result->has_value());

    CHECK_FALSE(terminal.modes().alternate_screen);

    const auto scrollback = scrollback_text(terminal);
    const auto screen = visible_screen(terminal);
    const std::string total_output = scrollback + "\n" + screen;

    CHECK(total_output.find("Pre-existing shell prompt") != std::string::npos);
}

TEST_CASE("Native TUI fullscreen long transcript exit outputs complete history in regular line-flow",
        "[coding_agent][tui][exit][fullscreen][long][issue1019][spec]") {
    tests::TempWorkspace temp;
    tests::EnvVarGuard agent_dir_guard{"PIKE_AGENT_DIR", temp.path().string()};
    tests::RuntimeFixture runtime;
    boost::asio::io_context io;

    // Small terminal height: 10 rows
    tui::VirtualTerminal terminal({.columns = 80, .rows = 10});
    REQUIRE(terminal.seed_shell_content({"Pre-existing line"}, {.column = 0, .row = 1}).has_value());

    auto created =
            runtime.run(create_session_async(runtime, session_options(temp, tests::make_scripted_fake_provider())));
    REQUIRE(created.has_value());
    tests::RuntimeLoopDriver runtime_driver(runtime);
    auto& session = *created->session;

    auto engine = std::make_shared<coding_agent::tui::InteractiveEngine>(
            terminal, io.get_executor(), coding_agent::TuiMode::Fullscreen);

    std::ostringstream out_stream;
    auto run = coding_agent::tui::InteractiveSessionRunBuilder{}
                       .with_session(session)
                       .with_agent_config_directory(temp.path())
                       .with_tui_mode(coding_agent::TuiMode::Fullscreen)
                       .with_output_stream(&out_stream)
                       .with_stdout_is_tty(true)
                       .build();

    std::optional<support::ExpectedVoid> run_result;
    boost::asio::co_spawn(
            io, drive_engine(engine, std::move(run)), [&](std::exception_ptr ex, support::ExpectedVoid result) {
                CHECK(ex == nullptr);
                run_result.emplace(std::move(result));
            });
    drain_ready(io);
    const tests::RunJoinGuard join_run{io, [&] { return run_result.has_value(); }};

    REQUIRE(terminal.modes().alternate_screen);

    // Send 3 conversation turns, exceeding 10 rows
    for (const std::string prompt : {"turn alpha", "turn beta", "turn gamma"}) {
        REQUIRE(terminal.inject_input(prompt + "\r"));
        REQUIRE(pump_until(io, [&] { return visible_screen(terminal).find("fake: " + prompt) != std::string::npos; }));
    }

    REQUIRE(terminal.inject_input("\x04"));
    REQUIRE(pump_until(io, [&] { return run_result.has_value(); }));
    REQUIRE(run_result.has_value());
    CHECK(run_result->has_value());

    CHECK_FALSE(terminal.modes().alternate_screen);

    // In regular mode line-flow, every turn is preserved in the scrollback+screen buffer
    const auto scrollback = scrollback_text(terminal);
    const auto screen = visible_screen(terminal);
    const std::string total_output = scrollback + "\n" + screen;

    CHECK(total_output.find("Pre-existing line") != std::string::npos);
    CHECK(total_output.find("turn alpha") != std::string::npos);
    CHECK(total_output.find("fake: turn alpha") != std::string::npos);
    CHECK(total_output.find("turn beta") != std::string::npos);
    CHECK(total_output.find("fake: turn beta") != std::string::npos);
    CHECK(total_output.find("turn gamma") != std::string::npos);
    CHECK(total_output.find("fake: turn gamma") != std::string::npos);
}

TEST_CASE("Native TUI fullscreen exit output probe verifies VirtualTerminal and PTY contracts",
        "[coding_agent][tui][exit][probe][issue1019][spec]") {
    SECTION("VirtualTerminal start/stop delta and modes during exit") {
        tests::TempWorkspace temp;
        tests::EnvVarGuard agent_dir_guard{"PIKE_AGENT_DIR", temp.path().string()};

        tests::RuntimeFixture runtime;
        boost::asio::io_context io;

        CallCountingTerminal terminal({.columns = 40, .rows = 10});
        REQUIRE(terminal.inner().seed_shell_content({"Line 1", "Line 2"}, {.column = 0, .row = 2}).has_value());

        auto created =
                runtime.run(create_session_async(runtime, session_options(temp, tests::make_scripted_fake_provider())));
        REQUIRE(created.has_value());
        tests::RuntimeLoopDriver runtime_driver(runtime);
        auto& session = *created->session;

        auto engine = std::make_shared<coding_agent::tui::InteractiveEngine>(
                terminal, io.get_executor(), coding_agent::TuiMode::Fullscreen);

        auto run = coding_agent::tui::InteractiveSessionRunBuilder{}
                           .with_session(session)
                           .with_agent_config_directory(temp.path())
                           .with_tui_mode(coding_agent::TuiMode::Fullscreen)
                           .build();

        std::optional<support::ExpectedVoid> run_result;
        boost::asio::co_spawn(
                io, drive_engine(engine, std::move(run)), [&](std::exception_ptr ex, support::ExpectedVoid result) {
                    CHECK(ex == nullptr);
                    run_result.emplace(std::move(result));
                });
        drain_ready(io);
        const tests::RunJoinGuard join_run{io, [&] { return run_result.has_value(); }};

        REQUIRE(terminal.modes().alternate_screen);
        CHECK(terminal.start_calls == 1);
        CHECK(terminal.stop_calls == 0);

        // Send user prompt
        REQUIRE(terminal.inner().inject_input("prompt text\r"));
        REQUIRE(pump_until(
                io, [&] { return visible_screen(terminal.inner()).find("fake: prompt text") != std::string::npos; }));

        const auto start_count_before_exit = terminal.start_calls;
        const auto stop_count_before_exit = terminal.stop_calls;

        // Request clean exit
        REQUIRE(terminal.inner().inject_input("\x04"));
        REQUIRE(pump_until(io, [&] { return run_result.has_value(); }));
        REQUIRE(run_result.has_value());
        CHECK(run_result->has_value());

        // Delta check: startDelta == 0, stopDelta == 2 (from fullscreen root stop, plus output-only finalization stop)
        const auto start_delta = terminal.start_calls - start_count_before_exit;
        const auto stop_delta = terminal.stop_calls - stop_count_before_exit;
        CHECK(start_delta == 0);
        CHECK(stop_delta == 2);
        CHECK(terminal.enter_output_only_calls == 1);

        // Terminal modes: raw_input must be false, started must be false
        CHECK_FALSE(terminal.modes().started);
        CHECK_FALSE(terminal.modes().raw_input);
        CHECK_FALSE(terminal.modes().alternate_screen);

        // Final output ordering: verify scrollback + screen contains expected content
        const auto scrollback = scrollback_text(terminal.inner());
        const auto screen = visible_screen(terminal.inner());
        const std::string total_output = scrollback + "\n" + screen;
        CHECK(total_output.find("Line 1") != std::string::npos);
        CHECK(total_output.find("prompt text") != std::string::npos);
        CHECK(total_output.find("fake: prompt text") != std::string::npos);

        // Trailing bytes from stop: last write was newline advance + cursor show
        REQUIRE_FALSE(terminal.inner().output().empty());
        const auto& last_output = terminal.inner().output().back();
        CHECK(last_output.find("\n") != std::string::npos);
        CHECK(last_output.find("B") != std::string::npos);
    }

    SECTION("ProcessTerminal output-only mode nonblocking flag management and PTY write") {
        auto pty = tests::open_pseudo_terminal(40, 10);
        REQUIRE(pty);

        boost::asio::io_context io;
        cch::tui::ProcessTerminal terminal({
                .input_fd = pty->slave.get(),
                .output_fd = pty->slave.get(),
                .executor = io.get_executor(),
        });

        // Initially unstarted: enter_output_only must be rejected
        CHECK_FALSE(terminal.modes().started);
        auto unstarted_result = terminal.enter_output_only();
        CHECK_FALSE(unstarted_result.has_value());
        CHECK(unstarted_result.error().code == cch::support::ErrorCode::Validation);

        // Start and stop to complete lifetime
        REQUIRE(terminal.start([](std::string) { return cch::support::ExpectedVoid{}; },
                                [](cch::tui::TerminalDimensions) { return cch::support::ExpectedVoid{}; })
                        .has_value());
        REQUIRE(terminal.stop().has_value());
        (void)cch::tests::read_available(pty->master.get());

        const auto initial_flags = ::fcntl(pty->slave.get(), F_GETFL, 0);
        CHECK(initial_flags >= 0);
        CHECK((initial_flags & O_NONBLOCK) == 0);

        // Enter output-only presentation mode
        REQUIRE(terminal.enter_output_only());
        const auto output_only_flags = ::fcntl(pty->slave.get(), F_GETFL, 0);
        CHECK((output_only_flags & O_NONBLOCK) != 0);

        // Write output in output-only mode
        REQUIRE(terminal.write("PTY output test\r\n"));

        // Read from master fd to verify delivery
        const auto received = cch::tests::read_available(pty->master.get());
        CHECK(received.find("PTY output test") != std::string::npos);

        // Leave output-only mode: flags must be restored
        REQUIRE(terminal.leave_output_only());
        const auto restored_flags = ::fcntl(pty->slave.get(), F_GETFL, 0);
        CHECK((restored_flags & O_NONBLOCK) == 0);
        CHECK(restored_flags == initial_flags);
    }

    SECTION("ProcessTerminal real PTY saturation tail reporting and flag restoration on leave and stop") {
        auto pty = tests::open_pseudo_terminal(40, 10);
        REQUIRE(pty);

        boost::asio::io_context io;
        cch::tui::ProcessTerminal terminal({
                .input_fd = pty->slave.get(),
                .output_fd = pty->slave.get(),
                .executor = io.get_executor(),
        });

        // 1. Never-started rejected
        auto never_started_result = terminal.enter_output_only();
        REQUIRE_FALSE(never_started_result.has_value());
        CHECK(never_started_result.error().code == cch::support::ErrorCode::Validation);

        // 2. Start and stop to establish completed start/stop lifetime
        REQUIRE(terminal.start([](std::string) { return cch::support::ExpectedVoid{}; },
                                [](cch::tui::TerminalDimensions) { return cch::support::ExpectedVoid{}; })
                        .has_value());
        REQUIRE(terminal.stop().has_value());

        const auto initial_flags = ::fcntl(pty->slave.get(), F_GETFL, 0);
        REQUIRE(initial_flags >= 0);
        REQUIRE((initial_flags & O_NONBLOCK) == 0);

        // 3. Enter output-only
        REQUIRE(terminal.enter_output_only().has_value());
        const auto output_only_flags = ::fcntl(pty->slave.get(), F_GETFL, 0);
        CHECK((output_only_flags & O_NONBLOCK) != 0);

        // 4. Nested enter_output_only rejected
        auto nested_result = terminal.enter_output_only();
        CHECK_FALSE(nested_result.has_value());
        CHECK(nested_result.error().code == cch::support::ErrorCode::Validation);

        // 5. Start during output-only rejected
        auto start_during_result = terminal.start([](std::string) { return cch::support::ExpectedVoid{}; },
                [](cch::tui::TerminalDimensions) { return cch::support::ExpectedVoid{}; });
        CHECK_FALSE(start_during_result.has_value());
        CHECK(start_during_result.error().code == cch::support::ErrorCode::Validation);

        // 6. drain_input and set_progress rejected in output-only
        CHECK_FALSE(terminal.drain_input().has_value());
        CHECK_FALSE(terminal.set_progress(true).has_value());

        // 7. Saturate PTY kernel buffer:
        // Linux PTY buffer is typically 4KB-16KB. Writing 4096-byte chunks until writes are queued.
        std::string chunk(4096, 'A');
        bool admitted = false;
        for (int i = 0; i < 16; ++i) {
            auto write_res = terminal.write(chunk);
            if (!write_res) break;
            admitted = true;
        }
        REQUIRE(admitted);

        // 8. leave_output_only without reading from master:
        // Must fail with Busy error because queued tail cannot be drained!
        auto leave_res = terminal.leave_output_only();
        CHECK_FALSE(leave_res.has_value());
        CHECK(leave_res.error().code == cch::support::ErrorCode::Busy);

        // Flags must still be restored even when leave fails with Busy!
        const auto restored_flags = ::fcntl(pty->slave.get(), F_GETFL, 0);
        CHECK((restored_flags & O_NONBLOCK) == 0);
        CHECK(restored_flags == initial_flags);

        CHECK_FALSE(terminal.modes().started);
        CHECK_FALSE(terminal.modes().raw_input);

        // 9. Now test stop() branch on saturated PTY
        auto pty2 = tests::open_pseudo_terminal(40, 10);
        REQUIRE(pty2);

        cch::tui::ProcessTerminal terminal2({
                .input_fd = pty2->slave.get(),
                .output_fd = pty2->slave.get(),
                .executor = io.get_executor(),
        });
        REQUIRE(terminal2
                        .start([](std::string) { return cch::support::ExpectedVoid{}; },
                                [](cch::tui::TerminalDimensions) { return cch::support::ExpectedVoid{}; })
                        .has_value());
        REQUIRE(terminal2.stop().has_value());

        const auto initial_flags2 = ::fcntl(pty2->slave.get(), F_GETFL, 0);
        REQUIRE(terminal2.enter_output_only().has_value());

        for (int i = 0; i < 16; ++i) {
            auto write_res = terminal2.write(chunk);
            if (!write_res) break;
        }

        // stop() branch on saturated output-only terminal:
        auto stop_res = terminal2.stop();
        CHECK_FALSE(stop_res.has_value());
        CHECK(stop_res.error().code == cch::support::ErrorCode::Busy);

        const auto restored_flags2 = ::fcntl(pty2->slave.get(), F_GETFL, 0);
        CHECK((restored_flags2 & O_NONBLOCK) == 0);
        CHECK(restored_flags2 == initial_flags2);

        CHECK_FALSE(terminal2.modes().started);
        CHECK_FALSE(terminal2.modes().raw_input);
    }

    SECTION("ProcessTerminal output-only synchronized update payload preservation, nested depth, and backpressure") {
        auto pty = tests::open_pseudo_terminal(40, 10);
        REQUIRE(pty);

        boost::asio::io_context io;
        cch::tui::ProcessTerminal terminal({
                .input_fd = pty->slave.get(),
                .output_fd = pty->slave.get(),
                .executor = io.get_executor(),
        });

        REQUIRE(terminal.start([](std::string) { return cch::support::ExpectedVoid{}; },
                                [](cch::tui::TerminalDimensions) { return cch::support::ExpectedVoid{}; })
                        .has_value());
        REQUIRE(terminal.stop().has_value());
        (void)cch::tests::read_available(pty->master.get());

        // Case A: begin_sync + write unique payload + stop(): payload delivered in order
        REQUIRE(terminal.enter_output_only().has_value());
        REQUIRE(terminal.begin_synchronized_update().has_value());
        REQUIRE(terminal.write("UNIQUE_SYNC_PAYLOAD_ABC").has_value());
        REQUIRE(terminal.stop().has_value());

        const auto received_a = cch::tests::read_available(pty->master.get());
        CHECK(received_a.find("UNIQUE_SYNC_PAYLOAD_ABC") != std::string::npos);
        CHECK(received_a.find("\x1b[?2026l") != std::string::npos);
        CHECK((::fcntl(pty->slave.get(), F_GETFL, 0) & O_NONBLOCK) == 0);

        // Case B: nested begin_sync + write unique payload + leave_output_only(): payload delivered
        REQUIRE(terminal.enter_output_only().has_value());
        REQUIRE(terminal.begin_synchronized_update().has_value());
        REQUIRE(terminal.begin_synchronized_update().has_value());
        REQUIRE(terminal.write("NESTED_PAYLOAD_XYZ").has_value());
        REQUIRE(terminal.leave_output_only().has_value());

        const auto received_b = cch::tests::read_available(pty->master.get());
        CHECK(received_b.find("NESTED_PAYLOAD_XYZ") != std::string::npos);
        CHECK(received_b.find("\x1b[?2026l") != std::string::npos);
        CHECK((::fcntl(pty->slave.get(), F_GETFL, 0) & O_NONBLOCK) == 0);

        // Case C: begin_sync + saturated PTY + leave_output_only(): reports Busy with remaining bytes
        REQUIRE(terminal.enter_output_only().has_value());
        REQUIRE(terminal.begin_synchronized_update().has_value());
        std::string chunk(4096, 'K');
        for (int i = 0; i < 16; ++i) {
            auto write_res = terminal.write(chunk);
            if (!write_res) break;
        }
        auto leave_res = terminal.leave_output_only();
        CHECK_FALSE(leave_res.has_value());
        CHECK(leave_res.error().code == cch::support::ErrorCode::Busy);
        CHECK(leave_res.error().detail.find("bytes remaining") != std::string::npos);
        CHECK((::fcntl(pty->slave.get(), F_GETFL, 0) & O_NONBLOCK) == 0);
    }

    SECTION("ProcessTerminal output-only cursor visibility, autowrap, and F_SETFL failure preservation") {
        auto pty = tests::open_pseudo_terminal(40, 10);
        REQUIRE(pty);

        boost::asio::io_context io;
        cch::tui::ProcessTerminal terminal({
                .input_fd = pty->slave.get(),
                .output_fd = pty->slave.get(),
                .executor = io.get_executor(),
        });

        REQUIRE(terminal.start([](std::string) { return cch::support::ExpectedVoid{}; },
                                [](cch::tui::TerminalDimensions) { return cch::support::ExpectedVoid{}; })
                        .has_value());
        REQUIRE(terminal.stop().has_value());
        (void)cch::tests::read_available(pty->master.get());

        // 1. Output-only mode hides cursor and disables autowrap
        REQUIRE(terminal.enter_output_only().has_value());
        REQUIRE(terminal.set_cursor_visible(false).has_value());
        REQUIRE(terminal.set_autowrap(false).has_value());
        CHECK_FALSE(terminal.modes().cursor_visible);
        CHECK_FALSE(terminal.modes().autowrap);

        // leave_output_only restores cursor visibility and autowrap
        REQUIRE(terminal.leave_output_only().has_value());
        CHECK(terminal.modes().cursor_visible);
        CHECK(terminal.modes().autowrap);

        const auto received = cch::tests::read_available(pty->master.get());
        CHECK(received.find("\x1b[?7h") != std::string::npos);
        CHECK(received.find("\x1b[?25h") != std::string::npos);

        // 2. F_SETFL failure preservation
        auto pty_fail = tests::open_pseudo_terminal(40, 10);
        REQUIRE(pty_fail);

        cch::tui::ProcessTerminal fail_term({
                .input_fd = pty_fail->slave.get(),
                .output_fd = pty_fail->slave.get(),
                .executor = io.get_executor(),
        });
        REQUIRE(fail_term
                        .start([](std::string) { return cch::support::ExpectedVoid{}; },
                                [](cch::tui::TerminalDimensions) { return cch::support::ExpectedVoid{}; })
                        .has_value());
        REQUIRE(fail_term.stop().has_value());

        REQUIRE(fail_term.enter_output_only().has_value());

        // Close slave descriptor directly to induce fcntl(F_SETFL) failure during leave
        ::close(pty_fail->slave.get());
        (void)pty_fail->slave.release();

        auto leave_fail = fail_term.leave_output_only();
        CHECK_FALSE(leave_fail.has_value());
        CHECK(leave_fail.error().message.find("could not restore output descriptor flags") != std::string::npos);

        // Unrestored record is preserved: subsequent enter_output_only and start must reject
        auto reenter = fail_term.enter_output_only();
        CHECK_FALSE(reenter.has_value());
        CHECK(reenter.error().code == cch::support::ErrorCode::Validation);

        auto restart = fail_term.start([](std::string) { return cch::support::ExpectedVoid{}; },
                [](cch::tui::TerminalDimensions) { return cch::support::ExpectedVoid{}; });
        CHECK_FALSE(restart.has_value());
        CHECK(restart.error().code == cch::support::ErrorCode::Process);
    }

    SECTION("ProcessTerminal broken descriptor error propagation and flag cleanup") {
        auto pty = tests::open_pseudo_terminal(40, 10);
        REQUIRE(pty);

        boost::asio::io_context io;
        cch::tui::ProcessTerminal terminal({
                .input_fd = pty->slave.get(),
                .output_fd = pty->slave.get(),
                .executor = io.get_executor(),
        });

        // Start and stop to complete lifetime
        REQUIRE(terminal.start([](std::string) { return cch::support::ExpectedVoid{}; },
                                [](cch::tui::TerminalDimensions) { return cch::support::ExpectedVoid{}; })
                        .has_value());
        REQUIRE(terminal.stop().has_value());

        const auto initial_flags = ::fcntl(pty->slave.get(), F_GETFL, 0);
        REQUIRE(terminal.enter_output_only().has_value());

        // Close master to break PTY
        pty->master.reset();

        // Write to broken PTY fails gracefully with EIO
        auto write_result = terminal.write("should fail on broken pty");
        CHECK_FALSE(write_result);

        // Leave output-only cleans up without hanging and restores descriptor flags
        (void)terminal.leave_output_only();
        const auto restored_flags = ::fcntl(pty->slave.get(), F_GETFL, 0);
        CHECK((restored_flags & O_NONBLOCK) == 0);
        CHECK(restored_flags == initial_flags);
    }

    SECTION("InteractiveEngine finish paired persistent session resume notice verification") {
        tests::TempWorkspace temp;
        tests::EnvVarGuard agent_dir_guard{"PIKE_AGENT_DIR", temp.path().string()};

        // 1. Paired Success Case: Persistent session with clean exit emits EXACT notice once
        {
            tests::RuntimeFixture runtime;
            boost::asio::io_context io;

            const auto session_file = temp.path() / "persistent-success.jsonl";
            auto opts = session_options(temp, tests::make_scripted_fake_provider());
            opts.session_target = coding_agent::ExplicitOpenOrCreateSessionTarget{session_file};

            auto created = runtime.run(create_session_async(runtime, std::move(opts)));
            REQUIRE(created.has_value());
            tests::RuntimeLoopDriver runtime_driver(runtime);
            auto& session = *created->session;

            tui::VirtualTerminal terminal({.columns = 40, .rows = 10});
            auto engine = std::make_shared<coding_agent::tui::InteractiveEngine>(
                    terminal, io.get_executor(), coding_agent::TuiMode::Fullscreen);

            std::ostringstream out_stream;
            auto run = coding_agent::tui::InteractiveSessionRunBuilder{}
                               .with_session(session)
                               .with_agent_config_directory(temp.path())
                               .with_tui_mode(coding_agent::TuiMode::Fullscreen)
                               .with_output_stream(&out_stream)
                               .with_stdout_is_tty(true)
                               .build();

            std::optional<support::ExpectedVoid> run_result;
            boost::asio::co_spawn(
                    io, drive_engine(engine, std::move(run)), [&](std::exception_ptr ex, support::ExpectedVoid result) {
                        CHECK(ex == nullptr);
                        run_result.emplace(std::move(result));
                    });
            drain_ready(io);
            const tests::RunJoinGuard join_run{io, [&] { return run_result.has_value(); }};

            // Send user prompt to flush session to disk (ADR 0064)
            REQUIRE(terminal.inject_input("prompt text\r"));
            REQUIRE(pump_until(
                    io, [&] { return visible_screen(terminal).find("fake: prompt text") != std::string::npos; }));

            // Exit cleanly
            REQUIRE(terminal.inject_input("\x04"));
            REQUIRE(pump_until(io, [&] { return run_result.has_value(); }));
            REQUIRE(run_result.has_value());
            CHECK(run_result->has_value());

            CHECK(std::filesystem::exists(session_file));
            const auto expected_cmd = coding_agent::tui::format_resume_command(
                    session.session_id(), session_file, temp.path(), /*stdout_is_tty=*/true, "pike");
            REQUIRE(expected_cmd.has_value());
            const std::string expected_notice =
                    std::string(coding_agent::tui::kResumeNoticePrefix) + *expected_cmd + "\n";
            CHECK(out_stream.str() == expected_notice);
            CHECK(out_stream.str().find("--session") != std::string::npos);
        }

        // 2. Paired Failure Case: Same persistent session setup, but exit failure emits ZERO notices
        {
            tests::RuntimeFixture runtime;
            boost::asio::io_context io;

            const auto session_file = temp.path() / "persistent-failure.jsonl";
            auto opts = session_options(temp, tests::make_scripted_fake_provider());
            opts.session_target = coding_agent::ExplicitOpenOrCreateSessionTarget{session_file};

            auto created = runtime.run(create_session_async(runtime, std::move(opts)));
            REQUIRE(created.has_value());
            tests::RuntimeLoopDriver runtime_driver(runtime);
            auto& session = *created->session;

            class FailingStopTerminal final : public cch::tui::Terminal {
            public:
                explicit FailingStopTerminal(cch::tui::VirtualTerminalOptions options) : inner_(std::move(options)) {}

                cch::support::ExpectedVoid start(
                        cch::tui::TerminalInputSink input_sink, cch::tui::TerminalResizeSink resize_sink) override {
                    return inner_.start(std::move(input_sink), std::move(resize_sink));
                }
                cch::support::ExpectedVoid stop() override {
                    if (fail_stop) {
                        return std::unexpected(
                                cch::support::make_error(cch::support::ErrorCode::Busy, "simulated stop failure"));
                    }
                    return inner_.stop();
                }
                cch::tui::TerminalDimensions dimensions() const override { return inner_.dimensions(); }
                cch::tui::TerminalCapabilities capabilities() const override { return inner_.capabilities(); }
                cch::tui::TerminalModeState modes() const override { return inner_.modes(); }
                cch::support::ExpectedVoid clear_screen() override { return inner_.clear_screen(); }
                cch::support::ExpectedVoid write(std::string_view output) override { return inner_.write(output); }
                cch::support::ExpectedVoid set_cursor(cch::tui::CursorPosition position) override {
                    return inner_.set_cursor(position);
                }
                cch::support::ExpectedVoid set_cursor_visible(bool visible) override {
                    return inner_.set_cursor_visible(visible);
                }
                cch::support::ExpectedVoid set_alternate_screen(bool enabled) override {
                    return inner_.set_alternate_screen(enabled);
                }
                cch::support::ExpectedVoid set_autowrap(bool enabled) override { return inner_.set_autowrap(enabled); }
                cch::support::ExpectedVoid set_scroll_margins(std::size_t top_row, std::size_t bottom_row) override {
                    return inner_.set_scroll_margins(top_row, bottom_row);
                }
                cch::support::ExpectedVoid reset_scroll_margins() override { return inner_.reset_scroll_margins(); }
                cch::support::ExpectedVoid begin_synchronized_update() override {
                    return inner_.begin_synchronized_update();
                }
                cch::support::ExpectedVoid end_synchronized_update() override {
                    return inner_.end_synchronized_update();
                }
                cch::support::ExpectedVoid set_title(std::string_view title) override {
                    return inner_.set_title(title);
                }
                cch::support::ExpectedVoid set_progress(bool active) override { return inner_.set_progress(active); }
                cch::support::ExpectedVoid drain_input(
                        std::chrono::milliseconds max_ms, std::chrono::milliseconds idle_ms) override {
                    return inner_.drain_input(max_ms, idle_ms);
                }
                cch::support::ExpectedVoid enter_output_only() override { return inner_.enter_output_only(); }
                cch::support::ExpectedVoid leave_output_only() override { return inner_.leave_output_only(); }

                cch::tui::VirtualTerminal inner_;
                bool fail_stop{false};
            };

            FailingStopTerminal terminal({.columns = 40, .rows = 10});
            auto engine = std::make_shared<coding_agent::tui::InteractiveEngine>(
                    terminal, io.get_executor(), coding_agent::TuiMode::Fullscreen);

            std::ostringstream out_stream;
            auto run = coding_agent::tui::InteractiveSessionRunBuilder{}
                               .with_session(session)
                               .with_agent_config_directory(temp.path())
                               .with_tui_mode(coding_agent::TuiMode::Fullscreen)
                               .with_output_stream(&out_stream)
                               .with_stdout_is_tty(true)
                               .build();

            std::optional<support::ExpectedVoid> run_result;
            boost::asio::co_spawn(
                    io, drive_engine(engine, std::move(run)), [&](std::exception_ptr ex, support::ExpectedVoid result) {
                        CHECK(ex == nullptr);
                        run_result.emplace(std::move(result));
                    });
            drain_ready(io);
            const tests::RunJoinGuard join_run{io, [&] { return run_result.has_value(); }};

            // Send user prompt to flush session to disk (ADR 0064)
            REQUIRE(terminal.inner_.inject_input("prompt text\r"));
            REQUIRE(pump_until(io,
                    [&] { return visible_screen(terminal.inner_).find("fake: prompt text") != std::string::npos; }));

            // Induce stop failure during exit
            terminal.fail_stop = true;

            REQUIRE(terminal.inner_.inject_input("\x04"));
            REQUIRE(pump_until(io, [&] { return run_result.has_value(); }));
            REQUIRE(run_result.has_value());

            // Exit must report failure
            CHECK_FALSE(run_result->has_value());

            // Session file still exists on disk
            CHECK(std::filesystem::exists(session_file));

            // Resume notice must be COMPLETELY SUPPRESSED (zero bytes emitted)
            CHECK(out_stream.str().empty());
        }

        // 3. Paired Failure Case: Persistent session with failing ostream streambuf returns error
        {
            tests::RuntimeFixture runtime;
            boost::asio::io_context io;

            const auto session_file = temp.path() / "persistent-streambuf-failure.jsonl";
            auto opts = session_options(temp, tests::make_scripted_fake_provider());
            opts.session_target = coding_agent::ExplicitOpenOrCreateSessionTarget{session_file};

            auto created = runtime.run(create_session_async(runtime, std::move(opts)));
            REQUIRE(created.has_value());
            tests::RuntimeLoopDriver runtime_driver(runtime);
            auto& session = *created->session;

            tui::VirtualTerminal terminal({.columns = 40, .rows = 10});
            auto engine = std::make_shared<coding_agent::tui::InteractiveEngine>(
                    terminal, io.get_executor(), coding_agent::TuiMode::Fullscreen);

            class FailingStreamBuf final : public std::streambuf {
            protected:
                int_type overflow(int_type) override { return traits_type::eof(); }
                int sync() override { return -1; }
            };

            FailingStreamBuf bad_buf;
            std::ostream bad_stream(&bad_buf);
            bad_stream.setstate(std::ios_base::badbit);

            auto run = coding_agent::tui::InteractiveSessionRunBuilder{}
                               .with_session(session)
                               .with_agent_config_directory(temp.path())
                               .with_tui_mode(coding_agent::TuiMode::Fullscreen)
                               .with_output_stream(&bad_stream)
                               .with_stdout_is_tty(true)
                               .build();

            std::optional<support::ExpectedVoid> run_result;
            boost::asio::co_spawn(
                    io, drive_engine(engine, std::move(run)), [&](std::exception_ptr ex, support::ExpectedVoid result) {
                        CHECK(ex == nullptr);
                        run_result.emplace(std::move(result));
                    });
            drain_ready(io);
            const tests::RunJoinGuard join_run{io, [&] { return run_result.has_value(); }};

            // Send user prompt to flush session to disk (ADR 0064)
            REQUIRE(terminal.inject_input("prompt text\r"));
            REQUIRE(pump_until(
                    io, [&] { return visible_screen(terminal).find("fake: prompt text") != std::string::npos; }));

            // Exit cleanly
            REQUIRE(terminal.inject_input("\x04"));
            REQUIRE(pump_until(io, [&] { return run_result.has_value(); }));
            REQUIRE(run_result.has_value());

            // Because bad_stream failed on flush/failbit, finish() must report an error!
            CHECK_FALSE(run_result->has_value());
            CHECK(run_result->error().code == support::ErrorCode::Process);
            CHECK(run_result->error().message.find("Native TUI failed to write resume notice") != std::string::npos);
        }
    }

    SECTION("Native TUI fullscreen exit delayed flow quiescence gates pending paint") {
        tests::TempWorkspace temp;
        tests::EnvVarGuard agent_dir_guard{"PIKE_AGENT_DIR", temp.path().string()};

        tests::RuntimeFixture runtime;
        boost::asio::io_context io;

        CallCountingTerminal terminal({.columns = 40, .rows = 10});
        auto created =
                runtime.run(create_session_async(runtime, session_options(temp, tests::make_scripted_fake_provider())));
        REQUIRE(created.has_value());
        tests::RuntimeLoopDriver runtime_driver(runtime);
        auto& session = *created->session;

        class DelayedClipboard final : public cch::tui::NativeClipboard {
        public:
            DelayedClipboard(boost::asio::io_context& io_ctx, std::chrono::milliseconds delay)
                : io_(io_ctx), delay_(delay) {}

            support::AsyncResult<cch::tui::NativeClipboardTextReadVariant> get_text(std::stop_token) override {
                auto timer = std::make_shared<boost::asio::steady_timer>(io_, delay_);
                return support::detail::make_async_result_on(io_.get_executor(),
                        [timer]()
                                -> boost::asio::awaitable<support::Expected<cch::tui::NativeClipboardTextReadVariant>> {
                            boost::system::error_code ec;
                            co_await timer->async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ec));
                            co_return cch::tui::NativeClipboardTextReadVariant{
                                    std::optional<std::string>{"delayed paste"}};
                        });
            }

            support::AsyncResult<cch::tui::NativeClipboardImageReadVariant> get_image(std::stop_token) override {
                return support::detail::make_async_result_on(io_.get_executor(),
                        []() -> boost::asio::awaitable<support::Expected<cch::tui::NativeClipboardImageReadVariant>> {
                            co_return cch::tui::NativeClipboardImageReadVariant{cch::tui::NativeClipboardUnavailable{}};
                        });
            }

        private:
            boost::asio::io_context& io_;
            std::chrono::milliseconds delay_;
        };

        auto engine = std::make_shared<coding_agent::tui::InteractiveEngine>(
                terminal, io.get_executor(), coding_agent::TuiMode::Fullscreen);

        auto run = coding_agent::tui::InteractiveSessionRunBuilder{}
                           .with_session(session)
                           .with_agent_config_directory(temp.path())
                           .with_tui_mode(coding_agent::TuiMode::Fullscreen)
                           .with_native_clipboard(std::make_unique<DelayedClipboard>(io, std::chrono::milliseconds{40}))
                           .build();

        std::optional<support::ExpectedVoid> run_result;
        boost::asio::co_spawn(
                io, drive_engine(engine, std::move(run)), [&](std::exception_ptr ex, support::ExpectedVoid result) {
                    CHECK(ex == nullptr);
                    run_result.emplace(std::move(result));
                });
        drain_ready(io);
        const tests::RunJoinGuard join_run{io, [&] { return run_result.has_value(); }};

        // Trigger paste: spawns flow that runs for 40ms
        REQUIRE(terminal.inner().inject_input("\x16"));

        // While flow is in flight, request exit
        REQUIRE(terminal.inner().inject_input("\x04"));

        // During the delay before flow quiesces, trigger an input event that would normally invalidate
        REQUIRE(terminal.inner().inject_input("x"));
        drain_ready(io);

        // During flow wait: enter_output_only must NOT have been called yet (0 final presentation frames)
        CHECK(terminal.enter_output_only_calls == 0);

        // Wait for complete quiescence and exit
        REQUIRE(pump_until(io, [&] { return run_result.has_value(); }));
        REQUIRE(run_result.has_value());
        CHECK(run_result->has_value());

        // After quiescence: exactly ONE final presentation frame rendered
        CHECK(terminal.enter_output_only_calls == 1);
    }
}

