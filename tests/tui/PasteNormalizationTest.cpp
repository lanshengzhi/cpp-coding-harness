// Paste normalization acceptance for ticket #953 (spec #946, E03): the public
// `normalize_pasted_text` contract plus the real Editor and Input components
// driven through the VirtualTerminal/decoder paste seam.
//
// Frozen authority: pi-v1.0.4 at 7c10bd4337495ee613f2224843ecdf349b80d1df,
// `Editor.handlePaste` / `Input.handlePaste`. Every case below is written so a
// cheap stand-in fails: dropping ESC bytes only, appending raw paste bytes, or
// splitting the normalized text into separate edits all produce different
// observable text or undo depth here.

#include <cch/tui/Editor.hpp>
#include <cch/tui/Input.hpp>
#include <cch/tui/Tui.hpp>
#include <cch/tui/Utils.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <utility>

namespace {

void key(cch::tui::Editor& editor, std::string name, bool ctrl = false) {
    // The default `tui.editor.undo` binding is ctrl+-.
    static_cast<void>(editor.handle_input(cch::tui::KeyEvent{.key = std::move(name), .ctrl = ctrl}));
}

void key(cch::tui::Input& input, std::string name, bool ctrl = false) {
    static_cast<void>(input.handle_input(cch::tui::KeyEvent{.key = std::move(name), .ctrl = ctrl}));
}

} // namespace

TEST_CASE("Pasted control replies resolve to the character they encode", "[tui][paste][issue953][spec]") {
    // ESC [ 106 ; 5 u is Ctrl+J: the whole reply is consumed and the newline it
    // encodes survives. A stand-in that merely drops ESC bytes would leave the
    // printable "[106;5u" tail in the buffer.
    CHECK(cch::tui::normalize_pasted_text("\x1b[106;5ub", {.multiline = true}) == "\nb");
    CHECK(cch::tui::normalize_pasted_text("\x1b[106;5ub", {.multiline = false}) == "b");

    // Shifted base codepoints still resolve: Ctrl+I is TAB and expands to four
    // columns, Ctrl+H is a dropped backspace control.
    CHECK(cch::tui::normalize_pasted_text("\x1b[105;5u\t", {.multiline = true}) == "        ");
    CHECK(cch::tui::normalize_pasted_text("x\x1b[104;5uy", {.multiline = true}) == "xy");

    // Release events carry no insertable character but are still consumed.
    CHECK(cch::tui::normalize_pasted_text("\x1b[106;5:3uq", {.multiline = true}) == "q");

    // An unterminated reply at the end of the buffer is not swallowed as a
    // complete control reply: only ESC is filtered, the rest stays visible.
    CHECK(cch::tui::normalize_pasted_text("a\x1b[106;5", {.multiline = true}) == "a[106;5");
}

TEST_CASE("Pasted newlines, tabs and control bytes normalize to the frozen text", "[tui][paste][issue953][spec]") {
    // CR, CRLF and lone LF all become one LF in a multiline target; TAB becomes
    // four spaces; C0/C1/DEL bytes are filtered out.
    CHECK(cch::tui::normalize_pasted_text("a\r\nb\rc\td\x01"
                                          "e",
                  {.multiline = true}) == "a\nb\nc    de");
    CHECK(cch::tui::normalize_pasted_text("a\r\nb\rc\td\x01"
                                          "e",
                  {.multiline = false}) == "abc    de");

    // Non-CSI-u terminal sequences keep the decoded-event hygiene contract:
    // the ESC control byte is filtered and the remaining bytes stay literal.
    CHECK(cch::tui::normalize_pasted_text("a\r\nb\rc\td\x1b"
                                          "[1be",
                  {.multiline = true}) == "a\nb\nc    d[1be");
    CHECK(cch::tui::normalize_pasted_text("\x1b[31mred\x1b[0m", {.multiline = true}) == "[31mred[0m");

    // Multi-byte content is preserved verbatim and not split by the scan.
    CHECK(cch::tui::normalize_pasted_text("\xe4\xb8\xad\xe6\x96\x87\t\xe4\xb8\xad", {.multiline = true}) ==
            "\xe4\xb8\xad\xe6\x96\x87    \xe4\xb8\xad");
}

TEST_CASE("A pasted file path gains spacing against adjacent words", "[tui][paste][issue953][spec]") {
    CHECK(cch::tui::normalize_pasted_text("/tmp/x", {.multiline = true, .boundaries = {.text_before = "foo"}}) ==
            " /tmp/x");
    CHECK(cch::tui::normalize_pasted_text("/tmp/x", {.multiline = true, .boundaries = {.text_after = "bar"}}) ==
            "/tmp/x ");
    CHECK(cch::tui::normalize_pasted_text("/tmp/x",
                  {.multiline = true, .boundaries = {.text_before = "foo ", .text_after = " bar"}}) == "/tmp/x");
    CHECK(cch::tui::normalize_pasted_text("/tmp/x", {.multiline = true}) == "/tmp/x");

    // A path glued to a word inside the same paste is separated, while a
    // relative path that already follows a space is left alone.
    CHECK(cch::tui::normalize_pasted_text("foo/tmp/x", {.multiline = true, .boundaries = {.text_before = "see"}}) ==
            "foo /tmp/x");
    CHECK(cch::tui::normalize_pasted_text("run ./x", {.multiline = true, .boundaries = {.text_before = "foo"}}) ==
            "run ./x");

    // Ordinary words never gain the path spacing a blind stand-in would add,
    // and scriptio continua are never split.
    CHECK(cch::tui::normalize_pasted_text(
                  "abc", {.multiline = true, .boundaries = {.text_before = "foo", .text_after = "bar"}}) == "abc");
    CHECK(cch::tui::normalize_pasted_text("\xe4\xb8\xad\xe6\x96\x87/\xe6\x96\x87\xe4\xbb\xb6",
                  {.multiline = true, .boundaries = {.text_before = "foo"}}) ==
            "\xe4\xb8\xad\xe6\x96\x87/\xe6\x96\x87\xe4\xbb\xb6");
}

TEST_CASE("Editor inserts a split bracketed paste as normalized text in one undo step",
        "[tui][paste][editor][issue953][spec]") {
    cch::tui::VirtualTerminal terminal({.columns = 40, .rows = 4});
    cch::tui::Tui tui(terminal);
    auto editor = std::make_unique<cch::tui::Editor>();
    auto* editor_pointer = editor.get();
    REQUIRE(tui.add_child(std::move(editor)));
    REQUIRE(tui.start());
    REQUIRE(tui.set_focus(editor_pointer));

    REQUIRE(terminal.inject_input("foo"));
    CHECK(editor_pointer->text() == "foo");

    // A path pasted straight after a word is separated by one space.
    REQUIRE(terminal.inject_input("\x1b[200~"));
    REQUIRE(terminal.inject_input("/tmp/x"));
    REQUIRE(terminal.inject_input("\x1b[201~"));
    CHECK(editor_pointer->text() == "foo /tmp/x");

    // The split paste is exactly one undo step.
    key(*editor_pointer, "-", true);
    CHECK(editor_pointer->text() == "foo");

    // A control reply inside the paste contributes its newline, not its bytes.
    REQUIRE(terminal.inject_input("\x1b[200~a\x1b[106;5u"));
    REQUIRE(terminal.inject_input("b\x1b[201~"));
    CHECK(editor_pointer->text() == "fooa\nb");

    // Tabs expand and one undo step removes the whole multiline paste.
    key(*editor_pointer, "-", true);
    CHECK(editor_pointer->text() == "foo");

    REQUIRE(terminal.inject_input("\x1b[200~a\tb\x1b[201~"));
    CHECK(editor_pointer->text() == "fooa    b");
    key(*editor_pointer, "-", true);
    CHECK(editor_pointer->text() == "foo");
}

TEST_CASE(
        "Input inserts a split bracketed paste as normalized single-line text", "[tui][paste][input][issue953][spec]") {
    cch::tui::VirtualTerminal terminal({.columns = 40, .rows = 2});
    cch::tui::Tui tui(terminal);
    auto input = std::make_unique<cch::tui::Input>();
    auto* input_pointer = input.get();
    REQUIRE(tui.add_child(std::move(input)));
    REQUIRE(tui.start());
    REQUIRE(tui.set_focus(input_pointer));

    REQUIRE(terminal.inject_input("\x1b[200~foo"));
    REQUIRE(terminal.inject_input("\x1b[106;5u"));
    REQUIRE(terminal.inject_input("/tmp/x\tz\x1b[201~"));

    // The newline is dropped by the single-line target, the path is separated
    // from the word before it, and the tab expands to four columns.
    CHECK(input_pointer->value() == "foo /tmp/x    z");

    // One undo step restores the pre-paste value.
    key(*input_pointer, "-", true);
    CHECK(input_pointer->value().empty());

    // The same normalization is reached through the decoded paste event seam.
    static_cast<void>(input_pointer->handle_input(cch::tui::PasteEvent{.text = "ab\x1b[106;5uc\t"}));
    CHECK(input_pointer->value() == "abc    ");
    key(*input_pointer, "-", true);
    CHECK(input_pointer->value().empty());
}
