#include "util/BoundedText.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

using namespace cch;

TEST_CASE("bounded_utf8 passes ASCII through and truncates at the byte budget", "[util][bounded-text][issue66]") {
    CHECK(util::bounded_utf8("hello", 10) == "hello");
    CHECK(util::bounded_utf8("hello world", 5) == "hello");
    CHECK(util::bounded_utf8("hello", 0).empty());
}

TEST_CASE("bounded_utf8 never splits a multibyte sequence", "[util][bounded-text][issue66]") {
    const std::string text = "a\xc3\xa9" "b"; // a é b
    CHECK(util::bounded_utf8(text, 2) == "a");
    CHECK(util::bounded_utf8(text, 3) == "a\xc3\xa9");
    CHECK(util::bounded_utf8(text, 4) == "a\xc3\xa9" "b");

    const std::string emoji = "\xf0\x9f\x99\x82"; // U+1F642
    CHECK(util::bounded_utf8(emoji, 3).empty());
    CHECK(util::bounded_utf8(emoji, 4) == emoji);
}

TEST_CASE("bounded_utf8 replaces invalid sequences with U+FFFD", "[util][bounded-text][issue66]") {
    CHECK(util::bounded_utf8("\xff", 10) == "\xef\xbf\xbd");
    CHECK(util::bounded_utf8("a\xc3", 10) == "a\xef\xbf\xbd");
    CHECK(util::bounded_utf8("\xc3\x28", 10) == "\xef\xbf\xbd" "(");
    // Rejected surrogate half: every byte becomes a replacement character.
    CHECK(util::bounded_utf8("\xed\xa0\x80", 16) == "\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd");
    // The replacement itself respects the budget.
    CHECK(util::bounded_utf8("\xff", 2).empty());
}

TEST_CASE("bounded_text returns the input when it fits", "[util][bounded-text][issue66]") {
    CHECK(util::bounded_text("hi", 10) == "hi");
    CHECK(util::bounded_text("anything", 0).empty());
}

TEST_CASE("bounded_text reserves room for the suffix when truncating", "[util][bounded-text][issue66]") {
    CHECK(util::bounded_text("hello world", 8, "..") == "hello ..");
    CHECK(util::bounded_text("hello", 2, "...") == "..");
}

TEST_CASE("bounded_text force_truncated appends the suffix even when the input fits", "[util][bounded-text][issue66]") {
    CHECK(util::bounded_text("abc", 5, "..", true) == "abc..");
    CHECK(util::bounded_text("abc", 5, "", true) == "abc");
}

TEST_CASE("bounded_redacted_text returns empty for a zero budget", "[util][bounded-text][issue66]") {
    CHECK(util::bounded_redacted_text("api_key=secret", 0).empty());
}

TEST_CASE("bounded_redacted_text redacts before truncating so secrets never leak", "[util][bounded-text][issue66]") {
    const std::string text = "api_key=" + std::string(300, 's');
    const auto result = util::bounded_redacted_text(text, 20);
    CHECK(result == "api_key=[REDACTED]");
    CHECK(result.find("sss") == std::string::npos);
}

TEST_CASE("bounded_redacted_text never splits the redaction marker", "[util][bounded-text][issue66]") {
    // After redaction the marker starts at byte 11; a budget of 13 lands inside it.
    const auto result = util::bounded_redacted_text("XX api_key=secret123", 13);
    CHECK(result == "XX [REDACTED]");
    CHECK(result.size() == 13);
}

TEST_CASE("bounded_redacted_text keeps a marker that fits the budget", "[util][bounded-text][issue66]") {
    CHECK(util::bounded_redacted_text("api_key=x", 100) == "api_key=[REDACTED]");
}

TEST_CASE("bounded_redacted_text bounds plain text on a UTF-8 boundary", "[util][bounded-text][issue66]") {
    const std::string accented = "ab" "\xc3\xa9\xc3\xa9\xc3\xa9"; // ab followed by three é
    CHECK(util::bounded_redacted_text(accented, 5) == "ab\xc3\xa9");
    CHECK(util::bounded_redacted_text("abcdef", 5) == "abcde");
}

TEST_CASE("bounded_redacted_text appends the suffix when truncating plain text", "[util][bounded-text][issue66]") {
    const auto result = util::bounded_redacted_text(std::string(100, 'a'), 10, "..");
    CHECK(result == "aaaaaaaa..");
}
