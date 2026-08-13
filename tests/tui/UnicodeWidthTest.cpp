#include <cch/tui/Text.hpp>
#include <cch/tui/VirtualTerminal.hpp>
#include <cch/tui/Utils.hpp>

#include "tui/UnicodeWidth.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace cch::tui::detail;
TEST_CASE("grapheme_width honors regional indicators and presentation selectors", "[tui][issue46][unicode]") {
    CHECK(cch::tui::visible_width("\xf0\x9f\x87\xa8") == 2); // isolated regional indicator C
    CHECK(cch::tui::visible_width("\xef\xb8\x8f") == 0); // standalone VS16
    CHECK(cch::tui::visible_width("A\xef\xb8\x8f") == 1); // VS16 does not promote a non-emoji base
    CHECK(cch::tui::visible_width("\xe2\x9d\xa4") == 1); // U+2764 text by default
    CHECK(cch::tui::visible_width("\xe2\x9d\xa4\xef\xb8\x8f") == 2); // VS16 emoji presentation
    CHECK(cch::tui::visible_width("\xe2\x9d\xa4\xef\xb8\x8e") == 1); // VS15 text presentation
}

TEST_CASE("visible_width and grapheme splitting handle decomposed Hangul", "[tui][issue46][unicode]") {
    const std::string hangul = "\xe1\x84\x80\xe1\x85\xa1\xe1\x86\xa8"; // 각
    const auto graphemes = split_graphemes(hangul);
    REQUIRE(graphemes.size() == 1);
    CHECK(graphemes[0] == hangul);
    CHECK(grapheme_width(graphemes[0]) == 2);
    CHECK(cch::tui::visible_width(hangul) == 2);
}

TEST_CASE("grapheme width counts trailing Thai and Lao AM vowels", "[tui][issue46][unicode]") {
    const std::string thai = "\xe0\xb8\x81\xe0\xb8\xb3"; // กำ
    const auto thai_graphemes = split_graphemes(thai);
    REQUIRE(thai_graphemes.size() == 1);
    CHECK(grapheme_width(thai_graphemes[0]) == 2);
    CHECK(cch::tui::visible_width(thai) == 2);

    const std::string lao = "\xe0\xba\x81\xe0\xba\xb3"; // ກຳ
    const auto lao_graphemes = split_graphemes(lao);
    REQUIRE(lao_graphemes.size() == 1);
    CHECK(grapheme_width(lao_graphemes[0]) == 2);
    CHECK(cch::tui::visible_width(lao) == 2);

    cch::tui::Text text(thai, 0, 0);
    REQUIRE_FALSE(text.render(1));
    const auto rendered = text.render(2);
    REQUIRE(rendered);
    REQUIRE(rendered->lines.size() == 1);
    CHECK(cch::tui::visible_width(rendered->lines[0]) == 2);

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
