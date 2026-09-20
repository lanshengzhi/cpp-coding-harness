#include "tui/EditorLayout.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace cch;
using tui::AutocompleteItem;
using tui::EditorTheme;
using tui::detail::BufferCursor;
using tui::detail::BufferDocument;
using tui::detail::BufferLine;
using tui::detail::BufferSegment;
using tui::detail::EditorCompletionMenuPresentation;
using tui::detail::EditorLayout;
using tui::detail::EditorLayoutOptions;
using tui::detail::EditorVisualLine;
using tui::detail::TextBuffer;

TEST_CASE("EditorLayout rejects non-positive visible width with validation error",
        "[tui][editor][layout][validation][issue751][spec]") {
    BufferDocument doc{BufferLine{BufferSegment{.text = "hello"}}};
    const auto result = EditorLayout::validate_width(doc, 0);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == support::ErrorCode::Validation);
    CHECK(result.error().message == "Editor requires a positive visible width");
}

TEST_CASE("EditorLayout rejects grapheme wider than visible width with validation error",
        "[tui][editor][layout][validation][issue751][spec]") {
    // "你" has visible width 2.
    BufferDocument doc{BufferLine{BufferSegment{.text = "你"}}};
    const auto result = EditorLayout::validate_width(doc, 1);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == support::ErrorCode::Validation);
    CHECK(result.error().message == "Editor grapheme is wider than the available visible width");
}

TEST_CASE("EditorLayout constructs visual lines with Unicode and CJK wrapping",
        "[tui][editor][layout][wrapping][issue751][spec]") {
    // "你好世界" is 4 CJK chars, each width 2 -> 8 cols total.
    BufferDocument doc{BufferLine{
            BufferSegment{.text = "你"},
            BufferSegment{.text = "好"},
            BufferSegment{.text = "世"},
            BufferSegment{.text = "界"},
    }};
    const auto lines = EditorLayout::construct_visual_lines(doc, 5);
    REQUIRE(lines.size() == 2);
    CHECK(lines[0].logical_line == 0);
    CHECK(lines[0].start == 0);
    CHECK(lines[0].end == 2);
    CHECK(lines[0].text == "你好");

    CHECK(lines[1].logical_line == 0);
    CHECK(lines[1].start == 2);
    CHECK(lines[1].end == 4);
    CHECK(lines[1].text == "世界");
}

TEST_CASE("EditorLayout computes empty document with fake cursor and padding",
        "[tui][editor][layout][cursor][issue751][spec]") {
    BufferDocument doc{BufferLine{}};
    EditorLayoutOptions options{
            .document = &doc,
            .cursor = BufferCursor{.line = 0, .column = 0},
            .width = 8,
            .max_visible_lines = 5,
            .available_height = 5,
            .scroll_offset = 0,
    };
    const auto layout = EditorLayout::compute(std::move(options));
    REQUIRE(layout);
    REQUIRE(layout->lines.size() == 1);
    CHECK(layout->lines[0] == "\x1b[7m \x1b[27m       ");
    CHECK(layout->scroll_offset == 0);
}

TEST_CASE("EditorLayout computes scroll offset and scroll borders",
        "[tui][editor][layout][scroll][theme][issue751][spec]") {
    TextBuffer buffer;
    buffer.set_text("A\nB\nC\nD\nE");

    EditorTheme theme;
    theme.border = [](std::string s) { return s; };

    // With available_height = 4 and border_rows = 2, content_height is 2.
    // Cursor on C (line 2, col 0) forces scroll_offset to 1.
    EditorLayoutOptions options{
            .document = &buffer.document(),
            .cursor = BufferCursor{.line = 2, .column = 0},
            .width = 16,
            .max_visible_lines = 2,
            .available_height = 4,
            .scroll_offset = 0,
            .theme = &theme,
    };
    const auto layout = EditorLayout::compute(std::move(options));
    REQUIRE(layout);
    CHECK(layout->scroll_offset == 1);
    REQUIRE(layout->lines.size() == 4);
    // Top border shows ↑ 1 more
    CHECK(layout->lines[0] == "─── ↑ 1 more ───");
    // Visible lines are B and C (with fake cursor on C)
    CHECK(layout->lines[1] == "B               ");
    CHECK(layout->lines[2] == "\x1b[7mC\x1b[27m               ");
    // Bottom border shows ↓ 2 more
    CHECK(layout->lines[3] == "─── ↓ 2 more ───");
}

TEST_CASE("EditorLayout propagates text and border styling failures", "[tui][editor][layout][theme][issue751][spec]") {
    BufferDocument doc{BufferLine{BufferSegment{.text = "hi"}}};

    EditorTheme theme;
    // Style hook that changes visible width triggers validation error
    theme.text = [](std::string) { return "longer text"; };

    EditorLayoutOptions options{
            .document = &doc,
            .cursor = BufferCursor{.line = 0, .column = 0},
            .width = 10,
            .theme = &theme,
    };
    const auto layout = EditorLayout::compute(std::move(options));
    REQUIRE_FALSE(layout);
    CHECK(layout.error().code == support::ErrorCode::Validation);
    CHECK(layout.error().message == "TUI Editor text style hook changed visible width");
}

TEST_CASE("EditorLayout appends autocomplete items when menu is open",
        "[tui][editor][layout][autocomplete][issue751][spec]") {
    BufferDocument doc{BufferLine{BufferSegment{.text = "/h"}}};
    EditorCompletionMenuPresentation menu{
            .open = true,
            .forced = false,
            .items =
                    {
                            AutocompleteItem{.value = "/help", .label = "help", .description = "show help"},
                            AutocompleteItem{.value = "/history", .label = "history", .description = "show history"},
                    },
            .prefix = "/h",
            .selected_index = 0,
    };

    EditorLayoutOptions options{
            .document = &doc,
            .cursor = BufferCursor{.line = 0, .column = 2},
            .width = 30,
            .max_visible_lines = 5,
            .available_height = 5,
            .scroll_offset = 0,
            .autocomplete_menu = &menu,
            .include_autocomplete = true,
    };
    const auto layout = EditorLayout::compute(std::move(options));
    REQUIRE(layout);
    // Line 0 is the editor text line. Lines 1 and 2 are autocomplete items.
    REQUIRE(layout->lines.size() == 3);
    CHECK(layout->lines[1].starts_with("> /help — show help"));
    CHECK(layout->lines[2].starts_with("  /history — show history"));
}
