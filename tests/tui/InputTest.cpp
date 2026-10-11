#include <cch/tui/Input.hpp>
#include <cch/tui/Tui.hpp>
#include <cch/tui/Utils.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include "support/PiTuiEvidence.hpp"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void key(cch::tui::Input& input, std::string name, bool ctrl = false, bool shift = false, bool alt = false) {
    static_cast<void>(input.handle_input(cch::tui::KeyEvent{
            .key = std::move(name),
            .ctrl = ctrl,
            .shift = shift,
            .alt = alt,
    }));
}

void type(cch::tui::Input& input, std::string text) {
    key(input, std::move(text));
}

} // namespace

TEST_CASE("Input types printable text and routes submit through the sink", "[tui][input][issue380][spec]") {
    std::vector<std::string> submitted;
    cch::tui::Input input(
        {},
        [&submitted](std::string value) -> cch::support::ExpectedVoid {
            submitted.push_back(std::move(value));
            return {};
        });

    type(input, "hello");
    key(input, "space");
    type(input, "world");
    CHECK(input.value() == "hello world");

    // Enter submits the current value without clearing it (pi's onSubmit).
    key(input, "enter");
    REQUIRE(submitted.size() == 1);
    CHECK(submitted[0] == "hello world");
    CHECK(input.value() == "hello world");

    // Non-printable keys never insert.
    key(input, "tab");
    key(input, "enter");
    CHECK(input.value() == "hello world");
    REQUIRE(submitted.size() == 2);
}

TEST_CASE("Input routes escape and ctrl+c through the escape sink", "[tui][input][issue380][spec]") {
    int escapes = 0;
    cch::tui::Input input({}, {}, [&escapes]() -> cch::support::ExpectedVoid { ++escapes; return {}; });

    key(input, "escape");
    key(input, "c", true);
    CHECK(escapes == 2);

    // Printable characters do not fire the escape sink.
    type(input, "x");
    CHECK(escapes == 2);
}

TEST_CASE("Input resolves a key shared by two actions in dispatch order", "[tui][input][spec]") {
    // f9 claims cancel and char-deletion; cancel leads pi's input.ts chain.
    auto registry = std::make_shared<const cch::tui::KeybindingRegistry>(std::vector<cch::tui::EffectiveKeybinding>{
            {.id = "tui.select.cancel", .keys = {"f9"}},
            {.id = "tui.editor.deleteCharBackward", .keys = {"f9"}},
            {.id = "tui.input.submit", .keys = {"enter"}},
    });
    int escapes = 0;
    std::vector<std::string> submitted;
    cch::tui::Input input(
            {.keybindings = std::move(registry)},
            [&submitted](std::string value) -> cch::support::ExpectedVoid {
                submitted.push_back(std::move(value));
                return {};
            },
            [&escapes]() -> cch::support::ExpectedVoid {
                ++escapes;
                return {};
            });
    type(input, "ab");
    key(input, "f9");
    CHECK(escapes == 1);
    CHECK(input.value() == "ab");
    key(input, "enter");
    REQUIRE(submitted.size() == 1);
    CHECK(submitted[0] == "ab");
}

TEST_CASE("Input edits Unicode with grapheme-aware cursor movement", "[tui][input][issue380][spec]") {
    cch::tui::Input input;
    type(input, "A\xc3\xa9\xf0\x9f\x98\x80");
    CHECK(input.value() == "A\xc3\xa9\xf0\x9f\x98\x80");

    key(input, "left");
    key(input, "delete");
    CHECK(input.value() == "A\xc3\xa9");

    key(input, "backspace");
    CHECK(input.value() == "A");
    key(input, "left");
    key(input, "delete");
    CHECK(input.value().empty());
}

TEST_CASE("Input matches ICU word navigation and keeps deletion undo atomic", "[tui][input][issue961]") {
    cch::tui::Input input;
    type(input, "don't stop 日本語 123,456.78 cafe\xcc\x81! 😀done");
    key(input, "home");
    key(input, "right", true);
    CHECK(input.value().substr(0, 3) == "don");
    key(input, "right", true);
    CHECK(input.value().substr(0, 4) == "don'");
    key(input, "right", true);
    CHECK(input.value().substr(0, 5) == "don't");
    key(input, "w", true);
    CHECK(input.value() == "don' stop 日本語 123,456.78 cafe\xcc\x81! 😀done");
    key(input, "-", true);
    CHECK(input.value() == "don't stop 日本語 123,456.78 cafe\xcc\x81! 😀done");

    key(input, "home");
    key(input, "w", true);
    CHECK(input.value() == "don't stop 日本語 123,456.78 cafe\xcc\x81! 😀done");
}

TEST_CASE("Input undo coalesces typed words and steps back through snapshots", "[tui][input][issue380][spec]") {
    cch::tui::Input input;
    type(input, "abc");
    type(input, " ");
    type(input, "def");
    CHECK(input.value() == "abc def");

    key(input, "-", true);  // undo: removes the coalesced "def" and its space
    CHECK(input.value() == "abc");
    key(input, "-", true);  // undo: removes the coalesced "abc"
    CHECK(input.value() == "");
    key(input, "-", true);  // nothing left to undo
    CHECK(input.value() == "");

    // Backspace is its own undo unit.
    type(input, "ab");
    key(input, "backspace");
    key(input, "-", true);
    CHECK(input.value() == "ab");
}

TEST_CASE("Input kill ring accumulates consecutive kills with pi ordering", "[tui][input][issue380][spec]") {
    cch::tui::Input input;

    // Backward kills accumulate prepended into one entry.
    type(input, "one two three");
    key(input, "w", true);  // deleteWordBackward: "three"
    key(input, "w", true);  // accumulates prepended: "two three"
    CHECK(input.value() == "one ");
    key(input, "y", true);  // yank restores both words in order
    CHECK(input.value() == "one two three");

    // Forward kills accumulate appended into one entry.
    key(input, "u", true);  // deleteToLineStart: "one two three"
    CHECK(input.value() == "");
    type(input, "one two three four");
    key(input, "home");
    key(input, "f", false, false, true);  // cursorWordRight: after "one"
    key(input, "d", false, false, true);  // deleteWordForward (alt+d): "two "
    key(input, "d", false, false, true);  // accumulates appended: "two three "
    CHECK(input.value() == "one four");
    key(input, "y", true);
    CHECK(input.value() == "one two three four");

    // Typing between kills breaks accumulation: the final kill stays its own
    // entry, so yank returns just it.
    key(input, "u", true);  // deleteToLineStart kills the prefix up to the cursor
    CHECK(input.value() == " four");
    type(input, "x");
    key(input, "u", true);  // kills "x" (accumulation broken by typing)
    CHECK(input.value() == " four");
    key(input, "y", true);
    CHECK(input.value() == "x four");
}

TEST_CASE("Input yank-pop cycles the kill ring after a yank", "[tui][input][issue380][spec]") {
    cch::tui::Input input;
    type(input, "first second");
    key(input, "home");
    key(input, "k", true);  // kill "first second"
    CHECK(input.value() == "");
    type(input, "third");
    key(input, "home");
    key(input, "k", true);  // kill "third"
    CHECK(input.value() == "");

    key(input, "y", true);  // yank most recent: "third"
    CHECK(input.value() == "third");
    key(input, "y", false, false, true);  // alt+y yank-pop rotates to "first second"
    CHECK(input.value() == "first second");
    key(input, "y", false, false, true);  // and back
    CHECK(input.value() == "third");

    // yank-pop before any yank is a no-op.
    cch::tui::Input fresh;
    type(fresh, "ab");
    key(fresh, "y", true);
    CHECK(fresh.value() == "ab");
}

TEST_CASE("Input moves and deletes by word", "[tui][input][issue380][spec]") {
    cch::tui::Input input;
    type(input, "one two three");
    key(input, "w", true);  // deleteWordBackward kills "three"
    CHECK(input.value() == "one two ");

    key(input, "b", false, false, true);  // cursorWordLeft: before "two"
    key(input, "w", true);  // deleteWordBackward kills the word before the cursor
    CHECK(input.value() == "two ");

    // Forward deletion from the start skips whitespace and the word run.
    cch::tui::Input forward;
    type(forward, "one two three");
    key(forward, "home");
    key(forward, "f", false, false, true);  // cursorWordRight: after "one"
    key(forward, "d", false, false, true);  // deleteWordForward removes "two "
    CHECK(forward.value() == "one three");
}

TEST_CASE("Input inserts bracketed-paste text cleanly at the cursor", "[tui][input][issue380][spec]") {
    cch::tui::Input input;
    type(input, "ab");
    key(input, "left");
    static_cast<void>(input.handle_input(cch::tui::PasteEvent{.text = "X\r\nY\tZ\r"}));
    CHECK(input.value() == "aXY    Zb");

    // Undo restores the pre-paste state in one step.
    key(input, "-", true);
    CHECK(input.value() == "ab");

    // Control characters are dropped from pastes; the rest of the sequence
    // stays literal (decoded-event hygiene, matching Editor::paste). The undo
    // also restored the cursor to its pre-paste position.
    static_cast<void>(input.handle_input(cch::tui::PasteEvent{.text = "\x1b[31mred\x1b[0m"}));
    CHECK(input.value() == "a[31mred[0mb");
}

TEST_CASE("Input renders with horizontal scrolling around the cursor", "[tui][input][issue380][spec]") {
    cch::tui::Input input;
    type(input, "abcdefghijklmnop");

    // Cursor at the end: the window shows the tail and reserves one column
    // for the cursor (pi's scrollWidth = availableWidth - 1).
    auto rendered = input.render(10);
    REQUIRE(rendered);
    REQUIRE(rendered->lines.size() == 1);
    CHECK(rendered->lines[0] == "> jklmnop\x1b[7m \x1b[27m");

    // Cursor in the middle: the window centers on the cursor.
    for (int index = 0; index < 8; ++index) key(input, "left");
    rendered = input.render(10);
    REQUIRE(rendered);
    CHECK(rendered->lines[0] == "> efgh\x1b[7mi\x1b[27mjkl");

    // Cursor near the start: the window shows the head.
    key(input, "home");
    rendered = input.render(10);
    REQUIRE(rendered);
    CHECK(rendered->lines[0] == "> \x1b[7ma\x1b[27mbcdefgh");

    // A short value fits with the cursor at the end.
    cch::tui::Input short_input;
    type(short_input, "abc");
    rendered = short_input.render(10);
    REQUIRE(rendered);
    CHECK(rendered->lines[0] == "> abc\x1b[7m \x1b[27m    ");

    // A value exactly filling the width scrolls one column for the cursor.
    cch::tui::Input exact;
    type(exact, "abcdefgh");
    rendered = exact.render(10);
    REQUIRE(rendered);
    CHECK(rendered->lines[0] == "> bcdefgh\x1b[7m \x1b[27m");
}

TEST_CASE("Input renders a reverse-video fake cursor at the cursor position", "[tui][input][cursor][spec]") {
    // pi's input.ts renders the cursor as reverse video on the grapheme at the
    // cursor (or a highlighted space at end of line); the C++ port must match.
    cch::tui::Input input;
    type(input, "hi");

    // Cursor at end of line: a highlighted space occupies the cell after it.
    auto rendered = input.render(10);
    REQUIRE(rendered);
    REQUIRE(rendered->lines.size() == 1);
    CHECK(rendered->lines[0] == "> hi\x1b[7m \x1b[27m     ");

    key(input, "home");
    rendered = input.render(10);
    REQUIRE(rendered);
    CHECK(rendered->lines[0] == "> \x1b[7mh\x1b[27mi      ");

    key(input, "right");
    rendered = input.render(10);
    REQUIRE(rendered);
    CHECK(rendered->lines[0] == "> h\x1b[7mi\x1b[27m      ");

    // The rendered line never exceeds the component width.
    for (const auto& line : rendered->lines) {
        CHECK(cch::tui::visible_width(line) <= 10);
    }
}

TEST_CASE("Input renders a placeholder reversing the first character only while the value is empty",
        "[tui][input][issue586][issue962][spec]") {
    // Empty value: reverses the first grapheme of the placeholder, followed by the rest,
    // padded to the component width.
    cch::tui::Input input({.placeholder = "Type here"});

    auto rendered = input.render(20);
    REQUIRE(rendered);
    REQUIRE(rendered->lines.size() == 1);
    CHECK(rendered->lines[0] == "> \x1b[7mT\x1b[27mype here         ");
    CHECK(cch::tui::visible_width(rendered->lines[0]) == 20);

    // Styled placeholder: custom styling applies to each character run.
    cch::tui::Input styled({
            .placeholder = "Type here",
            .placeholder_style = [](std::string_view text) { return "\x1b[2m" + std::string(text) + "\x1b[22m"; },
    });
    rendered = styled.render(20);
    REQUIRE(rendered);
    CHECK(rendered->lines[0] == "> \x1b[7m\x1b[2mT\x1b[22m\x1b[27m\x1b[2mype here\x1b[22m         ");
    CHECK(cch::tui::visible_width(rendered->lines[0]) == 20);

    // A non-empty value hides the placeholder entirely.
    type(input, "a");
    rendered = input.render(20);
    REQUIRE(rendered);
    CHECK(rendered->lines[0] == "> a\x1b[7m \x1b[27m                ");

    // The placeholder truncates to the columns left after prompt
    cch::tui::Input narrow({.placeholder = "Type here"});
    rendered = narrow.render(7);
    REQUIRE(rendered);
    CHECK(rendered->lines[0] == "> \x1b[7mT\x1b[27mype \x1b[0m");
    CHECK(cch::tui::visible_width(rendered->lines[0]) == 7);

    // No placeholder keeps the historical empty-value rendering.
    cch::tui::Input plain;
    rendered = plain.render(20);
    REQUIRE(rendered);
    CHECK(rendered->lines[0] == "> \x1b[7m \x1b[27m                 ");
}

TEST_CASE("Input aligns placeholder styling, LF submission, and scrolling with frozen pi", "[tui][input][issue962]") {
    const auto fixture = cch::tests::read_pi_tui_evidence("input.json");
    REQUIRE(fixture);

    const auto find_scenario = [&](std::string_view name) -> const cch::support::JsonValue* {
        for (const auto& scenario : fixture->at("scenarios").get_array()) {
            if (scenario.at("name").get_string() == name) return &scenario;
        }
        return nullptr;
    };

    // Scenario 1: input-placeholder-reversal-and-custom-styling
    {
        const auto* scenario = find_scenario("input-placeholder-reversal-and-custom-styling");
        REQUIRE(scenario != nullptr);
        const auto& expected = scenario->at("expected");

        cch::tui::Input input1a({
                .prompt = "> ",
                .placeholder = "Search...",
        });
        input1a.set_focused(true);
        auto rendered1a = input1a.render(16);
        REQUIRE(rendered1a);
        CHECK(rendered1a->lines[0] == expected.at("defaultEmpty").at("line").get_string());
        CHECK(cch::tui::visible_width(rendered1a->lines[0]) ==
                static_cast<std::size_t>(expected.at("defaultEmpty").at("visibleWidth").get_number()));
        CHECK(input1a.cursor_location() ==
                cch::tui::CursorPosition{
                        .column = static_cast<std::size_t>(expected.at("defaultEmpty").at("cursorColumn").get_number()),
                        .row = 0,
                });

        cch::tui::Input input1b({
                .prompt = "Find: ",
                .placeholder = "Filter items",
                .placeholder_style = [](std::string_view text) { return "\x1b[2m" + std::string(text) + "\x1b[22m"; },
        });
        input1b.set_focused(true);
        auto rendered1b = input1b.render(20);
        REQUIRE(rendered1b);
        CHECK(rendered1b->lines[0] == expected.at("styledEmpty").at("line").get_string());
        CHECK(cch::tui::visible_width(rendered1b->lines[0]) ==
                static_cast<std::size_t>(expected.at("styledEmpty").at("visibleWidth").get_number()));
        CHECK(input1b.cursor_location() ==
                cch::tui::CursorPosition{
                        .column = static_cast<std::size_t>(expected.at("styledEmpty").at("cursorColumn").get_number()),
                        .row = 0,
                });

        cch::tui::Input input1c({
                .prompt = "> ",
                .placeholder = "Search...",
        });
        input1c.set_focused(true);
        type(input1c, "abc");
        auto rendered1c = input1c.render(16);
        REQUIRE(rendered1c);
        CHECK(rendered1c->lines[0] == expected.at("populated").at("line").get_string());
        CHECK(cch::tui::visible_width(rendered1c->lines[0]) ==
                static_cast<std::size_t>(expected.at("populated").at("visibleWidth").get_number()));
        CHECK(input1c.cursor_location() ==
                cch::tui::CursorPosition{
                        .column = static_cast<std::size_t>(expected.at("populated").at("cursorColumn").get_number()),
                        .row = 0,
                });
    }

    // Scenario 2: input-raw-lf-submission-and-rebound-submit
    {
        const auto* scenario = find_scenario("input-raw-lf-submission-and-rebound-submit");
        REQUIRE(scenario != nullptr);
        const auto& expected = scenario->at("expected");

        // Rebound submit to ctrl+x
        auto resolution = cch::tui::resolve_keybindings({
                .definitions = cch::tui::builtin_tui_keybinding_definitions(),
                .overrides = {{.id = "tui.input.submit", .keys = {"ctrl+x"}}},
        });
        REQUIRE(resolution);
        auto keybindings = resolution->registry;

        std::vector<std::string> submitted2a;
        cch::tui::Input input2a(cch::tui::InputOptions{.keybindings = keybindings},
                [&submitted2a](std::string value) -> cch::support::ExpectedVoid {
                    submitted2a.push_back(std::move(value));
                    return {};
                });

        type(input2a, "hello");
        // Enter (\r) does NOT submit when rebound
        input2a.handle_input(cch::tui::KeyEvent{.key = "enter", .raw_sequence = "\r"});
        CHECK(submitted2a.empty());

        // Raw LF (\n) DOES submit even when submit action is rebound
        input2a.handle_input(cch::tui::KeyEvent{.key = "enter", .raw_sequence = "\n"});
        REQUIRE(submitted2a.size() == 1);
        CHECK(submitted2a[0] == expected.at("reboundLfSubmitted").get_array()[0].get_string());

        // Kitty protocol: \n decodes to shift+enter with raw_sequence "\n"
        std::vector<std::string> submitted2b;
        cch::tui::Input input2b(
                cch::tui::InputOptions{}, [&submitted2b](std::string value) -> cch::support::ExpectedVoid {
                    submitted2b.push_back(std::move(value));
                    return {};
                });
        type(input2b, "kitty");
        input2b.handle_input(cch::tui::KeyEvent{.key = "enter", .shift = true, .raw_sequence = "\n"});
        REQUIRE(submitted2b.size() == 1);
        CHECK(submitted2b[0] == expected.at("kittyLfSubmitted").get_array()[0].get_string());

        // Multiline bracketed paste normalizes without submitting
        std::vector<std::string> submitted2c;
        cch::tui::Input input2c(
                cch::tui::InputOptions{}, [&submitted2c](std::string value) -> cch::support::ExpectedVoid {
                    submitted2c.push_back(std::move(value));
                    return {};
                });
        input2c.handle_input(cch::tui::PasteEvent{
                .text = "first\r\nsecond\nthird\tval",
        });
        CHECK(input2c.value() == expected.at("pastedValue").get_string());
        CHECK(submitted2c.empty());
    }

    // Scenario 3: input-narrow-unicode-scroll-and-cursor
    {
        const auto* scenario = find_scenario("input-narrow-unicode-scroll-and-cursor");
        REQUIRE(scenario != nullptr);
        const auto& expected = scenario->at("expected");

        cch::tui::Input input3({.prompt = "> "});
        input3.set_value("éclair 世界 foo");
        input3.set_focused(true);

        // Home
        key(input3, "home");
        auto render_home = input3.render(12);
        REQUIRE(render_home);
        CHECK(render_home->lines[0] == expected.at("stepHome").at("line").get_string());
        CHECK(input3.cursor_location() ==
                cch::tui::CursorPosition{
                        .column = static_cast<std::size_t>(expected.at("stepHome").at("cursorColumn").get_number()),
                        .row = 0,
                });

        // Step right 6 times
        for (int i = 0; i < 6; ++i)
            key(input3, "right");
        auto render_right6 = input3.render(12);
        REQUIRE(render_right6);
        CHECK(render_right6->lines[0] == expected.at("stepRight6").at("line").get_string());
        CHECK(input3.cursor_location() ==
                cch::tui::CursorPosition{
                        .column = static_cast<std::size_t>(expected.at("stepRight6").at("cursorColumn").get_number()),
                        .row = 0,
                });

        // End
        key(input3, "end");
        auto render_end = input3.render(12);
        REQUIRE(render_end);
        CHECK(render_end->lines[0] == expected.at("stepEnd").at("line").get_string());
        CHECK(input3.cursor_location() ==
                cch::tui::CursorPosition{
                        .column = static_cast<std::size_t>(expected.at("stepEnd").at("cursorColumn").get_number()),
                        .row = 0,
                });
    }
}

TEST_CASE("Input cursor location follows the focus lifecycle and windowing", "[tui][input][issue380][spec]") {
    cch::tui::Input input;
    type(input, "hi");
    CHECK_FALSE(input.cursor_location().has_value());

    input.set_focused(true);
    CHECK_FALSE(input.cursor_location().has_value());  // no render yet

    REQUIRE(input.render(10));
    REQUIRE(input.cursor_location().has_value());
    CHECK((*input.cursor_location() == cch::tui::CursorPosition{.column = 4, .row = 0}));

    input.set_focused(false);
    CHECK_FALSE(input.cursor_location().has_value());

    // Scrolled window: the column tracks the cursor inside the window.
    input.set_focused(true);
    type(input, "x");  // "hix" at width 10 still fits
    key(input, "left");
    REQUIRE(input.render(4));
    REQUIRE(input.cursor_location().has_value());
    CHECK((*input.cursor_location() == cch::tui::CursorPosition{.column = 3, .row = 0}));
}

TEST_CASE("Input set_value replaces the value and clamps the cursor", "[tui][input][issue380][spec]") {
    cch::tui::Input input;
    type(input, "abcdef");
    for (int index = 0; index < 2; ++index) key(input, "left");
    input.set_value("hi");
    CHECK(input.value() == "hi");
    input.set_focused(true);
    REQUIRE(input.render(10));
    REQUIRE(input.cursor_location().has_value());
    CHECK((*input.cursor_location() == cch::tui::CursorPosition{.column = 4, .row = 0}));
}

TEST_CASE("Input surfaces sink failures and degenerate widths", "[tui][input][issue380][issue485][spec]") {
    cch::tui::Input failing({}, [](std::string) -> cch::support::ExpectedVoid {
        return std::unexpected(cch::support::make_error(
            cch::support::ErrorCode::Unknown, "boom"));
    });
    type(failing, "x");
    key(failing, "enter");
    auto rendered = failing.render(10);
    CHECK_FALSE(rendered.has_value());

    cch::tui::Input input;
    rendered = input.render(0);
    CHECK_FALSE(rendered.has_value());
    rendered = input.render(1);
    REQUIRE(rendered);
    CHECK(rendered->lines[0] == ">\x1b[0m");
}

TEST_CASE("Input integrates with Tui through the VirtualTerminal seam", "[tui][input][issue380][spec]") {
    cch::tui::VirtualTerminal terminal({.columns = 12, .rows = 3});
    cch::tui::Tui tui(terminal);
    std::vector<std::string> submitted;
    auto input = std::make_unique<cch::tui::Input>(
        cch::tui::InputOptions{},
        [&submitted](std::string value) -> cch::support::ExpectedVoid {
            submitted.push_back(std::move(value));
            return {};
        });
    auto* input_ptr = input.get();
    REQUIRE(tui.add_child(std::move(input)));
    REQUIRE(tui.start());
    REQUIRE(tui.set_focus(input_ptr));

    REQUIRE(terminal.inject_input("hello"));
    REQUIRE(tui.render());
    CHECK(input_ptr->value() == "hello");
    CHECK(terminal.screen()[0] == "> hello     ");
    CHECK(terminal.screen()[1].empty());
    CHECK((terminal.cursor() == cch::tui::CursorPosition{.column = 7, .row = 0}));

    // Enter through the decoded pipeline submits.
    REQUIRE(terminal.inject_input("\r"));
    REQUIRE(submitted.size() == 1);
    CHECK(submitted[0] == "hello");

    // Bracketed paste through the decoded pipeline frames one clean insert.
    REQUIRE(terminal.inject_input("\x1b[200~pa\x1b[201~"));
    CHECK(input_ptr->value() == "hellopa");
}

TEST_CASE("Input outcome-based admission proves boundary no-ops and side-effect freedom", "[tui][input][spec]") {
    cch::tui::Input input;

    // 1. Recognized boundary no-op: cursorLeft at start of input is consumed.
    input.set_value("");
    CHECK(input.handle_input(cch::tui::KeyEvent{.key = "left"}) == cch::tui::InputAdmissionOutcome::Consumed);
    CHECK(input.value().empty());

    // 2. Recognized boundary no-op: undo on empty undo stack is consumed.
    CHECK(input.handle_input(cch::tui::KeyEvent{.key = "-", .ctrl = true}) ==
            cch::tui::InputAdmissionOutcome::Consumed);

    // 3. Unhandled side-effect freedom: key releases are unhandled and modify no state.
    input.set_value("hello");
    CHECK(input.handle_input(cch::tui::KeyEvent{.key = "a", .type = cch::tui::KeyEventType::Release}) ==
            cch::tui::InputAdmissionOutcome::Unhandled);
    CHECK(input.value() == "hello");

    // 4. Unhandled side-effect freedom: unrecognized non-printable keys are unhandled and modify no state.
    CHECK(input.handle_input(cch::tui::KeyEvent{.key = "insert"}) == cch::tui::InputAdmissionOutcome::Unhandled);
    CHECK(input.value() == "hello");
}
