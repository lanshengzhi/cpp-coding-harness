#include "ai/BoundedText.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

using namespace cch;

TEST_CASE("bounded_utf8 passes ASCII through and truncates at the byte budget", "[ai][bounded-text][issue66]") {
    CHECK(ai::bounded_utf8("hello", 10) == "hello");
    CHECK(ai::bounded_utf8("hello world", 5) == "hello");
    CHECK(ai::bounded_utf8("hello", 0).empty());
}

TEST_CASE("bounded_utf8 never splits a multibyte sequence", "[ai][bounded-text][issue66]") {
    const std::string text = "a\xc3\xa9" "b"; // a é b
    CHECK(ai::bounded_utf8(text, 2) == "a");
    CHECK(ai::bounded_utf8(text, 3) == "a\xc3\xa9");
    CHECK(ai::bounded_utf8(text, 4) == "a\xc3\xa9" "b");

    const std::string emoji = "\xf0\x9f\x99\x82"; // U+1F642
    CHECK(ai::bounded_utf8(emoji, 3).empty());
    CHECK(ai::bounded_utf8(emoji, 4) == emoji);
}

TEST_CASE("bounded_utf8 replaces invalid sequences with U+FFFD", "[ai][bounded-text][issue66]") {
    CHECK(ai::bounded_utf8("\xff", 10) == "\xef\xbf\xbd");
    CHECK(ai::bounded_utf8("a\xc3", 10) == "a\xef\xbf\xbd");
    CHECK(ai::bounded_utf8("\xc3\x28", 10) == "\xef\xbf\xbd" "(");
    // Rejected surrogate half: every byte becomes a replacement character.
    CHECK(ai::bounded_utf8("\xed\xa0\x80", 16) == "\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd");
    // The replacement itself respects the budget.
    CHECK(ai::bounded_utf8("\xff", 2).empty());
}

TEST_CASE("bounded_text returns the input when it fits", "[ai][bounded-text][issue66]") {
    CHECK(ai::bounded_text("hi", 10) == "hi");
    CHECK(ai::bounded_text("anything", 0).empty());
}

TEST_CASE("bounded_text reserves room for the suffix when truncating", "[ai][bounded-text][issue66]") {
    CHECK(ai::bounded_text("hello world", 8, "..") == "hello ..");
    CHECK(ai::bounded_text("hello", 2, "...") == "..");
}

TEST_CASE("bounded_text force_truncated appends the suffix even when the input fits", "[ai][bounded-text][issue66]") {
    CHECK(ai::bounded_text("abc", 5, "..", true) == "abc..");
    CHECK(ai::bounded_text("abc", 5, "", true) == "abc");
}

TEST_CASE("bounded_redacted_text returns empty for a zero budget", "[ai][bounded-text][issue66]") {
    CHECK(ai::bounded_redacted_text("api_key=secret", 0).empty());
}

TEST_CASE("bounded_redacted_text redacts before truncating so secrets never leak", "[ai][bounded-text][issue66]") {
    const std::string text = "api_key=" + std::string(300, 's');
    const auto result = ai::bounded_redacted_text(text, 20);
    CHECK(result == "api_key=[REDACTED]");
    CHECK(result.find("sss") == std::string::npos);
}

TEST_CASE("bounded_redacted_text never splits the redaction marker", "[ai][bounded-text][issue66]") {
    // After redaction the marker starts at byte 11; a budget of 13 lands inside it.
    const auto result = ai::bounded_redacted_text("XX api_key=secret123", 13);
    CHECK(result == "XX [REDACTED]");
    CHECK(result.size() == 13);
}

TEST_CASE("bounded_redacted_text keeps a marker that fits the budget", "[ai][bounded-text][issue66]") {
    CHECK(ai::bounded_redacted_text("api_key=x", 100) == "api_key=[REDACTED]");
}

TEST_CASE("bounded_redacted_text bounds plain text on a UTF-8 boundary", "[ai][bounded-text][issue66]") {
    const std::string accented = "ab" "\xc3\xa9\xc3\xa9\xc3\xa9"; // ab followed by three é
    CHECK(ai::bounded_redacted_text(accented, 5) == "ab\xc3\xa9");
    CHECK(ai::bounded_redacted_text("abcdef", 5) == "abcde");
}

TEST_CASE("bounded_redacted_text appends the suffix when truncating plain text", "[ai][bounded-text][issue66]") {
    const auto result = ai::bounded_redacted_text(std::string(100, 'a'), 10, "..");
    CHECK(result == "aaaaaaaa..");
}
