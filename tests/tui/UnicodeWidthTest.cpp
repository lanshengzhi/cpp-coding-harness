#include "../../third_party/catch2/catch_test_macros.hpp"

#include <cch/tui/Text.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include "tui/UnicodeWidth.hpp"

#include <string>
#include <vector>

using namespace cch::tui::detail;

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
    const auto graphemes = split_graphemes(flag);
    REQUIRE(graphemes.size() == 1);
    CHECK(graphemes[0] == flag);
}

TEST_CASE("visible_width uses Unicode properties for symbols and combining marks", "[tui][issue46][unicode]") {
    CHECK(visible_width("\xe2\x94\x80") == 1); // U+2500 BOX DRAWINGS LIGHT HORIZONTAL
    CHECK(visible_width("\xe2\x9c\x93") == 1); // U+2713 CHECK MARK
    CHECK(visible_width("\xd7\x87") == 0); // U+05C7 HEBREW POINT QAMATS QATAN
    CHECK(visible_width("\xdb\x9e") == 1); // U+06DE ARABIC START OF RUB EL HIZB

    const std::string hebrew_cluster = "\xd7\x90\xd7\x87"; // א + U+05C7
    const auto graphemes = split_graphemes(hebrew_cluster);
    REQUIRE(graphemes.size() == 1);
    CHECK(visible_width(hebrew_cluster) == 1);
}

TEST_CASE("grapheme_width honors regional indicators and presentation selectors", "[tui][issue46][unicode]") {
    CHECK(visible_width("\xf0\x9f\x87\xa8") == 2); // isolated regional indicator C
    CHECK(visible_width("\xef\xb8\x8f") == 0); // standalone VS16
    CHECK(visible_width("A\xef\xb8\x8f") == 1); // VS16 does not promote a non-emoji base
    CHECK(visible_width("\xe2\x9d\xa4") == 1); // U+2764 text by default
    CHECK(visible_width("\xe2\x9d\xa4\xef\xb8\x8f") == 2); // VS16 emoji presentation
    CHECK(visible_width("\xe2\x9d\xa4\xef\xb8\x8e") == 1); // VS15 text presentation
}

TEST_CASE("visible_width and grapheme splitting handle decomposed Hangul", "[tui][issue46][unicode]") {
    const std::string hangul = "\xe1\x84\x80\xe1\x85\xa1\xe1\x86\xa8"; // 각
    const auto graphemes = split_graphemes(hangul);
    REQUIRE(graphemes.size() == 1);
    CHECK(graphemes[0] == hangul);
    CHECK(grapheme_width(graphemes[0]) == 2);
    CHECK(visible_width(hangul) == 2);
}

TEST_CASE("grapheme width counts trailing Thai and Lao AM vowels", "[tui][issue46][unicode]") {
    const std::string thai = "\xe0\xb8\x81\xe0\xb8\xb3"; // กำ
    const auto thai_graphemes = split_graphemes(thai);
    REQUIRE(thai_graphemes.size() == 1);
    CHECK(grapheme_width(thai_graphemes[0]) == 2);
    CHECK(visible_width(thai) == 2);

    const std::string lao = "\xe0\xba\x81\xe0\xba\xb3"; // ກຳ
    const auto lao_graphemes = split_graphemes(lao);
    REQUIRE(lao_graphemes.size() == 1);
    CHECK(grapheme_width(lao_graphemes[0]) == 2);
    CHECK(visible_width(lao) == 2);

    cch::tui::Text text(thai, 0, 0);
    REQUIRE_FALSE(text.render(1));
    const auto rendered = text.render(2);
    REQUIRE(rendered);
    REQUIRE(rendered->size() == 1);
    CHECK(visible_width((*rendered)[0]) == 2);

    cch::tui::VirtualTerminal narrow_terminal({.columns = 1, .rows = 1});
    REQUIRE(narrow_terminal.start(
        [](std::string) {},
        [](cch::tui::TerminalDimensions) {}));
    REQUIRE_FALSE(narrow_terminal.write(thai));

    cch::tui::VirtualTerminal terminal({.columns = 2, .rows = 1});
    REQUIRE(terminal.start(
        [](std::string) {},
        [](cch::tui::TerminalDimensions) {}));
    REQUIRE(terminal.write(thai));
    REQUIRE(terminal.cells().size() == 1);
    REQUIRE(terminal.cells()[0].size() == 2);
    CHECK(terminal.cells()[0][0].grapheme == thai);
    CHECK(terminal.cells()[0][1].continuation);
    const cch::tui::CursorPosition expected_cursor{.column = 2, .row = 0};
    CHECK(terminal.cursor() == expected_cursor);
}

TEST_CASE("codepoint_width returns correct widths for various categories", "[tui][issue46][unicode]") {
    // ASCII
    CHECK(codepoint_width('a') == 1);
    CHECK(codepoint_width(' ') == 1);
    CHECK(codepoint_width('Z') == 1);

    // CJK
    CHECK(codepoint_width(0x4E2D) == 2); // 中
    CHECK(codepoint_width(0x56FD) == 2); // 国
    CHECK(codepoint_width(0x30A2) == 2); // ア (Katakana)

    // Combining mark
    CHECK(codepoint_width(0x0301) == 0); // combining acute

    // Emoji
    CHECK(codepoint_width(0x1F600) == 2); // grinning face

    // Regional indicators are width 1 individually and width 2 as a flag pair.
    CHECK(codepoint_width(0x1F1E6) == 1); // Regional indicator A

    // Control chars (not tab/newline)
    CHECK(codepoint_width(0x00) == 0);
    CHECK(codepoint_width(0x01) == 0);
    CHECK(codepoint_width(0x7F) == 0);

    // Tab, LF
    CHECK(codepoint_width(0x09) == 3); // tab
}

TEST_CASE("extract_ansi_code parses CSI sequences", "[tui][issue46][unicode]") {
    std::string_view text = "\x1b[31mhello";
    auto result = extract_ansi_code(text, 0);
    REQUIRE(result);
    CHECK(result->code == "\x1b[31m");
    CHECK(result->length == 5);
}

TEST_CASE("extract_ansi_code parses OSC 8 hyperlinks (BEL-terminated)", "[tui][issue46][unicode]") {
    std::string_view text = "\x1b]8;;https://example.com\x07link";
    auto result = extract_ansi_code(text, 0);
    REQUIRE(result);
    CHECK(result->code == "\x1b]8;;https://example.com\x07");
    CHECK(result->length > 0);
}

TEST_CASE("extract_ansi_code returns nullopt for plain text", "[tui][issue46][unicode]") {
    CHECK_FALSE(extract_ansi_code("hello", 0));
    CHECK_FALSE(extract_ansi_code("hello", 3));
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

TEST_CASE("normalize_terminal_output expands tabs outside ANSI", "[tui][issue46][unicode]") {
    auto r = normalize_terminal_output("a\tb");
    REQUIRE(r);
    CHECK(*r == "a   b");
}

TEST_CASE("normalize_terminal_output preserves ANSI codes", "[tui][issue46][unicode]") {
    auto r = normalize_terminal_output("\x1b[31mhello\x1b[0m");
    REQUIRE(r);
    CHECK(*r == "\x1b[31mhello\x1b[0m");
}

TEST_CASE("normalize_terminal_output replaces malformed UTF-8", "[tui][issue46][unicode]") {
    // 0xFF is an invalid lead byte
    auto r = normalize_terminal_output("a\xff""b");
    REQUIRE(r);
    // Should contain U+FFD (EF BF BD)
    CHECK(r->find("\xef\xbf\xbd") != std::string::npos);
}

TEST_CASE("normalize_terminal_output rejects unsupported control chars", "[tui][issue46][unicode]") {
    auto r = normalize_terminal_output("a\x01""b");
    REQUIRE_FALSE(r);
    CHECK(r.error().code == cch::util::ErrorCode::Validation);
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

TEST_CASE("split_graphemes splits ASCII", "[tui][issue46][unicode]") {
    auto result = split_graphemes("abc");
    REQUIRE(result.size() == 3);
    CHECK(result[0] == "a");
    CHECK(result[1] == "b");
    CHECK(result[2] == "c");
}

TEST_CASE("split_graphemes keeps combining marks with their base", "[tui][issue46][unicode]") {
    const std::string e_acute = "e\xcc\x81";
    auto result = split_graphemes(e_acute);
    REQUIRE(result.size() == 1);
    CHECK(result[0] == "e\xcc\x81");
}

TEST_CASE("split_graphemes handles CJK characters", "[tui][issue46][unicode]") {
    const std::string cjk = "\xe4\xb8\xad\xe5\x9b\xbd"; // 中国
    const auto result = split_graphemes(cjk);
    REQUIRE(result.size() == 2);
}

TEST_CASE("split_graphemes keeps promised emoji sequences atomic", "[tui][issue46][unicode]") {
    const std::string toned_thumb = "\xf0\x9f\x91\x8d\xf0\x9f\x8f\xbd";
    const std::string keycap = "1\xef\xb8\x8f\xe2\x83\xa3";
    const std::string family =
        "\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9\xe2\x80\x8d"
        "\xf0\x9f\x91\xa7\xe2\x80\x8d\xf0\x9f\x91\xa6";

    const auto thumb_clusters = split_graphemes(toned_thumb);
    const auto keycap_clusters = split_graphemes(keycap);
    const auto family_clusters = split_graphemes(family);
    REQUIRE(thumb_clusters.size() == 1);
    REQUIRE(keycap_clusters.size() == 1);
    REQUIRE(family_clusters.size() == 1);
    CHECK(grapheme_width(thumb_clusters[0]) == 2);
    CHECK(grapheme_width(keycap_clusters[0]) == 2);
    CHECK(grapheme_width(family_clusters[0]) == 2);
}

TEST_CASE("grapheme_width measures ASCII as width 1", "[tui][issue46][unicode]") {
    CHECK(grapheme_width("a") == 1);
    CHECK(grapheme_width(" ") == 1);
}

TEST_CASE("grapheme_width measures combining clusters as base width", "[tui][issue46][unicode]") {
    CHECK(grapheme_width("e\xcc\x81") == 1); // e + combining acute
}

TEST_CASE("grapheme_width measures CJK as width 2", "[tui][issue46][unicode]") {
    CHECK(grapheme_width("\xe4\xb8\xad") == 2); // 中
}

TEST_CASE("grapheme_width measures emoji as width 2", "[tui][issue46][unicode]") {
    CHECK(grapheme_width("\xf0\x9f\x98\x80") == 2); // grinning face
}

TEST_CASE("normalize_terminal_output normalizes CRLF and lone CR", "[tui][issue46][unicode]") {
    const auto result = normalize_terminal_output("a\r\nb\rc");
    REQUIRE(result);
    CHECK(*result == "a\nb\nc");
}

TEST_CASE("normalize_terminal_output rejects unsafe terminal controls", "[tui][issue46][unicode]") {
    CHECK_FALSE(normalize_terminal_output("\x1b[10Gx"));
    CHECK_FALSE(normalize_terminal_output("\x1b" "7x"));
    CHECK_FALSE(normalize_terminal_output("\x1b_X\x1b\\"));
    CHECK_FALSE(normalize_terminal_output("\x1b]0;title\x07"));
    CHECK_FALSE(normalize_terminal_output("\x1b[999999999999999999999m"));
    CHECK_FALSE(normalize_terminal_output("a\x7f" "b"));
    CHECK_FALSE(normalize_terminal_output("a\xc2\x85" "b"));
}

TEST_CASE("normalize_terminal_output accepts only fully tracked SGR forms", "[tui][issue46][unicode]") {
    CHECK(normalize_terminal_output("\x1b[1;31mA\x1b[22;39m"));
    CHECK(normalize_terminal_output("\x1b[38;5;240mA\x1b[39m"));
    CHECK(normalize_terminal_output("\x1b[48;2;1;2;3mA\x1b[49m"));

    CHECK_FALSE(normalize_terminal_output("\x1b[6mA"));
    CHECK_FALSE(normalize_terminal_output("\x1b[53mA"));
    CHECK_FALSE(prepare_rendered_line("\x1b[6mA", 1));
    CHECK_FALSE(prepare_rendered_line("\x1b[53mA", 1));
    CHECK_FALSE(normalize_terminal_output("\x1b[38;5mA"));
    CHECK_FALSE(normalize_terminal_output("\x1b[38;5;256mA"));
    CHECK_FALSE(normalize_terminal_output("\x1b[48;2;1;2mA"));
    CHECK_FALSE(normalize_terminal_output("\x1b[48;2;1;2;999mA"));
}

TEST_CASE("AnsiStyleState tracks simple SGR codes", "[tui][issue46][unicode]") {
    AnsiStyleState state;
    state.process_ansi("\x1b[31m");
    CHECK(state.fg_color == "31");
    CHECK(state.has_active_codes());

    state.process_ansi("\x1b[0m");
    CHECK_FALSE(state.has_active_codes());
}

TEST_CASE("AnsiStyleState tracks multiple attributes", "[tui][issue46][unicode]") {
    AnsiStyleState state;
    state.process_ansi("\x1b[1;31m");
    CHECK(state.bold);
    CHECK(state.fg_color == "31");
    CHECK(state.get_active_codes().find("1") != std::string::npos);
    CHECK(state.get_active_codes().find("31") != std::string::npos);
}

TEST_CASE("AnsiStyleState generates line-end reset for underline", "[tui][issue46][unicode]") {
    AnsiStyleState state;
    state.process_ansi("\x1b[4m");
    CHECK(state.underline);
    auto reset = state.get_line_end_reset();
    CHECK(reset == "\x1b[0m"); // all SGR state is terminated at the line boundary
}

TEST_CASE("AnsiStyleState handles OSC 8 hyperlinks", "[tui][issue46][unicode]") {
    AnsiStyleState state;
    state.process_ansi("\x1b]8;;https://example.com\x07");
    CHECK(state.hyperlink == "https://example.com");
    CHECK(state.has_active_codes());

    // Close hyperlink
    state.process_ansi("\x1b]8;;\x07");
    CHECK(state.hyperlink.empty());
}

TEST_CASE("wrap_text rejects zero width", "[tui][issue46][unicode]") {
    auto r = wrap_text("hello", 0);
    REQUIRE_FALSE(r);
}

TEST_CASE("truncate_text handles zero width", "[tui][issue46][unicode]") {
    auto r = truncate_text("hello", 0);
    REQUIRE(r);
    CHECK(r->empty());
}
