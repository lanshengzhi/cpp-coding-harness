#include <cch/tui/Editor.hpp>
#include <cch/tui/Tui.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include "../../third_party/catch2/catch_test_macros.hpp"

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
