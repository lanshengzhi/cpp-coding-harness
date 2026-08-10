#include <cch/tui/Input.hpp>
#include <cch/tui/Tui.hpp>
#include <cch/tui/Utils.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include "../../third_party/catch2/catch_test_macros.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void key(cch::tui::Input& input, std::string name, bool ctrl = false, bool shift = false, bool alt = false) {
    input.handle_input(cch::tui::KeyEvent{
        .key = std::move(name),
        .ctrl = ctrl,
        .shift = shift,
        .alt = alt,
    });
}

void type(cch::tui::Input& input, std::string text) {
    key(input, std::move(text));
}

} // namespace

TEST_CASE("Input types printable text and routes submit through the sink", "[tui][input][issue380]") {
    std::vector<std::string> submitted;
    cch::tui::Input input({}, [&submitted](std::string value) { submitted.push_back(std::move(value)); });

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

TEST_CASE("Input routes escape and ctrl+c through the escape sink", "[tui][input][issue380]") {
    int escapes = 0;
    cch::tui::Input input({}, {}, [&escapes] { ++escapes; });

    key(input, "escape");
    key(input, "c", true);
    CHECK(escapes == 2);

    // Printable characters do not fire the escape sink.
    type(input, "x");
    CHECK(escapes == 2);
}

TEST_CASE("Input edits Unicode with grapheme-aware cursor movement", "[tui][input][issue380]") {
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

TEST_CASE("Input undo coalesces typed words and steps back through snapshots", "[tui][input][issue380]") {
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

TEST_CASE("Input kill ring accumulates consecutive kills with pi ordering", "[tui][input][issue380]") {
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

TEST_CASE("Input yank-pop cycles the kill ring after a yank", "[tui][input][issue380]") {
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

TEST_CASE("Input moves and deletes by word", "[tui][input][issue380]") {
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

TEST_CASE("Input inserts bracketed-paste text cleanly at the cursor", "[tui][input][issue380]") {
    cch::tui::Input input;
    type(input, "ab");
    key(input, "left");
    input.handle_input(cch::tui::PasteEvent{.text = "X\r\nY\tZ\r"});
    CHECK(input.value() == "aXY    Zb");

    // Undo restores the pre-paste state in one step.
    key(input, "-", true);
    CHECK(input.value() == "ab");

    // Control characters are dropped from pastes; the rest of the sequence
    // stays literal (decoded-event hygiene, matching Editor::paste). The undo
    // also restored the cursor to its pre-paste position.
    input.handle_input(cch::tui::PasteEvent{.text = "\x1b[31mred\x1b[0m"});
    CHECK(input.value() == "a[31mred[0mb");
}

TEST_CASE("Input renders with horizontal scrolling around the cursor", "[tui][input][issue380]") {
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

TEST_CASE("Input renders a reverse-video fake cursor at the cursor position", "[tui][input][cursor]") {
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

TEST_CASE("Input cursor location follows the focus lifecycle and windowing", "[tui][input][issue380]") {
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

TEST_CASE("Input set_value replaces the value and clamps the cursor", "[tui][input][issue380]") {
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

TEST_CASE("Input surfaces sink exceptions and degenerate widths", "[tui][input][issue380]") {
    cch::tui::Input throwing({}, [](std::string) { throw std::runtime_error("boom"); });
    type(throwing, "x");
    key(throwing, "enter");
    auto rendered = throwing.render(10);
    CHECK_FALSE(rendered.has_value());

    cch::tui::Input input;
    rendered = input.render(0);
    CHECK_FALSE(rendered.has_value());
    rendered = input.render(1);
    REQUIRE(rendered);
    CHECK(rendered->lines[0] == ">");
}

TEST_CASE("Input integrates with Tui through the VirtualTerminal seam", "[tui][input][issue380]") {
    cch::tui::VirtualTerminal terminal({.columns = 12, .rows = 3});
    cch::tui::Tui tui(terminal);
    std::vector<std::string> submitted;
    auto input = std::make_unique<cch::tui::Input>(
        cch::tui::InputOptions{},
        [&submitted](std::string value) { submitted.push_back(std::move(value)); });
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
