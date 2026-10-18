// #1020: the fullscreen settings rows (fullscreenScrollbar,
// fullscreenCopyOnSelect, fullscreenWheelScrollLines) applied to the live
// product consumers through `/settings`. Every case drives the real
// `InteractiveEngine` fullscreen root: the transcript `ScrollView` reservation,
// the `AltScreen` copy callback and Ctrl+X fall-through, and the wheel line
// count. The frozen artifact 36 comparison lives in
// `FullscreenSettingsEvidenceTest.cpp`; these are the distinguishing cases
// named by the ticket.

#include "coding_agent/AgentSession.hpp"
#include "coding_agent/SessionTarget.hpp"
#include "coding_agent/runtime/SessionFactory.hpp"
#include "coding_agent/tui/InteractiveEngine.hpp"
#include "coding_agent/tui/InteractiveMode.hpp"
#include "coding_agent/tui/InteractiveSessionRun.hpp"
#include "coding_agent/tui/TestTuiActionSink.hpp"
#include "support/ModelsFixture.hpp"
#include "support/PumpUntil.hpp"
#include "support/RuntimeFixture.hpp"
#include "support/RuntimeLoopDriver.hpp"
#include "support/TempWorkspace.hpp"

#include <cch/ai/Content.hpp>
#include <cch/coding_agent/ModelRuntime.hpp>
#include <cch/coding_agent/Settings.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <cstddef>
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
using tests::pump_until;

namespace {

using cch::coding_agent::tui::testing::ActionSinkRecorder;

[[nodiscard]] std::string visible_screen(const tui::VirtualTerminal& terminal) {
    std::string text;
    for (const auto& line : terminal.screen()) {
        text.append(line);
        text.push_back('\n');
    }
    return text;
}

[[nodiscard]] std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream file(path);
    return std::string{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

[[nodiscard]] bool is_scrollbar_glyph(std::string_view grapheme) {
    // U+2502 track, U+2503 thumb, U+2588 expanded active thumb.
    return grapheme == "\xe2\x94\x82" || grapheme == "\xe2\x94\x83" || grapheme == "\xe2\x96\x88";
}

/// The rows that carry a scrollbar cell in the last column: `always` paints
/// one on every transcript row, `auto` only during a transient reveal, and
/// `hidden` never (pi `isScrollbarVisible`).
[[nodiscard]] std::size_t scrollbar_rows(const tui::VirtualTerminal& terminal) {
    std::size_t rows = 0;
    for (const auto& cells : terminal.cells()) {
        if (!cells.empty() && is_scrollbar_glyph(cells.back().grapheme)) ++rows;
    }
    return rows;
}

/// The visible rows without the scrollbar column, so a transient `auto` bar
/// cannot hide a content match while the wheel window is compared.
[[nodiscard]] std::vector<std::string> content_rows(const tui::VirtualTerminal& terminal) {
    std::vector<std::string> rows;
    rows.reserve(terminal.cells().size());
    for (const auto& cells : terminal.cells()) {
        std::string row;
        for (std::size_t column = 0; column + 1 < cells.size(); ++column)
            row += cells[column].grapheme;
        rows.push_back(std::move(row));
    }
    return rows;
}

/// The row index of the `fake: N` transcript line, or nothing while the line
/// is off screen. Scrolling moves the line down by the wheel line count, so its
/// index before and after one notch measures the applied step without
/// depending on how much content scrolled in above it.
[[nodiscard]] std::optional<std::size_t> fake_turn_row(const std::vector<std::string>& rows, std::size_t turn) {
    const auto needle = "fake: " + std::to_string(turn);
    for (std::size_t index = 0; index < rows.size(); ++index) {
        if (rows[index].find(needle) != std::string::npos) return index;
    }
    return std::nullopt;
}

/// The turn number of the topmost visible `fake: N` row, which the wheel keeps
/// on screen: it sits at the top of the transcript and only moves down.
[[nodiscard]] std::size_t topmost_fake_turn(const std::vector<std::string>& rows) {
    for (const auto& row : rows) {
        const auto found = row.find("fake: ");
        if (found == std::string::npos) continue;
        std::size_t turn = 0;
        for (std::size_t index = found + 6; index < row.size(); ++index) {
            const char digit = row[index];
            if (digit < '0' || digit > '9') break;
            turn = turn * 10 + static_cast<std::size_t>(digit - '0');
        }
        if (turn > 0) return turn;
    }
    FAIL("the transcript shows no `fake: N` row to measure the wheel step");
    return 0;
}

/// The row shift one wheel notch applied to `turn`, or nothing when the line
/// never moved.
[[nodiscard]] std::optional<std::size_t> wheel_shift(
        const std::vector<std::string>& rows, std::size_t turn, std::size_t before) {
    const auto after = fake_turn_row(rows, turn);
    if (!after || *after == before) return std::nullopt;
    return *after - before;
}

/// The first wrapped row of the long unbroken user message: how many `x`
/// columns the transcript layout gave it, and what its last cell holds. With
/// `always` the reserved bar column costs the row one `x` and paints the bar
/// last; with `hidden` the unreserved layout fits one more `x`.
struct LongRowShape {
    std::size_t columns{0};
    std::string last_grapheme;
};

[[nodiscard]] LongRowShape long_row_shape(const tui::VirtualTerminal& terminal, std::string_view prefix) {
    for (const auto& cells : terminal.cells()) {
        std::string text;
        for (const auto& cell : cells)
            text += cell.grapheme;
        if (text.find(prefix) == std::string::npos) continue;
        return LongRowShape{static_cast<std::size_t>(std::count(text.begin(), text.end(), 'x')), cells.back().grapheme};
    }
    FAIL("the long user message row is not visible");
    return {};
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

[[nodiscard]] coding_agent::tui::InteractiveSessionRun make_run(coding_agent::AgentSession& session,
        const std::filesystem::path& agent_config_directory,
        coding_agent::tui::TuiActionSink action_sink) {
    return coding_agent::tui::InteractiveSessionRunBuilder{}
            .with_session(session)
            .with_agent_config_directory(agent_config_directory)
            .with_tui_mode(coding_agent::TuiMode::Fullscreen)
            .with_action_sink(std::move(action_sink))
            .build();
}

/// `run_interactive_mode` owns its engine privately; these cases need the
/// engine addressable for `switch_tui_mode`, so they drive the same sequence
/// the host runs (`start` → trust warning → exit wait → `finish`).
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

/// Submit `count` turns and wait for each fake assistant line. The transcript
/// then overflows the fullscreen viewport, which the scrollbar and wheel
/// cases need.
void submit_turns(tui::VirtualTerminal& terminal, boost::asio::io_context& io, std::size_t count) {
    for (std::size_t index = 1; index <= count; ++index) {
        const auto prompt = std::to_string(index);
        REQUIRE(terminal.inject_input(prompt + "\r"));
        REQUIRE(pump_until(io, [&] { return visible_screen(terminal).find("fake: " + prompt) != std::string::npos; }));
    }
}

/// Open `/settings` and move onto the row whose description starts with
/// `description` (only the focused row renders its description).
void focus_settings_row(tui::VirtualTerminal& terminal,
        boost::asio::io_context& io,
        std::size_t down_steps,
        std::string_view description) {
    REQUIRE(terminal.inject_input("/settings\r"));
    drain_ready(io);
    for (std::size_t step = 0; step < down_steps; ++step) {
        REQUIRE(terminal.inject_input("\x1b[B"));
    }
    REQUIRE(pump_until(io, [&] { return visible_screen(terminal).find(description) != std::string::npos; }));
}

void close_settings(tui::VirtualTerminal& terminal, boost::asio::io_context& io, std::string_view description) {
    REQUIRE(terminal.inject_input("\x1b"));
    REQUIRE(terminal.flush_input());
    drain_ready(io);
    REQUIRE_FALSE(visible_screen(terminal).find(description) != std::string::npos);
}

/// Drag-select `length` cells starting at the first `needle` cell of the row
/// that contains it; mouse reports are 1-based.
void select_cells(
        tui::VirtualTerminal& terminal, boost::asio::io_context& io, std::string_view needle, std::size_t length) {
    const auto& screen = terminal.screen();
    std::size_t row = 0;
    std::size_t column = std::string::npos;
    for (; row < screen.size(); ++row) {
        column = screen[row].find(needle);
        if (column != std::string::npos) break;
    }
    REQUIRE(row < screen.size());
    REQUIRE(column != std::string::npos);
    const auto start = std::to_string(column + 1);
    const auto end = std::to_string(column + length);
    const auto y = std::to_string(row + 1);
    REQUIRE(terminal.inject_input("\x1b[<0;" + start + ";" + y + "M"));
    REQUIRE(terminal.inject_input("\x1b[<32;" + end + ";" + y + "M"));
    REQUIRE(terminal.inject_input("\x1b[<0;" + end + ";" + y + "m"));
    drain_ready(io);
}

struct FullscreenRun {
    tests::RuntimeFixture runtime;
    tests::TempWorkspace workspace;
    tests::TempWorkspace config;
    std::shared_ptr<ActionSinkRecorder> actions{std::make_shared<ActionSinkRecorder>()};
    /// Owns the session the engine borrows; declared before the engine so the
    /// engine (and its coroutine) is destroyed first.
    std::optional<support::Expected<coding_agent::CreateAgentSessionResult>> created;
    tui::VirtualTerminal terminal;
    boost::asio::io_context io;
    std::optional<support::ExpectedVoid> run_result;
    std::shared_ptr<coding_agent::tui::InteractiveEngine> engine;
    tests::RunJoinGuard join_run;

    explicit FullscreenRun(std::string_view settings, tui::VirtualTerminalOptions terminal_options = {})
        : terminal(terminal_options), join_run(io, [this] { return run_result.has_value(); }) {
        config.write("settings.json", std::string{settings});
    }

    /// Create the session and start the engine; the runtime fixture and
    /// workspace must already exist (members above are constructed first).
    void start() {
        created.emplace(runtime.run(
                create_session_async(runtime, session_options(workspace, tests::make_scripted_fake_provider()))));
        REQUIRE(*created);
        runtime_driver.emplace(runtime);
        engine = std::make_shared<coding_agent::tui::InteractiveEngine>(
                terminal, io.get_executor(), coding_agent::TuiMode::Fullscreen);
        boost::asio::co_spawn(io,
                drive_engine(engine, make_run(*(*created)->session, config.path(), actions->make_sink())),
                [this](std::exception_ptr exception, support::ExpectedVoid result) {
                    CHECK(exception == nullptr);
                    run_result.emplace(std::move(result));
                });
        drain_ready(io);
        REQUIRE(terminal.modes().alternate_screen);
    }

    std::optional<tests::RuntimeLoopDriver> runtime_driver;
};

void close_run(FullscreenRun& run) {
    REQUIRE(run.terminal.inject_input("\x04"));
    REQUIRE(pump_until(run.io, [&] { return run.run_result.has_value(); }));
    REQUIRE(run.run_result);
    CHECK(*run.run_result);
}

} // namespace

TEST_CASE("Native TUI fullscreen scrollbar row applies the resolved mode to the live transcript",
        "[coding_agent][tui][fullscreen][settings-selector][issue1020][spec]") {
    FullscreenRun run(R"({"tuiMode": "fullscreen"})");
    run.start();
    submit_turns(run.terminal, run.io, 10);

    // The product default is auto: no bar while the transcript follows its end.
    CHECK(scrollbar_rows(run.terminal) == 0);

    focus_settings_row(run.terminal, run.io, 9, "Scrollbar behavior in fullscreen mode");
    REQUIRE(run.terminal.inject_input("\r")); // auto -> always
    REQUIRE(pump_until(run.io, [&] { return scrollbar_rows(run.terminal) > 0; }));
    REQUIRE(run.terminal.inject_input("\r")); // always -> hidden
    REQUIRE(pump_until(run.io, [&] { return scrollbar_rows(run.terminal) == 0; }));
    CHECK(read_text_file(run.config.path() / "settings.json").find("\"hidden\"") != std::string::npos);
    REQUIRE(run.terminal.inject_input("\r")); // hidden -> auto
    REQUIRE(pump_until(
            run.io, [&] { return visible_screen(run.terminal).find("Fullscreen scrollbar") != std::string::npos; }));
    CHECK(scrollbar_rows(run.terminal) == 0);

    close_settings(run.terminal, run.io, "Scrollbar behavior in fullscreen mode");
    close_run(run);
}

TEST_CASE("Native TUI fullscreen copy-on-select row drives the real copy callback and manual Ctrl+X",
        "[coding_agent][tui][fullscreen][settings-selector][issue1020][spec]") {
    FullscreenRun run(R"({"tuiMode": "fullscreen"})");
    run.start();
    submit_turns(run.terminal, run.io, 2);

    // Disable automatic copy through the row; the settings write and the live
    // renderer both take the new value.
    focus_settings_row(run.terminal, run.io, 10, "Automatically copy selected text in fullscreen mode");
    REQUIRE(run.terminal.inject_input("\r")); // true -> false
    drain_ready(run.io);
    close_settings(run.terminal, run.io, "Automatically copy selected text in fullscreen mode");
    CHECK(read_text_file(run.config.path() / "settings.json").find("\"fullscreenCopyOnSelect\": false") !=
            std::string::npos);

    // Release over the selection must not copy; Ctrl+X must copy the selection
    // itself instead of falling through to the last assistant message.
    select_cells(run.terminal, run.io, "fake", 4);
    CHECK(run.actions->write_clipboard.empty());
    REQUIRE(run.terminal.inject_input("\x18"));
    REQUIRE(pump_until(run.io, [&] { return !run.actions->write_clipboard.empty(); }));
    REQUIRE(run.actions->write_clipboard.size() == 1);
    CHECK(run.actions->write_clipboard.front().text == "fake");

    // Re-enable through the row: a release copies the selection automatically
    // and the manual Ctrl+X path defers to the last-message copy.
    focus_settings_row(run.terminal, run.io, 10, "Automatically copy selected text in fullscreen mode");
    REQUIRE(run.terminal.inject_input("\r")); // false -> true
    drain_ready(run.io);
    close_settings(run.terminal, run.io, "Automatically copy selected text in fullscreen mode");
    CHECK(read_text_file(run.config.path() / "settings.json").find("\"fullscreenCopyOnSelect\": true") !=
            std::string::npos);

    select_cells(run.terminal, run.io, "fake", 4);
    REQUIRE(pump_until(run.io, [&] { return run.actions->write_clipboard.size() == 2; }));
    CHECK(run.actions->write_clipboard.back().text == "fake");

    close_run(run);
}

TEST_CASE("Native TUI fullscreen wheel row changes the live lines-per-notch delta",
        "[coding_agent][tui][fullscreen][settings-selector][issue1020][spec]") {
    FullscreenRun run(R"({"tuiMode": "fullscreen"})");
    run.start();
    submit_turns(run.terminal, run.io, 12);

    // auto -> 1 -> 2 -> 3 -> 5: four confirms on the wheel row.
    focus_settings_row(run.terminal, run.io, 11, "Lines per mouse-wheel event in fullscreen mode");
    for (int step = 0; step < 4; ++step) {
        REQUIRE(run.terminal.inject_input("\r"));
    }
    drain_ready(run.io);
    close_settings(run.terminal, run.io, "Lines per mouse-wheel event in fullscreen mode");
    CHECK(read_text_file(run.config.path() / "settings.json").find("\"fullscreenWheelScrollLines\": 5") !=
            std::string::npos);

    // One fixed-5 notch moves the transcript window five lines: the topmost
    // visible transcript line keeps its identity and moves down five rows.
    const auto fixed_turn = topmost_fake_turn(content_rows(run.terminal));
    const auto fixed_before = fake_turn_row(content_rows(run.terminal), fixed_turn);
    REQUIRE(fixed_before.has_value());
    REQUIRE(run.terminal.inject_input("\x1b[<64;1;1M"));
    REQUIRE(pump_until(
            run.io, [&] { return wheel_shift(content_rows(run.terminal), fixed_turn, *fixed_before).has_value(); }));
    CHECK(wheel_shift(content_rows(run.terminal), fixed_turn, *fixed_before) == 5);

    // 5 -> 10 -> auto: two confirms; the first auto notch moves one line.
    focus_settings_row(run.terminal, run.io, 11, "Lines per mouse-wheel event in fullscreen mode");
    REQUIRE(run.terminal.inject_input("\r"));
    REQUIRE(run.terminal.inject_input("\r"));
    drain_ready(run.io);
    close_settings(run.terminal, run.io, "Lines per mouse-wheel event in fullscreen mode");
    CHECK(read_text_file(run.config.path() / "settings.json").find("\"fullscreenWheelScrollLines\": \"auto\"") !=
            std::string::npos);

    const auto auto_turn = topmost_fake_turn(content_rows(run.terminal));
    const auto auto_before = fake_turn_row(content_rows(run.terminal), auto_turn);
    REQUIRE(auto_before.has_value());
    REQUIRE(run.terminal.inject_input("\x1b[<64;1;1M"));
    REQUIRE(pump_until(
            run.io, [&] { return wheel_shift(content_rows(run.terminal), auto_turn, *auto_before).has_value(); }));
    CHECK(wheel_shift(content_rows(run.terminal), auto_turn, *auto_before) == 1);

    close_run(run);
}

TEST_CASE("Native TUI fullscreen hidden scrollbar persists, reloads and reserves no column",
        "[coding_agent][tui][fullscreen][settings-selector][issue1020][spec]") {
    FullscreenRun run(R"({"tuiMode": "fullscreen", "fullscreenScrollbar": "always"})");
    run.start();
    const std::string long_text(90, 'x');
    const auto long_prefix = long_text.substr(0, 40);
    REQUIRE(run.terminal.inject_input(long_text + "\r"));
    REQUIRE(pump_until(run.io, [&] { return visible_screen(run.terminal).find(long_prefix) != std::string::npos; }));
    REQUIRE(scrollbar_rows(run.terminal) > 0);

    // `always` reserves the bar column: the long message's first wrapped row
    // holds the bar in the last column and one fewer `x` than the unreserved
    // layout fits (frozen `contentWidthAtTen` 9 vs 10).
    const auto always_shape = long_row_shape(run.terminal, long_prefix);
    CHECK(is_scrollbar_glyph(always_shape.last_grapheme));

    // The literal persisted value reaches the live transcript through the
    // runtime reload path: rewrite the file and run `/reload`.
    run.config.write("settings.json", R"({"tuiMode": "fullscreen", "fullscreenScrollbar": "hidden"})");
    REQUIRE(run.terminal.inject_input("/reload\r"));
    REQUIRE(pump_until(run.io, [&] { return scrollbar_rows(run.terminal) == 0; }));
    CHECK(read_text_file(run.config.path() / "settings.json").find("\"hidden\"") != std::string::npos);

    // Hidden is a literal hidden: the transcript gives the message the whole
    // width, so the same wrapped row fits one more `x` and paints no bar.
    const auto hidden_shape = long_row_shape(run.terminal, long_prefix);
    CHECK_FALSE(is_scrollbar_glyph(hidden_shape.last_grapheme));
    CHECK(hidden_shape.columns == always_shape.columns + 1);

    close_run(run);
}

TEST_CASE("Native TUI fullscreen settings survive a regular/fullscreen mode switch",
        "[coding_agent][tui][fullscreen][settings-selector][issue1020][spec]") {
    FullscreenRun run(
            R"({"tuiMode": "fullscreen", "fullscreenScrollbar": "always", "fullscreenCopyOnSelect": false, "fullscreenWheelScrollLines": 5})");
    run.start();
    submit_turns(run.terminal, run.io, 12);
    REQUIRE(scrollbar_rows(run.terminal) > 0);

    // Switch to regular through the TUI mode row, then back to fullscreen.
    focus_settings_row(run.terminal, run.io, 7, "Interface layout; regular mode uses the terminal's normal scrollback");
    REQUIRE(run.terminal.inject_input("\r"));
    REQUIRE(pump_until(run.io, [&] { return !run.terminal.modes().alternate_screen; }));
    close_settings(run.terminal, run.io, "Interface layout; regular mode uses the terminal's normal scrollback");

    focus_settings_row(run.terminal, run.io, 7, "Interface layout; regular mode uses the terminal's normal scrollback");
    REQUIRE(run.terminal.inject_input("\r"));
    REQUIRE(pump_until(run.io, [&] { return run.terminal.modes().alternate_screen; }));
    close_settings(run.terminal, run.io, "Interface layout; regular mode uses the terminal's normal scrollback");

    // The re-entered fullscreen root keeps the persisted values: the bar is
    // visible, the wheel still moves five lines per notch, and the release
    // copies nothing.
    REQUIRE(pump_until(run.io, [&] { return scrollbar_rows(run.terminal) > 0; }));
    const auto marker_turn = topmost_fake_turn(content_rows(run.terminal));
    const auto marker_before = fake_turn_row(content_rows(run.terminal), marker_turn);
    REQUIRE(marker_before.has_value());
    REQUIRE(run.terminal.inject_input("\x1b[<64;1;1M"));
    REQUIRE(pump_until(
            run.io, [&] { return wheel_shift(content_rows(run.terminal), marker_turn, *marker_before).has_value(); }));
    CHECK(wheel_shift(content_rows(run.terminal), marker_turn, *marker_before) == 5);

    select_cells(run.terminal, run.io, "fake", 4);
    CHECK(run.actions->write_clipboard.empty());
    CHECK(read_text_file(run.config.path() / "settings.json").find("\"fullscreenScrollbar\": \"always\"") !=
            std::string::npos);

    close_run(run);
}
