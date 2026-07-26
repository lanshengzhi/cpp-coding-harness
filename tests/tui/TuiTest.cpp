#include "../../third_party/catch2/catch_test_macros.hpp"

#include <cch/tui/Text.hpp>
#include <cch/tui/Tui.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include "tui/UnicodeWidth.hpp"

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

class OverwideComponent final : public cch::tui::Component {
public:
    [[nodiscard]] cch::util::Expected<std::vector<std::string>> render(std::size_t) override {
        return std::vector<std::string>{"too wide"};
    }

    void invalidate() override {}
};

class FocusableInputComponent final
    : public cch::tui::Component,
      public cch::tui::InputHandler,
      public cch::tui::Focusable {
public:
    [[nodiscard]] cch::util::Expected<std::vector<std::string>> render(std::size_t) override {
        return std::vector<std::string>{"x"};
    }

    void invalidate() override {
        ++invalidation_count;
    }

    void handle_input(std::string_view input) override {
        received_input = input;
    }

    void set_focused(bool focused) override {
        focused_ = focused;
    }

    [[nodiscard]] bool focused() const override {
        return focused_;
    }

    std::size_t invalidation_count{0};
    std::string received_input;

private:
    bool focused_{false};
};

} // namespace

TEST_CASE("Tui renders attached Text through the VirtualTerminal seam", "[tui][issue45]") {
    cch::tui::VirtualTerminal terminal({.columns = 8, .rows = 2});
    cch::tui::Tui tui(terminal);
    // Use zero padding so the rendered output is just the text
    REQUIRE(tui.add_child(std::make_unique<cch::tui::Text>("hello", 0, 0)));

    REQUIRE(tui.start());
    REQUIRE(tui.render());

    // Text pads content to full width
    const std::vector<std::string> expected_screen{"hello   ", ""};
    CHECK(terminal.screen() == expected_screen);
    const std::vector<std::string> expected_output{"hello   "};
    CHECK(terminal.output() == expected_output);
    const cch::tui::CursorPosition expected_cursor{.column = 8, .row = 0};
    CHECK(terminal.cursor() == expected_cursor);
    CHECK(terminal.final_style() == cch::tui::VirtualTerminalStyle{});
    CHECK_FALSE(terminal.modes().cursor_visible);

    REQUIRE(tui.stop());
    CHECK_FALSE(terminal.modes().started);
    CHECK(terminal.modes().cursor_visible);
}

TEST_CASE("Tui exposes styled Text cells with a default final style", "[tui][issue46][unicode]") {
    cch::tui::VirtualTerminal terminal({.columns = 4, .rows = 1});
    cch::tui::Tui tui(terminal);
    REQUIRE(tui.add_child(std::make_unique<cch::tui::Text>("\x1b[31mA", 0, 0)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());

    REQUIRE(terminal.cells().size() == 1);
    REQUIRE(terminal.cells()[0].size() == 4);
    CHECK(terminal.cells()[0][0].grapheme == "A");
    CHECK(terminal.cells()[0][0].style.fg_color == "31");
    CHECK(terminal.final_style() == cch::tui::VirtualTerminalStyle{});
}

TEST_CASE("Tui rejects a null Component attachment", "[tui][issue45]") {
    cch::tui::VirtualTerminal terminal;
    cch::tui::Tui tui(terminal);

    const auto result = tui.add_child(nullptr);

    REQUIRE_FALSE(result);
    CHECK(result.error().code == cch::util::ErrorCode::Validation);
    CHECK(result.error().message == "TUI cannot attach a null Component");
}

TEST_CASE("Text accepts Unicode characters", "[tui][issue46][unicode]") {
    cch::tui::Text text("\xc3\xa9", 0, 0); // é in UTF-8

    const auto result = text.render(2);
    REQUIRE(result);
    REQUIRE(result->size() == 1);
    CHECK(cch::tui::detail::visible_width((*result)[0]) >= 1);
}

TEST_CASE("Tui rejects a Component line wider than its visible width", "[tui][issue45]") {
    cch::tui::VirtualTerminal terminal({.columns = 3, .rows = 1});
    cch::tui::Tui tui(terminal);
    REQUIRE(tui.add_child(std::make_unique<OverwideComponent>()));

    REQUIRE(tui.start());
    const auto result = tui.render();

    REQUIRE_FALSE(result);
    CHECK(result.error().code == cch::util::ErrorCode::Validation);
    CHECK(result.error().message == "TUI component rendered a line wider than its width bound");
    CHECK(result.error().detail == "line width 8 exceeds visible width 3");
    CHECK(result.error().detail.size() < 128);
    const std::vector<std::string> expected_screen{""};
    CHECK(terminal.screen() == expected_screen);
}

TEST_CASE("Tui routes focus, input, and invalidation through Component capabilities", "[tui][issue45]") {
    cch::tui::VirtualTerminal terminal({.columns = 4, .rows = 1});
    cch::tui::Tui tui(terminal);
    auto component = std::make_unique<FocusableInputComponent>();
    auto* component_pointer = component.get();
    REQUIRE(tui.add_child(std::move(component)));

    REQUIRE(tui.start());
    REQUIRE(tui.set_focus(component_pointer));
    REQUIRE(terminal.inject_input("q"));
    tui.invalidate();

    CHECK(component_pointer->focused());
    CHECK(component_pointer->received_input == "q");
    CHECK(component_pointer->invalidation_count == 1);
}

TEST_CASE("VirtualTerminal injects input and resize events deterministically", "[tui][issue45]") {
    cch::tui::VirtualTerminal terminal({.columns = 4, .rows = 1});
    std::string received_input;
    std::optional<cch::tui::TerminalDimensions> received_resize;

    REQUIRE(terminal.start(
        [&received_input](std::string input) { received_input = std::move(input); },
        [&received_resize](cch::tui::TerminalDimensions dimensions) { received_resize = dimensions; }));
    REQUIRE(terminal.inject_input("x"));
    REQUIRE(terminal.inject_resize({.columns = 6, .rows = 2}));

    CHECK(received_input == "x");
    REQUIRE(received_resize);
    const cch::tui::TerminalDimensions expected_dimensions{.columns = 6, .rows = 2};
    CHECK(*received_resize == expected_dimensions);
    CHECK(terminal.dimensions() == expected_dimensions);
}

TEST_CASE("VirtualTerminal preserves cursor-positioned writes and rejects overflow", "[tui][issue45]") {
    cch::tui::VirtualTerminal terminal({.columns = 4, .rows = 1});
    REQUIRE(terminal.start(
        [](std::string) {},
        [](cch::tui::TerminalDimensions) {}));
    REQUIRE(terminal.set_cursor({.column = 2, .row = 0}));
    REQUIRE(terminal.write("x"));

    const std::vector<std::string> expected_screen{"  x"};
    CHECK(terminal.screen() == expected_screen);
    const cch::tui::CursorPosition expected_cursor{.column = 3, .row = 0};
    CHECK(terminal.cursor() == expected_cursor);

    const auto result = terminal.write("yz");
    REQUIRE_FALSE(result);
    CHECK(result.error().code == cch::util::ErrorCode::Validation);
    CHECK(terminal.screen() == expected_screen);
    const std::vector<std::string> expected_output{"x"};
    CHECK(terminal.output() == expected_output);
}

TEST_CASE("VirtualTerminal accepts Unicode output", "[tui][issue46][unicode]") {
    cch::tui::VirtualTerminal terminal({.columns = 4, .rows = 1});
    REQUIRE(terminal.start(
        [](std::string) {},
        [](cch::tui::TerminalDimensions) {}));

    const auto result = terminal.write("\xc3\xa9"); // é

    REQUIRE(result);
    CHECK(terminal.screen().size() == 1);
    CHECK_FALSE(terminal.output().empty());
}

TEST_CASE("VirtualTerminal exposes visible cells and final style", "[tui][issue46][unicode]") {
    cch::tui::VirtualTerminal terminal({.columns = 4, .rows = 1});
    REQUIRE(terminal.start(
        [](std::string) {},
        [](cch::tui::TerminalDimensions) {}));

    REQUIRE(terminal.write("\x1b[31mA"));

    REQUIRE(terminal.cells().size() == 1);
    REQUIRE(terminal.cells()[0].size() == 4);
    CHECK(terminal.cells()[0][0].grapheme == "A");
    CHECK(terminal.cells()[0][0].style.fg_color == "31");
    CHECK(terminal.final_style().fg_color == "31");
    const std::vector<std::string> expected_screen{"A"};
    CHECK(terminal.screen() == expected_screen);
}

TEST_CASE("VirtualTerminal clears complete wide graphemes on overwrite", "[tui][issue46][unicode]") {
    cch::tui::VirtualTerminal terminal({.columns = 4, .rows = 1});
    REQUIRE(terminal.start(
        [](std::string) {},
        [](cch::tui::TerminalDimensions) {}));
    REQUIRE(terminal.write("\xe4\xb8\xad"));
    REQUIRE(terminal.cells()[0][1].continuation);

    REQUIRE(terminal.set_cursor({.column = 1, .row = 0}));
    REQUIRE(terminal.write("x"));

    CHECK(terminal.cells()[0][0].grapheme.empty());
    CHECK_FALSE(terminal.cells()[0][1].continuation);
    CHECK(terminal.cells()[0][1].grapheme == "x");
    const std::vector<std::string> expected_screen{" x"};
    CHECK(terminal.screen() == expected_screen);
}

TEST_CASE("VirtualTerminal bounds callback failures", "[tui][issue45]") {
    cch::tui::VirtualTerminal terminal;
    REQUIRE(terminal.start(
        [](std::string) { throw std::runtime_error("input failure"); },
        [](cch::tui::TerminalDimensions) { throw std::runtime_error("resize failure"); }));

    const auto input_result = terminal.inject_input("x");
    REQUIRE_FALSE(input_result);
    CHECK(input_result.error().message == "Virtual Terminal input sink failed");
    CHECK(input_result.error().detail == "the input callback threw an exception");

    const auto resize_result = terminal.inject_resize({.columns = 6, .rows = 2});
    REQUIRE_FALSE(resize_result);
    CHECK(resize_result.error().message == "Virtual Terminal resize sink failed");
    CHECK(resize_result.error().detail == "the resize callback threw an exception");
}
