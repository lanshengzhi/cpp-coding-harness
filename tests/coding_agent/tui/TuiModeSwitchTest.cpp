// #1018: the live TUI-mode switch (pi `switchTuiMode`,
// `interactive-mode.ts:883-933`) driven through the settings selector's TUI
// mode row. The four cases cover the ticket's acceptance criteria: the
// settings control performs the swap on the live run and persists it, the
// regular-root round trip keeps transcript/editor/session continuity, an
// overlay entry refuses the switch with everything intact, a switch while work
// is pending stays clean through Close, and the retained fullscreen transcript
// keeps its scroll window across a mode round trip.

#include "coding_agent/tui/InteractiveEngine.hpp"
#include "coding_agent/tui/InteractiveMode.hpp"
#include "coding_agent/tui/InteractiveSessionRun.hpp"
#include "coding_agent/AgentSession.hpp"
#include "coding_agent/SessionTarget.hpp"
#include "coding_agent/runtime/SessionFactory.hpp"
#include "support/GatedChatProvider.hpp"
#include "support/ModelsFixture.hpp"
#include "support/PumpUntil.hpp"
#include "support/RuntimeFixture.hpp"
#include "support/RuntimeLoopDriver.hpp"
#include "support/TempWorkspace.hpp"

#include <cch/ai/Content.hpp>
#include <cch/coding_agent/ModelRuntime.hpp>
#include <cch/tui/Overlay.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include <catch2/catch_test_macros.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <algorithm>
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
        coding_agent::TuiMode tui_mode) {
    return coding_agent::tui::InteractiveSessionRunBuilder{}
            .with_session(session)
            .with_agent_config_directory(agent_config_directory)
            .with_tui_mode(tui_mode)
            .build();
}

/// `run_interactive_mode` owns its engine privately; the overlay-refusal case
/// needs the engine addressable to present a real overlay entry, so it drives
/// the same sequence the host runs (`start` → trust warning → exit wait →
/// `finish`).
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

/// The TUI mode row is the sixth of the selector's seven items: behind the
/// thinking-level row and before the theme row.
constexpr std::string_view kTuiModeRowKeys = "\x1b[B\x1b[B\x1b[B\x1b[B\x1b[B";

/// Only the focused row renders its description, so this is what proves the
/// five moves above landed on the TUI mode row.
constexpr std::string_view kTuiModeRowDescription = "Interface layout;";

[[nodiscard]] bool on_tui_mode_row(const tui::VirtualTerminal& terminal) {
    return visible_screen(terminal).find(kTuiModeRowDescription) != std::string::npos;
}

/// Open `/settings` and move onto the TUI mode row.
void focus_tui_mode_row(tui::VirtualTerminal& terminal, boost::asio::io_context& io) {
    REQUIRE(terminal.inject_input("/settings\r"));
    drain_ready(io);
    REQUIRE(terminal.inject_input(std::string{kTuiModeRowKeys}));
    REQUIRE(pump_until(io, [&] { return on_tui_mode_row(terminal); }));
}

/// Leave the selector so the next `/settings` opens a fresh one instead of
/// typing into its search field.
void close_settings(tui::VirtualTerminal& terminal, boost::asio::io_context& io) {
    REQUIRE(terminal.inject_input("\x1b"));
    REQUIRE(terminal.flush_input());
    drain_ready(io);
    REQUIRE_FALSE(on_tui_mode_row(terminal));
}

class CustomEditor final : public tui::EditorComponent {
public:
    [[nodiscard]] std::string text() const override { return text_; }
    void set_text(std::string text) override { text_ = std::move(text); }
    void set_on_change(tui::EditorChangeSink sink) override { on_change_ = std::move(sink); }
    void set_on_submit(tui::EditorSubmitSink sink) override { on_submit_ = std::move(sink); }
    [[nodiscard]] support::Expected<tui::RenderResult> render(std::size_t) override {
        return tui::RenderResult{.lines = {std::format("custom:{}", text_)}};
    }
    void invalidate() override {}
    [[nodiscard]] tui::InputAdmissionOutcome handle_input(const tui::InputEventVariant&) override {
        return tui::InputAdmissionOutcome::Unhandled;
    }
    void set_focused(bool focused) override { focused_ = focused; }
    [[nodiscard]] bool focused() const override { return focused_; }

private:
    std::string text_;
    tui::EditorChangeSink on_change_;
    tui::EditorSubmitSink on_submit_;
    bool focused_{false};
};

} // namespace

TEST_CASE("Native TUI settings TUI mode row swaps the renderer and persists the mode",
        "[coding_agent][tui][fullscreen][settings-selector][issue1018][spec]") {
    tests::RuntimeFixture runtime;
    tests::TempWorkspace workspace;
    tests::TempWorkspace config;
    // The row shows the stored setting, so the first confirm cycles it to
    // `regular` and performs a real swap rather than the same-mode no-op.
    config.write("settings.json", R"({"tuiMode": "fullscreen"})");
    auto created = runtime.run(
            create_session_async(runtime, session_options(workspace, tests::make_scripted_fake_provider())));
    REQUIRE(created);
    tests::RuntimeLoopDriver runtime_driver(runtime);

    tui::VirtualTerminal terminal({.columns = 48, .rows = 24});
    boost::asio::io_context io;
    std::optional<support::ExpectedVoid> run_result;
    boost::asio::co_spawn(io,
            coding_agent::tui::run_interactive_mode(
                    terminal, make_run(*created->session, config.path(), coding_agent::TuiMode::Fullscreen)),
            [&](std::exception_ptr exception, support::ExpectedVoid result) {
                CHECK(exception == nullptr);
                run_result.emplace(std::move(result));
            });
    drain_ready(io);
    REQUIRE(terminal.modes().alternate_screen);

    for (const std::string prompt : {"one", "two"}) {
        REQUIRE(terminal.inject_input(prompt + "\r"));
        REQUIRE(pump_until(io, [&] { return visible_screen(terminal).find("fake: " + prompt) != std::string::npos; }));
    }
    // The draft is submitted before the selector opens: the slash command that
    // switches the mode needs an empty editor, and the editor is the one
    // instance every later submit reaches.
    REQUIRE(terminal.inject_input("draft\r"));
    REQUIRE(pump_until(io, [&] { return visible_screen(terminal).find("fake: draft") != std::string::npos; }));

    focus_tui_mode_row(terminal, io);
    REQUIRE(terminal.inject_input("\r"));
    // The fullscreen root leaves the alternate buffer for the regular root and
    // the status line reports the mode the run renders.
    REQUIRE(pump_until(io, [&] { return !terminal.modes().alternate_screen; }));
    REQUIRE(pump_until(io, [&] { return visible_screen(terminal).find("TUI mode: regular") != std::string::npos; }));
    CHECK(read_text_file(config.path() / "settings.json").find("\"regular\"") != std::string::npos);

    // Same session, transcript and editor: closing the selector hands the one
    // editor back over the same conversation, and it still submits.
    close_settings(terminal, io);
    // The regular root repaints the conversation tail; the newest submit is the
    // line still inside the terminal's visible rows.
    CHECK(visible_screen(terminal).find("fake: draft") != std::string::npos);
    REQUIRE(terminal.inject_input("again\r"));
    REQUIRE(pump_until(io, [&] { return visible_screen(terminal).find("fake: again") != std::string::npos; }));

    // Round trip back through the same row on the fullscreen root.
    focus_tui_mode_row(terminal, io);
    REQUIRE(terminal.inject_input("\r"));
    REQUIRE(pump_until(io, [&] { return terminal.modes().alternate_screen; }));
    REQUIRE(pump_until(io, [&] { return visible_screen(terminal).find("TUI mode: fullscreen") != std::string::npos; }));
    CHECK(read_text_file(config.path() / "settings.json").find("\"fullscreen\"") != std::string::npos);
    close_settings(terminal, io);
    // The newest submit is the transcript line the fullscreen window shows after
    // the round trip.
    CHECK(visible_screen(terminal).find("fake: again") != std::string::npos);
    REQUIRE(terminal.inject_input("once more\r"));
    REQUIRE(pump_until(io, [&] { return visible_screen(terminal).find("fake: once more") != std::string::npos; }));

    REQUIRE(terminal.inject_input("\x04"));
    REQUIRE(pump_until(io, [&] { return run_result.has_value(); }));
    REQUIRE(run_result);
    CHECK(*run_result);
    CHECK_FALSE(terminal.modes().started);
    CHECK_FALSE(terminal.modes().alternate_screen);
}

TEST_CASE("Native TUI TUI mode switch refuses while an overlay entry is present",
        "[coding_agent][tui][fullscreen][overlays][issue1018][spec]") {
    tests::RuntimeFixture runtime;
    tests::TempWorkspace workspace;
    tests::TempWorkspace config;
    config.write("settings.json", R"({"tuiMode": "fullscreen"})");
    auto created = runtime.run(
            create_session_async(runtime, session_options(workspace, tests::make_scripted_fake_provider())));
    REQUIRE(created);
    tests::RuntimeLoopDriver runtime_driver(runtime);

    tui::VirtualTerminal terminal({.columns = 48, .rows = 24});
    boost::asio::io_context io;
    std::optional<support::ExpectedVoid> run_result;
    auto engine = std::make_shared<coding_agent::tui::InteractiveEngine>(
            terminal, io.get_executor(), coding_agent::TuiMode::Fullscreen);
    boost::asio::co_spawn(io,
            drive_engine(engine, make_run(*created->session, config.path(), coding_agent::TuiMode::Fullscreen)),
            [&](std::exception_ptr exception, support::ExpectedVoid result) {
                CHECK(exception == nullptr);
                run_result.emplace(std::move(result));
            });
    drain_ready(io);
    const tests::RunJoinGuard join_run{io, [&] { return run_result.has_value(); }};
    REQUIRE(terminal.modes().alternate_screen);

    REQUIRE(terminal.inject_input("one\r"));
    REQUIRE(pump_until(io, [&] { return visible_screen(terminal).find("fake: one") != std::string::npos; }));
    focus_tui_mode_row(terminal, io);

    // An overlay entry that keeps input flowing to the settings selector: the
    // shape frozen pi's `hasOverlayEntries` guard refuses on. It is presented
    // through the seam the run's own flows use, so the engine sees a real entry.
    auto& presenter = static_cast<coding_agent::tui::ModalPresenter&>(*engine);
    presenter.show_overlay(std::make_unique<tui::Overlay>(tui::OverlayOptions{
            .width = 12.0,
            .anchor = tui::OverlayAnchor::TopRight,
            .non_capturing = true,
    }));
    drain_ready(io);
    REQUIRE(pump_until(io, [&] { return on_tui_mode_row(terminal); }));

    // 1. Hidden overlay refusal: pi's hasOverlayEntries refuses even when the
    // entry is temporarily hidden. Test both direct engine switch and UI switch.
    presenter.hide_overlay();
    drain_ready(io);
    REQUIRE(pump_until(io, [&] { return on_tui_mode_row(terminal); }));

    // Direct repository-private engine operation refuses while overlay is hidden:
    auto direct_hidden_refusal = engine->switch_tui_mode(coding_agent::TuiMode::Regular);
    REQUIRE(direct_hidden_refusal);
    CHECK_FALSE(*direct_hidden_refusal);
    CHECK(terminal.modes().alternate_screen);

    // UI attempt while hidden is refused with status message and mode intact:
    REQUIRE(terminal.inject_input("\r"));
    REQUIRE(pump_until(io, [&] {
        return visible_screen(terminal).find("Close active overlays before changing TUI mode") != std::string::npos;
    }));
    // Refused with everything intact: the fullscreen root is still live, the
    // transcript is still the same one, and the mode never changed. The row
    // rolls back to the live mode, so a second confirm is refused too instead
    // of cycling onto the mode already in effect.
    CHECK(terminal.modes().alternate_screen);
    CHECK(terminal.modes().started);
    CHECK(visible_screen(terminal).find("fake: one") != std::string::npos);
    CHECK(visible_screen(terminal).find("TUI mode: ") == std::string::npos);
    REQUIRE(pump_until(io, [&] { return on_tui_mode_row(terminal); }));
    CHECK(terminal.modes().alternate_screen);
    CHECK(visible_screen(terminal).find("TUI mode: ") == std::string::npos);

    // With the entry gone the very same row performs the swap, so the refusal
    // above was the overlay and not a broken control.
    presenter.close_overlay();
    drain_ready(io);
    REQUIRE(pump_until(io, [&] { return on_tui_mode_row(terminal); }));
    REQUIRE(terminal.inject_input("\r"));
    REQUIRE(pump_until(io, [&] { return visible_screen(terminal).find("TUI mode: regular") != std::string::npos; }));
    CHECK_FALSE(terminal.modes().alternate_screen);
    CHECK(visible_screen(terminal).find("fake: one") != std::string::npos);
    close_settings(terminal, io);

    REQUIRE(terminal.inject_input("\x04"));
    REQUIRE(pump_until(io, [&] { return run_result.has_value(); }));
    REQUIRE(run_result);
    CHECK(*run_result);
    CHECK_FALSE(terminal.modes().started);
    CHECK_FALSE(terminal.modes().alternate_screen);
}

TEST_CASE("Native TUI TUI mode switch during pending work keeps the run live through Close",
        "[coding_agent][tui][fullscreen][close][issue1018][spec]") {
    tests::RuntimeFixture runtime;
    tests::TempWorkspace workspace;
    tests::TempWorkspace config;
    config.write("settings.json", R"({"tuiMode": "fullscreen"})");
    auto client = std::make_shared<tests::GatedChatProvider>();
    auto* gated = client.get();
    auto created = runtime.run(create_session_async(runtime, session_options(workspace, std::move(client))));
    REQUIRE(created);
    tests::RuntimeLoopDriver runtime_driver(runtime);

    tui::VirtualTerminal terminal({.columns = 48, .rows = 24});
    boost::asio::io_context io;
    std::optional<support::ExpectedVoid> run_result;
    boost::asio::co_spawn(io,
            coding_agent::tui::run_interactive_mode(
                    terminal, make_run(*created->session, config.path(), coding_agent::TuiMode::Fullscreen)),
            [&](std::exception_ptr exception, support::ExpectedVoid result) {
                CHECK(exception == nullptr);
                run_result.emplace(std::move(result));
            });
    drain_ready(io);
    REQUIRE(terminal.modes().alternate_screen);

    // The provider request is admitted and parked at the gate: the run has work
    // in flight while the renderer swaps under it.
    REQUIRE(terminal.inject_input("pending turn\r"));
    REQUIRE(pump_until(io, [&] { return gated->requests.size() == 1; }));
    focus_tui_mode_row(terminal, io);
    REQUIRE(terminal.inject_input("\r"));
    REQUIRE(pump_until(io, [&] { return visible_screen(terminal).find("TUI mode: regular") != std::string::npos; }));
    CHECK_FALSE(terminal.modes().alternate_screen);

    // Releasing the parked turn delivers it through the new root: the engine's
    // callbacks follow the selection instead of painting through the retired
    // renderer, and the run keeps running.
    gated->release();
    REQUIRE(pump_until(io, [&] { return visible_screen(terminal).find("turn 1") != std::string::npos; }));
    CHECK(terminal.modes().started);
    close_settings(terminal, io);

    REQUIRE(terminal.inject_input("\x04"));
    REQUIRE(pump_until(io, [&] { return run_result.has_value(); }));
    REQUIRE(run_result);
    CHECK(*run_result);
    // Close restores the terminal modes from the regular root alone: nothing
    // the switch retired writes a stale mode back.
    CHECK_FALSE(terminal.modes().started);
    CHECK_FALSE(terminal.modes().alternate_screen);
    CHECK(terminal.modes().cursor_visible);
}

TEST_CASE("Native TUI fullscreen transcript keeps its scroll window across a mode round trip",
        "[coding_agent][tui][fullscreen][settings-selector][issue1018][spec]") {
    tests::RuntimeFixture runtime;
    tests::TempWorkspace workspace;
    tests::TempWorkspace config;
    config.write("settings.json", R"({"tuiMode": "fullscreen"})");
    auto created = runtime.run(
            create_session_async(runtime, session_options(workspace, tests::make_scripted_fake_provider())));
    REQUIRE(created);
    tests::RuntimeLoopDriver runtime_driver(runtime);

    tui::VirtualTerminal terminal({.columns = 48, .rows = 20});
    boost::asio::io_context io;
    std::optional<support::ExpectedVoid> run_result;
    boost::asio::co_spawn(io,
            coding_agent::tui::run_interactive_mode(
                    terminal, make_run(*created->session, config.path(), coding_agent::TuiMode::Fullscreen)),
            [&](std::exception_ptr exception, support::ExpectedVoid result) {
                CHECK(exception == nullptr);
                run_result.emplace(std::move(result));
            });
    drain_ready(io);
    REQUIRE(terminal.modes().alternate_screen);

    for (const std::string prompt : {"one", "two", "three", "four", "five", "six"}) {
        REQUIRE(terminal.inject_input(prompt + "\r"));
        REQUIRE(pump_until(io, [&] { return visible_screen(terminal).find("fake: " + prompt) != std::string::npos; }));
    }
    REQUIRE(terminal.inject_input("draft"));
    REQUIRE(pump_until(io, [&] { return visible_screen(terminal).find("draft") != std::string::npos; }));
    REQUIRE(terminal.inject_input("\r"));
    REQUIRE(pump_until(io, [&] { return visible_screen(terminal).find("fake: draft") != std::string::npos; }));

    // The dock covers the last seven rows; the transcript window is everything
    // above it. Wheel up over the transcript so it stops following the end.
    const auto follow_end = terminal.screen();
    REQUIRE(follow_end.size() == 20);
    const std::vector<std::string> follow_end_window{follow_end.begin(), follow_end.end() - 7};
    for (int notch = 0; notch < 3; ++notch) {
        REQUIRE(terminal.inject_input("\x1b[<64;1;1M"));
    }
    drain_ready(io);
    const auto scrolled = terminal.screen();
    REQUIRE(scrolled.size() == 20);
    const std::vector<std::string> scrolled_window{scrolled.begin(), scrolled.end() - 7};
    REQUIRE(scrolled_window.front() != follow_end_window.front());

    // Regular, then back to fullscreen through the settings row.
    focus_tui_mode_row(terminal, io);
    REQUIRE(terminal.inject_input("\r"));
    REQUIRE(pump_until(io, [&] { return visible_screen(terminal).find("TUI mode: regular") != std::string::npos; }));
    close_settings(terminal, io);
    focus_tui_mode_row(terminal, io);
    REQUIRE(terminal.inject_input("\r"));
    // The status line lands below the retained window while the transcript is
    // scrolled up, so this direction is asserted on the root and on a painted
    // frame rather than on the newest transcript line.
    REQUIRE(pump_until(io, [&] { return terminal.modes().alternate_screen; }));
    REQUIRE(pump_until(io, [&] {
        const auto screen = terminal.screen();
        return screen.size() == 20 && screen[19].find("fake-model") != std::string::npos;
    }));

    // The open selector is dock, not transcript, so its rows are dismissed
    // before the transcript window above the dock is compared.
    close_settings(terminal, io);
    // The retained viewport keeps the window it was scrolled to: a rebuilt
    // renderer would have snapped the transcript back to its follow-end window.
    const auto after = terminal.screen();
    REQUIRE(after.size() == 20);
    const std::vector<std::string> after_window{after.begin(), after.end() - 7};
    // The switch appended the mode status below the window, so the bottom row
    // carries the viewport's "unseen tail" hint while every row above it is the
    // window the transcript was scrolled to. A rebuilt renderer would have
    // snapped back to the follow-end window instead.
    CHECK(std::equal(after_window.begin(), after_window.end() - 1, scrolled_window.begin()));
    CHECK(after_window.back().find("Jump to latest message") != std::string::npos);
    CHECK(after_window != follow_end_window);
    close_settings(terminal, io);

    REQUIRE(terminal.inject_input("\x04"));
    REQUIRE(pump_until(io, [&] { return run_result.has_value(); }));
    REQUIRE(run_result);
    CHECK(*run_result);
}

TEST_CASE("Native TUI TUI mode switch preserves focus text and cursor identity across fullscreen to regular round trip",
        "[coding_agent][tui][fullscreen][focus][issue1018][spec]") {
    tests::RuntimeFixture runtime;
    tests::TempWorkspace workspace;
    tests::TempWorkspace config;
    config.write("settings.json", R"({"tuiMode": "fullscreen"})");
    auto created = runtime.run(
            create_session_async(runtime, session_options(workspace, tests::make_scripted_fake_provider())));
    REQUIRE(created);
    tests::RuntimeLoopDriver runtime_driver(runtime);

    tui::VirtualTerminal terminal({.columns = 48, .rows = 24});
    boost::asio::io_context io;
    std::optional<support::ExpectedVoid> run_result;
    auto engine = std::make_shared<coding_agent::tui::InteractiveEngine>(
            terminal, io.get_executor(), coding_agent::TuiMode::Fullscreen);
    boost::asio::co_spawn(io,
            drive_engine(engine, make_run(*created->session, config.path(), coding_agent::TuiMode::Fullscreen)),
            [&](std::exception_ptr exception, support::ExpectedVoid result) {
                CHECK(exception == nullptr);
                run_result.emplace(std::move(result));
            });
    drain_ready(io);
    const tests::RunJoinGuard join_run{io, [&] { return run_result.has_value(); }};
    REQUIRE(terminal.modes().alternate_screen);
    REQUIRE(pump_until(io, [&] { return engine->focus() != nullptr; }));

    // Focus target identity: prompt editor holds focus in fullscreen.
    auto* const initial_focus = engine->focus();
    REQUIRE(initial_focus != nullptr);

    // Text and cursor in fullscreen: draft prompt is entered and terminal cursor is visible.
    REQUIRE(terminal.inject_input("draft prompt"));
    REQUIRE(pump_until(io, [&] { return visible_screen(terminal).find("draft prompt") != std::string::npos; }));
    const auto fullscreen_cursor = terminal.cursor();
    CHECK(fullscreen_cursor.row > 0);
    CHECK(terminal.modes().cursor_visible);

    // Switch fullscreen -> regular directly on the engine (the frontend lifecycle
    // operation the settings consumer invokes).
    auto switched_to_regular = engine->switch_tui_mode(coding_agent::TuiMode::Regular);
    REQUIRE(switched_to_regular);
    REQUIRE(*switched_to_regular);
    drain_ready(io);
    REQUIRE(pump_until(io, [&] { return !terminal.modes().alternate_screen; }));

    // Focus target identity: the focused component on the regular root must be the
    // exact same component instance that held focus in fullscreen.
    CHECK(engine->focus() == initial_focus);

    // Text continuity: draft text is still present in the editor.
    CHECK(visible_screen(terminal).find("draft prompt") != std::string::npos);

    // Cursor in regular mode: hardware cursor is hidden during regular main-screen
    // execution, but position is maintained.
    CHECK_FALSE(terminal.modes().cursor_visible);
    const auto regular_cursor = terminal.cursor();
    CHECK(regular_cursor.row > 0);

    // Input continuity: typing directly into the terminal without opening or closing
    // settings or repairing focus immediately appends to the draft prompt.
    REQUIRE(terminal.inject_input(" continued"));
    REQUIRE(pump_until(
            io, [&] { return visible_screen(terminal).find("draft prompt continued") != std::string::npos; }));

    // Switch regular -> fullscreen back.
    auto switched_to_fullscreen = engine->switch_tui_mode(coding_agent::TuiMode::Fullscreen);
    REQUIRE(switched_to_fullscreen);
    REQUIRE(*switched_to_fullscreen);
    drain_ready(io);
    REQUIRE(pump_until(io, [&] { return terminal.modes().alternate_screen; }));

    // Roundtrip focus target identity: the same component instance is still focused.
    CHECK(engine->focus() == initial_focus);

    // Text and cursor continuity back in fullscreen.
    CHECK(visible_screen(terminal).find("draft prompt continued") != std::string::npos);
    REQUIRE(pump_until(io, [&] { return terminal.modes().cursor_visible; }));
    CHECK(terminal.cursor().row == fullscreen_cursor.row);

    // Submit the prompt and verify normal completion.
    REQUIRE(terminal.inject_input(" done\r"));
    REQUIRE(pump_until(io,
            [&] { return visible_screen(terminal).find("fake: draft prompt continued done") != std::string::npos; }));

    REQUIRE(terminal.inject_input("\x04"));
    REQUIRE(pump_until(io, [&] { return run_result.has_value(); }));
    REQUIRE(run_result);
    CHECK(*run_result);
}

TEST_CASE("Native TUI TUI mode switch rebinds extension listeners once with no repeat editor factory",
        "[coding_agent][tui][fullscreen][extensions][issue1018][spec]") {
    tests::RuntimeFixture runtime;
    tests::TempWorkspace workspace;
    tests::TempWorkspace config;
    config.write("settings.json", R"({"tuiMode": "fullscreen"})");
    auto created = runtime.run(
            create_session_async(runtime, session_options(workspace, tests::make_scripted_fake_provider())));
    REQUIRE(created);
    tests::RuntimeLoopDriver runtime_driver(runtime);

    tui::VirtualTerminal terminal({.columns = 48, .rows = 24});
    boost::asio::io_context io;
    std::optional<support::ExpectedVoid> run_result;

    std::size_t registration_factory_calls = 0;
    std::size_t editor_factory_calls = 0;
    std::size_t listener_invocations = 0;
    std::vector<std::string> delivered_inputs;

    auto extension_factory = [&](coding_agent::tui::ExtensionUiHost& host) {
        ++registration_factory_calls;
        host.on_terminal_input([&](std::string_view input) -> cch::tui::RawInputListenerResult {
            ++listener_invocations;
            delivered_inputs.emplace_back(input);
            return cch::tui::RawInputListenerResult::pass();
        });
        host.set_editor_component(
                [&](cch::tui::ITui&, cch::tui::EditorTheme, std::shared_ptr<coding_agent::tui::SharedKeybindings>) {
                    ++editor_factory_calls;
                    return std::make_shared<CustomEditor>();
                });
    };

    auto engine = std::make_shared<coding_agent::tui::InteractiveEngine>(
            terminal, io.get_executor(), coding_agent::TuiMode::Fullscreen);
    auto run = coding_agent::tui::InteractiveSessionRunBuilder{}
                       .with_session(*created->session)
                       .with_agent_config_directory(config.path())
                       .with_tui_mode(coding_agent::TuiMode::Fullscreen)
                       .with_extension_ui_registration_factory(std::move(extension_factory))
                       .build();

    boost::asio::co_spawn(
            io, drive_engine(engine, std::move(run)), [&](std::exception_ptr exception, support::ExpectedVoid result) {
                CHECK(exception == nullptr);
                run_result.emplace(std::move(result));
            });
    drain_ready(io);
    const tests::RunJoinGuard join_run{io, [&] { return run_result.has_value(); }};
    REQUIRE(terminal.modes().alternate_screen);

    // Initial registration: exactly 1 factory call, exactly 1 editor construction.
    CHECK(registration_factory_calls == 1);
    CHECK(editor_factory_calls == 1);

    // Initial input in fullscreen: exactly 1 delivery per input.
    REQUIRE(terminal.inject_input("x"));
    drain_ready(io);
    CHECK(listener_invocations == 1);
    REQUIRE(delivered_inputs.size() == 1);
    CHECK(delivered_inputs.back() == "x");

    // Switch fullscreen -> regular.
    auto switched_to_regular = engine->switch_tui_mode(coding_agent::TuiMode::Regular);
    REQUIRE(switched_to_regular);
    REQUIRE(*switched_to_regular);
    drain_ready(io);
    REQUIRE(pump_until(io, [&] { return !terminal.modes().alternate_screen; }));

    // No repeat registration factory and no repeat editor construction across switch.
    CHECK(registration_factory_calls == 1);
    CHECK(editor_factory_calls == 1);

    // Input on regular root: exactly 1 delivery per input.
    REQUIRE(terminal.inject_input("y"));
    drain_ready(io);
    CHECK(listener_invocations == 2);
    REQUIRE(delivered_inputs.size() == 2);
    CHECK(delivered_inputs.back() == "y");

    // Switch regular -> fullscreen.
    auto switched_to_fullscreen = engine->switch_tui_mode(coding_agent::TuiMode::Fullscreen);
    REQUIRE(switched_to_fullscreen);
    REQUIRE(*switched_to_fullscreen);
    drain_ready(io);
    REQUIRE(pump_until(io, [&] { return terminal.modes().alternate_screen; }));

    // Still no repeat factory calls after roundtrip.
    CHECK(registration_factory_calls == 1);
    CHECK(editor_factory_calls == 1);

    // Input on fullscreen root after roundtrip: exactly 1 delivery per input.
    REQUIRE(terminal.inject_input("z"));
    drain_ready(io);
    CHECK(listener_invocations == 3);
    REQUIRE(delivered_inputs.size() == 3);
    CHECK(delivered_inputs.back() == "z");

    // Clean exit.
    REQUIRE(terminal.inject_input("\x04"));
    REQUIRE(pump_until(io, [&] { return run_result.has_value(); }));
    REQUIRE(run_result);
    CHECK(*run_result);
}
