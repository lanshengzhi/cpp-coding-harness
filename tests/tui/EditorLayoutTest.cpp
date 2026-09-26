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

TEST_CASE("EditorLayout computes cursor position for empty and bordered documents",
        "[tui][editor][layout][cursor][issue752][spec]") {
    BufferDocument doc{BufferLine{}};

    // Case 1: unbordered empty document
    EditorLayoutOptions options{
            .document = &doc,
            .cursor = BufferCursor{.line = 0, .column = 0},
            .width = 10,
            .max_visible_lines = 5,
            .available_height = 5,
            .scroll_offset = 0,
    };
    const auto layout = EditorLayout::compute(std::move(options));
    REQUIRE(layout);
    REQUIRE(layout->cursor_position.has_value());
    CHECK(layout->cursor_position->column == 0);
    CHECK(layout->cursor_position->row == 0);

    // Case 2: bordered empty document (row 1: only the top border sits
    // above the content row; the bottom border must not shift the cursor)
    EditorTheme theme;
    theme.border = [](std::string s) { return s; };
    EditorLayoutOptions bordered_options{
            .document = &doc,
            .cursor = BufferCursor{.line = 0, .column = 0},
            .width = 10,
            .max_visible_lines = 5,
            .available_height = 5,
            .scroll_offset = 0,
            .theme = &theme,
    };
    const auto bordered_layout = EditorLayout::compute(std::move(bordered_options));
    REQUIRE(bordered_layout);
    REQUIRE(bordered_layout->cursor_position.has_value());
    CHECK(bordered_layout->cursor_position->column == 0);
    CHECK(bordered_layout->cursor_position->row == 1);
}

TEST_CASE("EditorLayout computes exact visual cursor position for wrapped Unicode graphemes",
        "[tui][editor][layout][cursor][wrapping][issue752][spec]") {
    // 4 CJK characters (each width 2): "你好世界"
    BufferDocument doc{BufferLine{
            BufferSegment{.text = "你"},
            BufferSegment{.text = "好"},
            BufferSegment{.text = "世"},
            BufferSegment{.text = "界"},
    }};

    // Cursor at grapheme 0 ("你"): visual line 0, col 0
    auto res0 = EditorLayout::compute(EditorLayoutOptions{
            .document = &doc,
            .cursor = BufferCursor{.line = 0, .column = 0},
            .width = 5,
    });
    REQUIRE(res0);
    REQUIRE(res0->cursor_position.has_value());
    CHECK(res0->cursor_position->column == 0);
    CHECK(res0->cursor_position->row == 0);

    // Cursor at grapheme 1 ("好"): visual line 0, col 2
    auto res1 = EditorLayout::compute(EditorLayoutOptions{
            .document = &doc,
            .cursor = BufferCursor{.line = 0, .column = 1},
            .width = 5,
    });
    REQUIRE(res1);
    REQUIRE(res1->cursor_position.has_value());
    CHECK(res1->cursor_position->column == 2);
    CHECK(res1->cursor_position->row == 0);

    // Cursor at grapheme 2 (boundary): visual line 0, col 4
    auto res2 = EditorLayout::compute(EditorLayoutOptions{
            .document = &doc,
            .cursor = BufferCursor{.line = 0, .column = 2},
            .width = 5,
    });
    REQUIRE(res2);
    REQUIRE(res2->cursor_position.has_value());
    CHECK(res2->cursor_position->column == 4);
    CHECK(res2->cursor_position->row == 0);

    // Cursor at grapheme 3 ("界"): visual line 1, col 2
    auto res3 = EditorLayout::compute(EditorLayoutOptions{
            .document = &doc,
            .cursor = BufferCursor{.line = 0, .column = 3},
            .width = 5,
    });
    REQUIRE(res3);
    REQUIRE(res3->cursor_position.has_value());
    CHECK(res3->cursor_position->column == 2);
    CHECK(res3->cursor_position->row == 1);

    // Cursor at grapheme 4 (end): visual line 1, col 4
    auto res4 = EditorLayout::compute(EditorLayoutOptions{
            .document = &doc,
            .cursor = BufferCursor{.line = 0, .column = 4},
            .width = 5,
    });
    REQUIRE(res4);
    REQUIRE(res4->cursor_position.has_value());
    CHECK(res4->cursor_position->column == 4);
    CHECK(res4->cursor_position->row == 1);
}

TEST_CASE("EditorLayout computes visual cursor position with scrolling and handles out-of-viewport",
        "[tui][editor][layout][cursor][scroll][issue752][spec]") {
    TextBuffer buffer;
    buffer.set_text("L0\nL1\nL2\nL3");

    // With max_visible_lines = 2, available_height = 2:
    // Cursor at line 2 (L2): forces scroll_offset to 1 (window [L1, L2]).
    // L2 is at index 1 within the visible window -> display row 1.
    auto layout = EditorLayout::compute(EditorLayoutOptions{
            .document = &buffer.document(),
            .cursor = BufferCursor{.line = 2, .column = 2},
            .width = 10,
            .max_visible_lines = 2,
            .available_height = 2,
            .scroll_offset = 0,
    });
    REQUIRE(layout);
    REQUIRE(layout->cursor_position.has_value());
    CHECK(layout->cursor_position->column == 2);
    CHECK(layout->cursor_position->row == 1);

    // Test compute_cursor_position directly:
    // When visual_row is outside [scroll_offset, scroll_offset + visible_count), returns nullopt.
    const auto visual = EditorLayout::construct_visual_lines(buffer.document(), 10);
    // Cursor on line 0 while scroll_offset = 2, visible_count = 2 -> out of viewport
    auto pos = EditorLayout::compute_cursor_position(
            buffer.document(), visual, BufferCursor{.line = 0, .column = 0}, 0, 2, 2);
    CHECK_FALSE(pos.has_value());

    // Empty visual lines returns nullopt
    auto empty_pos =
            EditorLayout::compute_cursor_position(buffer.document(), {}, BufferCursor{.line = 0, .column = 0}, 0, 0, 2);
    CHECK_FALSE(empty_pos.has_value());
}
