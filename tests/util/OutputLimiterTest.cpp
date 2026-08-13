#include "util/OutputLimiter.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <string>

using namespace cch;

TEST_CASE("tail output limiter redacts before applying byte and line limits", "[util][output-limiter][issue73]") {
    const auto redacted = util::limit_output_tail_redacted("api_key=secret");
    CHECK(redacted.text == "api_key=[REDACTED]");
    CHECK_FALSE(redacted.truncated);

    const auto lines = util::limit_output_tail_redacted(
        "a\nb\nc",
        util::OutputLimit{.max_bytes = 1024, .max_lines = 2});
    CHECK(lines.text == "b\nc");
    CHECK(lines.truncated);
}

TEST_CASE("tail output limiter starts on a UTF-8 character boundary", "[util][output-limiter][issue73]") {
    const std::string accented = "xx\xc3\xa9\xc3\xa9";
    const auto result = util::limit_output_tail_redacted(
        accented,
        util::OutputLimit{.max_bytes = 5, .max_lines = 2000});

    CHECK(result.text == "x\xc3\xa9\xc3\xa9");
    CHECK(result.truncated);
}

TEST_CASE("limit_output_tail preserves unredacted text when no limit is hit", "[util][output-limiter][issue73]") {
    const auto result = util::limit_output_tail("api_key=secret");
    CHECK(result.text == "api_key=secret");
    CHECK_FALSE(result.truncated);
}

TEST_CASE("limit_output_tail does not redact", "[util][output-limiter][issue73]") {
    const auto result = util::limit_output_tail(
        "api_key=secret",
        util::OutputLimit{.max_bytes = 1024, .max_lines = 2000});
    CHECK(result.text == "api_key=secret");
    CHECK_FALSE(result.truncated);
}

TEST_CASE("limit_output_tail applies byte and line limits", "[util][output-limiter][issue73]") {
    const auto result = util::limit_output_tail(
        "a\nb\nc",
        util::OutputLimit{.max_bytes = 1024, .max_lines = 2});
    CHECK(result.text == "b\nc");
    CHECK(result.truncated);
}

TEST_CASE("limit_output passes input under the limits through unchanged", "[util][output-limiter][issue66]") {
    const auto with_newline = util::limit_output("line1\nline2\n");
    CHECK(with_newline.text == "line1\nline2\n");
    CHECK_FALSE(with_newline.truncated);

    const auto without_newline = util::limit_output("line1\nline2");
    CHECK(without_newline.text == "line1\nline2");
    CHECK_FALSE(without_newline.truncated);
}

TEST_CASE("limit_output returns empty text for empty input", "[util][output-limiter][issue66]") {
    const auto result = util::limit_output("");
    CHECK(result.text.empty());
    CHECK_FALSE(result.truncated);
}

TEST_CASE("limit_output stops at the line limit and flags truncation", "[util][output-limiter][issue66]") {
    const auto result = util::limit_output("a\nb\nc\n", util::OutputLimit{.max_bytes = 1024, .max_lines = 2});
    CHECK(result.truncated);
    CHECK(result.text == "a\nb\n\n[output truncated]");
}

TEST_CASE("limit_output stops at the byte limit and flags truncation", "[util][output-limiter][issue66]") {
    const auto result = util::limit_output(
        "aaaa\nbbbb\n",
        util::OutputLimit{.max_bytes = 6, .max_lines = 2000});
    CHECK(result.truncated);
    CHECK(result.text == "aaaa\n\n[output truncated]");
}

TEST_CASE("limit_output falls back to a byte-bounded prefix for an oversized first line", "[util][output-limiter][issue66]") {
    const auto result = util::limit_output(
        std::string(100, 'x'),
        util::OutputLimit{.max_bytes = 10, .max_lines = 2000});
    CHECK(result.truncated);
    CHECK(result.text == "xxxxxxxxxx\n[output truncated]");
}

TEST_CASE("limit_output oversized-first-line fallback never splits a multibyte sequence", "[util][output-limiter][issue66]") {
    // 21 bytes: 'a' followed by ten é (2 bytes each); a budget of 4 lands mid-sequence.
    std::string utf8_accented = "a";
    for (std::size_t i = 0; i < 10; ++i) {
        utf8_accented += "\xc3\xa9";
    }
    const auto two_byte = util::limit_output(
        utf8_accented,
        util::OutputLimit{.max_bytes = 4, .max_lines = 2000});
    CHECK(two_byte.truncated);
    CHECK(two_byte.text == "a\xc3\xa9\n[output truncated]");

    // 12 bytes: three U+1F642 (4 bytes each); a budget of 5 lands mid-sequence.
    const std::string emojis = "\xf0\x9f\x99\x82\xf0\x9f\x99\x82\xf0\x9f\x99\x82";
    const auto four_byte = util::limit_output(
        emojis,
        util::OutputLimit{.max_bytes = 5, .max_lines = 2000});
    CHECK(four_byte.truncated);
    CHECK(four_byte.text == "\xf0\x9f\x99\x82\n[output truncated]");
}
