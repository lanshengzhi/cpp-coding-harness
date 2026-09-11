// P21: the startup-TUI host (pi `startup-ui.ts` + `session-picker.ts`
// subset) — the `--resume` session picker and the boot missing-cwd
// Continue/Cancel prompt — driven through the VirtualTerminal, with the G5
// controller-default theme init (settings-resolved theme or the COLORFGBG
// env default; theme registration skipped). No live credentials, no network
// validation.

#include <catch2/catch_test_macros.hpp>

#include "cli/StartupTui.hpp"

#include "support/EnvVarGuard.hpp"
#include "support/PumpUntil.hpp"
#include "support/TempWorkspace.hpp"
#include "support/Json.hpp"

#include "coding_agent/SessionDiscovery.hpp"
#include "coding_agent/SessionPathPolicy.hpp"
#include <cch/tui/VirtualTerminal.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>

#include <filesystem>
#include <chrono>
#include <format>
#include <fstream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace cch;
using tests::drain_ready;

namespace {

[[nodiscard]] std::string visible_screen(const tui::VirtualTerminal& terminal) {
    std::string text;
    for (const auto& line : terminal.screen()) {
        text.append(line);
        text.push_back('\n');
    }
    return text;
}

/// The fg color of the first cell at/after `text` in the screen (a
/// `color_at_text` helper like ThemeTest's).
[[nodiscard]] std::string color_at_text(
    const tui::VirtualTerminal& terminal,
    std::string_view text) {
    const auto screen = terminal.screen();
    for (std::size_t row = 0; row < screen.size(); ++row) {
        const auto column = screen[row].find(text);
        if (column != std::string::npos) {
            REQUIRE(column < terminal.cells()[row].size());
            return terminal.cells()[row][column].style.fg_color;
        }
    }
    REQUIRE(false);
    return {};
}

/// A TrueColor VirtualTerminal so theme colors assert as their exact RGB
/// escapes (pi's `theme.ts` paints with the truecolor prefix at this
/// capability).
[[nodiscard]] tui::VirtualTerminalOptions true_color_options() {
    tui::VirtualTerminalOptions options{.columns = 100, .rows = 40};
    options.capabilities.color = tui::TerminalColorCapability::TrueColor;
    return options;
}

/// Format a system clock time point as an ISO-8601 UTC timestamp without
/// fractional seconds (pi/C++ session header and entry timestamp shape).
[[nodiscard]] std::string format_iso_timestamp(std::chrono::system_clock::time_point time) {
    const auto time_point_s = std::chrono::floor<std::chrono::seconds>(time);
    const auto days = std::chrono::floor<std::chrono::days>(time_point_s);
    const std::chrono::year_month_day ymd{days};
    const auto time_of_day = std::chrono::hh_mm_ss{time_point_s - days};
    return std::format("{:04d}-{:02d}-{:02d}T{:02d}:{:02d}:{:02d}Z",
            static_cast<int>(ymd.year()),
            static_cast<int>(static_cast<unsigned>(ymd.month())),
            static_cast<int>(static_cast<unsigned>(ymd.day())),
            static_cast<int>(time_of_day.hours().count()),
            static_cast<int>(time_of_day.minutes().count()),
            static_cast<int>(time_of_day.seconds().count()));
}
struct SessionTimestamp {
    std::string iso;
    std::int64_t ms{0};
};

/// Derive the canonical ISO-8601 string and epoch millisecond representations
/// from a filesystem modification time.
[[nodiscard]] SessionTimestamp session_timestamp(std::filesystem::file_time_type modified) {
    const auto sys_time = std::chrono::file_clock::to_sys(modified);
    return SessionTimestamp{
            .iso = format_iso_timestamp(sys_time),
            .ms = std::chrono::duration_cast<std::chrono::milliseconds>(sys_time.time_since_epoch()).count(),
    };
}

/// Write a session file under the workspace-keyed default directory of
/// `sessions_root` with explicit entry timestamps reflecting `modified`;
/// returns the file path.
[[nodiscard]] std::filesystem::path write_session(const std::filesystem::path& sessions_root,
        const std::filesystem::path& workspace,
        std::string id,
        std::string first_message,
        std::filesystem::file_time_type modified) {
    const auto directory = sessions_root / coding_agent::session_paths::encode_workspace_key(workspace);
    std::filesystem::create_directories(directory);
    const auto path = directory / (id + ".jsonl");
    const auto timestamp = session_timestamp(modified);

    std::ofstream out(path, std::ios::trunc);
    REQUIRE(out.is_open());
    const auto header_json = support::write_json(support::JsonValue::object_t{
            {"type", "session"},
            {"version", 3},
            {"id", id},
            {"timestamp", timestamp.iso},
            {"cwd", workspace.string()},
            {"provider", "fake"},
            {"model", "fake-model"},
    });
    REQUIRE(header_json.has_value());
    out << *header_json << "\n";

    const auto message_json = support::write_json(support::JsonValue::object_t{
            {"type", "message"},
            {"id", "m-" + id},
            {"parentId", nullptr},
            {"timestamp", timestamp.iso},
            {"message",
                    support::JsonValue::object_t{
                            {"role", "user"},
                            {"content", std::move(first_message)},
                            {"timestamp", static_cast<double>(timestamp.ms)},
                    }},
    });
    REQUIRE(message_json.has_value());
    out << *message_json << "\n";
    out.close();

    std::error_code ec;
    std::filesystem::last_write_time(path, modified, ec);
    REQUIRE_FALSE(ec);
    return path;
}

/// The effective workspace-keyed default directory for `workspace`.
[[nodiscard]] std::filesystem::path default_directory(
    const std::filesystem::path& sessions_root,
    const std::filesystem::path& workspace) {
    return sessions_root /
        coding_agent::session_paths::encode_workspace_key(workspace);
}

/// A running startup-TUI host: terminal first (it outlives the io_context,
/// whose shutdown destroys the coroutine frame and its Tui last).
struct Running {
    tui::VirtualTerminal terminal{true_color_options()};
    boost::asio::io_context io;
    std::optional<support::Expected<cli::StartupPickerResult>> picker_result;
    std::optional<support::Expected<bool>> prompt_result;
};

} // namespace

TEST_CASE("startup TUI: the --resume picker lists sessions and selects one", "[cli][startup-tui][issue417][spec]") {
    tests::TempWorkspace agent_dir;
    tests::TempWorkspace workspace;
    const auto sessions_root = agent_dir.path() / "sessions";
    using namespace std::chrono;
    const auto now = std::filesystem::file_time_type::clock::now();
    const auto older = write_session(
        sessions_root, workspace.path(), "alpha-session",
        "Alpha first message", now - seconds(60));
    const auto newer = write_session(
        sessions_root, workspace.path(), "beta-session",
        "Beta first message", now - seconds(30));
    const auto local = default_directory(sessions_root, workspace.path());

    Running running;
    boost::asio::co_spawn(
        running.io,
        cli::run_startup_session_picker(
            running.terminal,
            {
                .agent_config_directory = agent_dir.path(),
                .theme_setting = std::nullopt,
            },
            [local] {
                return coding_agent::session_discovery::list_sessions_info(
                    local, std::nullopt);
            },
            [sessions_root] {
                return coding_agent::session_discovery::
                    list_all_sessions_info(sessions_root, std::nullopt);
            }),
        [&](std::exception_ptr exception,
            support::Expected<cli::StartupPickerResult> result) {
            CHECK(exception == nullptr);
            running.picker_result.emplace(std::move(result));
        });
    drain_ready(running.io);

    // The startup main screen renders the session selector with the
    // current-folder scope (pi selectSession's default).
    const auto screen = visible_screen(running.terminal);
    CHECK(screen.find("Resume Session (Current Folder)") != std::string::npos);
    CHECK(screen.find("Alpha first message") != std::string::npos);
    CHECK(screen.find("Beta first message") != std::string::npos);

    // The newest session is selected first; Enter resumes it.
    REQUIRE(running.terminal.inject_input("\r"));
    drain_ready(running.io);
    REQUIRE(running.picker_result);
    REQUIRE(*running.picker_result);
    CHECK((*running.picker_result)->outcome == cli::StartupPickerOutcome::Selected);
    CHECK((*running.picker_result)->session_path == newer);
    CHECK(older != newer);
}

TEST_CASE("startup TUI: session creation writes explicit entry timestamps matching configured test times",
        "[cli][startup-tui][issue565]") {
    tests::TempWorkspace agent_dir;
    tests::TempWorkspace workspace;
    const auto sessions_root = agent_dir.path() / "sessions";
    using namespace std::chrono;
    const auto now = std::filesystem::file_time_type::clock::now();
    const auto older_time = now - seconds(120);
    const auto newer_time = now - seconds(30);
    const auto expected = session_timestamp(older_time);

    const auto older_path =
            write_session(sessions_root, workspace.path(), "alpha-session", "Alpha message", older_time);
    const auto newer_path = write_session(sessions_root, workspace.path(), "beta-session", "Beta message", newer_time);
    CHECK(older_path != newer_path);

    std::ifstream file(older_path);
    REQUIRE(file.is_open());
    std::string header_line;
    std::string entry_line;
    REQUIRE(std::getline(file, header_line));
    REQUIRE(std::getline(file, entry_line));

    auto parsed_entry = support::read_json(entry_line);
    REQUIRE(parsed_entry.has_value());
    REQUIRE(parsed_entry->holds<support::JsonValue::object_t>());
    const auto& entry_obj = parsed_entry->get_object();
    const auto timestamp_it = entry_obj.find("timestamp");
    REQUIRE(timestamp_it != entry_obj.end());
    REQUIRE(timestamp_it->second.holds<std::string>());
    CHECK(timestamp_it->second.get_string() == expected.iso);

    const auto message_it = entry_obj.find("message");
    REQUIRE(message_it != entry_obj.end());
    REQUIRE(message_it->second.holds<support::JsonValue::object_t>());
    const auto& message_obj = message_it->second.get_object();
    const auto msg_ts_it = message_obj.find("timestamp");
    REQUIRE(msg_ts_it != message_obj.end());
    REQUIRE(msg_ts_it->second.holds<double>());
    CHECK(static_cast<std::int64_t>(msg_ts_it->second.get<double>()) == expected.ms);

    // Verify session discovery deterministic ordering (newer sorts ahead of older)
    const auto local = default_directory(sessions_root, workspace.path());
    const auto sessions = coding_agent::session_discovery::list_sessions_info(local, std::nullopt);
    REQUIRE(sessions.size() == 2);
    CHECK(sessions[0].id == "beta-session");
    CHECK(sessions[1].id == "alpha-session");
    CHECK(sessions[0].modified > sessions[1].modified);
}

TEST_CASE(
        "startup TUI: the --resume picker cancels on Escape and toggles scope", "[cli][startup-tui][issue417][spec]") {
    tests::TempWorkspace agent_dir;
    tests::TempWorkspace workspace;
    tests::TempWorkspace other_workspace;
    const auto sessions_root = agent_dir.path() / "sessions";
    using namespace std::chrono;
    const auto now = std::filesystem::file_time_type::clock::now();
    (void)write_session(
        sessions_root, workspace.path(), "local-session",
        "Local first message", now - seconds(60));
    (void)write_session(
        sessions_root, other_workspace.path(), "remote-session",
        "Remote first message", now - seconds(30));
    const auto local = default_directory(sessions_root, workspace.path());

    Running running;
    boost::asio::co_spawn(
        running.io,
        cli::run_startup_session_picker(
            running.terminal,
            {
                .agent_config_directory = agent_dir.path(),
                .theme_setting = std::nullopt,
            },
            [local] {
                return coding_agent::session_discovery::list_sessions_info(
                    local, std::nullopt);
            },
            [sessions_root] {
                return coding_agent::session_discovery::
                    list_all_sessions_info(sessions_root, std::nullopt);
            }),
        [&](std::exception_ptr exception,
            support::Expected<cli::StartupPickerResult> result) {
            CHECK(exception == nullptr);
            running.picker_result.emplace(std::move(result));
        });
    drain_ready(running.io);

    // The current-folder scope hides the other workspace's session; Tab
    // toggles to the All scope (pi tui.input.tab).
    CHECK(visible_screen(running.terminal).find("Remote first message") == std::string::npos);
    REQUIRE(running.terminal.inject_input("\t"));
    drain_ready(running.io);
    const auto all_screen = visible_screen(running.terminal);
    CHECK(all_screen.find("Resume Session (All)") != std::string::npos);
    // The All scope renders each session's cwd, so the long isolated temp
    // path can truncate the message column; assert on the stable prefix.
    CHECK(all_screen.find("Remote fi") != std::string::npos);

    // Escape cancels the picker (pi tui.select.cancel → selectSession null).
    REQUIRE(running.terminal.inject_input("\x1b"));
    REQUIRE(running.terminal.flush_input());
    drain_ready(running.io);
    REQUIRE(running.picker_result);
    REQUIRE(*running.picker_result);
    CHECK((*running.picker_result)->outcome == cli::StartupPickerOutcome::Cancelled);
}

TEST_CASE("startup TUI: the boot missing-cwd prompt shows Continue/Cancel with pi's verbatim text",
        "[cli][startup-tui][issue417][spec]") {
    tests::TempWorkspace agent_dir;
    tests::TempWorkspace fallback;
    tests::TempWorkspace vanished;

    // pi formatMissingSessionCwdPrompt, verbatim.
    const auto title = "cwd from session file does not exist\n" +
        vanished.path().string() + "\n\ncontinue in current cwd\n" +
        fallback.path().string();

    Running running;
    boost::asio::co_spawn(
        running.io,
        cli::run_startup_missing_cwd_prompt(
            running.terminal,
            {
                .agent_config_directory = agent_dir.path(),
                .theme_setting = std::nullopt,
            },
            title),
        [&](std::exception_ptr exception, support::Expected<bool> result) {
            CHECK(exception == nullptr);
            running.prompt_result.emplace(std::move(result));
        });
    drain_ready(running.io);

    const auto screen = visible_screen(running.terminal);
    CHECK(screen.find("cwd from session file does not exist") != std::string::npos);
    CHECK(screen.find(vanished.path().string()) != std::string::npos);
    CHECK(screen.find("continue in current cwd") != std::string::npos);
    CHECK(screen.find(fallback.path().string()) != std::string::npos);
    CHECK(screen.find("Continue") != std::string::npos);
    CHECK(screen.find("Cancel") != std::string::npos);

    // Escape cancels (pi Cancel → undefined → the boot exits 0).
    REQUIRE(running.terminal.inject_input("\x1b"));
    REQUIRE(running.terminal.flush_input());
    drain_ready(running.io);
    REQUIRE(running.prompt_result);
    REQUIRE(*running.prompt_result);
    CHECK_FALSE(**running.prompt_result);
}

TEST_CASE(
        "startup TUI: the boot missing-cwd Continue resolves the fallback cwd", "[cli][startup-tui][issue417][spec]") {
    tests::TempWorkspace agent_dir;
    tests::TempWorkspace fallback;
    tests::TempWorkspace vanished;

    const auto title = "cwd from session file does not exist\n" +
        vanished.path().string() + "\n\ncontinue in current cwd\n" +
        fallback.path().string();

    Running running;
    boost::asio::co_spawn(
        running.io,
        cli::run_startup_missing_cwd_prompt(
            running.terminal,
            {
                .agent_config_directory = agent_dir.path(),
                .theme_setting = std::nullopt,
            },
            title),
        [&](std::exception_ptr exception, support::Expected<bool> result) {
            CHECK(exception == nullptr);
            running.prompt_result.emplace(std::move(result));
        });
    drain_ready(running.io);

    // The first option (Continue) is selected; Enter resolves true.
    REQUIRE(running.terminal.inject_input("\r"));
    drain_ready(running.io);
    REQUIRE(running.prompt_result);
    REQUIRE(*running.prompt_result);
    CHECK(**running.prompt_result);
}

TEST_CASE("startup TUI: theme init resolves the settings theme (G5 controller default)",
        "[cli][startup-tui][theme][issue417][spec]") {
    tests::TempWorkspace agent_dir;
    tests::TempWorkspace fallback;
    tests::TempWorkspace vanished;
    const auto title = "cwd from session file does not exist\n" +
        vanished.path().string() + "\n\ncontinue in current cwd\n" +
        fallback.path().string();

    // pi startup-ui.ts: `initTheme(resolveThemeSetting(settings theme) ??
    // detected)`. The settings theme wins; the light accent is teal
    // (#5a8080), the dark accent #8abeb7.
    Running running;
    boost::asio::co_spawn(
        running.io,
        cli::run_startup_missing_cwd_prompt(
            running.terminal,
            {
                .agent_config_directory = agent_dir.path(),
                .theme_setting = "light",
            },
            title),
        [&](std::exception_ptr exception, support::Expected<bool> result) {
            CHECK(exception == nullptr);
            running.prompt_result.emplace(std::move(result));
        });
    drain_ready(running.io);
    CHECK(color_at_text(running.terminal, "Continue") == "38;2;90;128;128");
    REQUIRE(running.terminal.inject_input("\x1b"));
    REQUIRE(running.terminal.flush_input());
    drain_ready(running.io);
    REQUIRE(running.prompt_result);
}

TEST_CASE("startup TUI: theme init falls back to the COLORFGBG env default when unset",
        "[cli][startup-tui][theme][issue417][spec]") {
    tests::TempWorkspace agent_dir;
    tests::TempWorkspace fallback;
    tests::TempWorkspace vanished;
    const auto title = "cwd from session file does not exist\n" +
        vanished.path().string() + "\n\ncontinue in current cwd\n" +
        fallback.path().string();

    // pi startup-ui.ts: no settings theme → the detected terminal theme.
    // COLORFGBG "0;15" (background index 15, white) resolves to light; the
    // light accent is teal (#5a8080).
    tests::EnvVarGuard colorfgbg{"COLORFGBG"};
    colorfgbg.set("0;15");

    Running running;
    boost::asio::co_spawn(
        running.io,
        cli::run_startup_missing_cwd_prompt(
            running.terminal,
            {
                .agent_config_directory = agent_dir.path(),
                .theme_setting = std::nullopt,
            },
            title),
        [&](std::exception_ptr exception, support::Expected<bool> result) {
            CHECK(exception == nullptr);
            running.prompt_result.emplace(std::move(result));
        });
    drain_ready(running.io);
    CHECK(color_at_text(running.terminal, "Continue") == "38;2;90;128;128");
    REQUIRE(running.terminal.inject_input("\x1b"));
    REQUIRE(running.terminal.flush_input());
    drain_ready(running.io);
    REQUIRE(running.prompt_result);
}

TEST_CASE("startup TUI: theme init does not register resource themes", "[cli][startup-tui][theme][issue417][spec]") {
    tests::TempWorkspace agent_dir;
    tests::TempWorkspace fallback;
    tests::TempWorkspace vanished;
    const auto title = "cwd from session file does not exist\n" +
        vanished.path().string() + "\n\ncontinue in current cwd\n" +
        fallback.path().string();

    // A settings theme that exists only as a project `.pi/themes` resource
    // is not registered by the startup host (pi `setRegisteredThemes(
    // loadStartupThemes(...))` is skipped per the G5 record): the boot init
    // falls back to dark (#8abeb7 accent) instead of resolving the name.
    std::filesystem::create_directories(agent_dir.path() / "themes");
    {
        std::ofstream project_theme(
            agent_dir.path() / "themes" / "custom.json", std::ios::binary);
        project_theme << R"({"name": "custom", "colors": {"accent": "#ff0000"}})";
    }
    // The custom theme is NOT under <agent_config_directory>/themes — only
    // project-style discovery would see it. Point the settings at it.
    Running running;
    boost::asio::co_spawn(
        running.io,
        cli::run_startup_missing_cwd_prompt(
            running.terminal,
            {
                .agent_config_directory = agent_dir.path() / "config",
                .theme_setting = "custom",
            },
            title),
        [&](std::exception_ptr exception, support::Expected<bool> result) {
            CHECK(exception == nullptr);
            running.prompt_result.emplace(std::move(result));
        });
    drain_ready(running.io);
    // Dark fallback accent (the registered name never resolves).
    CHECK(color_at_text(running.terminal, "Continue") == "38;2;138;190;183");
    REQUIRE(running.terminal.inject_input("\x1b"));
    REQUIRE(running.terminal.flush_input());
    drain_ready(running.io);
    REQUIRE(running.prompt_result);
}
