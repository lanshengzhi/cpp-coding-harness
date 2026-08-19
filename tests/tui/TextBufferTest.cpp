#include "tui/TextBuffer.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace cch;
using namespace cch::tui::detail;

TEST_CASE("TextBuffer basic insertion and query", "[tui][text_buffer]") {
    TextBuffer buffer;
    CHECK(buffer.empty());
    CHECK(buffer.text().empty());
    CHECK(buffer.cursor() == BufferCursor{.line = 0, .column = 0});

    buffer.insert_text("hello");
    CHECK_FALSE(buffer.empty());
    CHECK(buffer.text() == "hello");
    CHECK(buffer.cursor() == BufferCursor{.line = 0, .column = 5});
    CHECK(buffer.line_count() == 1);
    CHECK(buffer.lines() == std::vector<std::string>{"hello"});
    CHECK(buffer.line_strings() == std::vector<std::string>{"hello"});

    buffer.insert_newline();
    buffer.insert_text("world");
    CHECK(buffer.line_count() == 2);
    CHECK(buffer.text() == "hello\nworld");
    CHECK(buffer.cursor() == BufferCursor{.line = 1, .column = 5});
    CHECK(buffer.lines() == std::vector<std::string>{"hello", "world"});
}

TEST_CASE("TextBuffer single-line mode flattens newlines", "[tui][text_buffer]") {
    TextBuffer buffer(TextBufferOptions{.multiline = false, .enable_paste_markers = false});
    buffer.insert_text("hello\nworld\r\nfoo");
    CHECK(buffer.line_count() == 1);
    CHECK(buffer.text() == "hello world foo");
    CHECK(buffer.cursor().line == 0);

    buffer.insert_newline();
    CHECK(buffer.line_count() == 1);
    CHECK(buffer.text() == "hello world foo");
}

TEST_CASE("TextBuffer cursor navigation and word movements", "[tui][text_buffer]") {
    TextBuffer buffer;
    buffer.insert_text("hello world test");
    CHECK(buffer.cursor().column == 16);

    buffer.move_to_line_start();
    CHECK(buffer.cursor().column == 0);

    buffer.move_word_forward();
    CHECK(buffer.cursor().column == 5); // after 'hello'

    buffer.move_word_forward();
    CHECK(buffer.cursor().column == 11); // after 'world'

    buffer.move_word_backward();
    CHECK(buffer.cursor().column == 6); // start of 'world'

    buffer.move_word_backward();
    CHECK(buffer.cursor().column == 0); // start of 'hello'

    buffer.move_to_line_end();
    CHECK(buffer.cursor().column == 16);

    buffer.jump_to("w", false);
    CHECK(buffer.cursor().column == 6);

    buffer.jump_to("t", true);
    CHECK(buffer.cursor().column == 12);
}

TEST_CASE("TextBuffer multiline jump_to searches across lines", "[tui][text_buffer]") {
    TextBuffer buffer;
    buffer.insert_text("line one\nline two\nline three");
    buffer.set_cursor(BufferCursor{.line = 0, .column = 0});

    buffer.jump_to("t", true);
    CHECK(buffer.cursor() == BufferCursor{.line = 1, .column = 5}); // 't' in 'two'

    buffer.jump_to("o", false);
    CHECK(buffer.cursor() == BufferCursor{.line = 0, .column = 5}); // 'o' in 'line one'
}

TEST_CASE("TextBuffer backspace and forward delete", "[tui][text_buffer]") {
    TextBuffer buffer;
    buffer.insert_text("abc");
    buffer.backspace();
    CHECK(buffer.text() == "ab");
    CHECK(buffer.cursor().column == 2);

    buffer.move_to_line_start();
    buffer.forward_delete();
    CHECK(buffer.text() == "b");
    CHECK(buffer.cursor().column == 0);

    // Multiline line joining
    buffer.set_text("first\nsecond");
    buffer.set_cursor(BufferCursor{.line = 1, .column = 0});
    buffer.backspace();
    CHECK(buffer.line_count() == 1);
    CHECK(buffer.text() == "firstsecond");
    CHECK(buffer.cursor() == BufferCursor{.line = 0, .column = 5});
}

TEST_CASE("TextBuffer Kill Ring and Yank Pop accumulation", "[tui][text_buffer]") {
    TextBuffer buffer;
    buffer.insert_text("one two three four");
    buffer.set_cursor(BufferCursor{.line = 0, .column = 8}); // after "one two "

    // Forward kill to line end
    buffer.kill_to_line_end();
    CHECK(buffer.text() == "one two ");

    buffer.set_cursor(BufferCursor{.line = 0, .column = 0});
    buffer.yank();
    CHECK(buffer.text() == "three fourone two ");

    // Word deletions with kill accumulation
    TextBuffer buffer2;
    buffer2.insert_text("alpha beta gamma");
    buffer2.set_cursor(BufferCursor{.line = 0, .column = 16});
    buffer2.delete_word_backward(); // deletes "gamma"
    buffer2.delete_word_backward(); // prepends "beta "
    CHECK(buffer2.text() == "alpha ");

    buffer2.set_cursor(BufferCursor{.line = 0, .column = 0});
    buffer2.yank();
    CHECK(buffer2.text() == "beta gammaalpha ");
}

TEST_CASE("TextBuffer Undo stack with typing coalescing", "[tui][text_buffer]") {
    TextBuffer buffer;
    // Typing coalescing: consecutive word characters coalesce
    buffer.insert_character("a");
    buffer.insert_character("b");
    buffer.insert_character("c");
    CHECK(buffer.text() == "abc");

    buffer.undo();
    CHECK(buffer.text().empty());

    // Whitespace breaks coalescing
    buffer.insert_character("a");
    buffer.insert_character(" ");
    buffer.insert_character("b");
    CHECK(buffer.text() == "a b");

    buffer.undo();
    CHECK(buffer.text() == "a");

    buffer.undo();
    CHECK(buffer.text().empty());
}

TEST_CASE("TextBuffer Paste markers in multiline mode", "[tui][text_buffer]") {
    TextBuffer buffer(TextBufferOptions{.multiline = true, .enable_paste_markers = true});
    std::string large_multiline = "line1\nline2\nline3\nline4\nline5\nline6\nline7\nline8\nline9\nline10\nline11\nline12";
    buffer.insert_paste(large_multiline);

    CHECK(buffer.text() == "[paste #1 +12 lines]");
    CHECK(buffer.expanded_text() == large_multiline);
    CHECK(buffer.cursor().line == 0);
    CHECK(buffer.cursor().column == 1); // 1 atomic segment

    // Moving over the marker
    buffer.move_left();
    CHECK(buffer.cursor().column == 0);
    buffer.move_right();
    CHECK(buffer.cursor().column == 1);

    // Deleting the marker
    buffer.backspace();
    CHECK(buffer.empty());
    CHECK(buffer.expanded_text().empty());
    CHECK(buffer.pastes().empty());
}
