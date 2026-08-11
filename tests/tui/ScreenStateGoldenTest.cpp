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

#include <cch/tui/Input.hpp>
#include <cch/tui/Overlay.hpp>
#include <cch/tui/SelectList.hpp>
#include <cch/tui/Text.hpp>
#include <cch/tui/Tui.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include "util/Json.hpp"

#include "../../third_party/catch2/catch_test_macros.hpp"

#include <format>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace cch;

// CatchLite has no INFO/SECTION; these carry a context string into the
// failure message so loop iterations are identifiable.
#define CHECK_WITH(expr, context) \
    ::CatchLite::check( \
        static_cast<bool>(expr), \
        (std::string{#expr} + " [" + (context) + "]").c_str(), __FILE__, __LINE__)
#define REQUIRE_WITH(expr, context) \
    ::CatchLite::require( \
        static_cast<bool>(expr), \
        (std::string{#expr} + " [" + (context) + "]").c_str(), __FILE__, __LINE__)

namespace {

[[nodiscard]] util::Expected<util::JsonValue> read_screen_state() {
    const auto path = std::filesystem::path{CCH_SOURCE_DIR} / "fixtures/pi-tui/screen-state.json";
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Unknown,
            "Failed to open screen-state fixture: " + path.string()));
    }
    const std::string json{
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{}};
    return util::read_json(json);
}

/// A fixed-line component for renderer scenarios (pi's test-local `Lines`).
class LinesComponent final : public tui::Component {
public:
    explicit LinesComponent(std::vector<std::string> lines) : lines_(std::move(lines)) {}

    void set_lines(std::vector<std::string> lines) {
        lines_ = std::move(lines);
        cache_valid_ = false;
    }

    [[nodiscard]] util::Expected<tui::RenderResult> render(std::size_t) override {
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
    [[nodiscard]] util::Expected<tui::RenderResult> render(std::size_t) override {
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
    REQUIRE_WITH(false, std::string{"unknown screen-state scenario "} + std::string(name));
    return {};
}

} // namespace

TEST_CASE("VirtualTerminal screen-state goldens match the committed snapshots", "[tui][differential][issue386]") {
    const auto fixture = read_screen_state();
    REQUIRE(fixture);
    const auto& root = fixture->get<util::JsonValue::object_t>();
    const auto& scenarios = root.at("scenarios").get<util::JsonValue::array_t>();
    REQUIRE_FALSE(scenarios.empty());

    for (const auto& entry : scenarios) {
        const auto& object = entry.get<util::JsonValue::object_t>();
        const auto name = object.at("name").get_string();
        const auto columns = static_cast<std::size_t>(object.at("columns").get_number());
        const auto rows = static_cast<std::size_t>(object.at("rows").get_number());
        const auto& expected = object.at("expected").get<util::JsonValue::array_t>();

        const auto context = std::string{"scenario "} + name;
        const auto actual = run_scenario(name, columns, rows);
        REQUIRE_WITH(actual.size() == expected.size(), context);
        for (std::size_t index = 0; index < expected.size(); ++index) {
            CHECK_WITH(actual[index] == expected[index].get_string(), context);
        }
    }
}
