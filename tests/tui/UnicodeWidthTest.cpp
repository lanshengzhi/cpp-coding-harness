#include "../../third_party/catch2/catch_test_macros.hpp"

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

TEST_CASE("visible_width handles regional indicators as width 2", "[tui][issue46][unicode]") {
    // U+1F1E6 U+1F1FA = regional indicator symbols (flag)
    const std::string flag = "\xf0\x9f\x87\xa6\xf0\x9f\x87\xba";
    CHECK(visible_width(flag) == 4); // Two RI symbols, each width 2
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

    // Regional indicator
    CHECK(codepoint_width(0x1F1E6) == 2); // Regional indicator A

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
    CHECK(r->size() == 1);
    CHECK(*r->data() == "hi");
}

TEST_CASE("wrap_text preserves literal newlines", "[tui][issue46][unicode]") {
    auto r = wrap_text("hello\nworld", 10);
    REQUIRE(r);
    CHECK(r->size() == 2);
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
    auto result = split_graphemes(cjk);
    REQUIRE(result.size() == 2);
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

TEST_CASE("normalize_terminal_output preserves LF and CR", "[tui][issue46][unicode]") {
    auto r = normalize_terminal_output("a\nb\rc");
    REQUIRE(r);
    CHECK(*r == "a\nb\rc");
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
    CHECK(reset == "\x1b[24m"); // underline off
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
