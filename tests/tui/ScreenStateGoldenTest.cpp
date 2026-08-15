// VirtualTerminal screen-state goldens for the pi-tui completion gate
// (issue #386, fork-B/renderer evidence): every scenario in
// `fixtures/pi-tui/screen-state.json` is replayed through the VirtualTerminal
// harness and the deterministic screen state (or forwarded-input outcome) is
// byte-compared with the committed snapshot. Each row cites the frozen pi
// test expectation it mirrors (`fixtures/pi-tui/README.md` keeps the
// row-by-row capability-to-source checklist).
//
// The scenarios cover the renderer (middle/last-line diff updates, clear on
// shrink), the cell-size response consumer (bare-ESC forwarding and response
// consumption), overlay placement (anchors, non-capturing pass-through), and
// component rendering (Input horizontal scroll, SelectList description
// normalization).

#include <cch/tui/Image.hpp>
#include <cch/tui/Input.hpp>
#include <cch/tui/Overlay.hpp>
#include <cch/tui/SelectList.hpp>
#include <cch/tui/Text.hpp>
#include <cch/tui/Tui.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include "support/Json.hpp"

#include <cch/support/Error.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <format>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace cch;

namespace {

[[nodiscard]] support::Expected<support::JsonValue> read_screen_state() {
    const auto path = std::filesystem::path{CCH_SOURCE_DIR} / "fixtures/pi-tui/screen-state.json";
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Unknown,
            "Failed to open screen-state fixture: " + path.string()));
    }
    const std::string json{
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{}};
    return support::read_json(json);
}

/// A fixed-line component for renderer scenarios (pi's test-local `Lines`).
class LinesComponent final : public tui::Component {
public:
    explicit LinesComponent(std::vector<std::string> lines) : lines_(std::move(lines)) {}

    void set_lines(std::vector<std::string> lines) {
        lines_ = std::move(lines);
        cache_valid_ = false;
    }

    [[nodiscard]] support::Expected<tui::RenderResult> render(std::size_t) override {
        if (cache_valid_) return tui::RenderResult{.lines = cached_};
        cached_ = lines_;
        cache_valid_ = true;
        return tui::RenderResult{.lines = lines_};
    }

    void invalidate() override {
        cache_valid_ = false;
    }

private:
    std::vector<std::string> lines_;
    std::vector<std::string> cached_;
    bool cache_valid_{false};
};

/// A component that records decoded key events (pi's test-local
/// `InputRecorder`).
class InputRecorder final
    : public tui::Component,
      public tui::InputHandler,
      public tui::Focusable {
public:
    [[nodiscard]] support::Expected<tui::RenderResult> render(std::size_t) override {
        return tui::RenderResult{.lines = {""}};
    }

    void invalidate() override {}

    void handle_input(const tui::InputEventVariant& input) override {
        received.push_back(input);
    }

    [[nodiscard]] bool accepts_key_releases() const override {
        return false;
    }

    void set_focused(bool) override {}
    [[nodiscard]] bool focused() const override {
        return true;
    }

    std::vector<tui::InputEventVariant> received;
};

/// A canonical 1x1 transparent PNG for the scrollback image-follows-content
/// scenario (decoded by the Image component through the stb seam).
[[nodiscard]] std::string tiny_png_base64() {
    return "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==";
}

/// Replay one golden scenario and return the observed outcome lines.
/// Every scenario drives the VirtualTerminal/Tui seams deterministically.
[[nodiscard]] std::vector<std::string> run_scenario(
    std::string_view name,
    std::size_t columns,
    std::size_t rows) {
    if (name == "renderer-middle-line-change") {
        tui::VirtualTerminal terminal({.columns = columns, .rows = rows});
        tui::Tui tui(terminal);
        auto lines = std::make_unique<LinesComponent>(
            std::vector<std::string>{"line one", "line two", "line thr"});
        auto* pointer = lines.get();
        REQUIRE(tui.add_child(std::move(lines)));
        REQUIRE(tui.start());
        REQUIRE(tui.render());
        pointer->set_lines(std::vector<std::string>{"line one", "line two", "line chg"});
        REQUIRE(tui.render());
        return terminal.screen();
    }
    if (name == "renderer-last-line-change") {
        tui::VirtualTerminal terminal({.columns = columns, .rows = rows});
        tui::Tui tui(terminal);
        auto lines = std::make_unique<LinesComponent>(
            std::vector<std::string>{"alpha", "beta", "gamma"});
        auto* pointer = lines.get();
        REQUIRE(tui.add_child(std::move(lines)));
        REQUIRE(tui.start());
        REQUIRE(tui.render());
        pointer->set_lines(std::vector<std::string>{"alpha", "beta", "changed"});
        REQUIRE(tui.render());
        return terminal.screen();
    }
    if (name == "shrink-to-zero") {
        tui::VirtualTerminal terminal({.columns = columns, .rows = rows});
        tui::Tui tui(terminal);
        auto lines = std::make_unique<LinesComponent>(
            std::vector<std::string>{"first", "second", "third"});
        auto* pointer = lines.get();
        REQUIRE(tui.add_child(std::move(lines)));
        REQUIRE(tui.start());
        REQUIRE(tui.render());
        pointer->set_lines({});
        REQUIRE(tui.render());
        return terminal.screen();
    }
    if (name == "shrink-to-single-line") {
        tui::VirtualTerminal terminal({.columns = columns, .rows = rows});
        tui::Tui tui(terminal);
        auto lines = std::make_unique<LinesComponent>(std::vector<std::string>{
            "Line 0", "Line 1", "Line 2", "Line 3"});
        auto* pointer = lines.get();
        REQUIRE(tui.add_child(std::move(lines)));
        REQUIRE(tui.start());
        REQUIRE(tui.render());
        pointer->set_lines(std::vector<std::string>{"Only"});
        REQUIRE(tui.render());
        return terminal.screen();
    }
    if (name == "shrink-clears-trailing-rows") {
        tui::VirtualTerminal terminal({.columns = columns, .rows = rows});
        tui::Tui tui(terminal);
        auto lines = std::make_unique<LinesComponent>(std::vector<std::string>{
            "Line 0", "Line 1", "Line 2", "Line 3", "Line 4", "Line 5"});
        auto* pointer = lines.get();
        REQUIRE(tui.add_child(std::move(lines)));
        REQUIRE(tui.start());
        REQUIRE(tui.render());
        pointer->set_lines(std::vector<std::string>{"Line 0", "Line 1"});
        REQUIRE(tui.render());
        return terminal.screen();
    }
    if (name == "cell-size-bare-escape") {
        tui::VirtualTerminal terminal({.columns = columns, .rows = rows});
        tui::Tui tui(terminal);
        auto recorder = std::make_unique<InputRecorder>();
        auto* pointer = recorder.get();
        REQUIRE(tui.add_child(std::move(recorder)));
        REQUIRE(tui.start());
        REQUIRE(tui.set_focus(pointer));
        REQUIRE(terminal.inject_input("\x1b"));
        REQUIRE(terminal.inject_input(""));
        if (pointer->received.size() == 1) {
            if (const auto* key = std::get_if<tui::KeyEvent>(&pointer->received.front())) {
                return {tui::key_id(*key)};
            }
        }
        return {};
    }
    if (name == "cell-size-response-consumed") {
        tui::VirtualTerminal terminal({.columns = columns, .rows = rows});
        tui::Tui tui(terminal);
        auto recorder = std::make_unique<InputRecorder>();
        auto* pointer = recorder.get();
        REQUIRE(tui.add_child(std::move(recorder)));
        REQUIRE(tui.start());
        REQUIRE(tui.set_focus(pointer));
        REQUIRE(terminal.inject_input("\x1b[6;20;10t"));
        CHECK(pointer->received.empty());
        REQUIRE(terminal.inject_input("q"));
        std::vector<std::string> outcome;
        if (!pointer->received.empty()) {
            if (const auto* key = std::get_if<tui::KeyEvent>(&pointer->received.front())) {
                outcome.push_back(tui::key_id(*key));
            }
        }
        const auto& pixels = terminal.capabilities().cell_pixels;
        outcome.push_back(pixels
            ? std::format("cell {}x{}", pixels->width, pixels->height)
            : std::string{"cell unset"});
        return outcome;
    }
    if (name == "overlay-bottom-right") {
        tui::VirtualTerminal terminal({.columns = columns, .rows = rows});
        tui::Tui tui(terminal);
        REQUIRE(tui.add_child(std::make_unique<tui::Text>("base", 0, 0)));
        tui::OverlayOptions options;
        options.position = tui::OverlayPosition::BottomRight;
        auto overlay = std::make_unique<tui::Overlay>(options);
        overlay->set_anchor(0, 0, columns, rows);
        REQUIRE(overlay->add_child(std::make_unique<tui::Text>("ov", 0, 0)));
        REQUIRE(tui.add_overlay(std::move(overlay)));
        REQUIRE(tui.start());
        REQUIRE(tui.render());
        return terminal.screen();
    }
    if (name == "overlay-top-left") {
        tui::VirtualTerminal terminal({.columns = columns, .rows = rows});
        tui::Tui tui(terminal);
        auto overlay = std::make_unique<tui::Overlay>();
        REQUIRE(overlay->add_child(std::make_unique<tui::Text>("ov", 0, 0)));
        REQUIRE(tui.add_overlay(std::move(overlay)));
        REQUIRE(tui.add_child(std::make_unique<tui::Text>("base", 0, 0)));
        REQUIRE(tui.start());
        REQUIRE(tui.render());
        return terminal.screen();
    }
    if (name == "overlay-non-capturing-passthrough") {
        tui::VirtualTerminal terminal({.columns = columns, .rows = rows});
        tui::Tui tui(terminal);
        auto recorder = std::make_unique<InputRecorder>();
        auto* pointer = recorder.get();
        REQUIRE(tui.add_child(std::move(recorder)));
        REQUIRE(tui.start());
        REQUIRE(tui.set_focus(pointer));
        tui::OverlayOptions options;
        options.non_capturing = true;
        auto overlay = std::make_unique<tui::Overlay>(options);
        REQUIRE(overlay->add_child(std::make_unique<tui::Text>("ov", 0, 0)));
        REQUIRE(tui.add_overlay(std::move(overlay)));
        REQUIRE(terminal.inject_input("a"));
        std::vector<std::string> outcome;
        if (!pointer->received.empty()) {
            if (const auto* key = std::get_if<tui::KeyEvent>(&pointer->received.front())) {
                outcome.push_back(tui::key_id(*key));
            }
        }
        return outcome;
    }
    if (name == "component-input-cjk") {
        tui::VirtualTerminal terminal({.columns = columns, .rows = rows});
        tui::Tui tui(terminal);
        auto input = std::make_unique<tui::Input>();
        input->set_value("가나다라마바사아자");
        auto* pointer = input.get();
        REQUIRE(tui.add_child(std::move(input)));
        REQUIRE(tui.start());
        REQUIRE(tui.set_focus(pointer));
        REQUIRE(tui.render());
        return terminal.screen();
    }
    if (name == "component-select-list") {
        tui::VirtualTerminal terminal({.columns = columns, .rows = rows});
        tui::Tui tui(terminal);
        auto list = std::make_unique<tui::SelectList>(
            std::vector<tui::SelectItem>{
                {.value = "short", .label = "Short", .description = "line one\nline two"},
                {
                    .value = "long",
                    .label = "A very long primary label",
                    .description = "second description",
                },
            },
            tui::SelectListOptions{
                .layout = {
                    .min_primary_column_width = 12,
                    .max_primary_column_width = 20,
                },
            });
        auto* pointer = list.get();
        REQUIRE(tui.add_child(std::move(list)));
        REQUIRE(tui.start());
        REQUIRE(tui.set_focus(pointer));
        REQUIRE(tui.render());
        return terminal.screen();
    }
    if (name == "scrollback-full-buffer-write") {
        tui::VirtualTerminal terminal({.columns = columns, .rows = rows});
        tui::Tui tui(terminal);
        REQUIRE(tui.add_child(std::make_unique<LinesComponent>(std::vector<std::string>{
            "line zero", "line one", "line two", "line thr", "line four"})));
        REQUIRE(tui.start());
        REQUIRE(tui.render());
        // The renderer writes the full composed buffer; overflow advanced into
        // the terminal's native scrollback (scrollback() ++ screen() == buffer).
        auto outcome = terminal.scrollback();
        outcome.insert(outcome.end(), terminal.screen().begin(), terminal.screen().end());
        return outcome;
    }
    if (name == "scrollback-viewport-top-tracking") {
        tui::VirtualTerminal terminal({.columns = columns, .rows = rows});
        tui::Tui tui(terminal);
        auto lines = std::make_unique<LinesComponent>(
            std::vector<std::string>{"one", "two", "three"});
        auto* pointer = lines.get();
        REQUIRE(tui.add_child(std::move(lines)));
        REQUIRE(tui.start());
        REQUIRE(tui.render());
        pointer->set_lines(std::vector<std::string>{"one", "two", "three", "four", "five"});
        REQUIRE(tui.render());
        // Appending content advances the viewport top over the buffer: the
        // first two lines scrolled into the terminal's scrollback.
        auto outcome = terminal.scrollback();
        outcome.insert(outcome.end(), terminal.screen().begin(), terminal.screen().end());
        return outcome;
    }
    if (name == "scrollback-startup-header-scrolls-away") {
        tui::VirtualTerminal terminal({.columns = columns, .rows = rows});
        tui::Tui tui(terminal);
        auto lines = std::make_unique<LinesComponent>(std::vector<std::string>{
            "header...", "res:one", "res:two"});
        auto* pointer = lines.get();
        REQUIRE(tui.add_child(std::move(lines)));
        REQUIRE(tui.start());
        REQUIRE(tui.render());
        // The first render shows the startup header and loaded-resources block
        // (no clear, pi "assumes clean screen"); once the buffer grows past one
        // screen the header scrolls away with the flow.
        const auto& first_screen = terminal.screen();
        REQUIRE(first_screen[0].find("header...") != std::string::npos);
        pointer->set_lines(std::vector<std::string>{
            "header...", "res:one", "res:two", "user msg", "assistant"});
        REQUIRE(tui.render());
        auto outcome = terminal.scrollback();
        outcome.insert(outcome.end(), terminal.screen().begin(), terminal.screen().end());
        return outcome;
    }
    if (name == "scrollback-first-render-no-clear") {
        tui::VirtualTerminal terminal({.columns = columns, .rows = rows});
        tui::Tui tui(terminal);
        REQUIRE(tui.add_child(std::make_unique<LinesComponent>(std::vector<std::string>{
            "line zero", "line one", "line two", "line thr", "line four"})));
        REQUIRE(tui.start());
        REQUIRE(tui.render());
        std::vector<std::string> outcome;
        outcome.push_back(
            terminal.check_clear_screen_called() ? "clear-screen" : "no-clear-screen");
        outcome.push_back(terminal.check_clear_scrollback_called()
            ? "clear-scrollback"
            : "no-clear-scrollback");
        auto full = terminal.scrollback();
        full.insert(full.end(), terminal.screen().begin(), terminal.screen().end());
        outcome.insert(outcome.end(), full.begin(), full.end());
        return outcome;
    }
    if (name == "scrollback-resize-clears-screen-and-scrollback") {
        tui::VirtualTerminal terminal({.columns = columns, .rows = rows});
        tui::Tui tui(terminal);
        REQUIRE(tui.add_child(std::make_unique<LinesComponent>(std::vector<std::string>{
            "line zero", "line one", "line two", "line thr", "line four"})));
        REQUIRE(tui.start());
        REQUIRE(tui.render());
        // A height change reflows from a clean screen: clear screen, home,
        // clear scrollback (`\x1b[2J\x1b[H\x1b[3J`), so the scroll history is
        // cleared and the buffer starts clean at the new height.
        REQUIRE(terminal.inject_resize({.columns = columns, .rows = rows + 2}));
        REQUIRE(tui.render());
        std::vector<std::string> outcome;
        outcome.push_back(
            terminal.check_clear_screen_called() ? "clear-screen" : "no-clear-screen");
        outcome.push_back(terminal.check_clear_scrollback_called()
            ? "clear-scrollback"
            : "no-clear-scrollback");
        outcome.insert(outcome.end(), terminal.screen().begin(), terminal.screen().end());
        return outcome;
    }
    if (name == "scrollback-image-follows-content") {
        tui::VirtualTerminal terminal({
            .columns = columns,
            .rows = rows,
            .capabilities = {
                .synchronized_output = true,
                .inline_images = tui::InlineImageProtocol::Kitty,
                .cell_pixels = tui::CellPixelDimensions{.width = 10, .height = 10},
            },
        });
        tui::Tui tui(terminal);
        auto top = std::make_unique<LinesComponent>(std::vector<std::string>{"one", "two"});
        REQUIRE(tui.add_child(std::move(top)));
        REQUIRE(tui.add_child(std::make_unique<tui::Image>(
            tui::ImageContent{
                .encoded_data = tiny_png_base64(),
                .mime_type = "image/png",
                .filename = "follow.png",
            },
            tui::ImageOptions{
                .constraints = {.max_width = 1, .max_height = 1},
            })));
        auto bottom = std::make_unique<LinesComponent>(
            std::vector<std::string>{"five", "six", "seven"});
        auto* bottom_pointer = bottom.get();
        REQUIRE(tui.add_child(std::move(bottom)));
        REQUIRE(tui.start());
        REQUIRE(tui.render());
        REQUIRE(terminal.images().size() == 1);
        // Growing the content below the image advances the viewport so the
        // image's buffer row scrolls into the terminal's scrollback; the
        // placement keeps its absolute row identity (fork-B image-follows-
        // content).
        bottom_pointer->set_lines(
            std::vector<std::string>{"five", "six", "seven", "eight", "nine"});
        REQUIRE(tui.render());
        std::vector<std::string> outcome;
        for (const auto& image : terminal.images()) {
            outcome.push_back(std::format(
                "image@row={},rows={}", image.region.row, image.region.rows));
        }
        outcome.push_back(std::format("viewport-top={}", terminal.viewport_top()));
        return outcome;
    }
    INFO(std::string{"unknown screen-state scenario "} + std::string(name));
    REQUIRE(false);
    return {};
}

} // namespace

TEST_CASE("VirtualTerminal screen-state goldens match the committed snapshots", "[tui][differential][issue386]") {
    const auto fixture = read_screen_state();
    REQUIRE(fixture);
    const auto& root = fixture->get<support::JsonValue::object_t>();
    const auto& scenarios = root.at("scenarios").get<support::JsonValue::array_t>();
    REQUIRE_FALSE(scenarios.empty());

    for (const auto& entry : scenarios) {
        const auto& object = entry.get<support::JsonValue::object_t>();
        const auto name = object.at("name").get_string();
        const auto columns = static_cast<std::size_t>(object.at("columns").get_number());
        const auto rows = static_cast<std::size_t>(object.at("rows").get_number());
        const auto& expected = object.at("expected").get<support::JsonValue::array_t>();

        const auto context = std::string{"scenario "} + name;
        INFO(context);
        const auto actual = run_scenario(name, columns, rows);
        REQUIRE(actual.size() == expected.size());
        for (std::size_t index = 0; index < expected.size(); ++index) {
            CHECK(actual[index] == expected[index].get_string());
        }
    }
}
