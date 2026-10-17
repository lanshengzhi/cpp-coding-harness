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
#include "support/EnvVarGuard.hpp"
#include "support/GatedChatProvider.hpp"
#include "support/ModelsFixture.hpp"
#include "support/PiTuiEvidence.hpp"
#include "support/ProtocolRecordingTerminal.hpp"
#include "support/PumpUntil.hpp"
#include "support/RuntimeFixture.hpp"
#include "support/RuntimeLoopDriver.hpp"
#include "support/TempWorkspace.hpp"

#include <cch/ai/Content.hpp>
#include <cch/coding_agent/ModelRuntime.hpp>
#include <cch/tui/Editor.hpp>
#include <cch/tui/Overlay.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include <catch2/catch_message.hpp>
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
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace cch;
using tests::drain_ready;
using tests::pump_until;

namespace {

using support::JsonValue;

[[nodiscard]] const JsonValue& find_evidence_scenario(const JsonValue& bundle, std::string_view name) {
    for (const auto& scenario : bundle.at("scenarios").get_array()) {
        if (scenario.at("name").get_string() == name) return scenario;
    }
    REQUIRE(false);
    return bundle;
}

[[nodiscard]] std::vector<std::string> strings_of(const JsonValue& value) {
    std::vector<std::string> strings;
    for (const auto& entry : value.get_array())
        strings.emplace_back(entry.get_string());
    return strings;
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

/// The TUI mode row is the eighth of the selector's nine items: behind the
/// thinking-level row and before the theme row.
constexpr std::string_view kTuiModeRowKeys = "\x1b[B\x1b[B\x1b[B\x1b[B\x1b[B\x1b[B\x1b[B";

/// Only the focused row renders its description, so this is what proves the
/// seven moves above landed on the TUI mode row.
constexpr std::string_view kTuiModeRowDescription = "Interface layout;";

[[nodiscard]] bool on_tui_mode_row(const tui::VirtualTerminal& terminal) {
    return visible_screen(terminal).find(kTuiModeRowDescription) != std::string::npos;
}

constexpr std::string_view kHardwareCursorRowKeys = "\x1b[B";
constexpr std::string_view kHardwareCursorRowDescription = "Show the terminal cursor";

[[nodiscard]] bool on_hardware_cursor_row(const tui::VirtualTerminal& terminal) {
    const auto screen = visible_screen(terminal);
    return screen.find("Show hardware cursor") != std::string::npos &&
           screen.find(kHardwareCursorRowDescription) != std::string::npos;
}

void focus_hardware_cursor_row(tui::VirtualTerminal& terminal, boost::asio::io_context& io) {
    REQUIRE(terminal.inject_input("/settings\r"));
    drain_ready(io);
    REQUIRE(terminal.inject_input(std::string{kHardwareCursorRowKeys}));
    REQUIRE(pump_until(io, [&] { return on_hardware_cursor_row(terminal); }));
}

void close_hardware_cursor_settings(tui::VirtualTerminal& terminal, boost::asio::io_context& io) {
    REQUIRE(terminal.inject_input("\x1b"));
    REQUIRE(terminal.flush_input());
    drain_ready(io);
    REQUIRE_FALSE(on_hardware_cursor_row(terminal));
}

constexpr std::string_view kTerminalProgressRowKeys = "\x1b[B\x1b[B\x1b[B";
constexpr std::string_view kTerminalProgressRowDescription = "Show OSC 9;4 progress indicators";

[[nodiscard]] bool on_terminal_progress_row(const tui::VirtualTerminal& terminal) {
    const auto screen = visible_screen(terminal);
    return screen.find("Terminal progress") != std::string::npos &&
           screen.find(kTerminalProgressRowDescription) != std::string::npos;
}

void focus_terminal_progress_row(tui::VirtualTerminal& terminal, boost::asio::io_context& io) {
    REQUIRE(terminal.inject_input("/settings\r"));
    drain_ready(io);
    REQUIRE(terminal.inject_input(std::string{kTerminalProgressRowKeys}));
    REQUIRE(pump_until(io, [&] { return on_terminal_progress_row(terminal); }));
}

void close_terminal_progress_settings(tui::VirtualTerminal& terminal, boost::asio::io_context& io) {
    REQUIRE(terminal.inject_input("\x1b"));
    REQUIRE(terminal.flush_input());
    drain_ready(io);
    REQUIRE_FALSE(on_terminal_progress_row(terminal));
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

    tui::VirtualTerminal terminal({.columns = 48, .rows = 26});
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
    auto overlay = std::make_unique<tui::Overlay>(tui::OverlayOptions{
            .width = 12.0,
            .anchor = tui::OverlayAnchor::TopRight,
            .non_capturing = true,
    });
    auto* const overlay_pointer = overlay.get(); // borrowed until close_overlay below.
    presenter.show_overlay(std::move(overlay));
    drain_ready(io);
    REQUIRE(pump_until(io, [&] { return on_tui_mode_row(terminal); }));

    // 1. Hidden overlay refusal: pi's hasOverlayEntries refuses even when the
    // entry is temporarily hidden. Test both direct engine switch and UI switch.
    overlay_pointer->set_visible(false);
    presenter.invalidate();
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

TEST_CASE("Native TUI terminal progress lifecycle and mode switch restoration honor settings and state",
        "[coding_agent][tui][fullscreen][progress][issue1037][spec]") {
    const auto artifact = tests::read_pi_tui_evidence("product-progress-protocol.json");
    REQUIRE(artifact);
    const auto& bundle = *artifact;
    const auto& lifecycle = find_evidence_scenario(bundle, "interactive-mode-handle-event-lifecycle")
                                    .at("expected")
                                    .at("lifecycleResults");
    const auto& matrix =
            find_evidence_scenario(bundle, "interactive-mode-switch-matrix").at("expected").at("switchResults");

    // ── Case 1: showTerminalProgress = true ──────────────────────────────────
    {
        tests::RuntimeFixture runtime;
        tests::TempWorkspace workspace;
        tests::TempWorkspace config;
        config.write("settings.json", R"({"tuiMode": "fullscreen", "terminal": {"showTerminalProgress": true}})");
        auto client = std::make_shared<tests::GatedChatProvider>();
        auto* gated = client.get();
        auto created = runtime.run(create_session_async(runtime, session_options(workspace, std::move(client))));
        REQUIRE(created);
        tests::RuntimeLoopDriver runtime_driver(runtime);

        tui::VirtualTerminal terminal({.columns = 80, .rows = 24});
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
        CHECK(tests::extract_osc_9_4_commands(terminal.output()).empty());

        // Turn start activates progress: exact vector comparison against turnStartEnabled
        const auto before_turn1 = terminal.output().size();
        REQUIRE(terminal.inject_input("pending turn\r"));
        REQUIRE(pump_until(io, [&] { return gated->requests.size() == 1; }));
        std::span<const std::string> turn1_delta(
                terminal.output().data() + before_turn1, terminal.output().size() - before_turn1);
        const auto turn1_start_osc = tests::extract_osc_9_4_commands(turn1_delta);
        const auto expected_turn_start =
                tests::extract_osc_9_4_commands(strings_of(lifecycle.at("turnStartEnabled").at("writes")));
        CHECK(turn1_start_osc == expected_turn_start);
        CHECK(turn1_start_osc == std::vector<std::string>{"\x1b]9;4;3\x07"});

        // Live switch to Regular while streaming: progress restored on new root
        const auto before_switch1 = terminal.output().size();
        auto switched1 = engine->switch_tui_mode(coding_agent::TuiMode::Regular, /* restore_progress */ true);
        REQUIRE(switched1);
        CHECK(*switched1);
        CHECK_FALSE(terminal.modes().alternate_screen);
        std::span<const std::string> switch1_delta(
                terminal.output().data() + before_switch1, terminal.output().size() - before_switch1);
        const auto switch1_osc = tests::extract_osc_9_4_commands(switch1_delta);
        const auto expected_switch1 = tests::extract_osc_9_4_commands(
                strings_of(matrix.at("fullscreenToRegular_streamingOnly_enabled_restoreTrue").at("writes")));
        CHECK(switch1_osc == expected_switch1);
        REQUIRE(switch1_osc.size() == 2);
        CHECK(switch1_osc[0] == "\x1b]9;4;0\x07");
        CHECK(switch1_osc[1] == "\x1b]9;4;3\x07");

        // Reverse live switch to Fullscreen while still streaming: progress restored on fullscreen root
        const auto before_switch2 = terminal.output().size();
        auto switched2 = engine->switch_tui_mode(coding_agent::TuiMode::Fullscreen, /* restore_progress */ true);
        REQUIRE(switched2);
        CHECK(*switched2);
        CHECK(terminal.modes().alternate_screen);
        std::span<const std::string> switch2_delta(
                terminal.output().data() + before_switch2, terminal.output().size() - before_switch2);
        const auto switch2_osc = tests::extract_osc_9_4_commands(switch2_delta);
        const auto expected_switch2 = tests::extract_osc_9_4_commands(
                strings_of(matrix.at("regularToFullscreen_streamingOnly_enabled_restoreTrue").at("writes")));
        CHECK(switch2_osc == expected_switch2);
        REQUIRE(switch2_osc.size() == 2);
        CHECK(switch2_osc[0] == "\x1b]9;4;0\x07");
        CHECK(switch2_osc[1] == "\x1b]9;4;3\x07");

        // Turn complete clears progress: exact vector comparison against agentEndEnabled
        const auto before_end1 = terminal.output().size();
        gated->release();
        REQUIRE(pump_until(io, [&] { return visible_screen(terminal).find("turn 1") != std::string::npos; }));
        std::span<const std::string> end1_delta(
                terminal.output().data() + before_end1, terminal.output().size() - before_end1);
        const auto end1_osc = tests::extract_osc_9_4_commands(end1_delta);
        const auto expected_turn_end =
                tests::extract_osc_9_4_commands(strings_of(lifecycle.at("agentEndEnabled").at("writes")));
        CHECK(end1_osc == expected_turn_end);
        CHECK(end1_osc == std::vector<std::string>{"\x1b]9;4;0\x07"});

        // Switch to Regular while idle: progress NOT restored
        const auto before_idle_switch1 = terminal.output().size();
        auto switched_idle1 = engine->switch_tui_mode(coding_agent::TuiMode::Regular, /* restore_progress */ true);
        REQUIRE(switched_idle1);
        CHECK(*switched_idle1);
        CHECK_FALSE(terminal.modes().alternate_screen);
        std::span<const std::string> idle_switch1_delta(
                terminal.output().data() + before_idle_switch1, terminal.output().size() - before_idle_switch1);
        const auto idle_switch1_osc = tests::extract_osc_9_4_commands(idle_switch1_delta);
        const auto expected_idle_switch1 = tests::extract_osc_9_4_commands(
                strings_of(matrix.at("fullscreenToRegular_idle_enabled_restoreTrue").at("writes")));
        CHECK(idle_switch1_osc == expected_idle_switch1);
        CHECK(idle_switch1_osc.empty());

        // Switch back to Fullscreen while idle: progress NOT restored
        const auto before_idle_switch2 = terminal.output().size();
        auto switched_idle2 = engine->switch_tui_mode(coding_agent::TuiMode::Fullscreen, /* restore_progress */ true);
        REQUIRE(switched_idle2);
        CHECK(*switched_idle2);
        CHECK(terminal.modes().alternate_screen);
        std::span<const std::string> idle_switch2_delta(
                terminal.output().data() + before_idle_switch2, terminal.output().size() - before_idle_switch2);
        const auto idle_switch2_osc = tests::extract_osc_9_4_commands(idle_switch2_delta);
        const auto expected_idle_switch2 = tests::extract_osc_9_4_commands(
                strings_of(matrix.at("regularToFullscreen_idle_enabled_restoreTrue").at("writes")));
        CHECK(idle_switch2_osc == expected_idle_switch2);
        CHECK(idle_switch2_osc.empty());

        // Switch with restore_progress = false: progress NOT restored even if streaming
        // Direction 1: fullscreen -> regular with restore_progress = false
        const auto before_turn2 = terminal.output().size();
        REQUIRE(terminal.inject_input("second turn\r"));
        REQUIRE(pump_until(io, [&] { return gated->requests.size() == 2; }));
        std::span<const std::string> turn2_delta(
                terminal.output().data() + before_turn2, terminal.output().size() - before_turn2);
        CHECK(tests::extract_osc_9_4_commands(turn2_delta) == expected_turn_start);

        const auto before_no_restore1 = terminal.output().size();
        auto switched_no_restore1 =
                engine->switch_tui_mode(coding_agent::TuiMode::Regular, /* restore_progress */ false);
        REQUIRE(switched_no_restore1);
        CHECK(*switched_no_restore1);
        CHECK_FALSE(terminal.modes().alternate_screen);
        std::span<const std::string> no_restore1_delta(
                terminal.output().data() + before_no_restore1, terminal.output().size() - before_no_restore1);
        const auto no_restore1_osc = tests::extract_osc_9_4_commands(no_restore1_delta);
        const auto expected_no_restore1 = tests::extract_osc_9_4_commands(
                strings_of(matrix.at("fullscreenToRegular_streamingOnly_enabled_restoreFalse").at("writes")));
        CHECK(no_restore1_osc == expected_no_restore1);
        CHECK(no_restore1_osc == std::vector<std::string>{"\x1b]9;4;0\x07"});

        gated->release();
        REQUIRE(pump_until(io, [&] { return visible_screen(terminal).find("turn 2") != std::string::npos; }));

        // Direction 2: regular -> fullscreen with restore_progress = false
        const auto before_turn3 = terminal.output().size();
        REQUIRE(terminal.inject_input("third turn\r"));
        REQUIRE(pump_until(io, [&] { return gated->requests.size() == 3; }));
        std::span<const std::string> turn3_delta(
                terminal.output().data() + before_turn3, terminal.output().size() - before_turn3);
        CHECK(tests::extract_osc_9_4_commands(turn3_delta) == expected_turn_start);

        const auto before_no_restore2 = terminal.output().size();
        auto switched_no_restore2 =
                engine->switch_tui_mode(coding_agent::TuiMode::Fullscreen, /* restore_progress */ false);
        REQUIRE(switched_no_restore2);
        CHECK(*switched_no_restore2);
        CHECK(terminal.modes().alternate_screen);
        std::span<const std::string> no_restore2_delta(
                terminal.output().data() + before_no_restore2, terminal.output().size() - before_no_restore2);
        const auto no_restore2_osc = tests::extract_osc_9_4_commands(no_restore2_delta);
        CHECK(no_restore2_osc == std::vector<std::string>{"\x1b]9;4;0\x07"});

        gated->release();
        REQUIRE(pump_until(io, [&] { return visible_screen(terminal).find("turn 3") != std::string::npos; }));

        REQUIRE(terminal.inject_input("\x04"));
        REQUIRE(pump_until(io, [&] { return run_result.has_value(); }));
        REQUIRE(run_result);
        CHECK(*run_result);
    }

    // ── Case 2: default showTerminalProgress = false ─────────────────────────
    {
        tests::RuntimeFixture runtime;
        tests::TempWorkspace workspace;
        tests::TempWorkspace config;
        config.write("settings.json", R"({"tuiMode": "fullscreen"})");
        auto client = std::make_shared<tests::GatedChatProvider>();
        auto* gated = client.get();
        auto opts = session_options(workspace, std::move(client));
        opts.session_target = coding_agent::ExplicitOpenOrCreateSessionTarget{workspace.path() / "session.jsonl"};
        auto created = runtime.run(create_session_async(runtime, std::move(opts)));
        REQUIRE(created);
        tests::RuntimeLoopDriver runtime_driver(runtime);

        tui::VirtualTerminal terminal({.columns = 80, .rows = 24});
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

        const auto before_turn = terminal.output().size();
        REQUIRE(terminal.inject_input("pending turn\r"));
        REQUIRE(pump_until(io, [&] { return gated->requests.size() == 1; }));
        // When disabled, turn start does NOT emit progress: matches turnStartDisabled
        std::span<const std::string> turn_delta(
                terminal.output().data() + before_turn, terminal.output().size() - before_turn);
        const auto turn_osc = tests::extract_osc_9_4_commands(turn_delta);
        const auto expected_turn_disabled =
                tests::extract_osc_9_4_commands(strings_of(lifecycle.at("turnStartDisabled").at("writes")));
        CHECK(turn_osc == expected_turn_disabled);
        CHECK(turn_osc.empty());

        // Switch fullscreen -> regular does NOT restore progress when disabled: matches
        // fullscreenToRegular_streamingOnly_disabled_restoreTrue
        const auto before_switch1 = terminal.output().size();
        auto switched1 = engine->switch_tui_mode(coding_agent::TuiMode::Regular, /* restore_progress */ true);
        REQUIRE(switched1);
        CHECK(*switched1);
        std::span<const std::string> switch1_delta(
                terminal.output().data() + before_switch1, terminal.output().size() - before_switch1);
        const auto switch1_osc = tests::extract_osc_9_4_commands(switch1_delta);
        const auto expected_switch1_disabled = tests::extract_osc_9_4_commands(
                strings_of(matrix.at("fullscreenToRegular_streamingOnly_disabled_restoreTrue").at("writes")));
        CHECK(switch1_osc == expected_switch1_disabled);
        CHECK(switch1_osc.empty());

        // Switch regular -> fullscreen does NOT restore progress when disabled: matches
        // regularToFullscreen_streamingOnly_disabled_restoreTrue
        const auto before_switch2 = terminal.output().size();
        auto switched2 = engine->switch_tui_mode(coding_agent::TuiMode::Fullscreen, /* restore_progress */ true);
        REQUIRE(switched2);
        CHECK(*switched2);
        std::span<const std::string> switch2_delta(
                terminal.output().data() + before_switch2, terminal.output().size() - before_switch2);
        const auto switch2_osc = tests::extract_osc_9_4_commands(switch2_delta);
        const auto expected_switch2_disabled = tests::extract_osc_9_4_commands(
                strings_of(matrix.at("regularToFullscreen_streamingOnly_disabled_restoreTrue").at("writes")));
        CHECK(switch2_osc == expected_switch2_disabled);
        CHECK(switch2_osc.empty());

        // Turn completes: matches agentEndDisabled
        const auto before_end = terminal.output().size();
        gated->release();
        REQUIRE(pump_until(io, [&] { return visible_screen(terminal).find("turn 1") != std::string::npos; }));
        std::span<const std::string> end_delta(
                terminal.output().data() + before_end, terminal.output().size() - before_end);
        const auto end_osc = tests::extract_osc_9_4_commands(end_delta);
        const auto expected_end_disabled =
                tests::extract_osc_9_4_commands(strings_of(lifecycle.at("agentEndDisabled").at("writes")));
        CHECK(end_osc == expected_end_disabled);
        CHECK(end_osc.empty());

        // Complete a second turn so the session history has entries to compact
        REQUIRE(terminal.inject_input("second\r"));
        REQUIRE(pump_until(io, [&] { return gated->requests.size() == 2; }));
        gated->release();
        REQUIRE(pump_until(io, [&] { return visible_screen(terminal).find("turn 2") != std::string::npos; }));

        // Register session subscription observer to synchronize compaction start event delivery
        bool comp_disabled_event_delivered = false;
        auto comp_disabled_sub = created->session->subscribe_session(
                [&](const coding_agent::AgentSessionEvent& ev) -> support::ExpectedVoid {
                    if (std::holds_alternative<coding_agent::CompactionStartEvent>(ev)) {
                        comp_disabled_event_delivered = true;
                    }
                    return {};
                });
        REQUIRE(comp_disabled_sub);

        const auto before_comp_disabled = terminal.output().size();
        std::optional<support::Expected<coding_agent::CompactionResult>> compact_disabled_result;
        boost::asio::co_spawn(
                io,
                [&]() -> boost::asio::awaitable<void> {
                    compact_disabled_result = co_await created->session->compact();
                    co_return;
                },
                boost::asio::detached);

        // Synchronize on compaction start event delivery: matches compactionStartDisabled
        REQUIRE(pump_until(io, [&] { return comp_disabled_event_delivered; }));
        std::span<const std::string> comp_start_disabled_delta(
                terminal.output().data() + before_comp_disabled, terminal.output().size() - before_comp_disabled);
        const auto comp_start_disabled_osc = tests::extract_osc_9_4_commands(comp_start_disabled_delta);
        const auto expected_comp_start_disabled =
                tests::extract_osc_9_4_commands(strings_of(lifecycle.at("compactionStartDisabled").at("writes")));
        CHECK(comp_start_disabled_osc == expected_comp_start_disabled);
        CHECK(comp_start_disabled_osc.empty());

        // Mode switch during disabled compaction: fullscreen -> regular (restoreProgress = true)
        // Matches fullscreenToRegular_compactingOnly_disabled_restoreTrue
        const auto before_switch_cd1 = terminal.output().size();
        auto switched_cd1 = engine->switch_tui_mode(coding_agent::TuiMode::Regular, /* restore_progress */ true);
        REQUIRE(switched_cd1);
        CHECK(*switched_cd1);
        std::span<const std::string> switch_cd1_delta(
                terminal.output().data() + before_switch_cd1, terminal.output().size() - before_switch_cd1);
        const auto switch_cd1_osc = tests::extract_osc_9_4_commands(switch_cd1_delta);
        const auto expected_switch_cd1 = tests::extract_osc_9_4_commands(
                strings_of(matrix.at("fullscreenToRegular_compactingOnly_disabled_restoreTrue").at("writes")));
        CHECK(switch_cd1_osc == expected_switch_cd1);
        CHECK(switch_cd1_osc.empty());

        // Reverse mode switch during disabled compaction: regular -> fullscreen (restoreProgress = true)
        // Matches regularToFullscreen_compactingOnly_disabled_restoreTrue
        const auto before_switch_cd2 = terminal.output().size();
        auto switched_cd2 = engine->switch_tui_mode(coding_agent::TuiMode::Fullscreen, /* restore_progress */ true);
        REQUIRE(switched_cd2);
        CHECK(*switched_cd2);
        std::span<const std::string> switch_cd2_delta(
                terminal.output().data() + before_switch_cd2, terminal.output().size() - before_switch_cd2);
        const auto switch_cd2_osc = tests::extract_osc_9_4_commands(switch_cd2_delta);
        const auto expected_switch_cd2 = tests::extract_osc_9_4_commands(
                strings_of(matrix.at("regularToFullscreen_compactingOnly_disabled_restoreTrue").at("writes")));
        CHECK(switch_cd2_osc == expected_switch_cd2);
        CHECK(switch_cd2_osc.empty());

        // Releasing summarization completes compaction: matches compactionEndDisabled
        const auto before_comp_end_disabled = terminal.output().size();
        gated->release();
        REQUIRE(pump_until(io, [&] { return compact_disabled_result.has_value(); }));
        std::span<const std::string> comp_end_disabled_delta(terminal.output().data() + before_comp_end_disabled,
                terminal.output().size() - before_comp_end_disabled);
        const auto comp_end_disabled_osc = tests::extract_osc_9_4_commands(comp_end_disabled_delta);
        const auto expected_comp_end_disabled =
                tests::extract_osc_9_4_commands(strings_of(lifecycle.at("compactionEndDisabled").at("writes")));
        CHECK(comp_end_disabled_osc == expected_comp_end_disabled);
        CHECK(comp_end_disabled_osc.empty());

        // Idle switches in both directions emit no progress
        const auto before_idle1 = terminal.output().size();
        auto switched_idle1 = engine->switch_tui_mode(coding_agent::TuiMode::Regular, /* restore_progress */ true);
        REQUIRE(switched_idle1);
        std::span<const std::string> idle1_delta(
                terminal.output().data() + before_idle1, terminal.output().size() - before_idle1);
        CHECK(tests::extract_osc_9_4_commands(idle1_delta).empty());

        const auto before_idle2 = terminal.output().size();
        auto switched_idle2 = engine->switch_tui_mode(coding_agent::TuiMode::Fullscreen, /* restore_progress */ true);
        REQUIRE(switched_idle2);
        std::span<const std::string> idle2_delta(
                terminal.output().data() + before_idle2, terminal.output().size() - before_idle2);
        CHECK(tests::extract_osc_9_4_commands(idle2_delta).empty());

        REQUIRE(terminal.inject_input("\x04"));
        REQUIRE(pump_until(io, [&] { return run_result.has_value(); }));
        REQUIRE(run_result);
        CHECK(*run_result);
    }

    // ── Case 3: compaction lifecycle and switch restoration in both directions ──
    {
        tests::RuntimeFixture runtime;
        tests::TempWorkspace workspace;
        tests::TempWorkspace config;
        config.write("settings.json", R"({"tuiMode": "fullscreen", "terminal": {"showTerminalProgress": true}})");
        auto client = std::make_shared<tests::GatedChatProvider>();
        auto* gated = client.get();
        auto opts = session_options(workspace, std::move(client));
        opts.session_target = coding_agent::ExplicitOpenOrCreateSessionTarget{workspace.path() / "session.jsonl"};
        auto created = runtime.run(create_session_async(runtime, std::move(opts)));
        REQUIRE(created);
        tests::RuntimeLoopDriver runtime_driver(runtime);

        tui::VirtualTerminal terminal({.columns = 80, .rows = 24});
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

        // Complete two turns so the session history has entries to compact.
        REQUIRE(terminal.inject_input("first\r"));
        REQUIRE(pump_until(io, [&] { return gated->requests.size() == 1; }));
        gated->release();
        REQUIRE(pump_until(io, [&] { return visible_screen(terminal).find("turn 1") != std::string::npos; }));

        REQUIRE(terminal.inject_input("second\r"));
        REQUIRE(pump_until(io, [&] { return gated->requests.size() == 2; }));
        gated->release();
        REQUIRE(pump_until(io, [&] { return visible_screen(terminal).find("turn 2") != std::string::npos; }));

        // Register session subscription observer to synchronize compaction start event delivery
        bool compaction_event_delivered = false;
        auto comp_sub = created->session->subscribe_session(
                [&](const coding_agent::AgentSessionEvent& ev) -> support::ExpectedVoid {
                    if (std::holds_alternative<coding_agent::CompactionStartEvent>(ev)) {
                        compaction_event_delivered = true;
                    }
                    return {};
                });
        REQUIRE(comp_sub);

        const auto before_compaction = terminal.output().size();
        std::optional<support::Expected<coding_agent::CompactionResult>> compact_result;
        boost::asio::co_spawn(
                io,
                [&]() -> boost::asio::awaitable<void> {
                    compact_result = co_await created->session->compact();
                    co_return;
                },
                boost::asio::detached);

        // Synchronize to actual compaction event delivery to avoid racing UI subscriber
        REQUIRE(pump_until(io, [&] { return compaction_event_delivered; }));
        std::span<const std::string> comp_start_delta(
                terminal.output().data() + before_compaction, terminal.output().size() - before_compaction);
        const auto comp_start_osc = tests::extract_osc_9_4_commands(comp_start_delta);
        const auto expected_comp_start =
                tests::extract_osc_9_4_commands(strings_of(lifecycle.at("compactionStartEnabled").at("writes")));
        CHECK(comp_start_osc == expected_comp_start);
        CHECK(comp_start_osc == std::vector<std::string>{"\x1b]9;4;3\x07"});

        // Mode switch during compaction: fullscreen -> regular (restoreProgress = true)
        const auto before_switch_c1 = terminal.output().size();
        auto switched_c1 = engine->switch_tui_mode(coding_agent::TuiMode::Regular, /* restore_progress */ true);
        REQUIRE(switched_c1);
        CHECK(*switched_c1);
        std::span<const std::string> switch_c1_delta(
                terminal.output().data() + before_switch_c1, terminal.output().size() - before_switch_c1);
        const auto switch_c1_osc = tests::extract_osc_9_4_commands(switch_c1_delta);
        const auto expected_switch_c1 = tests::extract_osc_9_4_commands(
                strings_of(matrix.at("fullscreenToRegular_compactingOnly_enabled_restoreTrue").at("writes")));
        CHECK(switch_c1_osc == expected_switch_c1);
        REQUIRE(switch_c1_osc.size() == 2);
        CHECK(switch_c1_osc[0] == "\x1b]9;4;0\x07");
        CHECK(switch_c1_osc[1] == "\x1b]9;4;3\x07");

        // Reverse mode switch during compaction: regular -> fullscreen (restoreProgress = true)
        const auto before_switch_c2 = terminal.output().size();
        auto switched_c2 = engine->switch_tui_mode(coding_agent::TuiMode::Fullscreen, /* restore_progress */ true);
        REQUIRE(switched_c2);
        CHECK(*switched_c2);
        std::span<const std::string> switch_c2_delta(
                terminal.output().data() + before_switch_c2, terminal.output().size() - before_switch_c2);
        const auto switch_c2_osc = tests::extract_osc_9_4_commands(switch_c2_delta);
        const auto expected_switch_c2 = tests::extract_osc_9_4_commands(
                strings_of(matrix.at("regularToFullscreen_compactingOnly_enabled_restoreTrue").at("writes")));
        CHECK(switch_c2_osc == expected_switch_c2);
        REQUIRE(switch_c2_osc.size() == 2);
        CHECK(switch_c2_osc[0] == "\x1b]9;4;0\x07");
        CHECK(switch_c2_osc[1] == "\x1b]9;4;3\x07");

        // Mode switch during compaction with restore_progress = false: regular -> fullscreen suppression
        const auto before_switch_c3 = terminal.output().size();
        auto switched_c3 = engine->switch_tui_mode(coding_agent::TuiMode::Regular, /* restore_progress */ false);
        REQUIRE(switched_c3);
        CHECK(*switched_c3);
        std::span<const std::string> switch_c3_delta(
                terminal.output().data() + before_switch_c3, terminal.output().size() - before_switch_c3);
        const auto switch_c3_osc = tests::extract_osc_9_4_commands(switch_c3_delta);
        CHECK(switch_c3_osc == std::vector<std::string>{"\x1b]9;4;0\x07"});

        // Releasing summarization completes compaction and emits clear sequence
        const auto before_comp_end = terminal.output().size();
        gated->release();
        REQUIRE(pump_until(io, [&] { return compact_result.has_value(); }));
        std::span<const std::string> comp_end_delta(
                terminal.output().data() + before_comp_end, terminal.output().size() - before_comp_end);
        const auto comp_end_osc = tests::extract_osc_9_4_commands(comp_end_delta);
        const auto expected_comp_end =
                tests::extract_osc_9_4_commands(strings_of(lifecycle.at("compactionEndEnabled").at("writes")));
        CHECK(comp_end_osc == expected_comp_end);
        CHECK(comp_end_osc == std::vector<std::string>{"\x1b]9;4;0\x07"});

        REQUIRE(terminal.inject_input("\x04"));
        REQUIRE(pump_until(io, [&] { return run_result.has_value(); }));
        REQUIRE(run_result);
        CHECK(*run_result);
    }

    // ── Case 4: settings selector UI navigation, persisted true->false re-read, agent_end clear-skip while disabled,
    // and stale progress teardown safety net ──
    {
        tests::RuntimeFixture runtime;
        tests::TempWorkspace workspace;
        tests::TempWorkspace config;
        config.write("settings.json", R"({"tuiMode": "fullscreen", "terminal": {"showTerminalProgress": true}})");
        auto client = std::make_shared<tests::GatedChatProvider>();
        auto* gated = client.get();
        auto created = runtime.run(create_session_async(runtime, session_options(workspace, std::move(client))));
        REQUIRE(created);
        tests::RuntimeLoopDriver runtime_driver(runtime);

        tui::VirtualTerminal terminal({.columns = 80, .rows = 24});
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

        // Turn start activates progress.
        const auto before_turn = terminal.output().size();
        REQUIRE(terminal.inject_input("pending turn\r"));
        REQUIRE(pump_until(io, [&] { return gated->requests.size() == 1; }));
        std::span<const std::string> turn_delta(
                terminal.output().data() + before_turn, terminal.output().size() - before_turn);
        CHECK(tests::extract_osc_9_4_commands(turn_delta) == std::vector<std::string>{"\x1b]9;4;3\x07"});

        // Open settings selector and navigate to Terminal progress row
        focus_terminal_progress_row(terminal, io);
        CHECK(on_terminal_progress_row(terminal));
        const auto screen_before = visible_screen(terminal);
        CHECK(screen_before.find("Terminal progress") != std::string::npos);
        CHECK(screen_before.find("true") != std::string::npos);

        // Enter cycles true -> false: updates settings persistence ONLY, no immediate OSC 9;4 sequence
        const auto before_toggle = terminal.output().size();
        REQUIRE(terminal.inject_input("\r"));
        drain_ready(io);
        REQUIRE(pump_until(io, [&] {
            return read_text_file(config.path() / "settings.json").find("\"showTerminalProgress\": false") !=
                   std::string::npos;
        }));
        std::span<const std::string> toggle_delta(
                terminal.output().data() + before_toggle, terminal.output().size() - before_toggle);
        CHECK(tests::extract_osc_9_4_commands(toggle_delta).empty());

        // Verify independent SettingsManager re-read reflects persisted false (distinct from live /reload round-trip)
        auto reloaded_mgr1 = coding_agent::SettingsManager::create(workspace.path(), config.path(), true);
        CHECK_FALSE(reloaded_mgr1.get_show_terminal_progress());

        // Close settings
        close_terminal_progress_settings(terminal, io);

        // Releasing turn completes; because it was toggled false, agent_end sees false and skips clear
        const auto before_end = terminal.output().size();
        gated->release();
        REQUIRE(pump_until(io, [&] { return visible_screen(terminal).find("turn 1") != std::string::npos; }));
        std::span<const std::string> end_delta(
                terminal.output().data() + before_end, terminal.output().size() - before_end);
        CHECK(tests::extract_osc_9_4_commands(end_delta).empty());

        // Close engine: stop teardown safety net triggers clear count 1
        const auto before_close = terminal.output().size();
        REQUIRE(terminal.inject_input("\x04"));
        REQUIRE(pump_until(io, [&] { return run_result.has_value(); }));
        REQUIRE(run_result);
        CHECK(*run_result);

        std::span<const std::string> close_delta(
                terminal.output().data() + before_close, terminal.output().size() - before_close);
        const auto close_osc = tests::extract_osc_9_4_commands(close_delta);
        // Clear count is exactly 1 (from shutdown teardown safety net)
        CHECK(close_osc == std::vector<std::string>{"\x1b]9;4;0\x07"});
    }

    // ── Case 5: real UI toggle to false, active-stop clear on first mode switch, and preference continuity across
    // bidirectional switches and subsequent disabled turn/compaction (composed boundary) ──
    {
        // Composed boundary note:
        // Frozen artifact 29 interactive-mode-switch-matrix disabled entries record switch transitions that started
        // while progress was already inactive. This test verifies the composition of two independently observed
        // behaviors:
        // 1. Terminal stop clear on active progress (independently captured in artifact 29 Scenario 1 stopActive and
        //    Scenario 4 Phase 4), triggered here because switching renderer modes physically stops the retired UI root.
        // 2. Disabled restore suppression across bidirectional switches and preference continuity into subsequent
        //    turn and compaction lifecycles.
        tests::RuntimeFixture runtime;
        tests::TempWorkspace workspace;
        tests::TempWorkspace config;
        config.write("settings.json", R"({"tuiMode": "fullscreen", "terminal": {"showTerminalProgress": true}})");
        auto client = std::make_shared<tests::GatedChatProvider>();
        auto* gated = client.get();
        auto opts = session_options(workspace, std::move(client));
        opts.session_target = coding_agent::ExplicitOpenOrCreateSessionTarget{workspace.path() / "session.jsonl"};
        auto created = runtime.run(create_session_async(runtime, std::move(opts)));
        REQUIRE(created);
        tests::RuntimeLoopDriver runtime_driver(runtime);

        tui::VirtualTerminal terminal({.columns = 80, .rows = 24});
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

        // Turn start activates progress
        const auto before_turn = terminal.output().size();
        REQUIRE(terminal.inject_input("active turn\r"));
        REQUIRE(pump_until(io, [&] { return gated->requests.size() == 1; }));
        std::span<const std::string> turn_delta(
                terminal.output().data() + before_turn, terminal.output().size() - before_turn);
        CHECK(tests::extract_osc_9_4_commands(turn_delta) == std::vector<std::string>{"\x1b]9;4;3\x07"});

        // Open settings selector and navigate to Terminal progress row
        focus_terminal_progress_row(terminal, io);
        CHECK(on_terminal_progress_row(terminal));
        const auto screen_before = visible_screen(terminal);
        CHECK(screen_before.find("Terminal progress") != std::string::npos);
        CHECK(screen_before.find("true") != std::string::npos);

        // Enter cycles true -> false: updates settings persistence ONLY
        const auto before_toggle = terminal.output().size();
        REQUIRE(terminal.inject_input("\r"));
        drain_ready(io);
        REQUIRE(pump_until(io, [&] {
            return read_text_file(config.path() / "settings.json").find("\"showTerminalProgress\": false") !=
                   std::string::npos;
        }));
        std::span<const std::string> toggle_delta(
                terminal.output().data() + before_toggle, terminal.output().size() - before_toggle);
        CHECK(tests::extract_osc_9_4_commands(toggle_delta).empty());

        // Close settings
        close_terminal_progress_settings(terminal, io);

        // First switch: fullscreen -> regular while streaming is still active, but setting was toggled to false.
        // The retiring fullscreen UI root stops, physically clearing the previously active progress
        // (\x1b]9;4;0\x07). The target regular UI root starts, but because showTerminalProgress is now false,
        // it suppresses restore.
        const auto before_switch1 = terminal.output().size();
        auto switched1 = engine->switch_tui_mode(coding_agent::TuiMode::Regular, /* restore_progress */ true);
        REQUIRE(switched1);
        CHECK(*switched1);
        std::span<const std::string> switch1_delta(
                terminal.output().data() + before_switch1, terminal.output().size() - before_switch1);
        const auto switch1_osc = tests::extract_osc_9_4_commands(switch1_delta);
        CHECK(switch1_osc == std::vector<std::string>{"\x1b]9;4;0\x07"});

        // Reverse switch: regular -> fullscreen while streaming is still active and setting is false.
        // The retiring regular UI root was never active, so its stop emits nothing; the incoming fullscreen
        // root sees showTerminalProgress false and restores nothing. Zero writes.
        const auto before_switch2 = terminal.output().size();
        auto switched2 = engine->switch_tui_mode(coding_agent::TuiMode::Fullscreen, /* restore_progress */ true);
        REQUIRE(switched2);
        CHECK(*switched2);
        std::span<const std::string> switch2_delta(
                terminal.output().data() + before_switch2, terminal.output().size() - before_switch2);
        const auto switch2_osc = tests::extract_osc_9_4_commands(switch2_delta);
        CHECK(switch2_osc.empty());

        // Complete the first turn: setting is false and progress was already cleared; agent_end emits nothing.
        const auto before_end1 = terminal.output().size();
        gated->release();
        REQUIRE(pump_until(io, [&] { return visible_screen(terminal).find("turn 1") != std::string::npos; }));
        std::span<const std::string> end1_delta(
                terminal.output().data() + before_end1, terminal.output().size() - before_end1);
        CHECK(tests::extract_osc_9_4_commands(end1_delta).empty());

        // Subsequent new turn: preference continuity ensures disabled setting suppresses turn_start and agent_end.
        const auto before_turn2 = terminal.output().size();
        REQUIRE(terminal.inject_input("turn two\r"));
        REQUIRE(pump_until(io, [&] { return gated->requests.size() == 2; }));
        std::span<const std::string> turn2_delta(
                terminal.output().data() + before_turn2, terminal.output().size() - before_turn2);
        CHECK(tests::extract_osc_9_4_commands(turn2_delta).empty());

        const auto before_end2 = terminal.output().size();
        gated->release();
        REQUIRE(pump_until(io, [&] { return visible_screen(terminal).find("turn 2") != std::string::npos; }));
        std::span<const std::string> end2_delta(
                terminal.output().data() + before_end2, terminal.output().size() - before_end2);
        CHECK(tests::extract_osc_9_4_commands(end2_delta).empty());

        // Subsequent compaction: preference continuity ensures disabled compaction emits no progress writes.
        bool comp_delivered = false;
        auto comp_sub = created->session->subscribe_session(
                [&](const coding_agent::AgentSessionEvent& ev) -> support::ExpectedVoid {
                    if (std::holds_alternative<coding_agent::CompactionStartEvent>(ev)) {
                        comp_delivered = true;
                    }
                    return {};
                });
        REQUIRE(comp_sub);

        const auto before_comp = terminal.output().size();
        std::optional<support::Expected<coding_agent::CompactionResult>> comp_res;
        boost::asio::co_spawn(
                io,
                [&]() -> boost::asio::awaitable<void> {
                    comp_res = co_await created->session->compact();
                    co_return;
                },
                boost::asio::detached);
        REQUIRE(pump_until(io, [&] { return comp_delivered; }));
        std::span<const std::string> comp_start_delta(
                terminal.output().data() + before_comp, terminal.output().size() - before_comp);
        CHECK(tests::extract_osc_9_4_commands(comp_start_delta).empty());

        const auto before_comp_end = terminal.output().size();
        gated->release();
        REQUIRE(pump_until(io, [&] { return comp_res.has_value(); }));
        std::span<const std::string> comp_end_delta(
                terminal.output().data() + before_comp_end, terminal.output().size() - before_comp_end);
        CHECK(tests::extract_osc_9_4_commands(comp_end_delta).empty());

        // Teardown: progress was cleared at the first mode switch and remained inactive, so terminal.stop() emits
        // nothing.
        const auto before_close = terminal.output().size();
        REQUIRE(terminal.inject_input("\x04"));
        REQUIRE(pump_until(io, [&] { return run_result.has_value(); }));
        REQUIRE(run_result);
        CHECK(*run_result);
        std::span<const std::string> close_delta(
                terminal.output().data() + before_close, terminal.output().size() - before_close);
        CHECK(tests::extract_osc_9_4_commands(close_delta).empty());
    }
}

TEST_CASE("Native TUI terminal progress teardown clear counts match independent frozen observation across all four "
          "lifecycle states",
        "[coding_agent][tui][progress][teardown][issue1037][spec]") {
    const auto artifact = tests::read_pi_tui_evidence("product-progress-protocol.json");
    REQUIRE(artifact);
    const auto& bundle = *artifact;
    const auto& counts =
            find_evidence_scenario(bundle, "interactive-mode-stop-clear-counts").at("expected").at("counts");

    // Subcase 1: activeEnabled -> 1 clear
    {
        tests::RuntimeFixture runtime;
        tests::TempWorkspace workspace;
        tests::TempWorkspace config;
        config.write("settings.json", R"({"tuiMode": "fullscreen", "terminal": {"showTerminalProgress": true}})");
        auto client = std::make_shared<tests::GatedChatProvider>();
        auto created = runtime.run(create_session_async(runtime, session_options(workspace, std::move(client))));
        REQUIRE(created);
        tests::RuntimeLoopDriver runtime_driver(runtime);

        tui::VirtualTerminal terminal({.columns = 80, .rows = 24});
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

        // Active state established at stop (matches artifact 29 Scenario 5.1: activeAtStop = true)
        REQUIRE(terminal.set_progress(true));

        // Stop while active: teardown clear count
        const auto before_stop = terminal.output().size();
        REQUIRE(terminal.inject_input("\x04"));
        REQUIRE(pump_until(io, [&] { return run_result.has_value(); }));
        std::span<const std::string> stop_delta(
                terminal.output().data() + before_stop, terminal.output().size() - before_stop);
        const auto stop_osc = tests::extract_osc_9_4_commands(stop_delta);
        const auto expected_clears =
                static_cast<std::size_t>(counts.at("activeEnabled").at("clearsEmitted").get_number());
        CHECK(stop_osc.size() == expected_clears);
        CHECK(stop_osc == std::vector<std::string>{"\x1b]9;4;0\x07"});
    }

    // Subcase 2: idleDisabled -> 0 clears
    {
        tests::RuntimeFixture runtime;
        tests::TempWorkspace workspace;
        tests::TempWorkspace config;
        config.write("settings.json", R"({"tuiMode": "fullscreen", "terminal": {"showTerminalProgress": false}})");
        auto client = std::make_shared<tests::GatedChatProvider>();
        auto created = runtime.run(create_session_async(runtime, session_options(workspace, std::move(client))));
        REQUIRE(created);
        tests::RuntimeLoopDriver runtime_driver(runtime);

        tui::VirtualTerminal terminal({.columns = 80, .rows = 24});
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

        // Stop while idle, disabled
        const auto before_stop = terminal.output().size();
        REQUIRE(terminal.inject_input("\x04"));
        REQUIRE(pump_until(io, [&] { return run_result.has_value(); }));
        std::span<const std::string> stop_delta(
                terminal.output().data() + before_stop, terminal.output().size() - before_stop);
        const auto stop_osc = tests::extract_osc_9_4_commands(stop_delta);
        const auto expected_clears =
                static_cast<std::size_t>(counts.at("idleDisabled").at("clearsEmitted").get_number());
        CHECK(stop_osc.size() == expected_clears);
        CHECK(stop_osc.empty());
    }

    // Subcase 3: idleEnabled -> 1 clear
    {
        tests::RuntimeFixture runtime;
        tests::TempWorkspace workspace;
        tests::TempWorkspace config;
        config.write("settings.json", R"({"tuiMode": "fullscreen", "terminal": {"showTerminalProgress": true}})");
        auto client = std::make_shared<tests::GatedChatProvider>();
        auto* gated = client.get();
        auto created = runtime.run(create_session_async(runtime, session_options(workspace, std::move(client))));
        REQUIRE(created);
        tests::RuntimeLoopDriver runtime_driver(runtime);

        tui::VirtualTerminal terminal({.columns = 80, .rows = 24});
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

        // Complete one turn so session is idle after turn completion
        REQUIRE(terminal.inject_input("first turn\r"));
        REQUIRE(pump_until(io, [&] { return gated->requests.size() == 1; }));
        gated->release();
        REQUIRE(pump_until(io, [&] { return visible_screen(terminal).find("turn 1") != std::string::npos; }));

        // Stop while idle, enabled
        const auto before_stop = terminal.output().size();
        REQUIRE(terminal.inject_input("\x04"));
        REQUIRE(pump_until(io, [&] { return run_result.has_value(); }));
        std::span<const std::string> stop_delta(
                terminal.output().data() + before_stop, terminal.output().size() - before_stop);
        const auto stop_osc = tests::extract_osc_9_4_commands(stop_delta);
        const auto expected_clears =
                static_cast<std::size_t>(counts.at("idleEnabled").at("clearsEmitted").get_number());
        CHECK(stop_osc.size() == expected_clears);
        CHECK(stop_osc == std::vector<std::string>{"\x1b]9;4;0\x07"});
    }

    // Subcase 4: activeToggledDisabled -> 1 clear
    {
        tests::RuntimeFixture runtime;
        tests::TempWorkspace workspace;
        tests::TempWorkspace config;
        config.write("settings.json", R"({"tuiMode": "fullscreen", "terminal": {"showTerminalProgress": true}})");
        auto client = std::make_shared<tests::GatedChatProvider>();
        auto* gated = client.get();
        auto created = runtime.run(create_session_async(runtime, session_options(workspace, std::move(client))));
        REQUIRE(created);
        tests::RuntimeLoopDriver runtime_driver(runtime);

        tui::VirtualTerminal terminal({.columns = 80, .rows = 24});
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

        // Turn start activates progress
        REQUIRE(terminal.inject_input("pending turn\r"));
        REQUIRE(pump_until(io, [&] { return gated->requests.size() == 1; }));

        // Toggle setting to false via UI
        focus_terminal_progress_row(terminal, io);
        REQUIRE(terminal.inject_input("\r"));
        drain_ready(io);
        REQUIRE(pump_until(io, [&] {
            return read_text_file(config.path() / "settings.json").find("\"showTerminalProgress\": false") !=
                   std::string::npos;
        }));
        close_terminal_progress_settings(terminal, io);

        // Turn ends: because setting is false, agent_end skips clear
        gated->release();
        REQUIRE(pump_until(io, [&] { return visible_screen(terminal).find("turn 1") != std::string::npos; }));

        // Stop: teardown safety net emits clear for orphaned active progress
        const auto before_stop = terminal.output().size();
        REQUIRE(terminal.inject_input("\x04"));
        REQUIRE(pump_until(io, [&] { return run_result.has_value(); }));
        std::span<const std::string> stop_delta(
                terminal.output().data() + before_stop, terminal.output().size() - before_stop);
        const auto stop_osc = tests::extract_osc_9_4_commands(stop_delta);
        const auto expected_clears =
                static_cast<std::size_t>(counts.at("activeToggledDisabled").at("clearsEmitted").get_number());
        CHECK(stop_osc.size() == expected_clears);
        CHECK(stop_osc == std::vector<std::string>{"\x1b]9;4;0\x07"});
    }
}

TEST_CASE("Native TUI reload reapplies showTerminalProgress preference from settings",
        "[coding_agent][tui][progress][reload][issue1037][spec]") {
    tests::RuntimeFixture runtime;
    tests::TempWorkspace workspace;
    tests::TempWorkspace config;
    config.write("settings.json", R"({"tuiMode": "fullscreen", "terminal": {"showTerminalProgress": false}})");
    auto created = runtime.run(
            create_session_async(runtime, session_options(workspace, tests::make_scripted_fake_provider())));
    REQUIRE(created);
    tests::RuntimeLoopDriver runtime_driver(runtime);

    tui::VirtualTerminal terminal({.columns = 80, .rows = 24});
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
    REQUIRE(pump_until(io, [&] { return engine->focus() != nullptr; }));

    // Mutate file on disk to true
    config.write("settings.json", R"({"tuiMode": "fullscreen", "terminal": {"showTerminalProgress": true}})");

    // Trigger /reload
    REQUIRE(terminal.inject_input("/reload\r"));
    drain_ready(io);
    focus_terminal_progress_row(terminal, io);
    const auto screen1 = visible_screen(terminal);
    CHECK(screen1.find("Terminal progress") != std::string::npos);
    CHECK(screen1.find("true") != std::string::npos);
    close_terminal_progress_settings(terminal, io);

    // Mutate file on disk to false
    config.write("settings.json", R"({"tuiMode": "fullscreen", "terminal": {"showTerminalProgress": false}})");

    // Trigger /reload again
    REQUIRE(terminal.inject_input("/reload\r"));
    drain_ready(io);
    focus_terminal_progress_row(terminal, io);
    const auto screen2 = visible_screen(terminal);
    CHECK(screen2.find("Terminal progress") != std::string::npos);
    CHECK(screen2.find("false") != std::string::npos);
    close_terminal_progress_settings(terminal, io);

    REQUIRE(terminal.inject_input("\x04"));
    REQUIRE(pump_until(io, [&] { return run_result.has_value(); }));
    REQUIRE(run_result);
    CHECK(*run_result);
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
        return screen.size() == 20 && (screen[19].find("fake-model") != std::string::npos ||
                                              screen[19].find("cpp-harness") != std::string::npos);
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

    // Text and cursor in fullscreen: draft prompt is entered and terminal cursor is hidden by default.
    REQUIRE(terminal.inject_input("draft prompt"));
    REQUIRE(pump_until(io, [&] { return visible_screen(terminal).find("draft prompt") != std::string::npos; }));
    const auto fullscreen_cursor = terminal.cursor();
    CHECK(fullscreen_cursor.row > 0);
    CHECK_FALSE(terminal.modes().cursor_visible);

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
    REQUIRE(pump_until(io, [&] { return terminal.cursor().row == fullscreen_cursor.row; }));
    CHECK_FALSE(terminal.modes().cursor_visible);
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

TEST_CASE("Native TUI InteractiveEngine resolves hardware cursor preference at startup",
        "[coding_agent][tui][cursor][issue1037][spec]") {
    struct StartupCase {
        coding_agent::TuiMode mode;
        std::optional<std::string> env_var;
        std::optional<std::string> config_json;
        bool expected_show_hardware_cursor;
        bool expected_cursor_visible;
    };

    const std::vector<StartupCase> startup_cases = {
            // 1. Regular mode with default/empty config
            {coding_agent::TuiMode::Regular, std::nullopt, std::nullopt, false, false},
            // 2. Regular mode with PI_HARDWARE_CURSOR=1 env fallback
            {coding_agent::TuiMode::Regular, "1", std::nullopt, true, true},
            // 3. Regular mode with explicit false override
            {coding_agent::TuiMode::Regular, "1", R"({"showHardwareCursor": false})", false, false},
            // 4. Regular mode with explicit true
            {coding_agent::TuiMode::Regular, std::nullopt, R"({"showHardwareCursor": true})", true, true},
            // 5. Fullscreen mode with default/empty config
            {coding_agent::TuiMode::Fullscreen, std::nullopt, R"({"tuiMode": "fullscreen"})", false, false},
            // 6. Fullscreen mode with PI_HARDWARE_CURSOR=1 env fallback
            {coding_agent::TuiMode::Fullscreen, "1", R"({"tuiMode": "fullscreen"})", true, true},
            // 7. Fullscreen mode with explicit false override
            {coding_agent::TuiMode::Fullscreen,
                    "1",
                    R"({"tuiMode": "fullscreen", "showHardwareCursor": false})",
                    false,
                    false},
            // 8. Fullscreen mode with explicit true
            {coding_agent::TuiMode::Fullscreen,
                    std::nullopt,
                    R"({"tuiMode": "fullscreen", "showHardwareCursor": true})",
                    true,
                    true},
    };

    for (const auto& test_case : startup_cases) {
        tests::EnvVarGuard env_guard{"PI_HARDWARE_CURSOR"};
        if (test_case.env_var) {
            env_guard.set(*test_case.env_var);
        } else {
            env_guard.unset();
        }

        tests::RuntimeFixture runtime;
        tests::TempWorkspace workspace;
        tests::TempWorkspace config;
        if (test_case.config_json) {
            config.write("settings.json", *test_case.config_json);
        }

        auto created = runtime.run(
                create_session_async(runtime, session_options(workspace, tests::make_scripted_fake_provider())));
        REQUIRE(created);
        tests::RuntimeLoopDriver runtime_driver(runtime);

        tui::VirtualTerminal terminal({.columns = 48, .rows = 24});
        boost::asio::io_context io;
        std::optional<support::ExpectedVoid> run_result;
        auto engine =
                std::make_shared<coding_agent::tui::InteractiveEngine>(terminal, io.get_executor(), test_case.mode);
        boost::asio::co_spawn(io,
                drive_engine(engine, make_run(*created->session, config.path(), test_case.mode)),
                [&](std::exception_ptr exception, support::ExpectedVoid result) {
                    CHECK(exception == nullptr);
                    run_result.emplace(std::move(result));
                });
        drain_ready(io);
        const tests::RunJoinGuard join_run{io, [&] { return run_result.has_value(); }};
        if (test_case.mode == coding_agent::TuiMode::Fullscreen) {
            REQUIRE(terminal.modes().alternate_screen);
        } else {
            REQUIRE_FALSE(terminal.modes().alternate_screen);
        }
        REQUIRE(pump_until(io, [&] { return engine->focus() != nullptr; }));

        CHECK(engine->show_hardware_cursor() == test_case.expected_show_hardware_cursor);
        CHECK(terminal.modes().cursor_visible == test_case.expected_cursor_visible);
        // Valid IME target positioned at editor prompt even when hidden
        CHECK(terminal.cursor().row > 0);

        REQUIRE(terminal.inject_input("\x04"));
        REQUIRE(pump_until(io, [&] { return run_result.has_value(); }));
        REQUIRE(run_result);
        CHECK(*run_result);
    }
}

TEST_CASE("Native TUI TUI mode switch preserves live hardware cursor preference in both directions",
        "[coding_agent][tui][cursor][renderer-switch][issue1037][spec]") {
    tests::EnvVarGuard env_guard{"PI_HARDWARE_CURSOR"};
    env_guard.unset();

    // 1. Regular -> Fullscreen with live preference true differing from setting (setting is false)
    {
        tests::RuntimeFixture runtime;
        tests::TempWorkspace workspace;
        tests::TempWorkspace config;
        config.write("settings.json", R"({"showHardwareCursor": false})");
        auto created = runtime.run(
                create_session_async(runtime, session_options(workspace, tests::make_scripted_fake_provider())));
        REQUIRE(created);
        tests::RuntimeLoopDriver runtime_driver(runtime);

        tui::VirtualTerminal terminal({.columns = 48, .rows = 24});
        boost::asio::io_context io;
        std::optional<support::ExpectedVoid> run_result;
        auto engine = std::make_shared<coding_agent::tui::InteractiveEngine>(
                terminal, io.get_executor(), coding_agent::TuiMode::Regular);
        boost::asio::co_spawn(io,
                drive_engine(engine, make_run(*created->session, config.path(), coding_agent::TuiMode::Regular)),
                [&](std::exception_ptr exception, support::ExpectedVoid result) {
                    CHECK(exception == nullptr);
                    run_result.emplace(std::move(result));
                });
        drain_ready(io);
        const tests::RunJoinGuard join_run{io, [&] { return run_result.has_value(); }};
        REQUIRE(pump_until(io, [&] { return engine->focus() != nullptr; }));

        // Type input into prompt to give editor cursor a distinct column
        REQUIRE(terminal.inject_input("prompt text"));
        drain_ready(io);
        REQUIRE(pump_until(io, [&] { return visible_screen(terminal).find("prompt text") != std::string::npos; }));

        const auto initial_cursor = terminal.cursor();
        CHECK(initial_cursor.column > 0);
        CHECK(initial_cursor.row > 0);
        CHECK_FALSE(engine->show_hardware_cursor());
        CHECK_FALSE(terminal.modes().cursor_visible);

        // Dynamically toggle live preference to true
        engine->set_show_hardware_cursor(true);
        drain_ready(io);
        REQUIRE(pump_until(io, [&] { return terminal.modes().cursor_visible; }));
        CHECK(engine->show_hardware_cursor());
        CHECK(terminal.cursor().column == initial_cursor.column);

        // Switch regular -> fullscreen: live preference true MUST transfer to the fullscreen root!
        auto switched = engine->switch_tui_mode(coding_agent::TuiMode::Fullscreen);
        REQUIRE(switched);
        REQUIRE(*switched);
        drain_ready(io);
        REQUIRE(pump_until(io, [&] { return terminal.modes().alternate_screen; }));

        // Fullscreen root must retain the live preference (true), not the stored setting (false)
        CHECK(engine->show_hardware_cursor());
        CHECK(terminal.modes().cursor_visible);
        // Target cursor cell coordinates maintained
        CHECK(terminal.cursor().column == 11);
        CHECK(terminal.cursor().row == 20);

        REQUIRE(terminal.inject_input("\x03\x04"));
        REQUIRE(pump_until(io, [&] { return run_result.has_value(); }));
        REQUIRE(run_result);
        CHECK(*run_result);
    }

    // 2. Fullscreen -> Regular with live preference false differing from setting (setting is true)
    {
        tests::RuntimeFixture runtime;
        tests::TempWorkspace workspace;
        tests::TempWorkspace config;
        config.write("settings.json", R"({"tuiMode": "fullscreen", "showHardwareCursor": true})");
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

        // Type input into prompt
        REQUIRE(terminal.inject_input("fullscreen text"));
        drain_ready(io);
        REQUIRE(pump_until(io, [&] { return visible_screen(terminal).find("fullscreen text") != std::string::npos; }));

        const auto initial_cursor = terminal.cursor();
        CHECK(initial_cursor.column == 15);
        CHECK(initial_cursor.row == 20);
        CHECK(engine->show_hardware_cursor());
        CHECK(terminal.modes().cursor_visible);

        // Dynamically toggle live preference to false
        engine->set_show_hardware_cursor(false);
        drain_ready(io);
        REQUIRE(pump_until(io, [&] { return !terminal.modes().cursor_visible; }));
        CHECK_FALSE(engine->show_hardware_cursor());
        CHECK(terminal.cursor().column == initial_cursor.column);
        CHECK(terminal.cursor().row == 20);

        // Switch fullscreen -> regular: live preference false MUST transfer to the regular root!
        auto switched = engine->switch_tui_mode(coding_agent::TuiMode::Regular);
        REQUIRE(switched);
        REQUIRE(*switched);
        drain_ready(io);
        REQUIRE(pump_until(io, [&] { return !terminal.modes().alternate_screen; }));

        // Regular root must retain the live preference (false), not the stored setting (true)
        CHECK_FALSE(engine->show_hardware_cursor());
        CHECK_FALSE(terminal.modes().cursor_visible);
        CHECK(terminal.cursor().column == 15);
        CHECK(terminal.cursor().row == 7);

        REQUIRE(terminal.inject_input("\x03\x04"));
        REQUIRE(pump_until(io, [&] { return run_result.has_value(); }));
        REQUIRE(run_result);
        CHECK(*run_result);
    }

    // 3. Stored setting modified on disk is NOT re-read during switch
    {
        tests::RuntimeFixture runtime;
        tests::TempWorkspace workspace;
        tests::TempWorkspace config;
        config.write("settings.json", R"({"showHardwareCursor": true})");
        auto created = runtime.run(
                create_session_async(runtime, session_options(workspace, tests::make_scripted_fake_provider())));
        REQUIRE(created);
        tests::RuntimeLoopDriver runtime_driver(runtime);

        tui::VirtualTerminal terminal({.columns = 48, .rows = 24});
        boost::asio::io_context io;
        std::optional<support::ExpectedVoid> run_result;
        auto engine = std::make_shared<coding_agent::tui::InteractiveEngine>(
                terminal, io.get_executor(), coding_agent::TuiMode::Regular);
        boost::asio::co_spawn(io,
                drive_engine(engine, make_run(*created->session, config.path(), coding_agent::TuiMode::Regular)),
                [&](std::exception_ptr exception, support::ExpectedVoid result) {
                    CHECK(exception == nullptr);
                    run_result.emplace(std::move(result));
                });
        drain_ready(io);
        const tests::RunJoinGuard join_run{io, [&] { return run_result.has_value(); }};
        REQUIRE(pump_until(io, [&] { return engine->focus() != nullptr; }));

        CHECK(engine->show_hardware_cursor());
        CHECK(terminal.modes().cursor_visible);

        // Mutate the file on disk to false
        config.write("settings.json", R"({"showHardwareCursor": false})");

        // Switch regular -> fullscreen: switch must NOT re-read the setting file
        auto switched = engine->switch_tui_mode(coding_agent::TuiMode::Fullscreen);
        REQUIRE(switched);
        REQUIRE(*switched);
        drain_ready(io);
        REQUIRE(pump_until(io, [&] { return terminal.modes().alternate_screen; }));

        CHECK(engine->show_hardware_cursor());
        CHECK(terminal.modes().cursor_visible);

        REQUIRE(terminal.inject_input("\x04"));
        REQUIRE(pump_until(io, [&] { return run_result.has_value(); }));
        REQUIRE(run_result);
        CHECK(*run_result);
    }
}

TEST_CASE("Native TUI settings hardware cursor row toggles live preference and persists setting",
        "[coding_agent][tui][cursor][settings-selector][issue1037][spec]") {
    tests::EnvVarGuard env_guard{"PI_HARDWARE_CURSOR"};
    env_guard.unset();

    tests::RuntimeFixture runtime;
    tests::TempWorkspace workspace;
    tests::TempWorkspace config;
    config.write("settings.json", R"({"showHardwareCursor": false})");
    auto created = runtime.run(
            create_session_async(runtime, session_options(workspace, tests::make_scripted_fake_provider())));
    REQUIRE(created);
    tests::RuntimeLoopDriver runtime_driver(runtime);

    tui::VirtualTerminal terminal({.columns = 48, .rows = 24});
    boost::asio::io_context io;
    std::optional<support::ExpectedVoid> run_result;
    auto engine = std::make_shared<coding_agent::tui::InteractiveEngine>(
            terminal, io.get_executor(), coding_agent::TuiMode::Regular);
    boost::asio::co_spawn(io,
            drive_engine(engine, make_run(*created->session, config.path(), coding_agent::TuiMode::Regular)),
            [&](std::exception_ptr exception, support::ExpectedVoid result) {
                CHECK(exception == nullptr);
                run_result.emplace(std::move(result));
            });
    drain_ready(io);
    const tests::RunJoinGuard join_run{io, [&] { return run_result.has_value(); }};
    REQUIRE(pump_until(io, [&] { return engine->focus() != nullptr; }));

    CHECK_FALSE(engine->show_hardware_cursor());
    CHECK_FALSE(terminal.modes().cursor_visible);

    // Open /settings and navigate to Show hardware cursor row
    focus_hardware_cursor_row(terminal, io);

    // Enter cycles false -> true: live renderer shows cursor immediately and persists to disk
    REQUIRE(terminal.inject_input("\r"));
    drain_ready(io);
    REQUIRE(pump_until(io, [&] { return terminal.modes().cursor_visible; }));
    CHECK(engine->show_hardware_cursor());
    CHECK(read_text_file(config.path() / "settings.json").find("\"showHardwareCursor\": true") != std::string::npos);

    // Enter again cycles true -> false: live renderer hides cursor immediately and persists to disk
    REQUIRE(terminal.inject_input("\r"));
    drain_ready(io);
    REQUIRE(pump_until(io, [&] { return !terminal.modes().cursor_visible; }));
    CHECK_FALSE(engine->show_hardware_cursor());
    CHECK(read_text_file(config.path() / "settings.json").find("\"showHardwareCursor\": false") != std::string::npos);

    close_hardware_cursor_settings(terminal, io);

    REQUIRE(terminal.inject_input("\x04"));
    REQUIRE(pump_until(io, [&] { return run_result.has_value(); }));
    REQUIRE(run_result);
    CHECK(*run_result);
}

TEST_CASE("Native TUI reload reapplies showHardwareCursor preference from settings",
        "[coding_agent][tui][cursor][reload][issue1037][spec]") {
    tests::EnvVarGuard env_guard{"PI_HARDWARE_CURSOR"};
    env_guard.unset();

    tests::RuntimeFixture runtime;
    tests::TempWorkspace workspace;
    tests::TempWorkspace config;
    config.write("settings.json", R"({"showHardwareCursor": false})");
    auto created = runtime.run(
            create_session_async(runtime, session_options(workspace, tests::make_scripted_fake_provider())));
    REQUIRE(created);
    tests::RuntimeLoopDriver runtime_driver(runtime);

    tui::VirtualTerminal terminal({.columns = 48, .rows = 24});
    boost::asio::io_context io;
    std::optional<support::ExpectedVoid> run_result;
    auto engine = std::make_shared<coding_agent::tui::InteractiveEngine>(
            terminal, io.get_executor(), coding_agent::TuiMode::Regular);
    boost::asio::co_spawn(io,
            drive_engine(engine, make_run(*created->session, config.path(), coding_agent::TuiMode::Regular)),
            [&](std::exception_ptr exception, support::ExpectedVoid result) {
                CHECK(exception == nullptr);
                run_result.emplace(std::move(result));
            });
    drain_ready(io);
    const tests::RunJoinGuard join_run{io, [&] { return run_result.has_value(); }};
    REQUIRE(pump_until(io, [&] { return engine->focus() != nullptr; }));

    CHECK_FALSE(engine->show_hardware_cursor());
    CHECK_FALSE(terminal.modes().cursor_visible);

    // Mutate file on disk to true
    config.write("settings.json", R"({"showHardwareCursor": true})");

    // Trigger /reload
    REQUIRE(terminal.inject_input("/reload\r"));
    drain_ready(io);
    REQUIRE(pump_until(io, [&] { return terminal.modes().cursor_visible; }));
    CHECK(engine->show_hardware_cursor());

    // Mutate file on disk to false
    config.write("settings.json", R"({"showHardwareCursor": false})");

    // Trigger /reload again
    REQUIRE(terminal.inject_input("/reload\r"));
    drain_ready(io);
    REQUIRE(pump_until(io, [&] { return !terminal.modes().cursor_visible; }));
    CHECK_FALSE(engine->show_hardware_cursor());

    REQUIRE(terminal.inject_input("\x04"));
    REQUIRE(pump_until(io, [&] { return run_result.has_value(); }));
    REQUIRE(run_result);
    CHECK(*run_result);
}

namespace {

class NestedCursorEditor final : public cch::tui::EditorComponent {
public:
    explicit NestedCursorEditor(std::size_t col = 5, std::size_t row = 1)
        : cursor_(cch::tui::CursorPosition{.column = col, .row = row}) {}

    [[nodiscard]] std::string text() const override { return ""; }
    void set_text(std::string) override {}
    void set_on_change(cch::tui::EditorChangeSink sink) override { on_change_ = std::move(sink); }
    void set_on_submit(cch::tui::EditorSubmitSink sink) override { on_submit_ = std::move(sink); }
    [[nodiscard]] cch::support::Expected<cch::tui::RenderResult> render(std::size_t) override {
        return cch::tui::RenderResult{.lines = {"line 0", "line 1 nested", "line 2"}};
    }
    void invalidate() override {}
    [[nodiscard]] cch::tui::InputAdmissionOutcome handle_input(const cch::tui::InputEventVariant&) override {
        return cch::tui::InputAdmissionOutcome::Unhandled;
    }
    void set_focused(bool focused) override { focused_ = focused; }
    [[nodiscard]] bool focused() const override { return focused_; }
    [[nodiscard]] std::optional<cch::tui::CursorPosition> cursor_location() const override {
        return focused_ ? cursor_ : std::nullopt;
    }
    void set_cursor(std::optional<cch::tui::CursorPosition> c) {
        cursor_ = c;
        if (on_change_) (void)on_change_("");
    }

private:
    cch::tui::EditorChangeSink on_change_;
    cch::tui::EditorSubmitSink on_submit_;
    bool focused_{false};
    std::optional<cch::tui::CursorPosition> cursor_;
};

} // namespace

TEST_CASE("Native TUI InteractiveEngine preserves exact nested and clipped cursor coordinates and protocol across mode "
          "switch in both directions",
        "[coding_agent][tui][cursor][renderer-switch][issue1037][spec]") {
    tests::EnvVarGuard env_guard{"PI_HARDWARE_CURSOR"};
    env_guard.unset();

    const auto artifact = tests::read_pi_tui_evidence("product-cursor-layout.json");
    REQUIRE(artifact);
    const auto& bundle = *artifact;
    REQUIRE(bundle.at("baseline").get_string() == "pi-v1.0.4");
    REQUIRE(bundle.at("revision").get_string() == "7c10bd4337495ee613f2224843ecdf349b80d1df");

    auto run_live_switch_scenario = [&](std::string_view scenario_name) {
        const auto& scenario = find_evidence_scenario(bundle, scenario_name);
        const auto& inputs = scenario.at("inputs");
        const auto& expected = scenario.at("expected");
        const auto cols = static_cast<std::size_t>(scenario.at("dimensions").at("columns").get_number());
        const auto rows = static_cast<std::size_t>(scenario.at("dimensions").at("rows").get_number());

        const std::string initial_mode_str = inputs.at("initialMode").get_string();
        const std::string target_mode_str = inputs.at("targetMode").get_string();
        const bool initial_pref = inputs.at("initialPreference").get_boolean();
        const bool live_pref = inputs.at("livePreference").get_boolean();
        const auto cursor_col = static_cast<std::size_t>(inputs.at("cursorCol").get_number());
        const auto cursor_row = static_cast<std::size_t>(inputs.at("cursorRow").get_number());

        const auto initial_mode =
                initial_mode_str == "fullscreen" ? coding_agent::TuiMode::Fullscreen : coding_agent::TuiMode::Regular;
        const auto target_mode =
                target_mode_str == "fullscreen" ? coding_agent::TuiMode::Fullscreen : coding_agent::TuiMode::Regular;

        tests::RuntimeFixture runtime;
        tests::TempWorkspace workspace;
        tests::TempWorkspace config;
        if (initial_pref) {
            config.write("settings.json", R"({"showHardwareCursor": true})");
        }

        auto created = runtime.run(
                create_session_async(runtime, session_options(workspace, tests::make_scripted_fake_provider())));
        REQUIRE(created);
        tests::RuntimeLoopDriver runtime_driver(runtime);

        std::shared_ptr<NestedCursorEditor> custom_editor;
        auto extension_factory = [&](coding_agent::tui::ExtensionUiHost& host) {
            host.set_editor_component(
                    [&](cch::tui::ITui&, cch::tui::EditorTheme, std::shared_ptr<coding_agent::tui::SharedKeybindings>) {
                        custom_editor = std::make_shared<NestedCursorEditor>(cursor_col, cursor_row);
                        return custom_editor;
                    });
        };

        tui::VirtualTerminal vt({.columns = cols, .rows = rows});
        tests::ProtocolRecordingTerminal terminal(vt);
        boost::asio::io_context io;
        std::optional<support::ExpectedVoid> run_result;
        auto engine = std::make_shared<coding_agent::tui::InteractiveEngine>(terminal, io.get_executor(), initial_mode);
        auto run = coding_agent::tui::InteractiveSessionRunBuilder{}
                           .with_session(*created->session)
                           .with_agent_config_directory(config.path())
                           .with_tui_mode(initial_mode)
                           .with_extension_ui_registration_factory(std::move(extension_factory))
                           .build();

        boost::asio::co_spawn(io,
                drive_engine(engine, std::move(run)),
                [&](std::exception_ptr exception, support::ExpectedVoid result) {
                    CHECK(exception == nullptr);
                    run_result.emplace(std::move(result));
                });
        drain_ready(io);
        const tests::RunJoinGuard join_run{io, [&] { return run_result.has_value(); }};
        if (initial_mode == coding_agent::TuiMode::Fullscreen) {
            REQUIRE(terminal.modes().alternate_screen);
        }
        REQUIRE(pump_until(io, [&] { return engine->focus() != nullptr; }));

        // The startup color query settles at its 100 ms deadline. Complete it
        // explicitly here so a load-delayed delivery cannot add a post-switch
        // paint frame to the recorded window; the frozen capture measured the
        // switch after that query had settled. The later live-preference update
        // and its render wait flush any resulting coalesced frame before the
        // recording starts.
        REQUIRE(terminal.inject_input("\x1b[?1;2c"));
        drain_ready(io);

        // 1. Initial phase
        const auto& initial_exp = expected.at("initial");
        CHECK(engine->show_hardware_cursor() == initial_exp.at("showHardwareCursor").get_boolean());
        CHECK(vt.modes().cursor_visible == initial_exp.at("cursorVisible").get_boolean());
        CHECK(vt.cursor().column == static_cast<std::size_t>(initial_exp.at("cursor").at("column").get_number()));
        CHECK(vt.cursor().row == static_cast<std::size_t>(initial_exp.at("cursor").at("row").get_number()));

        // 2. Live preference update
        const auto& live_exp = expected.at("live");
        engine->set_show_hardware_cursor(live_pref);
        drain_ready(io);
        REQUIRE(pump_until(
                io, [&] { return vt.modes().cursor_visible == live_exp.at("cursorVisible").get_boolean(); }));
        CHECK(engine->show_hardware_cursor() == live_exp.at("showHardwareCursor").get_boolean());
        CHECK(vt.cursor().column == static_cast<std::size_t>(live_exp.at("cursor").at("column").get_number()));
        CHECK(vt.cursor().row == static_cast<std::size_t>(live_exp.at("cursor").at("row").get_number()));

        // 3. Mode switch
        terminal.reset_recording();
        auto switched = engine->switch_tui_mode(target_mode);
        REQUIRE(switched);
        REQUIRE(*switched);
        drain_ready(io);
        if (target_mode == coding_agent::TuiMode::Fullscreen) {
            REQUIRE(pump_until(io, [&] { return vt.modes().alternate_screen; }));
        } else {
            REQUIRE(pump_until(io, [&] { return !vt.modes().alternate_screen; }));
        }

        const auto& switch_exp = expected.at("switch");
        CHECK(engine->show_hardware_cursor() == switch_exp.at("showHardwareCursor").get_boolean());
        CHECK(vt.modes().cursor_visible == switch_exp.at("cursorVisible").get_boolean());
        CHECK(vt.cursor().column == static_cast<std::size_t>(switch_exp.at("cursor").at("column").get_number()));
        CHECK(vt.cursor().row == static_cast<std::size_t>(switch_exp.at("cursor").at("row").get_number()));

        // 4. Protocol projection and ordered comparison against frozen artifact
        const bool is_target_fs = (target_mode == coding_agent::TuiMode::Fullscreen);
        std::vector<std::string> frozen_writes;
        for (const auto& w : switch_exp.at("rawWrites").get_array()) {
            frozen_writes.push_back(w.get_string());
        }
        std::vector<tests::FrozenTerminalEvent> frozen_events;
        for (const auto& ev : switch_exp.at("terminalEvents").get_array()) {
            frozen_events.push_back(tests::FrozenTerminalEvent{
                    .op = ev.at("op").get_string(),
                    .bytes = ev.get_object().contains("bytes") ? ev.at("bytes").get_string() : "",
            });
        }
        const auto frozen_ime =
                tests::extract_ime_tokens_from_frozen_evidence(frozen_writes, frozen_events, is_target_fs);
        const auto pike_ime = tests::project_final_ime_sequence(terminal, is_target_fs);

        REQUIRE(pike_ime.size() == frozen_ime.size());
        for (std::size_t i = 0; i < pike_ime.size(); ++i) {
            CHECK(pike_ime[i].kind == frozen_ime[i].kind);
            CHECK(pike_ime[i].canonical == frozen_ime[i].canonical);
        }

        REQUIRE(terminal.inject_input("\x04"));
        REQUIRE(pump_until(io, [&] { return run_result.has_value(); }));
        REQUIRE(run_result);
        CHECK(*run_result);
    };

    SECTION("regular to fullscreen with live true") {
        run_live_switch_scenario("switch-regular-to-fullscreen-live-true");
    }

    SECTION("regular to fullscreen with live false") {
        run_live_switch_scenario("switch-regular-to-fullscreen-live-false");
    }

    SECTION("fullscreen to regular with live false") {
        run_live_switch_scenario("switch-fullscreen-to-regular-live-false");
    }

    SECTION("fullscreen to regular with live true") {
        run_live_switch_scenario("switch-fullscreen-to-regular-live-true");
    }

    auto run_clipped_switch_scenario = [&](std::string_view scenario_name) {
        const auto& scenario = find_evidence_scenario(bundle, scenario_name);
        const auto& inputs = scenario.at("inputs");
        const auto& expected = scenario.at("expected");
        const auto cols = static_cast<std::size_t>(scenario.at("dimensions").at("columns").get_number());
        const auto constrained_rows = static_cast<std::size_t>(inputs.at("constrainedRows").get_number());
        const auto restored_rows = static_cast<std::size_t>(inputs.at("restoredRows").get_number());

        const std::string initial_mode_str = inputs.at("initialMode").get_string();
        const std::string target_mode_str = inputs.at("targetMode").get_string();
        const auto cursor_col = static_cast<std::size_t>(inputs.at("cursorCol").get_number());
        const auto cursor_row = static_cast<std::size_t>(inputs.at("cursorRow").get_number());

        const auto initial_mode =
                initial_mode_str == "fullscreen" ? coding_agent::TuiMode::Fullscreen : coding_agent::TuiMode::Regular;
        const auto target_mode =
                target_mode_str == "fullscreen" ? coding_agent::TuiMode::Fullscreen : coding_agent::TuiMode::Regular;

        tests::RuntimeFixture runtime;
        tests::TempWorkspace workspace;
        tests::TempWorkspace config;
        config.write("settings.json", R"({"showHardwareCursor": true})");

        auto created = runtime.run(
                create_session_async(runtime, session_options(workspace, tests::make_scripted_fake_provider())));
        REQUIRE(created);
        tests::RuntimeLoopDriver runtime_driver(runtime);

        std::shared_ptr<NestedCursorEditor> custom_editor;
        auto extension_factory = [&](coding_agent::tui::ExtensionUiHost& host) {
            host.set_editor_component(
                    [&](cch::tui::ITui&, cch::tui::EditorTheme, std::shared_ptr<coding_agent::tui::SharedKeybindings>) {
                        custom_editor = std::make_shared<NestedCursorEditor>(cursor_col, cursor_row);
                        return custom_editor;
                    });
        };

        tui::VirtualTerminal vt({.columns = cols, .rows = constrained_rows});
        tests::ProtocolRecordingTerminal terminal(vt);
        boost::asio::io_context io;
        std::optional<support::ExpectedVoid> run_result;
        auto engine = std::make_shared<coding_agent::tui::InteractiveEngine>(terminal, io.get_executor(), initial_mode);
        auto run = coding_agent::tui::InteractiveSessionRunBuilder{}
                           .with_session(*created->session)
                           .with_agent_config_directory(config.path())
                           .with_tui_mode(initial_mode)
                           .with_extension_ui_registration_factory(std::move(extension_factory))
                           .build();

        boost::asio::co_spawn(io,
                drive_engine(engine, std::move(run)),
                [&](std::exception_ptr exception, support::ExpectedVoid result) {
                    CHECK(exception == nullptr);
                    run_result.emplace(std::move(result));
                });
        drain_ready(io);
        const tests::RunJoinGuard join_run{io, [&] { return run_result.has_value(); }};
        if (initial_mode == coding_agent::TuiMode::Fullscreen) {
            REQUIRE(terminal.modes().alternate_screen);
        }
        REQUIRE(pump_until(io, [&] { return engine->focus() != nullptr; }));

        // 1. Initial clipped phase: cursor is within component bounds (col 5, row 1 of 3 lines),
        // but clipped by constrained terminal height (48x2) -> cursorVisible is false
        const auto& init_clipped_exp = expected.at("initialClipped");
        CHECK(engine->show_hardware_cursor() == init_clipped_exp.at("showHardwareCursor").get_boolean());
        CHECK(vt.modes().cursor_visible == init_clipped_exp.at("cursorVisible").get_boolean());

        // 2. Switch mode while clipped
        terminal.reset_recording();
        auto switched = engine->switch_tui_mode(target_mode);
        REQUIRE(switched);
        REQUIRE(*switched);
        drain_ready(io);
        if (target_mode == coding_agent::TuiMode::Fullscreen) {
            REQUIRE(pump_until(io, [&] { return vt.modes().alternate_screen; }));
        } else {
            REQUIRE(pump_until(io, [&] { return !vt.modes().alternate_screen; }));
        }

        const auto& switch_clipped_exp = expected.at("afterSwitchClipped");
        CHECK(engine->show_hardware_cursor() == switch_clipped_exp.at("showHardwareCursor").get_boolean());
        CHECK(vt.modes().cursor_visible == switch_clipped_exp.at("cursorVisible").get_boolean());

        const bool is_target_fs = (target_mode == coding_agent::TuiMode::Fullscreen);
        std::vector<std::string> frozen_clipped_writes;
        for (const auto& w : switch_clipped_exp.at("rawWrites").get_array()) {
            frozen_clipped_writes.push_back(w.get_string());
        }
        std::vector<tests::FrozenTerminalEvent> frozen_clipped_events;
        for (const auto& ev : switch_clipped_exp.at("terminalEvents").get_array()) {
            frozen_clipped_events.push_back(tests::FrozenTerminalEvent{
                    .op = ev.at("op").get_string(),
                    .bytes = ev.get_object().contains("bytes") ? ev.at("bytes").get_string() : "",
            });
        }
        const auto frozen_clipped_ime = tests::extract_ime_tokens_from_frozen_evidence(
                frozen_clipped_writes, frozen_clipped_events, is_target_fs);
        const auto pike_clipped_ime = tests::project_final_ime_sequence(terminal, is_target_fs);

        REQUIRE(pike_clipped_ime.size() == 1);
        CHECK(pike_clipped_ime[0].kind == tests::ImeTokenKind::Visibility);
        CHECK(pike_clipped_ime[0].visible == false);
        REQUIRE(frozen_clipped_ime.size() == 1);
        CHECK(frozen_clipped_ime[0].kind == tests::ImeTokenKind::Visibility);
        CHECK(frozen_clipped_ime[0].visible == false);

        // 3. Resize to 48x24: layout unclips, restoring visible cursor
        terminal.reset_recording();
        REQUIRE(terminal.inject_resize(tui::TerminalDimensions{.columns = cols, .rows = restored_rows}));
        drain_ready(io);
        const auto& restored_exp = expected.at("afterResizeRestored");
        REQUIRE(pump_until(
                io, [&] { return vt.modes().cursor_visible == restored_exp.at("cursorVisible").get_boolean(); }));

        CHECK(engine->show_hardware_cursor() == restored_exp.at("showHardwareCursor").get_boolean());
        CHECK(vt.modes().cursor_visible == restored_exp.at("cursorVisible").get_boolean());
        CHECK(vt.cursor().column == static_cast<std::size_t>(restored_exp.at("cursor").at("column").get_number()));
        CHECK(vt.cursor().row == static_cast<std::size_t>(restored_exp.at("cursor").at("row").get_number()));

        std::vector<std::string> frozen_restored_writes;
        for (const auto& w : restored_exp.at("rawWrites").get_array()) {
            frozen_restored_writes.push_back(w.get_string());
        }
        std::vector<tests::FrozenTerminalEvent> frozen_restored_events;
        for (const auto& ev : restored_exp.at("terminalEvents").get_array()) {
            frozen_restored_events.push_back(tests::FrozenTerminalEvent{
                    .op = ev.at("op").get_string(),
                    .bytes = ev.get_object().contains("bytes") ? ev.at("bytes").get_string() : "",
            });
        }
        const auto frozen_restored_ime = tests::extract_ime_tokens_from_frozen_evidence(
                frozen_restored_writes, frozen_restored_events, is_target_fs);
        const auto pike_restored_ime = tests::project_final_ime_sequence(terminal, is_target_fs);

        REQUIRE(pike_restored_ime.size() == frozen_restored_ime.size());
        for (std::size_t i = 0; i < pike_restored_ime.size(); ++i) {
            CHECK(pike_restored_ime[i].kind == frozen_restored_ime[i].kind);
            CHECK(pike_restored_ime[i].canonical == frozen_restored_ime[i].canonical);
        }

        REQUIRE(terminal.inject_input("\x04"));
        REQUIRE(pump_until(io, [&] { return run_result.has_value(); }));
        REQUIRE(run_result);
        CHECK(*run_result);
    };

    SECTION("clipped switch from fullscreen to regular and restore") {
        run_clipped_switch_scenario("clipped-switch-fullscreen-to-regular");
    }

    SECTION("clipped switch from regular to fullscreen and restore") {
        run_clipped_switch_scenario("clipped-switch-regular-to-fullscreen");
    }
}

TEST_CASE("Protocol decoder extracts exact ordered IME tokens from frozen bundled writes",
        "[coding_agent][tui][cursor][protocol][issue1037][spec]") {
    const auto artifact = tests::read_pi_tui_evidence("product-cursor-layout.json");
    REQUIRE(artifact);
    const auto& bundle = *artifact;

    for (const auto& scenario : bundle.at("scenarios").get_array()) {
        const auto name = scenario.at("name").get_string();
        DYNAMIC_SECTION("Scenario: " << name) {
            const auto target_mode = scenario.at("inputs").at("targetMode").get_string();
            const bool is_target_fs = (target_mode == "fullscreen");
            const auto& exp = scenario.at("expected");

            if (exp.get_object().contains("switch")) {
                const auto& sw = exp.at("switch");
                std::vector<std::string> writes;
                for (const auto& w : sw.at("rawWrites").get_array()) {
                    writes.push_back(w.get_string());
                }
                std::vector<tests::FrozenTerminalEvent> events;
                for (const auto& ev : sw.at("terminalEvents").get_array()) {
                    events.push_back(tests::FrozenTerminalEvent{
                            .op = ev.at("op").get_string(),
                            .bytes = ev.get_object().contains("bytes") ? ev.at("bytes").get_string() : "",
                    });
                }
                const auto tokens = tests::extract_ime_tokens_from_frozen_evidence(writes, events, is_target_fs);
                const bool visible = sw.at("cursorVisible").get_boolean();
                REQUIRE(!tokens.empty());
                CHECK(tokens.back().kind == tests::ImeTokenKind::Visibility);
                CHECK(tokens.back().visible == visible);
                if (tokens.size() > 1) {
                    CHECK(tokens[0].kind != tests::ImeTokenKind::Visibility);
                }
            } else if (exp.get_object().contains("afterSwitchClipped")) {
                const auto& cl = exp.at("afterSwitchClipped");
                std::vector<std::string> writes;
                for (const auto& w : cl.at("rawWrites").get_array()) {
                    writes.push_back(w.get_string());
                }
                std::vector<tests::FrozenTerminalEvent> events;
                for (const auto& ev : cl.at("terminalEvents").get_array()) {
                    events.push_back(tests::FrozenTerminalEvent{
                            .op = ev.at("op").get_string(),
                            .bytes = ev.get_object().contains("bytes") ? ev.at("bytes").get_string() : "",
                    });
                }
                const auto tokens = tests::extract_ime_tokens_from_frozen_evidence(writes, events, is_target_fs);
                REQUIRE(tokens.size() == 1);
                CHECK(tokens[0].kind == tests::ImeTokenKind::Visibility);
                CHECK(tokens[0].visible == false);

                const auto& res = exp.at("afterResizeRestored");
                std::vector<std::string> res_writes;
                for (const auto& w : res.at("rawWrites").get_array()) {
                    res_writes.push_back(w.get_string());
                }
                std::vector<tests::FrozenTerminalEvent> res_events;
                for (const auto& ev : res.at("terminalEvents").get_array()) {
                    res_events.push_back(tests::FrozenTerminalEvent{
                            .op = ev.at("op").get_string(),
                            .bytes = ev.get_object().contains("bytes") ? ev.at("bytes").get_string() : "",
                    });
                }
                const auto res_tokens =
                        tests::extract_ime_tokens_from_frozen_evidence(res_writes, res_events, is_target_fs);
                REQUIRE(res_tokens.size() == 2);
                CHECK(res_tokens[0].kind != tests::ImeTokenKind::Visibility);
                CHECK(res_tokens[1].kind == tests::ImeTokenKind::Visibility);
                CHECK(res_tokens[1].visible == true);
            }
        }
    }
}

TEST_CASE("Protocol projection detects order inversion and extraneous moves against oracle",
        "[coding_agent][tui][cursor][protocol][issue1037][mutation]") {
    const tests::ImeToken correct_move{
            .kind = tests::ImeTokenKind::RelativeMove,
            .col = 6,
            .row_delta = -3,
            .canonical = "\x1b[3A\x1b[6G",
    };
    const tests::ImeToken wrong_move{
            .kind = tests::ImeTokenKind::RelativeMove,
            .col = 1,
            .row_delta = -1,
            .canonical = "\x1b[1A\x1b[1G",
    };
    const tests::ImeToken vis_true{
            .kind = tests::ImeTokenKind::Visibility,
            .visible = true,
            .canonical = "\x1b[?25h",
    };

    const std::vector<tests::ImeToken> oracle{correct_move, vis_true};
    const tests::ImeToken fs_move{
            .kind = tests::ImeTokenKind::AbsoluteMove,
            .row = 21,
            .col = 6,
            .canonical = "\x1b[21;6H",
    };
    const std::vector<tests::ImeToken> oracle_fs{fs_move, vis_true};

    SECTION("Separating mutation: visibility before move fails comparison") {
        const std::vector<tests::ImeToken> inverted{vis_true, correct_move};
        CHECK_FALSE(inverted == oracle);
    }

    SECTION("Separating mutation: wrong move before correct move fails comparison") {
        const std::vector<tests::ImeToken> extraneous{wrong_move, correct_move, vis_true};
        CHECK_FALSE(extraneous == oracle);
    }

    SECTION("Frozen decoder does not repair reversed order in AltScreen raw writes") {
        // Mutate raw writes to place visibility (?25h) before move (21;6H)
        const std::vector<std::string> mutated_writes{"\x1b[?2026h\x1b[?25h\x1b[21;6H\x1b[?2026l"};
        const auto decoded = tests::extract_ime_tokens_from_frozen_evidence(mutated_writes, {}, /*is_fullscreen=*/true);
        REQUIRE(decoded.size() == 2);
        CHECK(decoded[0].kind == tests::ImeTokenKind::Visibility);
        CHECK(decoded[0].visible == true);
        CHECK(decoded[1].kind == tests::ImeTokenKind::AbsoluteMove);
        CHECK(decoded[1].row == 21);
        CHECK(decoded[1].col == 6);
        CHECK_FALSE(decoded == std::vector<tests::ImeToken>{tests::ImeToken{.kind = tests::ImeTokenKind::AbsoluteMove,
                                                                    .row = 21,
                                                                    .col = 6,
                                                                    .canonical = "\x1b[21;6H"},
                                       vis_true});
    }

    SECTION("Frozen decoder does not repair multiple moves or drop wrong moves in MainScreen events") {
        const std::vector<tests::FrozenTerminalEvent> mutated_events{
                tests::FrozenTerminalEvent{.op = "write", .bytes = "\x1b[?2026hpaint\r\nline\x1b[?2026l"},
                tests::FrozenTerminalEvent{.op = "write", .bytes = "\x1b[1A\x1b[1G"},
                tests::FrozenTerminalEvent{.op = "write", .bytes = "\x1b[3A\x1b[6G"},
                tests::FrozenTerminalEvent{.op = "showCursor"},
        };
        const auto decoded =
                tests::extract_ime_tokens_from_frozen_evidence({}, mutated_events, /*is_fullscreen=*/false);
        REQUIRE(decoded.size() == 3);
        CHECK(decoded[0] == wrong_move);
        CHECK(decoded[1] == correct_move);
        CHECK(decoded[2] == vis_true);
        CHECK_FALSE(decoded == oracle);
    }

    SECTION("Frozen regular decoder rejects unsupported C/D movement and malformed strict parse") {
        const std::vector<tests::FrozenTerminalEvent> mutated_events{
                tests::FrozenTerminalEvent{.op = "write", .bytes = "\x1b[?2026hpaint\r\nline\x1b[?2026l"},
                tests::FrozenTerminalEvent{.op = "write", .bytes = "\x1b[2C"},
                tests::FrozenTerminalEvent{.op = "write", .bytes = "\x1b[?25x"},
                tests::FrozenTerminalEvent{.op = "write", .bytes = "\x1b[3A\x1b[6G"},
                tests::FrozenTerminalEvent{.op = "showCursor"},
        };
        const auto decoded =
                tests::extract_ime_tokens_from_frozen_evidence({}, mutated_events, /*is_fullscreen=*/false);
        REQUIRE(decoded.size() == 4);
        CHECK(decoded[0].kind == tests::ImeTokenKind::Unsupported);
        CHECK(decoded[0].canonical == "\x1b[2C");
        CHECK(decoded[1].kind == tests::ImeTokenKind::Unsupported);
        CHECK(decoded[1].canonical == "\x1b[?25x");
        CHECK(decoded[2] == correct_move);
        CHECK(decoded[3] == vis_true);
        CHECK_FALSE(decoded == oracle);
    }

    SECTION("Frozen fullscreen decoder rejects unsupported C/D movement and malformed strict parse") {
        const std::vector<std::string> mutated_writes{
                "\x1b[?2026h\x1b[1;1H\x1b[2Kpaint\x1b[4D\x1b[?25x\x1b[;H\x1b[21;6H\x1b[?25h\x1b[?2026l"};
        const auto decoded = tests::extract_ime_tokens_from_frozen_evidence(mutated_writes, {}, /*is_fullscreen=*/true);
        REQUIRE(decoded.size() == 5);
        CHECK(decoded[0].kind == tests::ImeTokenKind::Unsupported);
        CHECK(decoded[0].canonical == "\x1b[4D");
        CHECK(decoded[1].kind == tests::ImeTokenKind::Unsupported);
        CHECK(decoded[1].canonical == "\x1b[?25x");
        CHECK(decoded[2].kind == tests::ImeTokenKind::Unsupported);
        CHECK(decoded[2].canonical == "\x1b[;H");
        CHECK(decoded[3] == fs_move);
        CHECK(decoded[4] == vis_true);
        CHECK_FALSE(decoded == oracle_fs);
    }

    SECTION("Pike regular projection rejects unsupported C/D movement, unexpected SetCursor, and malformed parse") {
        const std::vector<tests::ProtocolRecordingTerminal::ObservedOp> pike_ops{
                {.kind = tests::ProtocolRecordingTerminal::OpKind::BeginSync},
                {.kind = tests::ProtocolRecordingTerminal::OpKind::Write, .write_bytes = "paint"},
                {.kind = tests::ProtocolRecordingTerminal::OpKind::EndSync},
                {.kind = tests::ProtocolRecordingTerminal::OpKind::Write, .write_bytes = "\x1b[2C"},
                {.kind = tests::ProtocolRecordingTerminal::OpKind::SetCursor,
                        .cursor_position = {.column = 5, .row = 20}},
                {.kind = tests::ProtocolRecordingTerminal::OpKind::Write, .write_bytes = "\x1b[;G"},
                {.kind = tests::ProtocolRecordingTerminal::OpKind::Write, .write_bytes = "\x1b[3A\x1b[6G"},
                {.kind = tests::ProtocolRecordingTerminal::OpKind::SetCursorVisible, .visible = true},
        };
        const auto projected = tests::project_final_ime_sequence(pike_ops, /*is_fullscreen=*/false);
        REQUIRE(projected.size() == 5);
        CHECK(projected[0].kind == tests::ImeTokenKind::Unsupported);
        CHECK(projected[0].canonical == "\x1b[2C");
        CHECK(projected[1].kind == tests::ImeTokenKind::Unsupported);
        CHECK(projected[1].row == 21);
        CHECK(projected[1].col == 6);
        CHECK(projected[2].kind == tests::ImeTokenKind::Unsupported);
        CHECK(projected[2].canonical == "\x1b[;G");
        CHECK(projected[3] == correct_move);
        CHECK(projected[4] == vis_true);
        CHECK_FALSE(projected == oracle);
    }

    SECTION("Pike fullscreen projection rejects unsupported relative writes and C/D movement") {
        const std::vector<tests::ProtocolRecordingTerminal::ObservedOp> pike_fs_ops{
                {.kind = tests::ProtocolRecordingTerminal::OpKind::BeginSync},
                {.kind = tests::ProtocolRecordingTerminal::OpKind::Write, .write_bytes = "\x1b[1;1H\x1b[2Kpaint line"},
                {.kind = tests::ProtocolRecordingTerminal::OpKind::Write, .write_bytes = "\x1b[3D"},
                {.kind = tests::ProtocolRecordingTerminal::OpKind::Write, .write_bytes = "\x1b[1A\x1b[6G"},
                {.kind = tests::ProtocolRecordingTerminal::OpKind::SetCursor,
                        .cursor_position = {.column = 5, .row = 20}},
                {.kind = tests::ProtocolRecordingTerminal::OpKind::SetCursorVisible, .visible = true},
                {.kind = tests::ProtocolRecordingTerminal::OpKind::EndSync},
        };
        const auto projected_fs = tests::project_final_ime_sequence(pike_fs_ops, /*is_fullscreen=*/true);
        REQUIRE(projected_fs.size() == 4);
        CHECK(projected_fs[0].kind == tests::ImeTokenKind::Unsupported);
        CHECK(projected_fs[0].canonical == "\x1b[3D");
        CHECK(projected_fs[1].kind == tests::ImeTokenKind::Unsupported);
        CHECK(projected_fs[1].canonical == "\x1b[1A\x1b[6G");
        CHECK(projected_fs[2] == fs_move);
        CHECK(projected_fs[3] == vis_true);
        CHECK_FALSE(projected_fs == oracle_fs);
    }
}

namespace {

[[nodiscard]] std::string make_osc_10_reply(std::string_view hex) {
    if (hex.size() == 7 && hex.starts_with("#")) {
        const auto r = hex.substr(1, 2);
        const auto g = hex.substr(3, 2);
        const auto b = hex.substr(5, 2);
        return std::format("\x1b]10;rgb:{}{}/{}{}/{}{}\x07", r, r, g, g, b, b);
    }
    return std::format("\x1b]10;{}\x07", hex);
}

[[nodiscard]] std::string make_osc_11_reply(std::string_view hex) {
    if (hex.size() == 7 && hex.starts_with("#")) {
        const auto r = hex.substr(1, 2);
        const auto g = hex.substr(3, 2);
        const auto b = hex.substr(5, 2);
        return std::format("\x1b]11;rgb:{}{}/{}{}/{}{}\x07", r, r, g, g, b, b);
    }
    return std::format("\x1b]11;{}\x07", hex);
}

[[nodiscard]] coding_agent::tui::InteractiveSessionRun make_run_with_extension(coding_agent::AgentSession& session,
        const std::filesystem::path& agent_config_directory,
        coding_agent::TuiMode tui_mode,
        coding_agent::tui::ExtensionUiRegistrationFactory factory) {
    return coding_agent::tui::InteractiveSessionRunBuilder{}
            .with_session(session)
            .with_agent_config_directory(agent_config_directory)
            .with_tui_mode(tui_mode)
            .with_extension_ui_registration_factory(std::move(factory))
            .build();
}

} // namespace

TEST_CASE("Native TUI terminal theme rebinding protocol and mode switch acceptance against frozen artifact 31",
        "[coding_agent][tui][fullscreen][theme][issue1037][spec]") {
    const auto artifact = tests::read_pi_tui_evidence("product-theme-rebind-protocol.json");
    REQUIRE(artifact);
    const auto& bundle = *artifact;
    REQUIRE(bundle.at("baseline").get_string() == "pi-v1.0.4");
    REQUIRE(bundle.at("revision").get_string() == "7c10bd4337495ee613f2224843ecdf349b80d1df");

    const auto& s1 = find_evidence_scenario(bundle, "same_root_late_query");
    const auto& s2 = find_evidence_scenario(bundle, "direction_a_regular_to_fullscreen");
    const auto& s3 = find_evidence_scenario(bundle, "direction_b_fullscreen_to_regular");
    const auto& s4 = find_evidence_scenario(bundle, "unkeyed_reply_pending_target_fifo");
    const auto& s5 = find_evidence_scenario(bundle, "roundtrip_rebind_idempotency");
    const auto& s6 = find_evidence_scenario(bundle, "malformed_and_unsupported_controls");
    tests::EnvVarGuard environment{"COLORFGBG", "0;15"};

    // ── Direction A and B live engine switch acceptance ─────────────────────
    {
        tests::RuntimeFixture runtime;
        tests::TempWorkspace workspace;
        tests::TempWorkspace config;
        config.write("settings.json", R"({"tuiMode": "regular", "theme": "system"})");

        auto client = std::make_shared<tests::GatedChatProvider>();
        auto created = runtime.run(create_session_async(runtime, session_options(workspace, std::move(client))));
        REQUIRE(created);
        tests::RuntimeLoopDriver runtime_driver(runtime);

        tui::VirtualTerminal inner_vt({.columns = 80, .rows = 24});
        tests::ProtocolRecordingTerminal terminal(inner_vt);
        boost::asio::io_context io;
        std::optional<support::ExpectedVoid> run_result;

        cch::tui::EditorTheme live_editor_theme;
        auto factory = [&](coding_agent::tui::ExtensionUiHost& host) {
            host.set_editor_component([&](cch::tui::ITui&,
                                              cch::tui::EditorTheme theme,
                                              std::shared_ptr<coding_agent::tui::SharedKeybindings>) {
                live_editor_theme = std::move(theme);
                return std::make_shared<cch::tui::Editor>();
            });
        };

        auto engine = std::make_shared<coding_agent::tui::InteractiveEngine>(
                terminal, io.get_executor(), coding_agent::TuiMode::Regular);
        boost::asio::co_spawn(io,
                drive_engine(engine,
                        make_run_with_extension(
                                *created->session, config.path(), coding_agent::TuiMode::Regular, std::move(factory))),
                [&](std::exception_ptr exception, support::ExpectedVoid result) {
                    CHECK(exception == nullptr);
                    run_result.emplace(std::move(result));
                });
        drain_ready(io);
        const tests::RunJoinGuard join_run{io, [&] { return run_result.has_value(); }};

        // A light initial appearance makes the old dark reply discriminating.
        REQUIRE(live_editor_theme.border != nullptr);
        const auto light_border = live_editor_theme.border("─");
        CHECK_FALSE(light_border.empty());

        // Verify initial query commands on regular root match artifact 31 exact vector
        const auto initial_commands = tests::extract_theme_protocol_commands(inner_vt.output());
        const auto expected_initial = strings_of(s1.at("expected").at("initialQueryCommands"));
        CHECK(initial_commands == expected_initial);

        // ── Phase A2: Switch to fullscreen while query is pending ───────────
        const auto before_switch_a = inner_vt.output().size();
        auto switched_a = engine->switch_tui_mode(coding_agent::TuiMode::Fullscreen);
        REQUIRE(switched_a);
        CHECK(*switched_a);
        CHECK(inner_vt.modes().alternate_screen);

        std::span<const std::string> switch_a_delta(
                inner_vt.output().data() + before_switch_a, inner_vt.output().size() - before_switch_a);
        const auto switch_a_commands = tests::extract_theme_protocol_commands(switch_a_delta);
        const auto expected_switch_a = strings_of(s2.at("expected").at("phaseA2SwitchCommands"));
        CHECK(switch_a_commands == expected_switch_a);

        // ── Phase A3: Inject old query reply into active terminal ───────────
        const auto old_fg = s2.at("inputs").at("oldFg").get_string();
        const auto old_bg = s2.at("inputs").at("oldBg").get_string();
        REQUIRE(inner_vt.inject_input(make_osc_10_reply(old_fg)));
        REQUIRE(inner_vt.inject_input(make_osc_11_reply(old_bg)));
        REQUIRE(inner_vt.inject_input("\x1b[?1;2c"));
        drain_ready(io);

        // Active root has no pending query; old replies do not mutate theme state
        CHECK(live_editor_theme.border("─") == light_border);

        // ── Phase A5: Scheme notification on active fullscreen root ─────────
        const auto before_scheme_a = inner_vt.output().size();
        REQUIRE(inner_vt.inject_input("\x1b[?997;2n")); // light report
        drain_ready(io);

        std::span<const std::string> scheme_a_delta(
                inner_vt.output().data() + before_scheme_a, inner_vt.output().size() - before_scheme_a);
        const auto scheme_a_commands = tests::extract_theme_protocol_commands(scheme_a_delta);
        const auto expected_scheme_a = strings_of(s2.at("expected").at("phaseA5QueryCommands"));
        CHECK(scheme_a_commands == expected_scheme_a);

        // ── Phase A6: Answer new query on active fullscreen root ────────────
        const auto new_fg = s2.at("inputs").at("newFg").get_string();
        const auto new_bg = s2.at("inputs").at("newBg").get_string();
        REQUIRE(inner_vt.inject_input(make_osc_10_reply(new_fg)));
        REQUIRE(inner_vt.inject_input(make_osc_11_reply(new_bg)));
        REQUIRE(inner_vt.inject_input("\x1b[?1;2c"));
        drain_ready(io);

        CHECK(live_editor_theme.border("─") == light_border);

        // ── Phase B1: Trigger a query on fullscreen root so a query is PENDING BEFORE switch ──
        const auto before_pending_b = inner_vt.output().size();
        REQUIRE(inner_vt.inject_input("\x1b[?997;1n")); // dark report triggers new query burst
        drain_ready(io);
        std::span<const std::string> pending_b_delta(
                inner_vt.output().data() + before_pending_b, inner_vt.output().size() - before_pending_b);
        const auto pending_b_commands = tests::extract_theme_protocol_commands(pending_b_delta);
        CHECK(pending_b_commands == expected_scheme_a);
        // Extra #991 precedence challenge: accepted light background must
        // defeat this opposite dark scheme; it also leaves B's old query pending.
        CHECK(live_editor_theme.border("─") == light_border);

        // ── Phase B2: Direction B — Switch back to regular while query is pending ──
        const auto before_switch_b = inner_vt.output().size();
        auto switched_b = engine->switch_tui_mode(coding_agent::TuiMode::Regular);
        REQUIRE(switched_b);
        CHECK(*switched_b);
        CHECK_FALSE(inner_vt.modes().alternate_screen);

        std::span<const std::string> switch_b_delta(
                inner_vt.output().data() + before_switch_b, inner_vt.output().size() - before_switch_b);
        const auto switch_b_commands = tests::extract_theme_protocol_commands(switch_b_delta);
        const auto expected_switch_b = strings_of(s3.at("expected").at("phaseB2SwitchCommands"));
        CHECK(switch_b_commands == expected_switch_b);

        // ── Phase B3: Inject old fullscreen reply into regular root ─────────
        CHECK(live_editor_theme.border("─") == light_border);
        const auto old_fg_b = s3.at("inputs").at("oldFg").get_string();
        const auto old_bg_b = s3.at("inputs").at("oldBg").get_string();
        REQUIRE(inner_vt.inject_input(make_osc_10_reply(old_fg_b)));
        REQUIRE(inner_vt.inject_input(make_osc_11_reply(old_bg_b)));
        REQUIRE(inner_vt.inject_input("\x1b[?1;2c"));
        drain_ready(io);

        CHECK(live_editor_theme.border("─") == light_border);

        // ── Phase B5: Dark scheme notification on regular root ──────────────
        const auto before_scheme_b = inner_vt.output().size();
        REQUIRE(inner_vt.inject_input("\x1b[?997;1n")); // dark report
        drain_ready(io);

        std::span<const std::string> scheme_b_delta(
                inner_vt.output().data() + before_scheme_b, inner_vt.output().size() - before_scheme_b);
        const auto scheme_b_commands = tests::extract_theme_protocol_commands(scheme_b_delta);
        const auto expected_scheme_b = strings_of(s3.at("expected").at("phaseB5QueryCommands"));
        CHECK(scheme_b_commands == expected_scheme_b);

        // ── Phase B6: Answer dark query ─────────────────────────────────────
        const auto new_fg_b = s3.at("inputs").at("newFg").get_string();
        const auto new_bg_b = s3.at("inputs").at("newBg").get_string();
        REQUIRE(inner_vt.inject_input(make_osc_10_reply(new_fg_b)));
        REQUIRE(inner_vt.inject_input(make_osc_11_reply(new_bg_b)));
        REQUIRE(inner_vt.inject_input("\x1b[?1;2c"));
        drain_ready(io);

        // Retained light background suppressed B5, so only B6 can change this.
        const auto dark_border = live_editor_theme.border("─");
        CHECK(dark_border != light_border);

        // ── Scenario 5: Roundtrip rebind idempotency on live engine ─────────
        REQUIRE(engine->switch_tui_mode(coding_agent::TuiMode::Fullscreen));
        REQUIRE(engine->switch_tui_mode(coding_agent::TuiMode::Regular));

        const auto before_idempotent = inner_vt.output().size();
        REQUIRE(inner_vt.inject_input("\x1b[?997;2n")); // light report
        drain_ready(io);

        std::span<const std::string> idempotent_delta(
                inner_vt.output().data() + before_idempotent, inner_vt.output().size() - before_idempotent);
        const auto burst_commands = tests::extract_theme_protocol_commands(idempotent_delta);
        const auto expected_burst = strings_of(s5.at("expected").at("burstCommands"));
        CHECK(burst_commands == expected_burst);
        // Extra #991 precedence challenge: B's accepted dark background
        // defeats this opposite light scheme after the roundtrip.
        CHECK(live_editor_theme.border("─") == dark_border);

        // Clean shutdown
        REQUIRE(inner_vt.inject_input("\x04"));
        REQUIRE(pump_until(io, [&] { return run_result.has_value(); }));
    }

    // ── Scenario 1: Same-root late query acceptance control with actual timeout ──
    {
        tests::RuntimeFixture runtime;
        tests::TempWorkspace workspace;
        tests::TempWorkspace config;
        config.write("settings.json", R"({"tuiMode": "regular", "theme": "system"})");

        auto client = std::make_shared<tests::GatedChatProvider>();
        auto created = runtime.run(create_session_async(runtime, session_options(workspace, std::move(client))));
        REQUIRE(created);
        tests::RuntimeLoopDriver runtime_driver(runtime);

        tui::VirtualTerminal inner_vt({.columns = 80, .rows = 24});
        boost::asio::io_context io;
        std::optional<support::ExpectedVoid> run_result;

        cch::tui::EditorTheme live_editor_theme;
        auto factory = [&](coding_agent::tui::ExtensionUiHost& host) {
            host.set_editor_component([&](cch::tui::ITui&,
                                              cch::tui::EditorTheme theme,
                                              std::shared_ptr<coding_agent::tui::SharedKeybindings>) {
                live_editor_theme = std::move(theme);
                return std::make_shared<cch::tui::Editor>();
            });
        };

        auto engine = std::make_shared<coding_agent::tui::InteractiveEngine>(
                inner_vt, io.get_executor(), coding_agent::TuiMode::Regular);
        boost::asio::co_spawn(io,
                drive_engine(engine,
                        make_run_with_extension(
                                *created->session, config.path(), coding_agent::TuiMode::Regular, std::move(factory))),
                [&](std::exception_ptr exception, support::ExpectedVoid result) {
                    CHECK(exception == nullptr);
                    run_result.emplace(std::move(result));
                });
        drain_ready(io);
        const tests::RunJoinGuard join_run{io, [&] { return run_result.has_value(); }};

        const auto initial_light_border = live_editor_theme.border("─");

        // Wait 120ms (timeout is 100ms) with steady_timer on io:
        boost::asio::steady_timer timer{io, std::chrono::milliseconds{120}};
        bool timer_fired = false;
        timer.async_wait([&](const boost::system::error_code&) { timer_fired = true; });
        REQUIRE(pump_until(io, [&] { return timer_fired; }));

        // Query timed out without replies; initial light appearance remains.
        CHECK(live_editor_theme.border("─") == initial_light_border);

        // Inject delayed replies on the same active root with distinct colors
        const auto late_fg = s1.at("inputs").at("lateFg").get_string();
        const auto late_bg = s1.at("inputs").at("lateBg").get_string();
        REQUIRE(inner_vt.inject_input(make_osc_10_reply(late_fg)));
        REQUIRE(inner_vt.inject_input(make_osc_11_reply(late_bg)));
        REQUIRE(inner_vt.inject_input("\x1b[?1;2c"));
        drain_ready(io);

        // The dark background must change the initial light appearance.
        CHECK(live_editor_theme.border("─") != initial_light_border);

        REQUIRE(inner_vt.inject_input("\x04"));
        REQUIRE(pump_until(io, [&] { return run_result.has_value(); }));
    }

    // ── Scenario 4: Ambiguity control — unkeyed reply pending target FIFO ───
    {
        tests::RuntimeFixture runtime;
        tests::TempWorkspace workspace;
        tests::TempWorkspace config;
        config.write("settings.json", R"({"tuiMode": "regular", "theme": "system"})");

        auto client = std::make_shared<tests::GatedChatProvider>();
        auto created = runtime.run(create_session_async(runtime, session_options(workspace, std::move(client))));
        REQUIRE(created);
        tests::RuntimeLoopDriver runtime_driver(runtime);

        tui::VirtualTerminal inner_vt({.columns = 80, .rows = 24});
        boost::asio::io_context io;
        std::optional<support::ExpectedVoid> run_result;

        cch::tui::EditorTheme live_editor_theme;
        auto factory = [&](coding_agent::tui::ExtensionUiHost& host) {
            host.set_editor_component([&](cch::tui::ITui&,
                                              cch::tui::EditorTheme theme,
                                              std::shared_ptr<coding_agent::tui::SharedKeybindings>) {
                live_editor_theme = std::move(theme);
                return std::make_shared<cch::tui::Editor>();
            });
        };
        auto engine = std::make_shared<coding_agent::tui::InteractiveEngine>(
                inner_vt, io.get_executor(), coding_agent::TuiMode::Regular);
        boost::asio::co_spawn(io,
                drive_engine(engine,
                        make_run_with_extension(
                                *created->session, config.path(), coding_agent::TuiMode::Regular, std::move(factory))),
                [&](std::exception_ptr exception, support::ExpectedVoid result) {
                    CHECK(exception == nullptr);
                    run_result.emplace(std::move(result));
                });
        drain_ready(io);
        const tests::RunJoinGuard join_run{io, [&] { return run_result.has_value(); }};

        // Switch to fullscreen
        auto switched = engine->switch_tui_mode(coding_agent::TuiMode::Fullscreen);
        REQUIRE(switched);
        CHECK(*switched);

        // Keep the target light while issuing a query for the dark FIFO reply.
        const auto before_query = inner_vt.output().size();
        REQUIRE(inner_vt.inject_input("\x1b[?997;2n"));
        drain_ready(io);
        CHECK(tests::extract_theme_protocol_commands(std::span<const std::string>{inner_vt.output()}.subspan(
                      before_query)) == strings_of(s4.at("expected").at("queryCommands")));
        const auto light_border = live_editor_theme.border("─");

        // Unkeyed replies arrive FIFO
        const auto unkeyed_fg = s4.at("inputs").at("unkeyedFg").get_string();
        const auto unkeyed_bg = s4.at("inputs").at("unkeyedBg").get_string();
        REQUIRE(inner_vt.inject_input(make_osc_10_reply(unkeyed_fg)));
        REQUIRE(inner_vt.inject_input(make_osc_11_reply(unkeyed_bg)));
        REQUIRE(inner_vt.inject_input("\x1b[?1;2c"));
        drain_ready(io);

        const auto dark_border = live_editor_theme.border("─");
        CHECK(dark_border != light_border);
        const auto before_challenge = inner_vt.output().size();
        REQUIRE(inner_vt.inject_input("\x1b[?997;2n"));
        drain_ready(io);
        CHECK(tests::extract_theme_protocol_commands(std::span<const std::string>{inner_vt.output()}.subspan(
                      before_challenge)) == strings_of(s4.at("expected").at("queryCommands")));
        CHECK(live_editor_theme.border("─") == dark_border);

        REQUIRE(inner_vt.inject_input("\x04"));
        REQUIRE(pump_until(io, [&] { return run_result.has_value(); }));
    }

    // ── Scenario 6: Parser raw mutation and malformed control handling ───────
    {
        tests::RuntimeFixture runtime;
        tests::TempWorkspace workspace;
        tests::TempWorkspace config;
        config.write("settings.json", R"({"tuiMode": "regular", "theme": "system"})");

        auto client = std::make_shared<tests::GatedChatProvider>();
        auto created = runtime.run(create_session_async(runtime, session_options(workspace, std::move(client))));
        REQUIRE(created);
        tests::RuntimeLoopDriver runtime_driver(runtime);

        tui::VirtualTerminal inner_vt({.columns = 80, .rows = 24});
        boost::asio::io_context io;
        std::optional<support::ExpectedVoid> run_result;

        cch::tui::EditorTheme live_editor_theme;
        auto factory = [&](coding_agent::tui::ExtensionUiHost& host) {
            host.set_editor_component([&](cch::tui::ITui&,
                                              cch::tui::EditorTheme theme,
                                              std::shared_ptr<coding_agent::tui::SharedKeybindings>) {
                live_editor_theme = std::move(theme);
                return std::make_shared<cch::tui::Editor>();
            });
        };
        auto engine = std::make_shared<coding_agent::tui::InteractiveEngine>(
                inner_vt, io.get_executor(), coding_agent::TuiMode::Regular);
        boost::asio::co_spawn(io,
                drive_engine(engine,
                        make_run_with_extension(
                                *created->session, config.path(), coding_agent::TuiMode::Regular, std::move(factory))),
                [&](std::exception_ptr exception, support::ExpectedVoid result) {
                    CHECK(exception == nullptr);
                    run_result.emplace(std::move(result));
                });
        drain_ready(io);
        const tests::RunJoinGuard join_run{io, [&] { return run_result.has_value(); }};

        const auto light_border = live_editor_theme.border("─");

        // 1. Malformed OSC 10 (invalid hex)
        REQUIRE(inner_vt.inject_input("\x1b]10;rgb:invalid\x07"));
        drain_ready(io);

        // 2. Unsupported OSC 12 (cursor color)
        REQUIRE(inner_vt.inject_input("\x1b]12;rgb:00/00/00\x07"));
        drain_ready(io);

        // 3. Unrelated CSI sequence (cursor position report)
        REQUIRE(inner_vt.inject_input("\x1b[24;80R"));
        drain_ready(io);

        CHECK(live_editor_theme.border("─") == light_border);

        // The valid dark reply must still reach the sink after these controls.
        // The small controller case separately proves pending-before-DA1;
        // this complete engine run can exceed the 100 ms query timeout.
        const auto valid_bg = s6.at("inputs").at("validBg").get_string();
        REQUIRE(inner_vt.inject_input(make_osc_11_reply(valid_bg)));
        REQUIRE(inner_vt.inject_input("\x1b[?1;2c"));
        drain_ready(io);
        CHECK(live_editor_theme.border("─") != light_border);

        REQUIRE(inner_vt.inject_input("\x04"));
        REQUIRE(pump_until(io, [&] { return run_result.has_value(); }));
    }
}

TEST_CASE("Terminal theme protocol extractor detects mutations, extra commands, and handles fragmentation",
        "[coding_agent][tui][theme][protocol][issue1037][spec]") {
    const auto artifact = tests::read_pi_tui_evidence("product-theme-rebind-protocol.json");
    REQUIRE(artifact);
    const auto oracle = strings_of(
            find_evidence_scenario(*artifact, "same_root_late_query").at("expected").at("initialQueryCommands"));
    REQUIRE(oracle.size() == 20);
    const auto join = [](const std::vector<std::string>& commands) {
        std::string bytes;
        for (const auto& command : commands)
            bytes += command;
        return bytes;
    };
    const auto raw = join(oracle);
    const auto fragmented = [](std::string_view bytes) {
        tests::ThemeProtocolExtractor extractor;
        for (const char& byte : bytes)
            extractor.feed(std::string_view{&byte, 1});
        extractor.finish();
        return extractor.take_commands();
    };
    CHECK(fragmented(raw) == oracle);
    CHECK(fragmented("paint\x1b[31m\x1b]0;title\x07" + raw + "\x1b[2K") == oracle);
    auto reordered = oracle;
    std::swap(reordered[1], reordered[2]);
    CHECK(fragmented(join(reordered)) != oracle);
    auto missing = oracle;
    missing.pop_back();
    CHECK(fragmented(join(missing)) != oracle);
    CHECK(fragmented(raw + oracle.front()) != oracle);

    // Appending anomalies must fail the same complete oracle comparison;
    // merely ignoring malformed relevant controls would pass these cases.
    const std::vector<std::string> anomalies{"\x1b]10;rgb:bad\x07",
            "\x1b]11;?;extra\x07",
            "\x1b]4;bad;?\x07",
            "\x1b]4:0;?\x07",
            "\x1b[?2031;h",
            "\x1b[?2031x",
            "\x1b[1c",
            "\x1b[?;c",
            "\x1b]10;?",
            "\x1b]11;?\x1b",
            "\x1b]4;0;?",
            "\x1b[?2031"};
    for (const auto& anomaly : anomalies) {
        CAPTURE(anomaly);
        const auto parsed = fragmented(raw + anomaly);
        CHECK(parsed != oracle);
        REQUIRE(parsed.size() == oracle.size() + 1);
        CHECK(parsed.back() == anomaly);
    }
    const std::string st = "\x1b]4;0;?\x1b\\";
    CHECK(fragmented(st) == std::vector<std::string>{st});
}
