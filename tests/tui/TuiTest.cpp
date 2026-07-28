#include <cch/tui/Input.hpp>
#include <cch/tui/Text.hpp>
#include <cch/tui/Tui.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include "tui/UnicodeWidth.hpp"

#include "../../third_party/catch2/catch_test_macros.hpp"

#include <array>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

class OverwideComponent final : public cch::tui::Component {
public:
    [[nodiscard]] cch::util::Expected<cch::tui::RenderResult> render(std::size_t) override {
        return cch::tui::RenderResult{.lines = {"too wide"}};
    }

    void invalidate() override {}
};

class PartialStartupFailureTerminal final : public cch::tui::Terminal {
public:
    [[nodiscard]] cch::util::ExpectedVoid start(
        cch::tui::TerminalInputSink,
        cch::tui::TerminalResizeSink) override {
        modes_.started = true;
        return {};
    }

    [[nodiscard]] cch::util::ExpectedVoid stop() override {
        return std::unexpected(cch::util::make_error(
            cch::util::ErrorCode::Process,
            "terminal restoration failed",
            "stop detail"));
    }

    [[nodiscard]] cch::tui::TerminalDimensions dimensions() const override { return {}; }
    [[nodiscard]] cch::tui::TerminalCapabilities capabilities() const override { return {}; }
    [[nodiscard]] cch::tui::TerminalModeState modes() const override { return modes_; }
    [[nodiscard]] cch::util::ExpectedVoid clear_screen() override { return {}; }
    [[nodiscard]] cch::util::ExpectedVoid write(std::string_view) override { return {}; }
    [[nodiscard]] cch::util::ExpectedVoid set_cursor(cch::tui::CursorPosition) override { return {}; }
    [[nodiscard]] cch::util::ExpectedVoid set_cursor_visible(bool visible) override {
        if (!visible) {
            return std::unexpected(cch::util::make_error(
                cch::util::ErrorCode::Process,
                "cursor hiding failed",
                "cursor detail"));
        }
        return {};
    }
    [[nodiscard]] cch::util::Expected<cch::tui::TerminalImageHandle> place_image(
        const cch::tui::TerminalImage&) override {
        return cch::tui::TerminalImageHandle{};
    }
    [[nodiscard]] cch::util::ExpectedVoid remove_image(
        cch::tui::TerminalImageHandle,
        const cch::tui::CellRegion&) override {
        return {};
    }
    [[nodiscard]] cch::util::ExpectedVoid begin_synchronized_update() override { return {}; }
    [[nodiscard]] cch::util::ExpectedVoid end_synchronized_update() override { return {}; }

private:
    cch::tui::TerminalModeState modes_;
};

class FocusableInputComponent final
    : public cch::tui::Component,
      public cch::tui::InputHandler,
      public cch::tui::Focusable {
public:
    [[nodiscard]] cch::util::Expected<cch::tui::RenderResult> render(std::size_t) override {
        return cch::tui::RenderResult{.lines = {"x"}};
    }

    void invalidate() override {
        ++invalidation_count;
    }

    void handle_input(const cch::tui::InputEventVariant& input) override {
        received_input.push_back(input);
    }

    [[nodiscard]] bool accepts_key_releases() const override {
        return accept_key_releases;
    }

    void set_focused(bool focused) override {
        focused_ = focused;
    }

    [[nodiscard]] bool focused() const override {
        return focused_;
    }

    std::size_t invalidation_count{0};
    std::vector<cch::tui::InputEventVariant> received_input;
    bool accept_key_releases{false};

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
    // Synchronized update markers wrap the rendered line on first render
    REQUIRE(terminal.output().size() == 3);
    CHECK(terminal.output()[0] == "\x1b[?2026h");
    CHECK(terminal.output()[1] == "hello   ");
    CHECK(terminal.output()[2] == "\x1b[?2026l");
    const cch::tui::CursorPosition expected_cursor{.column = 0, .row = 0};
    CHECK(terminal.cursor() == expected_cursor);
    CHECK(terminal.final_style() == cch::tui::VirtualTerminalStyle{});
    CHECK_FALSE(terminal.modes().cursor_visible);

    REQUIRE(tui.stop());
    CHECK_FALSE(terminal.modes().started);
    CHECK(terminal.modes().cursor_visible);
}

TEST_CASE("Tui clears and repaints unchanged retained content", "[tui][render][issue60]") {
    cch::tui::VirtualTerminal terminal({.columns = 8, .rows = 2});
    cch::tui::Tui tui(terminal);
    REQUIRE(tui.add_child(std::make_unique<cch::tui::Text>("hello", 0, 0)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());
    (void)terminal.check_clear_screen_called();

    REQUIRE(tui.clear_screen());
    CHECK(terminal.check_clear_screen_called());
    const std::vector<std::string> cleared_screen{"", ""};
    CHECK(terminal.screen() == cleared_screen);

    REQUIRE(tui.render());
    const std::vector<std::string> repainted_screen{"hello   ", ""};
    CHECK(terminal.screen() == repainted_screen);
    REQUIRE(tui.stop());
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

TEST_CASE(
    "Tui preserves cursor and restoration failures from partial startup",
    "[tui][terminal][issue58]") {
    PartialStartupFailureTerminal terminal;
    cch::tui::Tui tui(terminal);

    const auto result = tui.start();

    REQUIRE_FALSE(result);
    CHECK(result.error().message == "TUI startup failed and terminal restoration was incomplete");
    CHECK(result.error().detail.find("cursor hiding failed") != std::string::npos);
    CHECK(result.error().detail.find("terminal restoration failed") != std::string::npos);
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
    REQUIRE(result->lines.size() == 1);
    CHECK(cch::tui::detail::visible_width(result->lines[0]) >= 1);
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
    REQUIRE(component_pointer->received_input.size() == 1);
    CHECK(std::get<cch::tui::KeyEvent>(component_pointer->received_input[0]).key == "q");
    CHECK(component_pointer->invalidation_count == 1);
}

TEST_CASE("Tui resolves a lone Escape key when the terminal flushes ambiguity", "[tui][input][issue47]") {
    cch::tui::VirtualTerminal terminal;
    cch::tui::Tui tui(terminal);
    auto component = std::make_unique<FocusableInputComponent>();
    auto* component_pointer = component.get();
    REQUIRE(tui.add_child(std::move(component)));
    REQUIRE(tui.start());
    REQUIRE(tui.set_focus(component_pointer));

    REQUIRE(terminal.inject_input("\x1b"));
    CHECK(component_pointer->received_input.empty());
    REQUIRE(terminal.flush_input());

    REQUIRE(component_pointer->received_input.size() == 1);
    CHECK(std::get<cch::tui::KeyEvent>(component_pointer->received_input[0]).key == "escape");
}

TEST_CASE("Tui maps baseline terminal protocols to the same semantic key", "[tui][input][issue47]") {
    cch::tui::VirtualTerminal terminal;
    cch::tui::Tui tui(terminal);
    auto component = std::make_unique<FocusableInputComponent>();
    auto* component_pointer = component.get();
    REQUIRE(tui.add_child(std::move(component)));
    REQUIRE(tui.start());
    REQUIRE(tui.set_focus(component_pointer));

    REQUIRE(terminal.inject_input("\x03\x1b[99;5u\x1b[27;5;99~"));

    REQUIRE(component_pointer->received_input.size() == 3);
    const cch::tui::KeyEvent expected{
        .key = "c",
        .ctrl = true,
    };
    for (const auto& input : component_pointer->received_input) {
        CHECK(std::get<cch::tui::KeyEvent>(input) == expected);
    }
}

TEST_CASE("Tui decodes the supported legacy special key vocabulary", "[tui][input][issue47]") {
    struct RawKey {
        std::string_view raw;
        std::string_view identifier;
    };
    constexpr std::array<RawKey, 25> kKeys{{
        {"\t", "tab"}, {"\r", "enter"}, {" ", "space"}, {"\x7f", "backspace"},
        {"\x1b[A", "up"}, {"\x1b[B", "down"}, {"\x1b[C", "right"}, {"\x1b[D", "left"},
        {"\x1b[H", "home"}, {"\x1b[F", "end"}, {"\x1b[2~", "insert"}, {"\x1b[3~", "delete"},
        {"\x1b[5~", "pageUp"}, {"\x1b[6~", "pageDown"}, {"\x1b[E", "clear"},
        {"\x1bOP", "f1"}, {"\x1b[24~", "f12"}, {"\x1b[Z", "shift+tab"},
        {"\x1b[2^", "ctrl+insert"}, {"\x1b[e", "shift+clear"}, {"\x1bOe", "ctrl+clear"},
        {"\x1b" "B", "alt+left"}, {"\x1b" "F", "alt+right"},
        {"\x1b" "b", "alt+left"}, {"A", "shift+a"},
    }};

    cch::tui::VirtualTerminal terminal;
    cch::tui::Tui tui(terminal);
    auto component = std::make_unique<FocusableInputComponent>();
    auto* component_pointer = component.get();
    REQUIRE(tui.add_child(std::move(component)));
    REQUIRE(tui.start());
    REQUIRE(tui.set_focus(component_pointer));

    for (const auto& key : kKeys) REQUIRE(terminal.inject_input(std::string(key.raw)));

    REQUIRE(component_pointer->received_input.size() == kKeys.size());
    for (std::size_t index = 0; index < kKeys.size(); ++index) {
        CHECK(cch::tui::matches_key(
            std::get<cch::tui::KeyEvent>(component_pointer->received_input[index]),
            kKeys[index].identifier));
    }
}

TEST_CASE("Tui decodes Kitty alternate keys keypad and modified navigation", "[tui][input][issue47]") {
    cch::tui::VirtualTerminal terminal;
    cch::tui::Tui tui(terminal);
    auto component = std::make_unique<FocusableInputComponent>();
    component->accept_key_releases = true;
    auto* component_pointer = component.get();
    REQUIRE(tui.add_child(std::move(component)));
    REQUIRE(tui.start());
    REQUIRE(tui.set_focus(component_pointer));

    REQUIRE(terminal.inject_input(
        "\x1b[1089::99;5u"
        "\x1b[57400u"
        "\x1b[1;6:2D"
        "\x1b[3;3:3~"));

    REQUIRE(component_pointer->received_input.size() == 4);
    CHECK(cch::tui::matches_key(std::get<cch::tui::KeyEvent>(component_pointer->received_input[0]), "ctrl+c"));
    CHECK(cch::tui::matches_key(std::get<cch::tui::KeyEvent>(component_pointer->received_input[1]), "1"));
    const auto& repeated_left = std::get<cch::tui::KeyEvent>(component_pointer->received_input[2]);
    CHECK(cch::tui::matches_key(repeated_left, "shift+ctrl+left"));
    CHECK(repeated_left.type == cch::tui::KeyEventType::Repeat);
    const auto& released_delete = std::get<cch::tui::KeyEvent>(component_pointer->received_input[3]);
    CHECK(cch::tui::matches_key(released_delete, "alt+delete"));
    CHECK(released_delete.type == cch::tui::KeyEventType::Release);
}

TEST_CASE("Tui buffers split input and separates batched terminal sequences", "[tui][input][issue47]") {
    cch::tui::VirtualTerminal terminal;
    cch::tui::Tui tui(terminal);
    auto component = std::make_unique<FocusableInputComponent>();
    auto* component_pointer = component.get();
    REQUIRE(tui.add_child(std::move(component)));
    REQUIRE(tui.start());
    REQUIRE(tui.set_focus(component_pointer));

    REQUIRE(terminal.inject_input("\x1b"));
    REQUIRE(terminal.inject_input("[1;"));
    CHECK(component_pointer->received_input.empty());
    REQUIRE(terminal.inject_input("5C\x1b[A\x1b[B"));

    REQUIRE(component_pointer->received_input.size() == 3);
    const cch::tui::KeyEvent expected_right{.key = "right", .ctrl = true};
    CHECK(std::get<cch::tui::KeyEvent>(component_pointer->received_input[0]) == expected_right);
    CHECK(std::get<cch::tui::KeyEvent>(component_pointer->received_input[1]).key == "up");
    CHECK(std::get<cch::tui::KeyEvent>(component_pointer->received_input[2]).key == "down");
}

TEST_CASE("Tui preserves large batches without dropping semantic keys", "[tui][input][issue47]") {
    cch::tui::VirtualTerminal terminal;
    cch::tui::Tui tui(terminal);
    auto component = std::make_unique<FocusableInputComponent>();
    auto* component_pointer = component.get();
    REQUIRE(tui.add_child(std::move(component)));
    REQUIRE(tui.start());
    REQUIRE(tui.set_focus(component_pointer));

    REQUIRE(terminal.inject_input(std::string(5000, 'x')));

    REQUIRE(component_pointer->received_input.size() == 5000);
    CHECK(std::get<cch::tui::KeyEvent>(component_pointer->received_input.front()).key == "x");
    CHECK(std::get<cch::tui::KeyEvent>(component_pointer->received_input.back()).key == "x");
}

TEST_CASE("Tui filters key releases unless the focused Component opts in", "[tui][input][issue47]") {
    cch::tui::VirtualTerminal terminal;
    cch::tui::Tui tui(terminal);
    auto default_component = std::make_unique<FocusableInputComponent>();
    auto* default_pointer = default_component.get();
    REQUIRE(tui.add_child(std::move(default_component)));
    auto release_component = std::make_unique<FocusableInputComponent>();
    release_component->accept_key_releases = true;
    auto* release_pointer = release_component.get();
    REQUIRE(tui.add_child(std::move(release_component)));
    REQUIRE(tui.start());

    REQUIRE(tui.set_focus(default_pointer));
    REQUIRE(terminal.inject_input("\x1b[97u\x1b[97;1:3u"));
    REQUIRE(default_pointer->received_input.size() == 1);
    CHECK(std::get<cch::tui::KeyEvent>(default_pointer->received_input[0]).type ==
        cch::tui::KeyEventType::Press);

    REQUIRE(tui.set_focus(release_pointer));
    REQUIRE(terminal.inject_input("\x1b[97;1:3u"));
    REQUIRE(release_pointer->received_input.size() == 1);
    CHECK(std::get<cch::tui::KeyEvent>(release_pointer->received_input[0]).type ==
        cch::tui::KeyEventType::Release);
}

TEST_CASE("Tui delivers bracketed paste atomically with opaque control-looking content", "[tui][input][issue47]") {
    cch::tui::VirtualTerminal terminal;
    cch::tui::Tui tui(terminal);
    auto component = std::make_unique<FocusableInputComponent>();
    auto* component_pointer = component.get();
    REQUIRE(tui.add_child(std::move(component)));
    REQUIRE(tui.start());
    REQUIRE(tui.set_focus(component_pointer));

    REQUIRE(terminal.inject_input("a\x1b[20"));
    REQUIRE(terminal.inject_input("0~line 1\n\x1b[A\x1b[13;5u"));
    REQUIRE(terminal.inject_input("\x1b[20"));
    REQUIRE(terminal.inject_input("1~b"));

    REQUIRE(component_pointer->received_input.size() == 3);
    CHECK(std::get<cch::tui::KeyEvent>(component_pointer->received_input[0]).key == "a");
    const auto& paste = std::get<cch::tui::PasteEvent>(component_pointer->received_input[1]);
    CHECK(paste.text == "line 1\n\x1b[A\x1b[13;5u");
    CHECK(paste.original_bytes == paste.text.size());
    CHECK(paste.lines == 2);
    CHECK_FALSE(paste.truncated);
    CHECK(std::get<cch::tui::KeyEvent>(component_pointer->received_input[2]).key == "b");
}

TEST_CASE("Tui bounds large paste payloads and preserves deterministic metadata", "[tui][input][issue47]") {
    cch::tui::VirtualTerminal terminal;
    cch::tui::Tui tui(terminal);
    auto component = std::make_unique<FocusableInputComponent>();
    auto* component_pointer = component.get();
    REQUIRE(tui.add_child(std::move(component)));
    REQUIRE(tui.start());
    REQUIRE(tui.set_focus(component_pointer));

    std::string content(cch::tui::kMaxPasteBytes + 5, 'x');
    content[10] = '\n';
    REQUIRE(terminal.inject_input("\x1b[200~" + content + "\x1b[201~q"));

    REQUIRE(component_pointer->received_input.size() == 2);
    const auto& paste = std::get<cch::tui::PasteEvent>(component_pointer->received_input[0]);
    CHECK(paste.text.size() == cch::tui::kMaxPasteBytes);
    CHECK(paste.original_bytes == content.size());
    CHECK(paste.lines == 2);
    CHECK(paste.truncated);
    CHECK(std::get<cch::tui::KeyEvent>(component_pointer->received_input[1]).key == "q");
}

TEST_CASE("Tui abandons incomplete paste safely when the terminal flushes input", "[tui][input][issue47]") {
    cch::tui::VirtualTerminal terminal;
    cch::tui::Tui tui(terminal);
    auto component = std::make_unique<FocusableInputComponent>();
    auto* component_pointer = component.get();
    REQUIRE(tui.add_child(std::move(component)));
    REQUIRE(tui.start());
    REQUIRE(tui.set_focus(component_pointer));

    REQUIRE(terminal.inject_input("\x1b[200~unfinished\n\x1b[A"));
    CHECK(component_pointer->received_input.empty());
    REQUIRE(terminal.flush_input());
    REQUIRE(terminal.inject_input("q"));

    REQUIRE(component_pointer->received_input.size() == 1);
    CHECK(std::get<cch::tui::KeyEvent>(component_pointer->received_input[0]).key == "q");
}

TEST_CASE("Tui discards overlong terminal control payloads through their terminators", "[tui][input][issue47]") {
    cch::tui::VirtualTerminal terminal;
    cch::tui::Tui tui(terminal);
    auto component = std::make_unique<FocusableInputComponent>();
    auto* component_pointer = component.get();
    REQUIRE(tui.add_child(std::move(component)));
    REQUIRE(tui.start());
    REQUIRE(tui.set_focus(component_pointer));

    REQUIRE(terminal.inject_input("\x1b]" + std::string(300, 'x') + "payload"));
    CHECK(component_pointer->received_input.empty());
    REQUIRE(terminal.inject_input("\x1b\\q"));
    REQUIRE(terminal.inject_input("\x1bP" + std::string(300, 'y') + "payload"));
    REQUIRE(terminal.inject_input("\x1b\\r"));

    REQUIRE(component_pointer->received_input.size() == 2);
    CHECK(std::get<cch::tui::KeyEvent>(component_pointer->received_input[0]).key == "q");
    CHECK(std::get<cch::tui::KeyEvent>(component_pointer->received_input[1]).key == "r");
}

TEST_CASE("Tui recovers after invalid unsupported and overlong input", "[tui][input][issue47]") {
    cch::tui::VirtualTerminal terminal;
    cch::tui::Tui tui(terminal);
    auto component = std::make_unique<FocusableInputComponent>();
    auto* component_pointer = component.get();
    REQUIRE(tui.add_child(std::move(component)));
    REQUIRE(tui.start());
    REQUIRE(tui.set_focus(component_pointer));

    REQUIRE(terminal.inject_input("\x1b[999999999999999999999;5u\x1b[99;9u"));
    REQUIRE(terminal.inject_input("\x1b[" + std::string(300, '1')));
    REQUIRE(terminal.inject_input("\x1b[A"));
    REQUIRE(terminal.inject_input("\xc3"));
    REQUIRE(terminal.flush_input());
    REQUIRE(terminal.inject_input("q"));

    REQUIRE(component_pointer->received_input.size() == 2);
    CHECK(std::get<cch::tui::KeyEvent>(component_pointer->received_input[0]).key == "up");
    CHECK(std::get<cch::tui::KeyEvent>(component_pointer->received_input[1]).key == "q");
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
