#include <cch/tui/Utils.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace cch::tui;

TEST_CASE("visible_width measures ASCII correctly", "[tui][issue46][unicode]") {
    CHECK(visible_width("") == 0);
    CHECK(visible_width("hello") == 5);
    CHECK(visible_width("hello world") == 11);
    CHECK(visible_width(" ") == 1);
    CHECK(visible_width("\t") == 3);
    CHECK(visible_width("a\tb") == 5); // 1 + 3 + 1
}

TEST_CASE("visible_width measures CJK characters as width 2", "[tui][issue46][unicode]") {
    const std::string zhong = "\xe4\xb8\xad";
    const std::string guo = "\xe5\x9b\xbd";
    CHECK(visible_width(zhong) == 2);
    CHECK(visible_width(zhong + guo) == 4);
    CHECK(visible_width(std::string("a") + zhong + guo + std::string("b")) == 6);
}

TEST_CASE("visible_width measures emoji as width 2", "[tui][issue46][unicode]") {
    // U+1F600 = grinning face
    CHECK(visible_width("\xf0\x9f\x98\x80") == 2);
    // U+1F44D = thumbs up
    CHECK(visible_width("\xf0\x9f\x91\x8d") == 2);
}

TEST_CASE("visible_width handles combining marks as width 0", "[tui][issue46][unicode]") {
    // U+0301 = combining acute accent
    const std::string accent = "\xcc\x81";
    CHECK(visible_width(accent) == 0);
    // e + combining acute = é (1 visible column)
    const std::string e_acute = "e\xcc\x81";
    CHECK(visible_width(e_acute) == 1);
}

TEST_CASE("visible_width strips ANSI escape codes", "[tui][issue46][unicode]") {
    CHECK(visible_width("\x1b[31mred\x1b[0m") == 3);
    CHECK(visible_width("\x1b[1mbold\x1b[22m") == 4);
    CHECK(visible_width("\x1b[38;5;240mgray\x1b[0m") == 4);
    CHECK(visible_width("\x1b[31m\x1b[1mred bold\x1b[0m") == 8);
}

TEST_CASE("visible_width handles OSC 8 hyperlinks", "[tui][issue46][unicode]") {
    const std::string link = "\x1b]8;;https://example.com\x07link\x1b]8;;\x07";
    CHECK(visible_width(link) == 4);
}

TEST_CASE("visible_width handles a regional indicator pair as one flag", "[tui][issue46][unicode]") {
    const std::string flag = "\xf0\x9f\x87\xa6\xf0\x9f\x87\xba";
    CHECK(visible_width(flag) == 2);
}

TEST_CASE("visible_width uses Unicode properties for symbols and combining marks", "[tui][issue46][unicode]") {
    CHECK(visible_width("\xe2\x94\x80") == 1); // U+2500 BOX DRAWINGS LIGHT HORIZONTAL
    CHECK(visible_width("\xe2\x9c\x93") == 1); // U+2713 CHECK MARK
    CHECK(visible_width("\xd7\x87") == 0); // U+05C7 HEBREW POINT QAMATS QATAN
    CHECK(visible_width("\xdb\x9e") == 1); // U+06DE ARABIC START OF RUB EL HIZB
}

TEST_CASE("truncate_text truncates ASCII correctly", "[tui][issue46][unicode]") {
    auto r = truncate_text("hello world", 5);
    REQUIRE(r);
    // Result includes ANSI reset codes around the ellipsis
    CHECK(visible_width(*r) == 5);
    CHECK(r->find("...") != std::string::npos);
}

TEST_CASE("truncate_text passes through short text", "[tui][issue46][unicode]") {
    auto r = truncate_text("hi", 10);
    REQUIRE(r);
    CHECK(*r == "hi");
}

TEST_CASE("truncate_text pads to width when requested", "[tui][issue46][unicode]") {
    auto r = truncate_text("hi", 10, "...", true);
    REQUIRE(r);
    // "hi" + 8 spaces = 10
    CHECK(visible_width(*r) == 10);
}

TEST_CASE("truncate_text handles CJK characters", "[tui][issue46][unicode]") {
    // 中 (width 2) + 国 (width 2) + ... (width 3) = 7
    // truncating to width 4 should keep at most 1 CJK char + ellipsis
    const std::string cjk = "\xe4\xb8\xad\xe5\x9b\xbd"; // 中国
    auto r = truncate_text(cjk, 4);
    REQUIRE(r);
    CHECK(visible_width(*r) <= 4);
}

TEST_CASE("truncate_text preserves ANSI styling at truncation boundary", "[tui][issue46][unicode]") {
    auto r = truncate_text("\x1b[31mhello world\x1b[0m", 5);
    REQUIRE(r);
    // Should produce styled "he..." with proper reset
    CHECK(r->find("\x1b[0m") != std::string::npos);
}

TEST_CASE("truncate_text handles zero width", "[tui][issue46][unicode]") {
    auto r = truncate_text("hello", 0);
    REQUIRE(r);
    CHECK(r->empty());
}

TEST_CASE("wrap_text keeps ANSI controls atomic and terminates every line", "[tui][issue46][unicode]") {
    const auto result = wrap_text("\x1b[31mABCD", 2);
    REQUIRE(result);
    REQUIRE(result->size() == 2);
    CHECK((*result)[0] == "\x1b[31mAB\x1b[0m");
    CHECK((*result)[1] == "\x1b[31mCD\x1b[0m");
}

TEST_CASE("wrap_text closes and reopens hyperlinks across physical lines", "[tui][issue46][unicode]") {
    const auto result = wrap_text("\x1b]8;;https://example.com\x07" "AB", 1);
    REQUIRE(result);
    REQUIRE(result->size() == 2);
    CHECK((*result)[0] == "\x1b]8;;https://example.com\x07" "A\x1b]8;;\x07");
    CHECK((*result)[1] == "\x1b]8;;https://example.com\x07" "B\x1b]8;;\x07");
}

TEST_CASE("wrap_text rejects a grapheme wider than the line", "[tui][issue46][unicode]") {
    const auto result = wrap_text("\xe4\xb8\xad", 1);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == cch::util::ErrorCode::Validation);
}

TEST_CASE("wrap_text preserves control ordering around whitespace", "[tui][issue46][unicode]") {
    const auto styled = wrap_text("\x1b[31mA \x1b[0mB", 10);
    REQUIRE(styled);
    REQUIRE(styled->size() == 1);
    CHECK((*styled)[0] == "\x1b[31mA \x1b[0mB");

    cch::tui::VirtualTerminal styled_terminal({.columns = 10, .rows = 1});
    REQUIRE(styled_terminal.start(
        [](std::string) {},
        [](cch::tui::TerminalDimensions) {}));
    REQUIRE(styled_terminal.write((*styled)[0]));
    REQUIRE(styled_terminal.cells().size() == 1);
    REQUIRE(styled_terminal.cells()[0].size() == 10);
    CHECK(styled_terminal.cells()[0][0].style.fg_color == "31");
    CHECK(styled_terminal.cells()[0][1].grapheme == " ");
    CHECK(styled_terminal.cells()[0][1].style.fg_color == "31");
    CHECK(styled_terminal.cells()[0][2].grapheme == "B");
    CHECK(styled_terminal.cells()[0][2].style.fg_color.empty());
    CHECK(styled_terminal.final_style() == cch::tui::VirtualTerminalStyle{});

    const auto linked = wrap_text("A \x1b]8;;u\x07" "B\x1b]8;;\x07", 10);
    REQUIRE(linked);
    REQUIRE(linked->size() == 1);
    CHECK((*linked)[0] == "A \x1b]8;;u\x07" "B\x1b]8;;\x07");

    cch::tui::VirtualTerminal linked_terminal({.columns = 10, .rows = 1});
    REQUIRE(linked_terminal.start(
        [](std::string) {},
        [](cch::tui::TerminalDimensions) {}));
    REQUIRE(linked_terminal.write((*linked)[0]));
    CHECK(linked_terminal.cells()[0][1].grapheme == " ");
    CHECK(linked_terminal.cells()[0][1].style.hyperlink.empty());
    CHECK(linked_terminal.cells()[0][2].style.hyperlink == "u");
    CHECK(linked_terminal.final_style() == cch::tui::VirtualTerminalStyle{});

    const auto trailing = wrap_text("\x1b[31mA  ", 10);
    REQUIRE(trailing);
    REQUIRE(trailing->size() == 1);
    cch::tui::VirtualTerminal trailing_terminal({.columns = 10, .rows = 1});
    REQUIRE(trailing_terminal.start(
        [](std::string) {},
        [](cch::tui::TerminalDimensions) {}));
    REQUIRE(trailing_terminal.write((*trailing)[0]));
    CHECK(trailing_terminal.cells()[0][1].grapheme == " ");
    CHECK(trailing_terminal.cells()[0][2].grapheme == " ");
    CHECK(trailing_terminal.cells()[0][1].style.fg_color == "31");
    CHECK(trailing_terminal.cells()[0][2].style.fg_color == "31");
    CHECK(trailing_terminal.final_style() == cch::tui::VirtualTerminalStyle{});

    const auto wrapped = wrap_text("\x1b[31mA \x1b[0mB", 2);
    REQUIRE(wrapped);
    REQUIRE(wrapped->size() == 2);
    cch::tui::VirtualTerminal wrapped_terminal({.columns = 2, .rows = 2});
    REQUIRE(wrapped_terminal.start(
        [](std::string) {},
        [](cch::tui::TerminalDimensions) {}));
    REQUIRE(wrapped_terminal.write((*wrapped)[0]));
    CHECK(wrapped_terminal.final_style() == cch::tui::VirtualTerminalStyle{});
    REQUIRE(wrapped_terminal.set_cursor({.column = 0, .row = 1}));
    REQUIRE(wrapped_terminal.write((*wrapped)[1]));
    CHECK(wrapped_terminal.cells()[0][0].style.fg_color == "31");
    CHECK(wrapped_terminal.cells()[1][0].style.fg_color.empty());
    CHECK(wrapped_terminal.final_style() == cch::tui::VirtualTerminalStyle{});
}

TEST_CASE("wrap_text prefers word boundaries and falls back for long words", "[tui][issue46][unicode]") {
    const auto words = wrap_text("hello world", 7);
    REQUIRE(words);
    REQUIRE(words->size() == 2);
    CHECK((*words)[0] == "hello");
    CHECK((*words)[1] == "world");

    const auto styled = wrap_text("\x1b[31mhello world", 7);
    REQUIRE(styled);
    REQUIRE(styled->size() == 2);
    CHECK((*styled)[0] == "\x1b[31mhello\x1b[0m");
    CHECK((*styled)[1] == "\x1b[31mworld\x1b[0m");

    const auto long_word = wrap_text("abcdefgh", 3);
    REQUIRE(long_word);
    REQUIRE(long_word->size() == 3);
    CHECK((*long_word)[0] == "abc");
    CHECK((*long_word)[1] == "def");
    CHECK((*long_word)[2] == "gh");
}

TEST_CASE("wrap_text wraps long ASCII text", "[tui][issue46][unicode]") {
    auto r = wrap_text("hello world foo bar", 5);
    REQUIRE(r);
    CHECK(r->size() >= 3);
    for (const auto& line : *r) {
        CHECK(visible_width(line) <= 5);
    }
}

TEST_CASE("wrap_text does not wrap short text", "[tui][issue46][unicode]") {
    auto r = wrap_text("hi", 10);
    REQUIRE(r);
    REQUIRE(r->size() == 1);
    CHECK(*r->data() == "hi");
}

TEST_CASE("wrap_text preserves literal newlines", "[tui][issue46][unicode]") {
    auto r = wrap_text("hello\nworld", 10);
    REQUIRE(r);
    CHECK(r->size() == 2);
}

TEST_CASE("wrap_text drops a separator when the next wide grapheme cannot fit", "[tui][issue46][unicode]") {
    const auto plain = wrap_text("abc 中文测", 4);
    REQUIRE(plain);
    const std::vector<std::string> expected_plain{"abc", "中文", "测"};
    CHECK(*plain == expected_plain);

    const auto styled = wrap_text("abc \x1b[31m中文测", 4);
    REQUIRE(styled);
    const std::vector<std::string> expected_styled{
        "abc",
        "\x1b[31m中文\x1b[0m",
        "\x1b[31m测\x1b[0m",
    };
    CHECK(*styled == expected_styled);

    const auto linked = wrap_text("abc \x1b]8;;u\x07中文测\x1b]8;;\x07", 4);
    REQUIRE(linked);
    const std::vector<std::string> expected_linked{
        "abc",
        "\x1b]8;;u\x07中文\x1b]8;;\x07",
        "\x1b]8;;u\x07测\x1b]8;;\x07",
    };
    CHECK(*linked == expected_linked);
}

TEST_CASE("wrap_text fills the current line before breaking a long CJK run", "[tui][issue46][unicode]") {
    const std::string text = "This is an example 中文汉字测试段落内容中文汉字测试段落内容.";
    const auto result = wrap_text(text, 40);
    REQUIRE(result);
    const std::vector<std::string> expected{
        "This is an example 中文汉字测试段落内容",
        "中文汉字测试段落内容.",
    };
    CHECK(*result == expected);

    const auto styled = wrap_text("\x1b[31m" + text + "\x1b[0m", 40);
    REQUIRE(styled);
    REQUIRE(styled->size() == 2);
    CHECK((*styled)[0] == "\x1b[31mThis is an example 中文汉字测试段落内容\x1b[0m");
    CHECK((*styled)[1] == "\x1b[31m中文汉字测试段落内容.\x1b[0m");
    CHECK(visible_width((*styled)[0]) <= 40);
    CHECK(visible_width((*styled)[1]) <= 40);
}

TEST_CASE("wrap_text handles CJK word wrapping", "[tui][issue46][unicode]") {
    // Long CJK string that needs wrapping
    const std::string long_cjk =
        "\xe4\xb8\xad\xe5\x9b\xbd\xe4\xb8\xad\xe5\x9b\xbd" // 中国中国
        "\xe4\xb8\xad\xe5\x9b\xbd\xe4\xb8\xad\xe5\x9b\xbd"; // 中国中国
    auto r = wrap_text(long_cjk, 4);
    REQUIRE(r);
    for (const auto& line : *r) {
        CHECK(visible_width(line) <= 4);
    }
}

TEST_CASE("wrap_text rejects zero width", "[tui][issue46][unicode]") {
    auto r = wrap_text("hello", 0);
    REQUIRE_FALSE(r);
}

TEST_CASE("slice_by_column extracts a plain ASCII range", "[tui][issue46][unicode]") {
    const auto sliced = slice_by_column("hello world", 0, 5);
    REQUIRE(sliced);
    CHECK(*sliced == "hello");

    const auto tail = slice_by_column("hello world", 6, 6);
    REQUIRE(tail);
    CHECK(*tail == "world");
}

TEST_CASE("slice_by_column carries leading styling into the slice", "[tui][issue46][unicode]") {
    const auto sliced = slice_by_column("\x1b[31mred\x1b[0m blue", 1, 2);
    REQUIRE(sliced);
    CHECK(*sliced == "\x1b[31med");
    CHECK(visible_width(*sliced) == 2);
}

TEST_CASE("slice_by_column excludes a wide grapheme crossing the range end when strict", "[tui][issue46][unicode]") {
    const std::string line = "abcd\xe8\xae\xa9" "EFGH"; // abcd让EFGH (让 spans columns 4-5)
    const auto loose = slice_by_column(line, 4, 1, false);
    REQUIRE(loose);
    CHECK(visible_width(*loose) == 2); // wide grapheme included, extending past the end

    const auto strict = slice_by_column(line, 4, 1, true);
    REQUIRE(strict);
    CHECK(strict->empty()); // wide grapheme excluded at the strict boundary

    const auto ascii = slice_by_column(line, 6, 4, true);
    REQUIRE(ascii);
    CHECK(*ascii == "EFGH");
}

TEST_CASE("slice_by_column handles empty ranges and out-of-range starts", "[tui][issue46][unicode]") {
    const auto empty = slice_by_column("hello", 0, 0);
    REQUIRE(empty);
    CHECK(empty->empty());

    const auto past_end = slice_by_column("hello", 10, 3);
    REQUIRE(past_end);
    CHECK(past_end->empty());
}

TEST_CASE("strip_terminal_sequences removes SGR and hyperlink codes", "[tui][issue46][unicode]") {
    CHECK(strip_terminal_sequences("\x1b[31mred\x1b[0m") == "red");
    CHECK(strip_terminal_sequences("\x1b]8;;https://example.com\x07link\x1b]8;;\x07") == "link");
    CHECK(strip_terminal_sequences("plain") == "plain");
    CHECK(strip_terminal_sequences("\x1b[1;38;5;240mstyled\x1b[0m text") == "styled text");
}

TEST_CASE("strip_terminal_sequences preserves visible text and newlines", "[tui][issue46][unicode]") {
    CHECK(strip_terminal_sequences("a\x1b[31mb\x1b[0m\nc") == "ab\nc");
}

TEST_CASE("strip_terminal_sequences strips ST-terminated and unterminated sequences", "[tui][issue46][unicode]") {
    CHECK(strip_terminal_sequences("\x1b]8;;u\x1b\\" "text") == "text");
    // An unterminated OSC is not a recognized sequence; the ESC byte survives.
    CHECK(strip_terminal_sequences("\x1b]0;title") == "\x1b]0;title");
}

TEST_CASE("strip_terminal_sequences strips cursor-movement finals like pi", "[tui][issue46][unicode]") {
    CHECK(strip_terminal_sequences("a\x1b[2Gb") == "ab");
    CHECK(strip_terminal_sequences("a\x1b[2Kb") == "ab");
    CHECK(strip_terminal_sequences("a\x1b[2Jb") == "ab");
    CHECK(strip_terminal_sequences("a\x1b[2Hb") == "ab");
    // A CSI sequence with an unrecognized final byte is preserved byte for byte.
    CHECK(strip_terminal_sequences("a\x1b[2zb") == "a\x1b[2zb");
}
