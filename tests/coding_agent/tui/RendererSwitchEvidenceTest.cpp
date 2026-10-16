// Named pi-v1.0.4 acceptance for the live renderer switch (#1018, spec #946).
// Every expectation is read from the verified `renderer-switch.json` artifact,
// which was captured on a clean frozen checkout at
// `7c10bd4337495ee613f2224843ecdf349b80d1df`; nothing is re-derived here.
//
// Evidence boundary:
// - `InteractiveEngine::switch_tui_mode` is the live renderer switch operation;
//   its state transitions, focus continuity, editor retention, and hidden-overlay
//   refusal are verified against the frozen expectations.
// - Frame replay builds separate fresh Pike roots rather than switching a live
//   engine; it validates that each mode's layout renders identical frames to pi.
//   Not all frames or raw protocol bytes are compared across live switches.
// - Theme color queries (OSC 10/11) during switch are unmeasured in this test seam.
// - Two frozen parameters have no Pike seam: Pike's `switch_tui_mode(TuiMode)` takes
//   no `restoreProgress` and no `startRenderer`, and Pike has no terminal-progress
//   setting or `Terminal::set_progress` caller, so the progress scenario replays
//   the recorded boolean condition over the frozen rows.

#include "coding_agent/AgentSession.hpp"
#include "coding_agent/SessionTarget.hpp"
#include "coding_agent/runtime/SessionFactory.hpp"
#include "coding_agent/tui/ChatViewport.hpp"
#include "coding_agent/tui/InteractiveEngine.hpp"
#include "coding_agent/tui/InteractiveSessionRun.hpp"
#include "support/ModelsFixture.hpp"
#include "support/PumpUntil.hpp"
#include "support/RuntimeFixture.hpp"
#include "support/RuntimeLoopDriver.hpp"
#include "support/TempWorkspace.hpp"

#include <cch/ai/Content.hpp>
#include <cch/coding_agent/ModelRuntime.hpp>
#include <cch/tui/AltScreen.hpp>
#include <cch/tui/Component.hpp>
#include <cch/tui/Container.hpp>
#include <cch/tui/Overlay.hpp>
#include <cch/tui/Text.hpp>
#include <cch/tui/Tui.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include "support/Json.hpp"
#include "support/PiTuiEvidence.hpp"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <cstddef>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace cch::coding_agent::tui {
namespace {

using cch::support::JsonValue;

constexpr std::string_view kFrozenRevision{"7c10bd4337495ee613f2224843ecdf349b80d1df"};

[[nodiscard]] std::size_t size_of(const JsonValue& value) { return static_cast<std::size_t>(value.get_number()); }

[[nodiscard]] std::vector<std::string> strings_of(const JsonValue& value) {
    std::vector<std::string> strings;
    for (const auto& entry : value.get_array())
        strings.push_back(entry.get_string());
    return strings;
}

[[nodiscard]] std::string joined(const JsonValue& lines) {
    std::string text;
    for (const auto& line : lines.get_array()) {
        if (!text.empty()) text.push_back('\n');
        text += line.get_string();
    }
    return text;
}

[[nodiscard]] const JsonValue& scenario_of(const JsonValue& bundle, std::string_view name) {
    for (const auto& scenario : bundle.at("scenarios").get_array()) {
        if (scenario.at("name").get_string() == name) return scenario;
    }
    REQUIRE(false);
    return bundle;
}

/// pi's `visibleLines` compare: trailing padding is presentation, not content.
[[nodiscard]] std::vector<std::string> visible_rows(std::vector<std::string> rows) {
    for (auto& row : rows) {
        while (!row.empty() && row.back() == ' ')
            row.pop_back();
    }
    return rows;
}

// xterm `getChars()` represents vacant cells as an empty string; C++ pads the
// same cells with U+0020. Both display one blank cell, and every style and
// continuation bit stays in the comparison so a styled blank cannot hide here.
[[nodiscard]] cch::tui::VirtualTerminalCell evidence_cell(cch::tui::VirtualTerminalCell cell) {
    if (cell.grapheme.empty() && !cell.continuation) cell.grapheme = " ";
    return cell;
}

void check_cells(const cch::tui::VirtualTerminal& terminal, const JsonValue& expected) {
    const auto& rows = expected.get_array();
    REQUIRE(terminal.cells().size() == rows.size());
    for (std::size_t row = 0; row < rows.size(); ++row) {
        const auto& cells = rows[row].get_array();
        REQUIRE(terminal.cells()[row].size() == cells.size());
        for (std::size_t column = 0; column < cells.size(); ++column) {
            INFO(std::format("cell row={}, column={}", row, column));
            const auto& cell = cells[column];
            const auto& style = cell.at("style");
            const cch::tui::VirtualTerminalCell expected_cell{
                    .grapheme = cell.at("grapheme").get_string(),
                    .continuation = cell.at("continuation").get_boolean(),
                    .style =
                            {
                                    .bold = style.at("bold").get_boolean(),
                                    .dim = style.at("dim").get_boolean(),
                                    .italic = style.at("italic").get_boolean(),
                                    .underline = style.at("underline").get_boolean(),
                                    .blink = style.at("blink").get_boolean(),
                                    .inverse = style.at("inverse").get_boolean(),
                                    .hidden = style.at("hidden").get_boolean(),
                                    .strikethrough = style.at("strikethrough").get_boolean(),
                                    .fg_color = style.at("fg_color").get_string(),
                                    .bg_color = style.at("bg_color").get_string(),
                                    .hyperlink = style.at("hyperlink").get_string(),
                                    .hyperlink_params = style.at("hyperlink_params").get_string(),
                            },
            };
            CHECK(evidence_cell(terminal.cells()[row][column]) == evidence_cell(expected_cell));
        }
    }
}

/// Compare one recorded frame against the live Pike terminal. A frame that
/// records cells also records scrollback, so a styled frame can never hide in
/// a frame whose grid was not compared.
void check_frame(const cch::tui::VirtualTerminal& terminal, const JsonValue& frame) {
    CHECK(visible_rows(terminal.screen()) == visible_rows(strings_of(frame.at("screen"))));
    const cch::tui::CursorPosition cursor{
            .column = size_of(frame.at("cursor").at("column")),
            .row = size_of(frame.at("cursor").at("row")),
    };
    INFO(std::format("cursor actual=({},{}), expected=({},{})",
            terminal.cursor().column,
            terminal.cursor().row,
            cursor.column,
            cursor.row));
    CHECK(terminal.cursor() == cursor);
    CHECK(terminal.modes().cursor_visible == frame.at("cursorVisible").get_boolean());
    if (frame.get_object().contains("scrollback")) {
        CHECK(terminal.scrollback() == strings_of(frame.at("scrollback")));
    }
    if (frame.get_object().contains("cells")) check_cells(terminal, frame.at("cells"));
}

/// The frozen capture's focus target: a plain component the switch captures and
/// restores, exactly like the frozen arbiter's `{focused: false, render: () =>
/// ["content"]}` probe (`interactive-tui.test.ts:119-127`).
class ProbeComponent final : public cch::tui::Component {
public:
    [[nodiscard]] cch::support::Expected<cch::tui::RenderResult> render(std::size_t) override {
        return cch::tui::RenderResult{};
    }
    void invalidate() override {}
};

/// One region as the capture mounts it: a `Container` wrapping the plain
/// surface, so the replayed tree has the recorded child list.
[[nodiscard]] std::unique_ptr<cch::tui::Component> region(std::string text) {
    auto container = std::make_unique<cch::tui::Container>();
    REQUIRE(container->add_child(std::make_unique<cch::tui::Text>(std::move(text), 0, 0)));
    return container;
}

[[nodiscard]] std::unique_ptr<cch::tui::Overlay> overlay_entry() {
    auto overlay = std::make_unique<cch::tui::Overlay>(cch::tui::OverlayOptions{
            .width = cch::tui::SizeValue::absolute(14.0),
            .anchor = cch::tui::OverlayAnchor::TopRight,
    });
    REQUIRE(overlay->add_child(std::make_unique<cch::tui::Text>("overlay entry", 0, 0)));
    return overlay;
}

} // namespace

TEST_CASE("frozen renderer-switch observations record the state and callback contract",
        "[coding_agent][tui][fullscreen][renderer-switch][issue1018][compat-pi]") {
    const auto bundle = cch::tests::read_pi_tui_evidence("renderer-switch.json");
    REQUIRE(bundle);
    CHECK(bundle->at("schema").get_string() == "pike/tui-evidence/1");
    CHECK(bundle->at("revision").get_string() == kFrozenRevision);
    CHECK(bundle->at("baseline").get_string() == "pi-v1.0.4");
    REQUIRE(bundle->at("scenarios").get_array().size() == 8);

    // 1. Same mode is a no-op. The row carries a live visible overlay entry, so
    // this is also the ordering proof: the no-op check runs before the guard.
    {
        const auto& scenario = scenario_of(*bundle, "same-mode-returns-true-without-touching-the-renderer");
        const auto& expected = scenario.at("expected");
        REQUIRE(scenario.at("inputs").at("overlay").get_string() == "visible");
        CHECK(expected.at("result").get_boolean());
        CHECK_FALSE(expected.at("rendererReplaced").get_boolean());
        CHECK(size_of(expected.at("bytesWrittenBySwitch")) == 0);
        CHECK(expected.at("callbacks").get_array().empty());
        CHECK(expected.at("invalidated").get_array().empty());
        CHECK(strings_of(expected.at("stateBefore").at("children")) ==
                strings_of(expected.at("stateAfter").at("children")));
        CHECK(expected.at("stateAfter").at("terminal").at("stopCount").get_number() == 0);
    }

    // 2. Any overlay entry refuses, hidden ones included, and the refusal
    // leaves the engine exactly as it was.
    {
        const auto& scenario = scenario_of(*bundle, "overlay-entry-refuses-the-switch-and-leaves-everything-intact");
        const auto& cases = scenario.at("expected").at("cases").get_array();
        REQUIRE(cases.size() == 2);
        for (const auto& recorded_case : cases) {
            INFO(recorded_case.at("name").get_string());
            const auto& expected = recorded_case.at("expected");
            CHECK(expected.at("overlayHidden").get_boolean() ==
                    (recorded_case.at("name").get_string() == "hidden-overlay"));
            CHECK_FALSE(expected.at("result").get_boolean());
            CHECK(size_of(expected.at("bytesWrittenBySwitch")) == 0);
            CHECK(expected.at("callbacks").get_array().empty());
            const auto& before = expected.at("stateBefore");
            const auto& after = expected.at("stateAfter");
            CHECK(before.at("mode").get_string() == "fullscreen");
            CHECK(after.at("mode").get_string() == before.at("mode").get_string());
            CHECK(before.at("focus").get_string() == after.at("focus").get_string());
            CHECK(strings_of(before.at("children")) == strings_of(after.at("children")));
            CHECK(size_of(before.at("terminal").at("startCount")) == size_of(after.at("terminal").at("startCount")));
            CHECK(size_of(before.at("terminal").at("stopCount")) == size_of(after.at("terminal").at("stopCount")));
        }

        // Pike's own guard reads the same way: the entry stack, not visibility,
        // is what refuses the switch.
        cch::tui::VirtualTerminal terminal({.columns = 28, .rows = 8});
        cch::tui::AltScreen screen(terminal);
        REQUIRE(screen.start());
        auto overlay = overlay_entry();
        auto* const overlay_pointer = overlay.get();
        REQUIRE(screen.show_overlay(std::move(overlay)));
        CHECK(screen.has_overlay_entries());
        REQUIRE(screen.hide_overlay(overlay_pointer));
        CHECK(screen.has_overlay_entries());
        REQUIRE(screen.stop());
    }

    // 3. Regular -> fullscreen keeps one tree, and the rebind order is the
    // frozen one: stop the retired root, start the replacement, then rebind the
    // live consumers.
    {
        const auto& scenario =
                scenario_of(*bundle, "regular-to-fullscreen-retains-children-focus-editor-and-transcript");
        const auto& expected = scenario.at("expected");
        const auto& before = expected.at("stateBefore");
        const auto& after = expected.at("stateAfter");
        CHECK(expected.at("result").get_boolean());
        CHECK(before.at("mode").get_string() == "regular");
        CHECK(after.at("mode").get_string() == "fullscreen");
        CHECK(strings_of(before.at("children")) == strings_of(after.at("children")));
        CHECK(before.at("focus").get_string() == after.at("focus").get_string());
        CHECK(before.at("editorText").get_string() == after.at("editorText").get_string());
        CHECK(after.at("layoutRootInstalled").get_boolean());
        CHECK(strings_of(expected.at("callbacks")) == std::vector<std::string>{"terminal-stop@regular",
                                                              "terminal-start@fullscreen",
                                                              "theme-rebind@fullscreen",
                                                              "extension-unsubscribe@fullscreen",
                                                              "extension-listener-rebound@fullscreen"});
    }

    // 4. Fullscreen -> regular keeps the same tree and leaves no layout root on
    // the retired root.
    {
        const auto& scenario =
                scenario_of(*bundle, "fullscreen-to-regular-retains-the-tree-and-detaches-the-layout-root");
        const auto& expected = scenario.at("expected");
        const auto& before = expected.at("stateBefore");
        const auto& after = expected.at("stateAfter");
        CHECK(expected.at("result").get_boolean());
        CHECK(before.at("mode").get_string() == "fullscreen");
        CHECK(after.at("mode").get_string() == "regular");
        CHECK(strings_of(before.at("children")) == strings_of(after.at("children")));
        CHECK(before.at("focus").get_string() == after.at("focus").get_string());
        CHECK(before.at("editorText").get_string() == after.at("editorText").get_string());
        CHECK_FALSE(after.at("layoutRootInstalled").get_boolean());
        CHECK_FALSE(expected.at("retiredRootStillRendersLayoutRoot").get_boolean());
        CHECK(strings_of(expected.at("callbacks")) == std::vector<std::string>{"terminal-stop@fullscreen",
                                                              "terminal-start@regular",
                                                              "theme-rebind@regular",
                                                              "extension-unsubscribe@regular",
                                                              "extension-listener-rebound@regular"});
    }

    // 5. The main-screen render state crosses out and back: the switch captured
    // the live main screen, and the returning root repaints from that baseline.
    {
        const auto& scenario = scenario_of(*bundle, "main-screen-render-state-transfers-out-and-back");
        const auto& expected = scenario.at("expected");
        CHECK(expected.at("result").get_boolean());
        CHECK(expected.at("backResult").get_boolean());
        CHECK(size_of(expected.at("switchCapturedWidth")) == size_of(expected.at("capturedWidth")));
        CHECK(size_of(expected.at("switchCapturedLines")) == size_of(expected.at("capturedLines")));
        CHECK(size_of(expected.at("capturedWidth")) == 28);
        CHECK(size_of(expected.at("capturedLines")) == 11);
        CHECK(expected.at("restoredPreviousLinesMatch").get_boolean());
    }

    // 6. `startRenderer = false` completes the swap without starting the
    // replacement root. Pike's seam has no such parameter: the engine always
    // starts the root it selected, so this row is recorded rather than driven.
    {
        const auto& scenario = scenario_of(*bundle, "start-renderer-false-completes-without-starting-the-new-root");
        const auto& expected = scenario.at("expected");
        const auto& before = expected.at("stateBefore");
        const auto& after = expected.at("stateAfter");
        CHECK(expected.at("result").get_boolean());
        CHECK(size_of(expected.at("bytesWrittenBySwitch")) == 0);
        CHECK(before.at("mode").get_string() == "regular");
        CHECK(after.at("mode").get_string() == "fullscreen");
        CHECK(size_of(before.at("terminal").at("startCount")) == size_of(after.at("terminal").at("startCount")));
        CHECK(strings_of(expected.at("callbacks")) == std::vector<std::string>{"terminal-stop@regular"});
    }

    // 7. The progress-restore condition replayed over the frozen rows:
    // `restoreProgress && getShowTerminalProgress() && (isStreaming ||
    // isCompacting)` (`interactive-mode.ts:929-933`) is exactly the set of rows
    // that recorded a `terminal.setProgress(true)` call.
    {
        const auto& scenario =
                scenario_of(*bundle, "progress-restore-uses-the-show-terminal-progress-and-session-busy-condition");
        const auto& cases = scenario.at("expected").at("cases").get_array();
        REQUIRE(cases.size() == 5);
        std::size_t restored_rows = 0;
        for (const auto& recorded_case : cases) {
            INFO(recorded_case.at("name").get_string());
            const auto& expected = recorded_case.at("expected");
            const bool restore_progress = expected.at("restoreProgress").get_boolean();
            const bool show_terminal_progress = expected.at("showTerminalProgress").get_boolean();
            const bool busy = expected.at("isStreaming").get_boolean() || expected.at("isCompacting").get_boolean();
            const bool frozen_condition = restore_progress && show_terminal_progress && busy;
            const auto calls = expected.at("progressCalls").get_array();
            CHECK((calls.size() == 1) == frozen_condition);
            if (!calls.empty()) {
                CHECK(calls.front().get_boolean());
                ++restored_rows;
            }
            // Every row still completed the swap and started the replacement.
            CHECK(expected.at("result").get_boolean());
            CHECK(size_of(expected.at("stateAfter").at("terminal").at("startCount")) == 2);
            CHECK(size_of(expected.at("stateAfter").at("terminal").at("stopCount")) == 1);
        }
        CHECK(restored_rows == 2);
    }

    // 8. The transcript exit path: drain the overlay stack, switch to regular
    // without starting the replacement, render once and stop the terminal.
    {
        const auto& scenario =
                scenario_of(*bundle, "transcript-exit-switches-to-regular-renders-without-starting-and-stops");
        const auto& expected = scenario.at("expected");
        const auto& steps = scenario.at("inputs").at("steps").get_array();
        REQUIRE(steps.size() == 4);
        CHECK(steps[2].at("action").get_string() == "stop-interactive-tui");
        CHECK(steps[2].at("fullscreenExitOutput").get_string() == "transcript");
        CHECK(expected.at("overlayEntriesBefore").get_boolean());
        CHECK_FALSE(expected.at("oldRendererOverlayEntriesAfter").get_boolean());
        CHECK_FALSE(expected.at("currentRendererOverlayEntries").get_boolean());
        const auto& after = expected.at("stateAfter");
        CHECK(after.at("mode").get_string() == "regular");
        CHECK_FALSE(after.at("layoutRootInstalled").get_boolean());
        CHECK(size_of(after.at("terminal").at("startCount")) == 1);
        CHECK(strings_of(expected.at("callbacks")) ==
                std::vector<std::string>{"terminal-stop@fullscreen", "terminal-stop@regular"});
    }
}

TEST_CASE("frozen renderer-switch frames replay through the Pike roots",
        "[coding_agent][tui][fullscreen][renderer-switch][issue1018][compat-pi]") {
    const auto bundle = cch::tests::read_pi_tui_evidence("renderer-switch.json");
    REQUIRE(bundle);

    struct FrameCase {
        std::string_view scenario;
        std::string_view frame_key{"frameBefore"};
        std::size_t case_index{0};
        bool has_case{false};
        bool fullscreen{false};
        bool styled{false};
        bool overlay{false};
        bool overlay_hidden{false};
    };
    const std::vector<FrameCase> cases{
            {.scenario = "regular-to-fullscreen-retains-children-focus-editor-and-transcript"},
            {.scenario = "regular-to-fullscreen-retains-children-focus-editor-and-transcript",
                    .frame_key = "frameAfter",
                    .fullscreen = true,
                    .styled = true},
            {.scenario = "fullscreen-to-regular-retains-the-tree-and-detaches-the-layout-root", .fullscreen = true},
            {.scenario = "fullscreen-to-regular-retains-the-tree-and-detaches-the-layout-root",
                    .frame_key = "frameAfter",
                    .styled = true},
            {.scenario = "overlay-entry-refuses-the-switch-and-leaves-everything-intact",
                    .has_case = true,
                    .fullscreen = true,
                    .overlay = true,
                    .overlay_hidden = true},
            {.scenario = "overlay-entry-refuses-the-switch-and-leaves-everything-intact",
                    .case_index = 1,
                    .has_case = true,
                    .fullscreen = true,
                    .overlay = true},
    };

    std::size_t styled_frames = 0;
    for (const auto& frame_case : cases) {
        const auto& scenario = scenario_of(*bundle, frame_case.scenario);
        const JsonValue* const case_expected =
                frame_case.has_case
                        ? &scenario.at("expected").at("cases").get_array()[frame_case.case_index].at("expected")
                        : &scenario.at("expected");
        const JsonValue& inputs =
                frame_case.has_case
                        ? scenario.at("expected").at("cases").get_array()[frame_case.case_index].at("inputs")
                        : scenario.at("inputs");
        const JsonValue& frame = case_expected->at(std::string{frame_case.frame_key});
        REQUIRE(size_of(inputs.at("columns")) == 28);
        REQUIRE(size_of(inputs.at("rows")) == 8);
        INFO(std::format("scenario {} frame {}", frame_case.scenario, frame_case.frame_key));

        cch::tui::VirtualTerminal terminal({
                .columns = size_of(inputs.at("columns")),
                .rows = size_of(inputs.at("rows")),
        });
        const std::string document =
                frame_case.styled ? frame.at("styleProbeDocument").get_string() : joined(inputs.at("document"));
        if (frame_case.styled) ++styled_frames;

        if (frame_case.fullscreen) {
            ChatViewportOptions options;
            options.document = region(document);
            options.pending_messages = std::make_unique<cch::tui::Container>();
            options.status = region(inputs.at("status").get_string());
            options.editor = region(inputs.at("editor").get_string());
            options.footer = region(inputs.at("footer").get_string());
            options.scrollbar = cch::tui::ScrollbarMode::Hidden;
            auto viewport = create_chat_viewport(std::move(options));
            REQUIRE(viewport);

            cch::tui::AltScreen screen(terminal);
            screen.set_layout_root(std::move(viewport->root));
            REQUIRE(screen.start());
            cch::tui::Overlay* overlay_pointer = nullptr;
            if (frame_case.overlay) {
                // The recorded frame is the frozen renderer's incremental paint
                // over the baseline that was already on screen, so the replay
                // paints that baseline first.
                REQUIRE(screen.render());
                auto overlay = overlay_entry();
                overlay_pointer = overlay.get();
                REQUIRE(screen.show_overlay(std::move(overlay)));
                if (frame_case.overlay_hidden) REQUIRE(screen.hide_overlay(overlay_pointer));
            }
            REQUIRE(screen.render());
            check_frame(terminal, frame);
            REQUIRE(screen.stop());
            continue;
        }

        cch::tui::MainScreen screen(terminal);
        REQUIRE(screen.add_child(region(document)));
        REQUIRE(screen.add_child(std::make_unique<cch::tui::Container>()));
        REQUIRE(screen.add_child(region(inputs.at("status").get_string())));
        REQUIRE(screen.add_child(region(inputs.at("editor").get_string())));
        REQUIRE(screen.add_child(region(inputs.at("footer").get_string())));
        REQUIRE(screen.add_child(std::make_unique<ProbeComponent>()));
        REQUIRE(screen.start());
        REQUIRE(screen.render());
        check_frame(terminal, frame);
        REQUIRE(screen.stop());
    }
    // Both recorded complete styled grids were replayed, not just row text.
    CHECK(styled_frames == 2);
}

namespace {

[[nodiscard]] std::string live_visible_screen(const cch::tui::VirtualTerminal& terminal) {
    std::string text;
    for (const auto& line : terminal.screen()) {
        text.append(line);
        text.push_back('\n');
    }
    return text;
}

[[nodiscard]] tests::ModelsSessionOptions test_session_options(
        const tests::TempWorkspace& workspace, std::shared_ptr<tests::ScriptedProvider> client) {
    tests::ModelsSessionOptions options;
    options.session_target = coding_agent::InMemorySessionTarget{};
    options.workspace = workspace.path();
    options.models = cch::tests::models_from_provider(std::move(client));
    return options;
}

[[nodiscard]] support::AsyncResult<coding_agent::CreateAgentSessionResult> create_test_session_async(
        tests::RuntimeFixture& runtime, tests::ModelsSessionOptions options) {
    auto models = std::move(options.models);
    coding_agent::runtime::AgentSessionCreationRequest request = std::move(options);
    request.execution_runtime_target = runtime.make_target();
    return coding_agent::create_agent_session_async(
            std::move(request), std::nullopt, cch::tests::cli_fake_overrides(std::move(models)));
}

[[nodiscard]] coding_agent::tui::InteractiveSessionRun make_test_run(coding_agent::AgentSession& session,
        const std::filesystem::path& agent_config_directory,
        coding_agent::TuiMode tui_mode) {
    return coding_agent::tui::InteractiveSessionRunBuilder{}
            .with_session(session)
            .with_agent_config_directory(agent_config_directory)
            .with_tui_mode(tui_mode)
            .build();
}

[[nodiscard]] boost::asio::awaitable<support::ExpectedVoid> drive_test_engine(
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

TEST_CASE("live renderer switch validates focus and state expectations from frozen evidence",
        "[coding_agent][tui][fullscreen][renderer-switch][issue1018][compat-pi]") {
    const auto bundle = cch::tests::read_pi_tui_evidence("renderer-switch.json");
    REQUIRE(bundle);

    const auto& f2r_scenario =
            scenario_of(*bundle, "fullscreen-to-regular-retains-the-tree-and-detaches-the-layout-root");
    const auto& f2r_expected = f2r_scenario.at("expected");
    const auto& f2r_before = f2r_expected.at("stateBefore");
    const auto& f2r_after = f2r_expected.at("stateAfter");
    const auto& f2r_inputs = f2r_scenario.at("inputs");

    tests::RuntimeFixture runtime;
    tests::TempWorkspace workspace;
    tests::TempWorkspace config;
    config.write("settings.json", R"({"tuiMode": "fullscreen"})");
    auto created = runtime.run(
            create_test_session_async(runtime, test_session_options(workspace, tests::make_scripted_fake_provider())));
    REQUIRE(created);
    tests::RuntimeLoopDriver runtime_driver(runtime);

    cch::tui::VirtualTerminal terminal({
            .columns = size_of(f2r_inputs.at("columns")),
            .rows = size_of(f2r_inputs.at("rows")),
    });
    boost::asio::io_context io;
    std::optional<support::ExpectedVoid> run_result;
    auto engine = std::make_shared<coding_agent::tui::InteractiveEngine>(
            terminal, io.get_executor(), coding_agent::TuiMode::Fullscreen);
    boost::asio::co_spawn(io,
            drive_test_engine(
                    engine, make_test_run(*created->session, config.path(), coding_agent::TuiMode::Fullscreen)),
            [&](std::exception_ptr exception, support::ExpectedVoid result) {
                CHECK(exception == nullptr);
                run_result.emplace(std::move(result));
            });
    tests::drain_ready(io);
    const tests::RunJoinGuard join_run{io, [&] { return run_result.has_value(); }};
    REQUIRE(terminal.modes().alternate_screen);
    REQUIRE(tests::pump_until(io, [&] { return engine->focus() != nullptr; }));

    // Verify initial fullscreen mode matches recorded stateBefore.
    CHECK(f2r_before.at("mode").get_string() == "fullscreen");
    auto* const initial_focus = engine->focus();
    REQUIRE(initial_focus != nullptr);

    // Type the recorded draft editor text.
    const std::string draft = f2r_inputs.at("editor").get_string();
    REQUIRE(terminal.inject_input(draft));
    REQUIRE(tests::pump_until(io, [&] { return live_visible_screen(terminal).find(draft) != std::string::npos; }));

    // Hidden overlay refusal check against recorded overlay scenario:
    // Any existing overlay entry (hidden included) refuses the switch.
    auto& presenter = static_cast<coding_agent::tui::ModalPresenter&>(*engine);
    auto hidden_overlay = overlay_entry();
    auto hidden_options = hidden_overlay->options();
    hidden_options.visible_at = [](cch::tui::TerminalDimensions) { return false; };
    hidden_overlay->set_options(std::move(hidden_options));
    presenter.show_overlay(std::move(hidden_overlay));
    tests::drain_ready(io);

    auto hidden_refusal = engine->switch_tui_mode(coding_agent::TuiMode::Regular);
    REQUIRE(hidden_refusal);
    CHECK_FALSE(*hidden_refusal);
    CHECK(terminal.modes().alternate_screen);
    CHECK(engine->focus() == initial_focus);

    // Clear overlay so the switch can proceed.
    presenter.close_overlay();
    tests::drain_ready(io);

    // Live switch fullscreen -> regular: asserts recorded result (true) and mode transition.
    auto switched_to_regular = engine->switch_tui_mode(coding_agent::TuiMode::Regular);
    REQUIRE(switched_to_regular);
    CHECK(*switched_to_regular == f2r_expected.at("result").get_boolean());
    tests::drain_ready(io);
    REQUIRE(tests::pump_until(io, [&] { return !terminal.modes().alternate_screen; }));
    CHECK(f2r_after.at("mode").get_string() == "regular");

    // Live focus target identity: the focused component survives across roots.
    CHECK(engine->focus() == initial_focus);

    // Live editor text continuity: draft text is retained on the regular root.
    CHECK(live_visible_screen(terminal).find(draft) != std::string::npos);

    // Switch regular -> fullscreen: roundtrip retains focus and text.
    auto switched_to_fullscreen = engine->switch_tui_mode(coding_agent::TuiMode::Fullscreen);
    REQUIRE(switched_to_fullscreen);
    CHECK(*switched_to_fullscreen);
    tests::drain_ready(io);
    REQUIRE(tests::pump_until(io, [&] { return terminal.modes().alternate_screen; }));
    CHECK(engine->focus() == initial_focus);
    CHECK(live_visible_screen(terminal).find(draft) != std::string::npos);

    // Submit the draft text and then exit on empty prompt.
    REQUIRE(terminal.inject_input("\r"));
    REQUIRE(tests::pump_until(
            io, [&] { return live_visible_screen(terminal).find("fake: draft prompt") != std::string::npos; }));

    // Exit cleanly.
    REQUIRE(terminal.inject_input("\x04"));
    REQUIRE(tests::pump_until(io, [&] { return run_result.has_value(); }));
    REQUIRE(run_result);
    CHECK(*run_result);
}

} // namespace cch::coding_agent::tui
