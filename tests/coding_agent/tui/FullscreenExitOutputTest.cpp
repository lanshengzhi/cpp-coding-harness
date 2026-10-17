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
#include <cch/tui/Editor.hpp>
#include <cch/tui/Overlay.hpp>
#include <cch/tui/VirtualTerminal.hpp>

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

} // namespace

// ── 1. SettingsManager Tests ─────────────────────────────────────────────────

TEST_CASE("SettingsManager parses and persists fullscreenExitOutput", "[coding_agent][settings][exit][issue1019][spec]") {
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
            if (c == '\'') replaced += "'\\''";
            else replaced += c;
        }
        CHECK(*cmd == "pike --session-dir '" + replaced + "' --session quotes-id");
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
            {"Existing main buffer line 1", "Existing main buffer line 2"}, {.column = 0, .row = 2}).has_value());

    auto created = runtime.run(create_session_async(runtime, session_options(temp, tests::make_scripted_fake_provider())));
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
    REQUIRE(pump_until(io, [&] {
        return visible_screen(terminal).find("fake: Hello from user prompt") != std::string::npos;
    }));

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
            {"Existing main buffer line 1", "Existing main buffer line 2"}, {.column = 0, .row = 2}).has_value());

    auto created = runtime.run(create_session_async(runtime, session_options(temp, tests::make_scripted_fake_provider())));
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
    REQUIRE(pump_until(io, [&] {
        return visible_screen(terminal).find("fake: Secret user message") != std::string::npos;
    }));

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
    REQUIRE(terminal.seed_shell_content(
            {"Existing regular line 1"}, {.column = 0, .row = 1}).has_value());

    auto created = runtime.run(create_session_async(runtime, session_options(temp, tests::make_scripted_fake_provider())));
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
    REQUIRE(pump_until(io, [&] {
        return visible_screen(terminal).find("fake: Regular mode prompt") != std::string::npos;
    }));

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

TEST_CASE("Native TUI fullscreen exit dismisses active overlays before stop",
        "[coding_agent][tui][exit][fullscreen][overlay][issue1019][spec]") {
    tests::TempWorkspace temp;
    tests::EnvVarGuard agent_dir_guard{"PIKE_AGENT_DIR", temp.path().string()};
    tests::RuntimeFixture runtime;
    boost::asio::io_context io;

    tui::VirtualTerminal terminal({.columns = 80, .rows = 24});
    REQUIRE(terminal.seed_shell_content(
            {"Existing line"}, {.column = 0, .row = 1}).has_value());

    auto created = runtime.run(create_session_async(runtime, session_options(temp, tests::make_scripted_fake_provider())));
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
    REQUIRE(pump_until(io, [&] {
        return visible_screen(terminal).find("fake: Conversation before overlay") != std::string::npos;
    }));

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
    REQUIRE(terminal.seed_shell_content(
            {"Pre-existing shell prompt"}, {.column = 0, .row = 1}).has_value());

    auto created = runtime.run(create_session_async(runtime, session_options(temp, tests::make_scripted_fake_provider())));
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
    REQUIRE(terminal.seed_shell_content(
            {"Pre-existing line"}, {.column = 0, .row = 1}).has_value());

    auto created = runtime.run(create_session_async(runtime, session_options(temp, tests::make_scripted_fake_provider())));
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
        REQUIRE(pump_until(io, [&] {
            return visible_screen(terminal).find("fake: " + prompt) != std::string::npos;
        }));
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
