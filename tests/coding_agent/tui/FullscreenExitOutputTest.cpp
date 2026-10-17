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
#include "support/ProtocolRecordingTerminal.hpp"
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
#include <fcntl.h>
#include <iostream>
#include <termios.h>

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

    using Terminal::stop;
    cch::support::ExpectedVoid stop() override { return stop(cch::tui::TerminalStopOptions{}); }
    cch::support::ExpectedVoid stop(cch::tui::TerminalStopOptions options) override {
        ++stop_calls;
        return inner_.stop(options);
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
    const auto bundle = tests::read_pi_tui_evidence("fullscreen-exit-output.json");
    REQUIRE(bundle);
    const auto& scenarios = bundle->at("scenarios").get_array();
    auto sc_it = std::find_if(scenarios.begin(), scenarios.end(), [](const auto& s) {
        return s.at("name").get_string() == "format-resume-command-matrix";
    });
    REQUIRE(sc_it != scenarios.end());
    const auto& sc = *sc_it;
    const auto& inputs = sc.at("inputs");
    const auto& exp = sc.at("expected");

    tests::TempWorkspace temp;
    const auto workspace = temp.path() / "workspace";
    std::filesystem::create_directories(workspace);

    const auto sessions_root = coding_agent::sessions_root_path();
    const auto default_dir = sessions_root / coding_agent::session_paths::encode_workspace_key(workspace);
    std::filesystem::create_directories(default_dir);

    const std::string default_id = inputs.at("defaultSession").at("sessionId").get_string();
    const auto default_session_file = default_dir / (default_id + ".json");
    std::ofstream(default_session_file) << "{}\n";

    SECTION("default session dir produces pi --session <id>") {
        auto cmd = coding_agent::tui::format_resume_command(
                default_id, default_session_file, workspace, /*stdout_is_tty=*/true, "pi");
        REQUIRE(cmd.has_value());
        CHECK(*cmd == exp.at("cmdDefault").get_string());

        auto cmd_pike = coding_agent::tui::format_resume_command(
                default_id, default_session_file, workspace, /*stdout_is_tty=*/true, "pike");
        REQUIRE(cmd_pike.has_value());
        CHECK(*cmd_pike == "pike --session " + default_id);
    }

    SECTION("custom session dir without spaces") {
        const auto custom_dir = temp.path() / "custom/sessions";
        std::filesystem::create_directories(custom_dir);
        const std::string no_space_id = inputs.at("customNoSpace").at("sessionId").get_string();
        const auto custom_session_file = custom_dir / (no_space_id + ".json");
        std::ofstream(custom_session_file) << "{}\n";

        auto cmd = coding_agent::tui::format_resume_command(
                no_space_id, custom_session_file, workspace, /*stdout_is_tty=*/true, "pi");
        REQUIRE(cmd.has_value());
        CHECK(*cmd == "pi --session-dir " + custom_dir.string() + " --session " + no_space_id);
    }

    SECTION("custom session dir with spaces quotes the path") {
        const auto custom_dir = temp.path() / "my custom sessions/dir";
        std::filesystem::create_directories(custom_dir);
        const std::string spaces_id = inputs.at("customSpaces").at("sessionId").get_string();
        const auto custom_session_file = custom_dir / (spaces_id + ".json");
        std::ofstream(custom_session_file) << "{}\n";

        auto cmd = coding_agent::tui::format_resume_command(
                spaces_id, custom_session_file, workspace, /*stdout_is_tty=*/true, "pi");
        REQUIRE(cmd.has_value());
        CHECK(*cmd == "pi --session-dir '" + custom_dir.string() + "' --session " + spaces_id);
    }

    SECTION("custom session dir with single quotes escapes them") {
        const auto custom_dir = temp.path() / "my user's sessions/dir";
        std::filesystem::create_directories(custom_dir);
        const std::string quotes_id = inputs.at("customSingleQuote").at("sessionId").get_string();
        const auto custom_session_file = custom_dir / (quotes_id + ".json");
        std::ofstream(custom_session_file) << "{}\n";

        auto cmd = coding_agent::tui::format_resume_command(
                quotes_id, custom_session_file, workspace, /*stdout_is_tty=*/true, "pi");
        REQUIRE(cmd.has_value());
        CHECK(cmd->find("'\\''") != std::string::npos);
        CHECK(*cmd == "pi --session-dir '" + [&] {
            std::string r;
            for (char c : custom_dir.string()) {
                if (c == '\'')
                    r += "'\\''";
                else
                    r += c;
            }
            return r;
        }() + "' --session " + quotes_id);
    }

    SECTION("custom session dir with shell metacharacters is strictly quoted") {
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
                    "meta-id", custom_session_file, workspace, /*stdout_is_tty=*/true, "pi");
            REQUIRE(cmd.has_value());
            CHECK(*cmd == "pi --session-dir '" + custom_dir.string() + "' --session meta-id");
        }
    }

    SECTION("custom session dir with allowed whitelist characters is unquoted") {
        const auto custom_dir = temp.path() / "dir-123_abc.XYZ~:@";
        std::filesystem::create_directories(custom_dir);
        const auto custom_session_file = custom_dir / "test.json";
        std::ofstream(custom_session_file) << "{}\n";

        auto cmd = coding_agent::tui::format_resume_command(
                "whitelist-id", custom_session_file, workspace, /*stdout_is_tty=*/true, "pi");
        REQUIRE(cmd.has_value());
        CHECK(*cmd == "pi --session-dir " + custom_dir.string() + " --session whitelist-id");
    }

    SECTION("unpersisted session (nullopt path) returns nullopt") {
        auto cmd = coding_agent::tui::format_resume_command(
                default_id, std::nullopt, workspace, /*stdout_is_tty=*/true, "pi");
        CHECK_FALSE(cmd.has_value());
        CHECK(exp.at("cmdNotPersisted").holds<std::nullptr_t>());
    }

    SECTION("non-existent session file returns nullopt") {
        const auto missing_file = default_dir / "does_not_exist.json";
        auto cmd = coding_agent::tui::format_resume_command(
                "missing", missing_file, workspace, /*stdout_is_tty=*/true, "pi");
        CHECK_FALSE(cmd.has_value());
        CHECK(exp.at("cmdMissingFile").holds<std::nullptr_t>());
    }

    SECTION("non-TTY stdout returns nullopt") {
        auto cmd = coding_agent::tui::format_resume_command(
                default_id, default_session_file, workspace, /*stdout_is_tty=*/false, "pi");
        CHECK_FALSE(cmd.has_value());
        CHECK(exp.at("cmdNotTTY").holds<std::nullptr_t>());
    }

    SECTION("prefix constant matches pi chalk.dim ANSI string") {
        CHECK(coding_agent::tui::kResumeNoticePrefix == exp.at("chalkDimHeader").get_string() + " ");
        auto cmd = coding_agent::tui::format_resume_command(
                default_id, default_session_file, workspace, /*stdout_is_tty=*/true, "pi");
        REQUIRE(cmd.has_value());
        CHECK(std::string(coding_agent::tui::kResumeNoticePrefix) + *cmd + "\n" == exp.at("styledNotice").get_string());
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
    REQUIRE(terminal.inner()
                    .seed_shell_content({"[Main Banner Shell 1]", "[Main Banner Shell 2]"}, {.column = 0, .row = 2})
                    .has_value());

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
                using Terminal::stop;
                cch::support::ExpectedVoid stop() override { return stop(cch::tui::TerminalStopOptions{}); }
                cch::support::ExpectedVoid stop(cch::tui::TerminalStopOptions options) override {
                    if (fail_stop) {
                        return std::unexpected(
                                cch::support::make_error(cch::support::ErrorCode::Busy, "simulated stop failure"));
                    }
                    return inner_.stop(options);
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

// ── 5. Exact Phase Boundary Protocol & Artifact Consumers ────────────────────

inline std::vector<std::string> extract_control_tokens(std::string_view raw) {
    std::vector<std::string> tokens;
    const std::size_t len = raw.size();
    for (std::size_t i = 0; i < len;) {
        if (raw[i] == '\x1b') {
            if (i + 1 < len && raw[i + 1] == '[') {
                const auto start = i;
                i += 2;
                // Parameter bytes (0x30-0x3F): '0'-'9', ':', ';', '<', '=', '>', '?'
                while (i < len && static_cast<unsigned char>(raw[i]) >= 0x30 &&
                        static_cast<unsigned char>(raw[i]) <= 0x3F)
                    ++i;
                // Intermediate bytes (0x20-0x2F)
                while (i < len && static_cast<unsigned char>(raw[i]) >= 0x20 &&
                        static_cast<unsigned char>(raw[i]) <= 0x2F)
                    ++i;
                // Final byte (0x40-0x7E)
                if (i < len && static_cast<unsigned char>(raw[i]) >= 0x40 && static_cast<unsigned char>(raw[i]) <= 0x7E)
                    ++i;
                tokens.push_back(std::string(raw.substr(start, i - start)));
                continue;
            }
            if (i + 1 < len && raw[i + 1] == ']') {
                const auto start = i;
                i += 2;
                while (i < len && raw[i] != '\x07' && !(raw[i] == '\x1b' && i + 1 < len && raw[i + 1] == '\\'))
                    ++i;
                if (i < len && raw[i] == '\x07') {
                    ++i;
                } else if (i + 1 < len && raw[i] == '\x1b' && raw[i + 1] == '\\') {
                    i += 2;
                }
                tokens.push_back(std::string(raw.substr(start, i - start)));
                continue;
            }
            if (i + 1 < len) {
                tokens.push_back(std::string(raw.substr(i, 2)));
                i += 2;
                continue;
            }
            tokens.push_back(std::string(raw.substr(i, 1)));
            ++i;
            continue;
        }
        if (raw[i] == '\r') {
            if (i + 1 < len && raw[i + 1] == '\n') {
                tokens.push_back("\r\n");
                i += 2;
            } else {
                tokens.push_back("\r");
                ++i;
            }
            continue;
        }
        if (raw[i] == '\n') {
            tokens.push_back("\n");
            ++i;
            continue;
        }
        ++i;
    }
    return tokens;
}

class ComponentRef final : public cch::tui::Component {
public:
    explicit ComponentRef(cch::tui::Component& target) : target_(target) {}
    support::Expected<cch::tui::RenderResult> render(std::size_t width) override { return target_.render(width); }
    void invalidate() override { target_.invalidate(); }

private:
    cch::tui::Component& target_;
};

class CountingComponent final : public cch::tui::Component {
public:
    explicit CountingComponent(std::unique_ptr<cch::tui::Component> inner, std::shared_ptr<std::size_t> counter)
        : inner_(std::move(inner)), counter_(std::move(counter)) {}

    support::Expected<cch::tui::RenderResult> render(std::size_t width) override {
        if (counter_) ++(*counter_);
        if (inner_) return inner_->render(width);
        return cch::tui::RenderResult{};
    }

    void invalidate() override {
        if (inner_) inner_->invalidate();
    }

    [[nodiscard]] std::size_t render_count() const noexcept { return counter_ ? *counter_ : 0; }

private:
    std::unique_ptr<cch::tui::Component> inner_;
    std::shared_ptr<std::size_t> counter_;
};

struct ProtocolPhaseSnapshot {
    std::string phase;
    std::string active_buffer;
    std::size_t cursor_x{0};
    std::size_t cursor_y{0};
    std::size_t cursor_base_y{0};
    std::size_t cursor_viewport_y{0};
    bool cursor_hidden{false};
    bool bracketed_paste_mode{false};
    bool alternate_screen{false};
    bool wraparound_mode{true};
    std::vector<std::string> screen_lines_with_spaces;
    std::vector<std::string> screen_lines_trimmed;
    std::vector<std::string> full_buffer_lines_with_spaces;
    std::vector<std::string> full_buffer_lines_trimmed;
    std::vector<std::string> scrollback_lines_with_spaces;
    std::vector<std::string> scrollback_lines_trimmed;
    std::size_t start_count{0};
    std::size_t stop_count{0};
    std::size_t component_render_count{0};
    std::size_t start_count_delta{0};
    std::size_t stop_count_delta{0};
    std::size_t render_now_count_delta{0};
    std::size_t raw_writes_total_count{0};
    std::size_t raw_writes_total_bytes{0};
    std::vector<std::string> phase_raw_writes;
    std::vector<std::string> phase_control_tokens;
    std::vector<tests::ProtocolRecordingTerminal::ObservedOp> phase_ops;

    // Retained for scenario compatibility
    std::vector<std::string> screen_lines;
    std::size_t render_now_count{0};
};

struct ExitScenarioParams {
    std::string name{};
    std::size_t columns{40};
    std::size_t rows{10};
    std::string initial_mode{"fullscreen"};
    std::string exit_output{"transcript"};
    std::vector<std::string> initial_main_buffer{};
    std::vector<std::string> document_lines{};
    std::string editor_text{};
    std::optional<std::string> overlay_text{std::nullopt};
    std::vector<std::string> pending_mutation{};
    bool forced_render_now_before_stop{false};
};

struct ExitRunResult {
    std::vector<ProtocolPhaseSnapshot> phases;
    std::vector<std::string> main_buffer_lines;
    std::string final_cursor_cleanup_writes;
    std::size_t start_count_delta{0};
    std::size_t stop_count_delta{0};
    std::size_t render_now_count_delta{0};
    std::size_t total_raw_writes_count{0};
    std::size_t total_raw_bytes{0};
    std::vector<std::string> relevant_control_tokens;
    std::string buffer_active_before;
    std::string buffer_active_after;
    bool has_overlay_entries_after{false};
};

inline std::vector<std::string> get_strings(const support::JsonValue& obj, const std::string& key) {
    std::vector<std::string> res;
    if (!obj.get_object().contains(key) || !obj.at(key).holds<support::JsonValue::array_t>()) return res;
    for (const auto& v : obj.at(key).get_array()) {
        res.push_back(v.get_string());
    }
    return res;
}

ExitRunResult run_exit_scenario(const ExitScenarioParams& params) {
    ExitRunResult result;
    cch::tui::VirtualTerminal vt({.columns = params.columns, .rows = params.rows});
    if (!params.initial_main_buffer.empty()) {
        REQUIRE(vt.seed_shell_content(
                          params.initial_main_buffer, {.column = 0, .row = params.initial_main_buffer.size()})
                        .has_value());
    }
    tests::ProtocolRecordingTerminal terminal(vt);
    std::size_t root_render_count = 0;
    auto component_render_counter = std::make_shared<std::size_t>(0);
    std::size_t ops_prev_idx = 0;
    std::size_t start_count_before_exit = 0;
    std::size_t stop_count_before_exit = 0;

    auto make_snapshot = [&](std::string phase_name) {
        ProtocolPhaseSnapshot snap;
        snap.phase = std::move(phase_name);
        snap.active_buffer = terminal.inner().modes().alternate_screen ? "alternate" : "normal";
        snap.cursor_x = terminal.inner().cursor().column;
        snap.cursor_y = terminal.inner().cursor().row;
        snap.cursor_base_y = terminal.inner().scrollback_cells().size();
        snap.cursor_viewport_y = terminal.inner().viewport_top();
        snap.cursor_hidden = !terminal.inner().modes().cursor_visible;
        snap.bracketed_paste_mode = terminal.inner().modes().bracketed_paste;
        snap.alternate_screen = terminal.inner().modes().alternate_screen;
        snap.wraparound_mode = terminal.inner().modes().autowrap;

        const std::size_t cols = terminal.inner().dimensions().columns;
        for (const auto& row : terminal.inner().cells()) {
            std::string line;
            for (const auto& cell : row) {
                if (cell.continuation) continue;
                line += cell.grapheme.empty() ? " " : cell.grapheme;
            }
            if (line.size() < cols) {
                line.append(cols - line.size(), ' ');
            }
            snap.screen_lines_with_spaces.push_back(line);
        }
        snap.screen_lines_trimmed = terminal.inner().screen();
        snap.screen_lines = snap.screen_lines_trimmed;

        for (const auto& row : terminal.inner().scrollback_cells()) {
            std::string line;
            for (const auto& cell : row) {
                if (cell.continuation) continue;
                line += cell.grapheme.empty() ? " " : cell.grapheme;
            }
            if (line.size() < cols) {
                line.append(cols - line.size(), ' ');
            }
            snap.scrollback_lines_with_spaces.push_back(line);
        }
        snap.scrollback_lines_trimmed = terminal.inner().scrollback();

        snap.full_buffer_lines_with_spaces = snap.scrollback_lines_with_spaces;
        snap.full_buffer_lines_with_spaces.insert(snap.full_buffer_lines_with_spaces.end(),
                snap.screen_lines_with_spaces.begin(),
                snap.screen_lines_with_spaces.end());

        snap.full_buffer_lines_trimmed = snap.scrollback_lines_trimmed;
        snap.full_buffer_lines_trimmed.insert(snap.full_buffer_lines_trimmed.end(),
                snap.screen_lines_trimmed.begin(),
                snap.screen_lines_trimmed.end());

        snap.start_count = terminal.start_count();
        snap.stop_count = terminal.stop_count();
        snap.component_render_count = *component_render_counter;
        snap.render_now_count = root_render_count;
        snap.start_count_delta = terminal.start_count() - start_count_before_exit;
        snap.stop_count_delta = terminal.stop_count() - stop_count_before_exit;
        snap.render_now_count_delta = root_render_count;

        const auto& all_ops = terminal.ops();
        for (std::size_t i = ops_prev_idx; i < all_ops.size(); ++i) {
            snap.phase_ops.push_back(all_ops[i]);
            if (all_ops[i].kind == tests::ProtocolRecordingTerminal::OpKind::Write) {
                snap.phase_raw_writes.push_back(all_ops[i].write_bytes);
            }
        }
        ops_prev_idx = all_ops.size();

        std::size_t total_writes = 0;
        std::size_t total_bytes = 0;
        for (const auto& op : all_ops) {
            if (op.kind == tests::ProtocolRecordingTerminal::OpKind::Write) {
                ++total_writes;
                total_bytes += op.write_bytes.size();
            }
        }
        snap.raw_writes_total_count = total_writes;
        snap.raw_writes_total_bytes = total_bytes;

        std::string phase_combined;
        for (const auto& w : snap.phase_raw_writes)
            phase_combined += w;
        snap.phase_control_tokens = extract_control_tokens(phase_combined);

        return snap;
    };

    auto doc_c = std::make_unique<cch::tui::Container>();
    auto* doc_ptr = doc_c.get();
    for (const auto& line : params.document_lines) {
        (void)doc_c->add_child(std::make_unique<cch::tui::Text>(line, 0, 0));
    }
    auto pending_c = std::make_unique<cch::tui::Container>();
    auto status_c = std::make_unique<cch::tui::Container>();
    auto editor_c = std::make_unique<cch::tui::Container>();
    if (!params.editor_text.empty()) {
        (void)editor_c->add_child(std::make_unique<cch::tui::Text>(params.editor_text, 0, 0));
    }
    auto footer_c = std::make_unique<cch::tui::Container>();

    if (params.initial_mode == "fullscreen") {
        cch::coding_agent::tui::ChatViewportOptions vp_opts;
        vp_opts.document = std::make_unique<ComponentRef>(*doc_c);
        vp_opts.pending_messages = std::make_unique<ComponentRef>(*pending_c);
        vp_opts.status = std::make_unique<ComponentRef>(*status_c);
        vp_opts.editor = std::make_unique<ComponentRef>(*editor_c);
        vp_opts.footer = std::make_unique<ComponentRef>(*footer_c);
        vp_opts.scrollbar = cch::tui::ScrollbarMode::Hidden;
        auto viewport = cch::coding_agent::tui::create_chat_viewport(std::move(vp_opts));
        REQUIRE(viewport.has_value());

        cch::tui::AltScreen screen(terminal);
        screen.set_layout_root(std::move(viewport->root));
        REQUIRE(screen.add_child(std::move(doc_c)));
        REQUIRE(screen.add_child(std::move(pending_c)));
        REQUIRE(screen.add_child(std::move(status_c)));
        REQUIRE(screen.add_child(std::move(editor_c)));
        REQUIRE(screen.add_child(std::move(footer_c)));

        cch::tui::Overlay* overlay_ptr = nullptr;
        if (params.overlay_text.has_value()) {
            auto overlay = std::make_unique<cch::tui::Overlay>();
            (void)overlay->add_child(std::make_unique<cch::tui::Text>(*params.overlay_text, 0, 0));
            overlay_ptr = overlay.get();
            REQUIRE(screen.add_overlay(std::move(overlay)));
            REQUIRE(screen.has_overlay_entries());
        }

        REQUIRE(screen.start());
        REQUIRE(screen.render());

        start_count_before_exit = terminal.start_count();
        stop_count_before_exit = terminal.stop_count();

        result.buffer_active_before = terminal.inner().modes().alternate_screen ? "alternate" : "normal";
        result.phases.push_back(make_snapshot("settledbeforeexit"));

        if (params.exit_output == "transcript") {
            if (overlay_ptr != nullptr) {
                screen.clear_overlays();
            }
            auto transferred = screen.take_children();
            (void)screen.take_layout_root();
            REQUIRE(screen.stop(cch::tui::TuiStopOptions{.preserve_screen = true}));
            result.phases.push_back(make_snapshot("switchstop"));

            cch::tui::MainScreen main_screen(terminal);
            main_screen.set_render_complete_sink([&](const support::ExpectedVoid& res) {
                if (res) ++root_render_count;
            });
            auto root_container = std::make_unique<cch::tui::Container>();
            for (auto& child : transferred) {
                (void)root_container->add_child(std::move(child));
            }
            REQUIRE(main_screen.add_child(
                    std::make_unique<CountingComponent>(std::move(root_container), component_render_counter)));
            REQUIRE(main_screen.render_final_presentation());
            result.phases.push_back(make_snapshot("finalrender"));

            const auto ops_before_final_stop = terminal.ops().size();
            REQUIRE(main_screen.stop(cch::tui::TuiStopOptions{.preserve_screen = false}));
            result.phases.push_back(make_snapshot("finalstop"));
            result.has_overlay_entries_after = main_screen.has_overlay_entries();

            std::string cleanup;
            for (std::size_t i = ops_before_final_stop; i < terminal.ops().size(); ++i) {
                if (terminal.ops()[i].kind == tests::ProtocolRecordingTerminal::OpKind::Write) {
                    cleanup += terminal.ops()[i].write_bytes;
                }
            }
            result.final_cursor_cleanup_writes = std::move(cleanup);
        } else {
            // resume-hint
            if (overlay_ptr != nullptr) {
                screen.clear_overlays();
            }
            const auto ops_before_final_stop = terminal.ops().size();
            REQUIRE(screen.stop(cch::tui::TuiStopOptions{.preserve_screen = true}));
            result.phases.push_back(make_snapshot("finalstop"));
            result.has_overlay_entries_after = screen.has_overlay_entries();

            std::string cleanup;
            for (std::size_t i = ops_before_final_stop; i < terminal.ops().size(); ++i) {
                if (terminal.ops()[i].kind == tests::ProtocolRecordingTerminal::OpKind::Write) {
                    cleanup += terminal.ops()[i].write_bytes;
                }
            }
            result.final_cursor_cleanup_writes = std::move(cleanup);
        }
    } else {
        // Regular mode
        cch::tui::MainScreen screen(terminal);
        REQUIRE(screen.add_child(std::move(doc_c)));
        REQUIRE(screen.add_child(std::move(pending_c)));
        REQUIRE(screen.add_child(std::move(status_c)));
        REQUIRE(screen.add_child(std::move(editor_c)));
        REQUIRE(screen.add_child(std::move(footer_c)));

        REQUIRE(screen.start());
        REQUIRE(screen.render());

        start_count_before_exit = terminal.start_count();
        stop_count_before_exit = terminal.stop_count();

        result.buffer_active_before = terminal.inner().modes().alternate_screen ? "alternate" : "normal";
        result.phases.push_back(make_snapshot("settledbeforeexit"));

        if (!params.pending_mutation.empty()) {
            for (const auto& mut : params.pending_mutation) {
                (void)doc_ptr->add_child(std::make_unique<cch::tui::Text>(mut, 0, 0));
            }
            result.phases.push_back(make_snapshot("pendingmutation"));
        }

        if (params.forced_render_now_before_stop) {
            screen.set_render_complete_sink([&](const support::ExpectedVoid& res) {
                if (res) ++root_render_count;
            });
            REQUIRE(screen.render());
            result.phases.push_back(make_snapshot("finalrender"));
        }

        const auto ops_before_final_stop = terminal.ops().size();
        REQUIRE(screen.stop(cch::tui::TuiStopOptions{.preserve_screen = false}));
        result.phases.push_back(make_snapshot("finalstop"));
        result.has_overlay_entries_after = screen.has_overlay_entries();

        std::string cleanup;
        for (std::size_t i = ops_before_final_stop; i < terminal.ops().size(); ++i) {
            if (terminal.ops()[i].kind == tests::ProtocolRecordingTerminal::OpKind::Write) {
                cleanup += terminal.ops()[i].write_bytes;
            }
        }
        result.final_cursor_cleanup_writes = std::move(cleanup);
    }

    result.buffer_active_after = terminal.inner().modes().alternate_screen ? "alternate" : "normal";
    result.start_count_delta = terminal.start_count() - start_count_before_exit;
    result.stop_count_delta = terminal.stop_count() - stop_count_before_exit;
    result.render_now_count_delta = root_render_count;

    std::size_t total_writes = 0;
    std::size_t total_bytes = 0;
    std::string all_writes_combined;
    for (const auto& op : terminal.ops()) {
        if (op.kind == tests::ProtocolRecordingTerminal::OpKind::Write) {
            ++total_writes;
            total_bytes += op.write_bytes.size();
            all_writes_combined += op.write_bytes;
        }
    }
    result.total_raw_writes_count = total_writes;
    result.total_raw_bytes = total_bytes;
    result.relevant_control_tokens = extract_control_tokens(all_writes_combined);

    auto trim_right = [](std::string_view s) {
        auto end = s.find_last_not_of(" \t\r\n");
        return end == std::string_view::npos ? std::string{} : std::string(s.substr(0, end + 1));
    };
    for (const auto& line : terminal.inner().scrollback()) {
        auto trimmed = trim_right(line);
        if (!trimmed.empty()) result.main_buffer_lines.push_back(std::move(trimmed));
    }
    for (const auto& line : terminal.inner().screen()) {
        auto trimmed = trim_right(line);
        if (!trimmed.empty()) result.main_buffer_lines.push_back(std::move(trimmed));
    }

    return result;
}

TEST_CASE("Native TUI public fullscreen exit output characterizes frozen artifact 33 scenarios",
        "[coding_agent][tui][exit][artifact33][issue1019][spec]") {
    const auto bundle = tests::read_pi_tui_evidence("fullscreen-exit-output.json");
    REQUIRE(bundle);
    const auto& scenarios = bundle->at("scenarios").get_array();

    for (std::size_t s_idx = 0; s_idx < 6; ++s_idx) {
        const auto& s = scenarios[s_idx];
        const auto name = s.at("name").get_string();
        DYNAMIC_SECTION("scenario: " << name) {
            const auto& dims = s.at("dimensions");
            const auto& inputs = s.at("inputs");
            const auto& exp = s.at("expected");

            auto initial_main_buffer = get_strings(inputs, "initialMainBuffer");
            auto document_lines = get_strings(inputs, "documentLines");
            auto editor_text = inputs.get_object().contains("editorText") ? inputs.at("editorText").get_string() : "";
            if (name == "overlay-dismissal-exit") {
                // Sourced from frozen capture harness (fullscreen-exit-output-scenarios.mts:354-356)
                if (initial_main_buffer.empty()) initial_main_buffer = {"Main buffer row"};
                if (document_lines.empty()) document_lines = {"User message"};
                if (editor_text.empty()) editor_text = "active prompt";
            }

            ExitScenarioParams params{
                    .name = name,
                    .columns = static_cast<std::size_t>(dims.at("columns").get_number()),
                    .rows = static_cast<std::size_t>(dims.at("rows").get_number()),
                    .initial_mode = inputs.at("initialMode").get_string(),
                    .exit_output = inputs.at("fullscreenExitOutput").get_string(),
                    .initial_main_buffer = std::move(initial_main_buffer),
                    .document_lines = std::move(document_lines),
                    .editor_text = std::move(editor_text),
                    .overlay_text = inputs.get_object().contains("overlayText")
                                            ? std::optional(inputs.at("overlayText").get_string())
                                            : std::nullopt,
                    .pending_mutation = {},
                    .forced_render_now_before_stop = false,
            };

            auto res = run_exit_scenario(params);

            if (exp.get_object().contains("bufferActiveTypeBefore")) {
                CHECK(res.buffer_active_before == exp.at("bufferActiveTypeBefore").get_string());
            }
            if (exp.get_object().contains("bufferActiveTypeAfter")) {
                CHECK(res.buffer_active_after == exp.at("bufferActiveTypeAfter").get_string());
            }
            if (exp.get_object().contains("startCountDelta")) {
                CHECK(res.start_count_delta == static_cast<std::size_t>(exp.at("startCountDelta").get_number()));
            }
            if (exp.get_object().contains("stopCountDelta")) {
                CHECK(res.stop_count_delta == static_cast<std::size_t>(exp.at("stopCountDelta").get_number()));
            }
            if (exp.get_object().contains("hasOverlayEntriesAfter")) {
                CHECK(res.has_overlay_entries_after == exp.at("hasOverlayEntriesAfter").get_boolean());
            }
            if (exp.get_object().contains("mainBufferLines")) {
                CHECK(res.main_buffer_lines == get_strings(exp, "mainBufferLines"));
            }
        }
    }
}

TEST_CASE("Native TUI fullscreen exit characterizes live artifact 35 fullscreen-transcript-short probe",
        "[coding_agent][tui][exit][artifact35][issue1019][probe][fullscreen_short]") {
    const auto bundle = tests::read_pi_tui_evidence("fullscreen-exit-live.json");
    REQUIRE(bundle);
    const auto& scenarios = bundle->at("scenarios").get_array();
    auto sc_it = std::find_if(scenarios.begin(), scenarios.end(), [](const auto& s) {
        return s.at("name").get_string() == "fullscreen-transcript-short";
    });
    REQUIRE(sc_it != scenarios.end());
    const auto& sc = *sc_it;
    const auto& dims = sc.at("dimensions");
    const auto& inputs = sc.at("inputs");
    const auto& exp = sc.at("expected");

    ExitScenarioParams params{
            .name = "fullscreen-transcript-short",
            .columns = static_cast<std::size_t>(dims.at("columns").get_number()),
            .rows = static_cast<std::size_t>(dims.at("rows").get_number()),
            .initial_mode = inputs.at("initialMode").get_string(),
            .exit_output = inputs.at("fullscreenExitOutput").get_string(),
            .initial_main_buffer = get_strings(inputs, "initialMainBuffer"),
            .document_lines = get_strings(inputs, "documentLines"),
            .editor_text = inputs.at("editorText").get_string(),
            .overlay_text = std::nullopt,
            .pending_mutation = get_strings(inputs, "pendingMutation"),
            .forced_render_now_before_stop = inputs.at("forcedRenderNowBeforeStop").get_boolean(),
    };

    SECTION("VirtualTerminal public interface and protocol snapshot probe") {
        auto res = run_exit_scenario(params);

        // Scenario-level expectations from live artifact 35:
        CHECK(res.buffer_active_before == exp.at("bufferActiveTypeBefore").get_string());
        CHECK(res.buffer_active_after == exp.at("bufferActiveTypeAfter").get_string());
        CHECK(res.start_count_delta == static_cast<std::size_t>(exp.at("startCountDelta").get_number()));
        CHECK(res.stop_count_delta == static_cast<std::size_t>(exp.at("stopCountDelta").get_number()));
        CHECK(res.render_now_count_delta == static_cast<std::size_t>(exp.at("renderNowCountDelta").get_number()));

        // Final cursor & modes:
        const auto& exp_final_cursor = exp.at("finalCursor");
        const auto& final_snap = res.phases.back();
        CHECK(final_snap.cursor_x == static_cast<std::size_t>(exp_final_cursor.at("x").get_number()));
        CHECK(final_snap.cursor_y == static_cast<std::size_t>(exp_final_cursor.at("y").get_number()));
        CHECK(final_snap.cursor_base_y == static_cast<std::size_t>(exp_final_cursor.at("baseY").get_number()));
        CHECK(final_snap.cursor_viewport_y == static_cast<std::size_t>(exp_final_cursor.at("viewportY").get_number()));
        CHECK(final_snap.cursor_hidden == !exp.at("finalCursorVisible").get_boolean());
        const auto& exp_final_modes = exp.at("finalModes");
        CHECK(final_snap.bracketed_paste_mode == exp_final_modes.at("bracketedPasteMode").get_boolean());
        CHECK(final_snap.alternate_screen == exp_final_modes.at("alternateScreen").get_boolean());
        CHECK(final_snap.wraparound_mode == exp_final_modes.at("wraparoundMode").get_boolean());

        // Final screen & full active buffer lines:
        CHECK(final_snap.screen_lines_with_spaces == get_strings(exp, "finalScreenLinesWithSpaces"));
        CHECK(final_snap.full_buffer_lines_with_spaces == get_strings(exp, "finalFullBufferLinesWithSpaces"));

        std::string exp_cleanup;
        for (const auto& w : exp.at("finalCursorCleanupWrites").get_array())
            exp_cleanup += w.get_string();
        CHECK(res.final_cursor_cleanup_writes == exp_cleanup);

        // Exact per-phase comparisons:
        const auto& exp_phases = exp.at("phases").get_array();
        REQUIRE(res.phases.size() == exp_phases.size());
        for (std::size_t p_idx = 0; p_idx < exp_phases.size(); ++p_idx) {
            const auto& ep = exp_phases[p_idx];
            const auto& sp = res.phases[p_idx];
            DYNAMIC_SECTION("phase " << p_idx << ": " << ep.at("phase").get_string()) {
                CHECK(sp.phase == ep.at("phase").get_string());
                CHECK(sp.active_buffer == ep.at("activeBuffer").get_string());
                CHECK(sp.cursor_x == static_cast<std::size_t>(ep.at("cursor").at("x").get_number()));
                CHECK(sp.cursor_y == static_cast<std::size_t>(ep.at("cursor").at("y").get_number()));
                CHECK(sp.cursor_base_y == static_cast<std::size_t>(ep.at("cursor").at("baseY").get_number()));
                CHECK(sp.cursor_viewport_y == static_cast<std::size_t>(ep.at("cursor").at("viewportY").get_number()));
                CHECK(sp.cursor_hidden == ep.at("cursorHidden").get_boolean());
                const auto& ep_modes = ep.at("modes");
                CHECK(sp.bracketed_paste_mode == ep_modes.at("bracketedPasteMode").get_boolean());
                CHECK(sp.alternate_screen == ep_modes.at("alternateScreen").get_boolean());
                CHECK(sp.wraparound_mode == ep_modes.at("wraparoundMode").get_boolean());
                CHECK(sp.screen_lines_with_spaces == get_strings(ep, "screenLinesWithSpaces"));
                CHECK(sp.full_buffer_lines_with_spaces == get_strings(ep, "fullBufferLinesWithSpaces"));
                CHECK(sp.start_count_delta == static_cast<std::size_t>(ep.at("startCountDelta").get_number()));
                CHECK(sp.stop_count_delta == static_cast<std::size_t>(ep.at("stopCountDelta").get_number()));
                CHECK(sp.render_now_count_delta == static_cast<std::size_t>(ep.at("renderNowCountDelta").get_number()));
                CHECK(sp.component_render_count == static_cast<std::size_t>(ep.at("renderNowCountDelta").get_number()));
            }
        }
    }

    SECTION("ProcessTerminal real PTY byte stream probe") {
        auto pty = tests::open_pseudo_terminal(40, 10);
        REQUIRE(pty);

        // 1. Explicitly configure slave termios with OPOST OFF (documented raw output)
        termios orig_termios{};
        REQUIRE(tcgetattr(pty->slave.get(), &orig_termios) == 0);
        termios raw_slave_termios = orig_termios;
        raw_slave_termios.c_oflag &= ~OPOST;
        REQUIRE(tcsetattr(pty->slave.get(), TCSANOW, &raw_slave_termios) == 0);

        // Snapshot initial fd flags and termios on slave
        const int initial_slave_flags = fcntl(pty->slave.get(), F_GETFL);
        termios initial_slave_termios{};
        REQUIRE(tcgetattr(pty->slave.get(), &initial_slave_termios) == 0);

        // 2. Initial shell banner written to SLAVE fd before start
        std::string expected_banner;
        for (const auto& line : params.initial_main_buffer) {
            const std::string banner_line = line + "\r\n";
            expected_banner += banner_line;
            REQUIRE(::write(pty->slave.get(), banner_line.data(), banner_line.size()) ==
                    static_cast<ssize_t>(banner_line.size()));
        }

        // 3. Read master independently to verify exact joined banner before starting ProcessTerminal
        std::string received_banner;
        while (received_banner.size() < expected_banner.size()) {
            char buf[256];
            const auto n = ::read(pty->master.get(), buf, sizeof(buf));
            if (n <= 0) break;
            received_banner.append(buf, static_cast<std::size_t>(n));
        }
        CHECK(received_banner == expected_banner);

        boost::asio::io_context io;
        cch::tui::ProcessTerminal pty_terminal({
                .input_fd = pty->slave.get(),
                .output_fd = pty->slave.get(),
                .executor = io.get_executor(),
        });

        auto doc_c = std::make_unique<cch::tui::Container>();
        for (const auto& line : params.document_lines) {
            (void)doc_c->add_child(std::make_unique<cch::tui::Text>(line, 0, 0));
        }
        auto pending_c = std::make_unique<cch::tui::Container>();
        auto status_c = std::make_unique<cch::tui::Container>();
        auto editor_c = std::make_unique<cch::tui::Container>();
        (void)editor_c->add_child(std::make_unique<cch::tui::Text>(params.editor_text, 0, 0));
        auto footer_c = std::make_unique<cch::tui::Container>();

        cch::coding_agent::tui::ChatViewportOptions vp_opts;
        vp_opts.document = std::make_unique<ComponentRef>(*doc_c);
        vp_opts.pending_messages = std::make_unique<ComponentRef>(*pending_c);
        vp_opts.status = std::make_unique<ComponentRef>(*status_c);
        vp_opts.editor = std::make_unique<ComponentRef>(*editor_c);
        vp_opts.footer = std::make_unique<ComponentRef>(*footer_c);
        vp_opts.scrollbar = cch::tui::ScrollbarMode::Hidden;
        auto viewport = cch::coding_agent::tui::create_chat_viewport(std::move(vp_opts));
        REQUIRE(viewport.has_value());

        cch::tui::AltScreen screen(pty_terminal);
        screen.set_layout_root(std::move(viewport->root));
        REQUIRE(screen.add_child(std::move(doc_c)));
        REQUIRE(screen.add_child(std::move(pending_c)));
        REQUIRE(screen.add_child(std::move(status_c)));
        REQUIRE(screen.add_child(std::move(editor_c)));
        REQUIRE(screen.add_child(std::move(footer_c)));

        REQUIRE(screen.start());
        REQUIRE(screen.render());
        const auto p0_bytes = cch::tests::read_available(pty->master.get());
        (void)p0_bytes;

        // Drain buffered input before exit matching product caller and document:
        // ProcessTerminal pushes the keyboard protocol on startup, so drain_input()
        // delivers the exact pop sequence for the active protocol.
        const std::string expected_drain_bytes = "\x1b[<u";
        REQUIRE(pty_terminal.drain_input());
        const auto drain_bytes = cch::tests::read_available(pty->master.get());
        CHECK(drain_bytes == expected_drain_bytes);

        auto transferred = screen.take_children();
        (void)screen.take_layout_root();
        REQUIRE(screen.stop(cch::tui::TuiStopOptions{.preserve_screen = true}));
        const auto p1_bytes = cch::tests::read_available(pty->master.get());

        cch::tui::MainScreen main_screen(pty_terminal);
        for (auto& child : transferred) {
            REQUIRE(main_screen.add_child(std::move(child)));
        }
        REQUIRE(main_screen.render_final_presentation());
        const auto p2_bytes = cch::tests::read_available(pty->master.get());

        REQUIRE(main_screen.stop(cch::tui::TuiStopOptions{.preserve_screen = false}));
        const auto p3_bytes = cch::tests::read_available(pty->master.get());

        // Drain phase boundaries and compare fd flags and termios restoration at finish
        const int final_slave_flags = fcntl(pty->slave.get(), F_GETFL);
        termios final_slave_termios{};
        REQUIRE(tcgetattr(pty->slave.get(), &final_slave_termios) == 0);
        CHECK(final_slave_flags == initial_slave_flags);
        CHECK(cch::tests::same_terminal_state(final_slave_termios, initial_slave_termios));

        auto join_strings = [](const std::vector<std::string>& strs) {
            std::string res;
            for (const auto& s : strs)
                res += s;
            return res;
        };

        const auto& exp_phases = exp.at("phases").get_array();
        const auto exp_p1_bytes = join_strings(get_strings(exp_phases[1], "phaseXtermWrites"));
        const auto exp_p2_bytes = join_strings(get_strings(exp_phases[2], "phaseXtermWrites"));
        const auto exp_p3_bytes = join_strings(get_strings(exp_phases[3], "phaseXtermWrites"));

        // Compare FULL joined actual exit bytes against artifact 35 phaseXtermWrites baseline
        CHECK(p1_bytes == exp_p1_bytes);
        CHECK(p2_bytes == exp_p2_bytes);
        CHECK(p3_bytes == exp_p3_bytes);

        const auto p1_tokens = extract_control_tokens(p1_bytes);
        const auto p2_tokens = extract_control_tokens(p2_bytes);
        const auto p3_tokens = extract_control_tokens(p3_bytes);

        CHECK(p1_tokens == get_strings(exp_phases[1], "phaseControlTokens"));
        CHECK(p2_tokens == get_strings(exp_phases[2], "phaseControlTokens"));
        CHECK(p3_tokens == get_strings(exp_phases[3], "phaseControlTokens"));
    }
}
