#include <cch/tui/Editor.hpp>
#include <cch/tui/Tui.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include "../../third_party/catch2/catch_test_macros.hpp"

#include <format>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void key(cch::tui::Editor& editor, std::string name, bool ctrl = false, bool shift = false, bool alt = false) {
    editor.handle_input(cch::tui::KeyEvent{
        .key = std::move(name),
        .ctrl = ctrl,
        .shift = shift,
        .alt = alt,
    });
}

void type(cch::tui::Editor& editor, std::string text) {
    key(editor, std::move(text));
}

} // namespace

TEST_CASE(
    "Editor edits Unicode text through semantic cursor and deletion operations",
    "[tui][editor][issue48][issue57]") {
    cch::tui::Editor editor;
    type(editor, "A\xc3\xa9\xf0\x9f\x98\x80");
    key(editor, "left");
    key(editor, "backspace");
    CHECK(editor.text() == "A\xf0\x9f\x98\x80");
    CHECK((editor.cursor() == cch::tui::EditorCursor{.line = 0, .column = 1}));

    editor.set_text("one two");
    key(editor, "home");
    key(editor, "right", true);
    CHECK((editor.cursor() == cch::tui::EditorCursor{.line = 0, .column = 3}));
    key(editor, "w", true);
    CHECK(editor.text() == " two");

    editor.set_text("ab");
    key(editor, "backspace", false, true);
    CHECK(editor.text() == "a");
    key(editor, "home");
    key(editor, "delete", false, true);
    CHECK(editor.text().empty());
}

TEST_CASE("Editor preserves multiline undo kill yank and jump state", "[tui][editor][issue48]") {
    cch::tui::Editor editor;
    editor.insert_text_at_cursor("first second\nthird");
    key(editor, "home");
    key(editor, "k", true);
    CHECK(editor.text() == "first second\n");
    key(editor, "y", true);
    CHECK(editor.text() == "first second\nthird");
    key(editor, "-", true);
    CHECK(editor.text() == "first second\n");

    editor.set_text("one two three");
    key(editor, "home");
    key(editor, "]", true);
    key(editor, "t");
    CHECK((editor.cursor() == cch::tui::EditorCursor{.line = 0, .column = 4}));
}

TEST_CASE("Editor rotates the kill ring only after a yank", "[tui][editor][issue48]") {
    cch::tui::Editor editor;
    editor.set_text("first");
    key(editor, "home");
    key(editor, "k", true);
    editor.set_text("second");
    key(editor, "home");
    key(editor, "k", true);
    key(editor, "y", true);
    CHECK(editor.text() == "second");
    key(editor, "y", false, false, true);
    CHECK(editor.text() == "first");
}

TEST_CASE("Editor yanks multiline and pasted content without losing marker semantics", "[tui][editor][issue48]") {
    cch::tui::Editor editor;
    editor.set_text("first\nsecond");
    key(editor, "home");
    key(editor, "u", true);
    editor.set_text({});
    key(editor, "y", true);
    CHECK(editor.text() == "\n");
    key(editor, "y", false, false, true);
    CHECK(editor.text() == "\n");

    editor.set_text({});
    editor.handle_input(cch::tui::PasteEvent{.text = std::string(1001, 'x'), .original_bytes = 1001, .lines = 1});
    key(editor, "home");
    key(editor, "k", true);
    key(editor, "y", true);
    CHECK(editor.expanded_text() == std::string(1001, 'x'));
}

TEST_CASE("Editor makes a large bracketed paste editable without submitting", "[tui][editor][issue48]") {
    std::vector<std::string> submitted;
    cch::tui::Editor editor({}, {}, [&submitted](std::string text) { submitted.push_back(std::move(text)); });
    editor.handle_input(cch::tui::PasteEvent{
        .text = std::string(1001, 'x'),
        .original_bytes = 1001,
        .lines = 1,
    });

    CHECK(editor.text() == "[paste #1 1001 chars]");
    CHECK(editor.expanded_text() == std::string(1001, 'x'));
    CHECK(submitted.empty());
    key(editor, "backspace");
    CHECK(editor.text().empty());
    CHECK(editor.expanded_text().empty());
}

TEST_CASE("Editor accepts caller supplied command and filesystem suggestions", "[tui][editor][issue48][issue60]") {
    cch::tui::Editor editor;
    editor.set_autocomplete_provider([](const cch::tui::AutocompleteRequest& request)
        -> std::optional<cch::tui::AutocompleteSuggestions> {
        if (request.lines[request.cursor.line].starts_with("/")) {
            return cch::tui::AutocompleteSuggestions{
                .items = {{.value = "help", .label = "help", .description = {}},
                          {.value = "history", .label = "history", .description = {}}},
                .prefix = request.lines[request.cursor.line],
            };
        }
        if (request.lines[request.cursor.line].starts_with("@")) {
            return cch::tui::AutocompleteSuggestions{
                .items = {{.value = "src/", .label = "src/", .description = {}}},
                .prefix = "@s",
            };
        }
        return std::nullopt;
    });

    editor.set_text("/");
    key(editor, "tab");
    REQUIRE(editor.autocomplete_open());
    CHECK(editor.autocomplete_selected_index() == 0);
    key(editor, "down");
    CHECK(editor.autocomplete_selected_index() == 1);
    key(editor, "up");
    CHECK(editor.autocomplete_selected_index() == 0);

    editor.set_text({});
    type(editor, "/he");
    REQUIRE(editor.autocomplete_open());
    key(editor, "tab");
    CHECK(editor.text() == "/help");
    CHECK_FALSE(editor.autocomplete_open());

    editor.set_text("@s");
    key(editor, "tab");
    REQUIRE(editor.autocomplete_open());
    key(editor, "escape");
    CHECK_FALSE(editor.autocomplete_open());
    CHECK(editor.text() == "@s");
}

TEST_CASE("Editor keeps its active cursor in a narrow virtual terminal viewport", "[tui][editor][issue48]") {
    cch::tui::Editor editor({.max_visible_lines = 3});
    editor.insert_text_at_cursor("abcdefgh\nijklmnop\nqrstuvwx\nyz");
    auto lines = editor.render(4);
    REQUIRE(lines);
    CHECK(lines->lines.size() <= 3);
    CHECK_FALSE(lines->lines.empty());
    CHECK(lines->lines.back().find("yz") != std::string::npos);

    cch::tui::VirtualTerminal terminal({.columns = 4, .rows = 3});
    REQUIRE(terminal.start([](std::string) {}, [](cch::tui::TerminalDimensions) {}));
    for (std::size_t row = 0; row < lines->lines.size(); ++row) {
        REQUIRE(terminal.set_cursor({.column = 0, .row = row}));
        REQUIRE(terminal.write(lines->lines[row]));
    }
    CHECK(terminal.screen().back().find("yz") != std::string::npos);
}

TEST_CASE("Editor scrolls the active cursor into the Virtual Terminal viewport", "[tui][editor][issue48]") {
    cch::tui::VirtualTerminal terminal({.columns = 4, .rows = 2});
    cch::tui::Tui tui(terminal);
    auto editor = std::make_unique<cch::tui::Editor>();
    auto* editor_pointer = editor.get();
    REQUIRE(tui.add_child(std::move(editor)));
    REQUIRE(tui.start());
    REQUIRE(tui.set_focus(editor_pointer));
    editor_pointer->insert_text_at_cursor("abcdefgh\nijklmnop\nqrstuvwx\nyz");
    REQUIRE(tui.render());
    CHECK(terminal.screen().back().find("yz") != std::string::npos);
}

TEST_CASE("Editor edits Unicode input through the Virtual Terminal seam", "[tui][editor][issue48]") {
    cch::tui::VirtualTerminal terminal({.columns = 4, .rows = 2});
    cch::tui::Tui tui(terminal);
    auto editor = std::make_unique<cch::tui::Editor>();
    auto* editor_pointer = editor.get();
    REQUIRE(tui.add_child(std::move(editor)));
    REQUIRE(tui.start());
    REQUIRE(tui.set_focus(editor_pointer));
    REQUIRE(terminal.inject_input("A\xc3\xa9"));
    REQUIRE(terminal.inject_input("\x1b[D"));
    REQUIRE(terminal.inject_input("\x7f"));
    CHECK(editor_pointer->text() == "\xc3\xa9");
}

TEST_CASE("Editor converts the semantic space key back to fresh text", "[tui][editor][issue58]") {
    cch::tui::VirtualTerminal terminal({.columns = 12, .rows = 2});
    cch::tui::Tui tui(terminal);
    auto editor = std::make_unique<cch::tui::Editor>();
    auto* editor_pointer = editor.get();
    REQUIRE(tui.add_child(std::move(editor)));
    REQUIRE(tui.start());
    REQUIRE(tui.set_focus(editor_pointer));
    REQUIRE(terminal.inject_input("two words"));
    CHECK(editor_pointer->text() == "two words");
}

TEST_CASE("Editor receives decoder paste events through the Virtual Terminal seam", "[tui][editor][issue48]") {
    cch::tui::VirtualTerminal terminal({.columns = 4, .rows = 3});
    cch::tui::Tui tui(terminal);
    auto editor = std::make_unique<cch::tui::Editor>(cch::tui::EditorOptions{.max_visible_lines = 3});
    auto* editor_pointer = editor.get();
    REQUIRE(tui.add_child(std::move(editor)));
    REQUIRE(tui.start());
    REQUIRE(tui.set_focus(editor_pointer));
    REQUIRE(terminal.inject_input("\x1b[200~" + std::string(1001, 'x') + "\x1b[201~"));
    CHECK(editor_pointer->text() == "[paste #1 1001 chars]");
    CHECK(editor_pointer->expanded_text() == std::string(1001, 'x'));
    REQUIRE(tui.render());
    CHECK_FALSE(terminal.screen().empty());
}

TEST_CASE(
    "Editor submits and resets repeatedly with configured newline operations",
    "[tui][editor][issue48][issue57]") {
    cch::tui::KeybindingResolutionRequest request;
    request.definitions = cch::tui::builtin_tui_keybinding_definitions();
    request.overrides = {
        {.id = "tui.input.submit", .keys = {"ctrl+enter"}},
        {.id = "tui.input.newLine", .keys = {"enter"}},
    };
    const auto keybindings = cch::tui::resolve_keybindings(std::move(request));
    REQUIRE(keybindings);

    std::vector<std::string> changes;
    std::vector<std::string> submitted;
    cch::tui::Editor editor(
        {.keybindings = keybindings->registry},
        [&changes](std::string text) { changes.push_back(std::move(text)); },
        [&submitted](std::string text) { submitted.push_back(std::move(text)); });

    type(editor, "one");
    key(editor, "enter");
    type(editor, "two");
    key(editor, "enter", true);
    REQUIRE(submitted.size() == 1);
    CHECK(submitted[0] == "one\ntwo");
    CHECK(editor.text().empty());
    type(editor, "next");
    key(editor, "enter", true);
    REQUIRE(submitted.size() == 2);
    CHECK(submitted[1] == "next");
    CHECK(changes.back().empty());
}

TEST_CASE(
    "Editor applies replaceable generic styling without changing visible width",
    "[tui][editor][theme][issue55]") {
    cch::tui::Editor editor;
    cch::tui::EditorTheme first;
    first.text = [](std::string text) { return "\x1b[31m" + text + "\x1b[39m"; };
    editor.set_theme(std::move(first));
    editor.set_text("hello");

    const auto red = editor.render(8);

    REQUIRE(red);
    REQUIRE(red->lines.size() == 1);
    CHECK(red->lines[0].find("\x1b[31m") != std::string::npos);

    cch::tui::EditorTheme second;
    second.text = [](std::string text) { return "\x1b[34m" + text + "\x1b[39m"; };
    editor.set_theme(std::move(second));
    const auto blue = editor.render(8);

    REQUIRE(blue);
    CHECK(blue->lines[0].find("\x1b[34m") != std::string::npos);
    CHECK(blue->lines[0].find("\x1b[31m") == std::string::npos);
}

TEST_CASE("Editor rejects failing or width-changing generic styling", "[tui][editor][theme][issue55]") {
    cch::tui::Editor editor;
    cch::tui::EditorTheme wider;
    wider.text = [](std::string text) { return text + "x"; };
    editor.set_theme(std::move(wider));

    const auto width_failure = editor.render(8);

    REQUIRE_FALSE(width_failure);
    CHECK(width_failure.error().code == cch::util::ErrorCode::Validation);
    CHECK(width_failure.error().message.find("changed visible width") != std::string::npos);

    cch::tui::EditorTheme throwing;
    throwing.text = [](std::string) -> std::string { throw std::runtime_error("style failed"); };
    editor.set_theme(std::move(throwing));
    const auto callback_failure = editor.render(8);

    REQUIRE_FALSE(callback_failure);
    CHECK(callback_failure.error().message.find("style hook failed") != std::string::npos);
}

TEST_CASE("Editor does nothing on Up when history is empty", "[tui][editor][history][issue379]") {
    cch::tui::Editor editor;
    key(editor, "up");
    CHECK(editor.text().empty());
}

TEST_CASE("Editor recalls the most recent entry on Up when empty", "[tui][editor][history][issue379]") {
    cch::tui::Editor editor;
    editor.add_to_history("first prompt");
    editor.add_to_history("second prompt");

    key(editor, "up");

    CHECK(editor.text() == "second prompt");
}

TEST_CASE("Editor cycles through history entries on repeated Up", "[tui][editor][history][issue379]") {
    cch::tui::Editor editor;
    editor.add_to_history("first");
    editor.add_to_history("second");
    editor.add_to_history("third");

    key(editor, "up");
    CHECK(editor.text() == "third");
    key(editor, "up");
    CHECK(editor.text() == "second");
    key(editor, "up");
    CHECK(editor.text() == "first");
    key(editor, "up");
    CHECK(editor.text() == "first");  // Stays at the oldest entry
}

TEST_CASE("Editor jumps to start before entering history from a non-empty draft", "[tui][editor][history][issue379]") {
    cch::tui::Editor editor;
    editor.add_to_history("prompt");
    editor.set_text("draft");
    key(editor, "left");
    key(editor, "left");

    key(editor, "up");  // Jumps to start before history browsing
    CHECK(editor.text() == "draft");
    CHECK((editor.cursor() == cch::tui::EditorCursor{.line = 0, .column = 0}));

    key(editor, "up");  // At start - shows "prompt"
    CHECK(editor.text() == "prompt");

    key(editor, "down");  // Restores draft
    CHECK(editor.text() == "draft");
    CHECK((editor.cursor() == cch::tui::EditorCursor{.line = 0, .column = 0}));
}

TEST_CASE("Editor navigates forward through history with Down", "[tui][editor][history][issue379]") {
    cch::tui::Editor editor;
    editor.add_to_history("first");
    editor.add_to_history("second");
    editor.add_to_history("third");
    editor.set_text("draft");

    // Go to oldest: start-jump, then third, second, first
    key(editor, "up");
    key(editor, "up");
    key(editor, "up");
    key(editor, "up");

    // Navigate back
    key(editor, "down");
    CHECK(editor.text() == "second");
    key(editor, "down");
    CHECK(editor.text() == "third");
    key(editor, "down");
    CHECK(editor.text() == "draft");
}

TEST_CASE("Editor exits history mode when typing a character", "[tui][editor][history][issue379]") {
    cch::tui::Editor editor;
    editor.add_to_history("old prompt");

    key(editor, "up");  // Shows "old prompt" with cursor at start
    type(editor, "x");

    CHECK(editor.text() == "xold prompt");
}

TEST_CASE("Editor exits history mode on set_text", "[tui][editor][history][issue379]") {
    cch::tui::Editor editor;
    editor.add_to_history("first");
    editor.add_to_history("second");

    key(editor, "up");  // Shows "second"
    editor.set_text("");

    key(editor, "up");  // Starts fresh from most recent
    CHECK(editor.text() == "second");
}

TEST_CASE("Editor does not add empty strings to history", "[tui][editor][history][issue379]") {
    cch::tui::Editor editor;
    editor.add_to_history("");
    editor.add_to_history("   ");
    editor.add_to_history("valid");

    key(editor, "up");
    CHECK(editor.text() == "valid");
    key(editor, "up");  // No more entries
    CHECK(editor.text() == "valid");
}

TEST_CASE("Editor does not add consecutive duplicates to history", "[tui][editor][history][issue379]") {
    cch::tui::Editor editor;
    editor.add_to_history("same");
    editor.add_to_history("same");
    editor.add_to_history("same");

    key(editor, "up");
    CHECK(editor.text() == "same");
    key(editor, "up");  // Only one entry
    CHECK(editor.text() == "same");
}

TEST_CASE("Editor allows non-consecutive duplicates in history", "[tui][editor][history][issue379]") {
    cch::tui::Editor editor;
    editor.add_to_history("first");
    editor.add_to_history("second");
    editor.add_to_history("first");  // Not consecutive, added

    key(editor, "up");
    CHECK(editor.text() == "first");
    key(editor, "up");
    CHECK(editor.text() == "second");
    key(editor, "up");
    CHECK(editor.text() == "first");  // Older one
}

TEST_CASE("Editor uses cursor movement instead of history when content is present", "[tui][editor][history][issue379]") {
    cch::tui::Editor editor;
    editor.add_to_history("history item");
    editor.set_text("line1\nline2");

    key(editor, "up");  // Cursor movement, not history
    type(editor, "X");  // Verify cursor position

    CHECK(editor.text() == "line1X\nline2");
}

TEST_CASE("Editor limits history to 100 entries", "[tui][editor][history][issue379]") {
    cch::tui::Editor editor;
    for (int index = 0; index < 105; ++index) {
        editor.add_to_history(std::format("prompt {}", index));
    }

    for (int index = 0; index < 100; ++index) key(editor, "up");

    // Oldest kept entry, not entry 0
    CHECK(editor.text() == "prompt 5");

    key(editor, "up");  // No change at the oldest entry
    CHECK(editor.text() == "prompt 5");
}

TEST_CASE("Editor places the cursor at the start after browsing upward", "[tui][editor][history][issue379]") {
    cch::tui::Editor editor;
    editor.add_to_history("older entry");
    editor.add_to_history("line1\nline2\nline3");

    key(editor, "up");  // Shows multi-line entry at start
    CHECK(editor.text() == "line1\nline2\nline3");
    CHECK((editor.cursor() == cch::tui::EditorCursor{.line = 0, .column = 0}));

    key(editor, "up");  // Immediately navigates to older entry
    CHECK(editor.text() == "older entry");
    CHECK((editor.cursor() == cch::tui::EditorCursor{.line = 0, .column = 0}));
}

TEST_CASE("Editor places the cursor at the end after browsing downward", "[tui][editor][history][issue379]") {
    cch::tui::Editor editor;
    editor.add_to_history("older entry");
    editor.add_to_history("line1\nline2\nline3");
    editor.add_to_history("newer entry");

    key(editor, "up");  // newer entry
    key(editor, "up");  // multi-line entry
    key(editor, "up");  // older entry

    key(editor, "down");  // Shows multi-line entry at end
    CHECK(editor.text() == "line1\nline2\nline3");
    CHECK((editor.cursor() == cch::tui::EditorCursor{.line = 2, .column = 5}));

    key(editor, "down");  // Immediately navigates to newer entry
    CHECK(editor.text() == "newer entry");
}

TEST_CASE("Editor allows opposite-direction cursor movement within a multi-line entry", "[tui][editor][history][issue379]") {
    cch::tui::Editor editor;
    editor.add_to_history("line1\nline2\nline3");

    key(editor, "up");  // Shows entry at start
    CHECK((editor.cursor() == cch::tui::EditorCursor{.line = 0, .column = 0}));

    key(editor, "down");  // Cursor moves to line2
    CHECK(editor.text() == "line1\nline2\nline3");
    CHECK((editor.cursor() == cch::tui::EditorCursor{.line = 1, .column = 0}));

    key(editor, "up");  // Cursor moves back to line1
    CHECK(editor.text() == "line1\nline2\nline3");
    CHECK((editor.cursor() == cch::tui::EditorCursor{.line = 0, .column = 0}));
}

TEST_CASE("Editor records every submit path and recalls the submitted entry", "[tui][editor][history][issue379]") {
    std::vector<std::string> submitted;
    cch::tui::Editor editor(
        {},
        {},
        [&submitted](std::string text) { submitted.push_back(std::move(text)); });

    type(editor, "one");
    key(editor, "enter");
    REQUIRE(submitted.size() == 1);
    CHECK(submitted[0] == "one");

    // The submitted entry is immediately recallable
    key(editor, "up");
    CHECK(editor.text() == "one");
    key(editor, "down");  // back to the empty draft

    // Whitespace-only submissions are trimmed away and not recorded
    type(editor, "two");
    key(editor, "enter");
    REQUIRE(submitted.size() == 2);
    type(editor, "   ");
    key(editor, "enter");
    REQUIRE(submitted.size() == 3);
    key(editor, "up");
    CHECK(editor.text() == "two");
}

TEST_CASE("Editor recalling history is undoable back to the draft", "[tui][editor][history][issue379]") {
    cch::tui::Editor editor;
    editor.add_to_history("recalled");
    editor.set_text("draft");

    key(editor, "up");  // start-jump
    key(editor, "up");  // "recalled" (browsing captured the draft)
    key(editor, "-", true);  // undo

    CHECK(editor.text() == "draft");
    CHECK((editor.cursor() == cch::tui::EditorCursor{.line = 0, .column = 0}));
}
