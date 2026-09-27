#include "agent/harness/OutputLimiter.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <string>

using namespace cch;

TEST_CASE("tail output limiter redacts before applying byte and line limits",
        "[harness][output-limiter][issue73][spec]") {
    const auto redacted = harness::limit_output_tail_redacted("api_key=secret");
    CHECK(redacted.text == "api_key=[REDACTED]");
    CHECK_FALSE(redacted.truncated);

    const auto lines = harness::limit_output_tail_redacted(
        "a\nb\nc",
        harness::OutputLimit{.max_bytes = 1024, .max_lines = 2});
    CHECK(lines.text == "b\nc");
    CHECK(lines.truncated);
}

TEST_CASE("tail output limiter starts on a UTF-8 character boundary", "[harness][output-limiter][issue73][spec]") {
    const std::string accented = "xx\xc3\xa9\xc3\xa9";
    const auto result = harness::limit_output_tail_redacted(
        accented,
        harness::OutputLimit{.max_bytes = 5, .max_lines = 2000});

    CHECK(result.text == "x\xc3\xa9\xc3\xa9");
    CHECK(result.truncated);
}

TEST_CASE("limit_output_tail preserves unredacted text when no limit is hit",
        "[harness][output-limiter][issue73][spec]") {
    const auto result = harness::limit_output_tail("api_key=secret");
    CHECK(result.text == "api_key=secret");
    CHECK_FALSE(result.truncated);
}

TEST_CASE("limit_output_tail does not redact", "[harness][output-limiter][issue73][spec]") {
    const auto result = harness::limit_output_tail(
        "api_key=secret",
        harness::OutputLimit{.max_bytes = 1024, .max_lines = 2000});
    CHECK(result.text == "api_key=secret");
    CHECK_FALSE(result.truncated);
}

TEST_CASE("limit_output_tail applies byte and line limits", "[harness][output-limiter][issue73][spec]") {
    const auto result = harness::limit_output_tail(
        "a\nb\nc",
        harness::OutputLimit{.max_bytes = 1024, .max_lines = 2});
    CHECK(result.text == "b\nc");
    CHECK(result.truncated);
}

TEST_CASE("truncate_output_head passes input under both limits through with every fact",
        "[harness][output-limiter][issue823][spec]") {
    const auto result = harness::truncate_output_head("line1\nline2\n");

    // The whole text, and no marker of any kind: the old limiter appended
    // "\n[output truncated]" and this case is the negative twin that would let
    // a reappearing marker through.
    CHECK(result.text == "line1\nline2\n");
    CHECK_FALSE(result.truncated);
    CHECK_FALSE(result.truncated_by.has_value());
    CHECK(result.total_lines == 2);
    CHECK(result.total_bytes == 12);
    CHECK(result.output_lines == 2);
    CHECK(result.output_bytes == 12);
    CHECK_FALSE(result.last_line_partial);
    CHECK_FALSE(result.first_line_exceeds_limit);
    CHECK(result.max_lines == 2000);
    CHECK(result.max_bytes == 50 * 1024);
}

TEST_CASE("truncate_output_head returns empty text and the first-line flag for the empty input's neighbour",
        "[harness][output-limiter][issue823][spec]") {
    // The empty input is not truncated at all, so `content` stays empty and no
    // flag is set: a limiter that emitted a marker here would be visible.
    const auto result = harness::truncate_output_head("");

    CHECK(result.text.empty());
    CHECK_FALSE(result.truncated);
    CHECK(result.total_lines == 0);
    CHECK(result.output_lines == 0);
    CHECK(result.first_line_exceeds_limit == false);
}

TEST_CASE("truncate_output_head stops at the line limit and keeps whole lines",
        "[harness][output-limiter][issue823][spec]") {
    const auto result =
            harness::truncate_output_head("a\nb\nc\nd\n", harness::OutputLimit{.max_bytes = 1024, .max_lines = 2});

    CHECK(result.truncated);
    CHECK(result.truncated_by == harness::OutputTruncationKind::Lines);
    // Whole lines only: the third line is not even partially present, and no
    // marker follows the kept text.
    CHECK(result.text == "a\nb");
    CHECK(result.output_lines == 2);
    CHECK(result.output_bytes == 3);
    CHECK(result.total_lines == 4);
    CHECK_FALSE(result.first_line_exceeds_limit);
}

TEST_CASE("truncate_output_head stops at the byte limit and names it", "[harness][output-limiter][issue823][spec]") {
    const auto result = harness::truncate_output_head(
            "aaaa\nbbbb\ncccc\n", harness::OutputLimit{.max_bytes = 10, .max_lines = 2000});

    CHECK(result.truncated);
    CHECK(result.truncated_by == harness::OutputTruncationKind::Bytes);
    // 4 + 1 (the joining newline) + 4 = 9 fits; the third line would make 14.
    CHECK(result.text == "aaaa\nbbbb");
    CHECK(result.output_lines == 2);
    CHECK(result.output_bytes == 9);
    CHECK(result.total_lines == 3);
    CHECK(result.max_bytes == 10);
}

TEST_CASE("truncate_output_head yields empty text for an oversized first line",
        "[harness][output-limiter][issue823][spec]") {
    const auto result = harness::truncate_output_head(
            std::string(100, 'x'), harness::OutputLimit{.max_bytes = 10, .max_lines = 2000});

    CHECK(result.truncated);
    CHECK(result.truncated_by == harness::OutputTruncationKind::Bytes);
    CHECK(result.first_line_exceeds_limit);
    // pi never returns a partial line from head truncation, so the text is
    // empty; the old limiter's byte-bounded prefix ("xxxxxxxxxx") is gone.
    CHECK(result.text.empty());
    CHECK(result.output_lines == 0);
    CHECK(result.output_bytes == 0);
    // The totals still describe the input so the renderer can report it.
    CHECK(result.total_lines == 1);
    CHECK(result.total_bytes == 100);
}

TEST_CASE("truncate_output_head keeps the trailing empty line out of the counts but in the split",
        "[harness][output-limiter][issue823][spec]") {
    // pi has two different splits: `splitLinesForCounting` (truncation) drops the
    // trailing empty element, `read.ts` keeps it. Both are pinned so the two
    // shapes stay distinguishable.
    CHECK(harness::split_lines_for_counting("a\nb\n").size() == 2);
    CHECK(harness::split_lines("a\nb\n").size() == 3);
    CHECK(harness::split_lines("a\nb").size() == 2);
    CHECK(harness::split_lines("").size() == 1);
    CHECK(harness::split_lines_for_counting("").empty());
}

TEST_CASE("truncate_output_head never splits a multibyte sequence", "[harness][output-limiter][issue823][spec]") {
    // 21 bytes: 'a' followed by ten é (2 bytes each); a budget of 4 lands
    // mid-sequence, and head truncation keeps a whole line or nothing.
    std::string utf8_accented = "a";
    for (std::size_t i = 0; i < 10; ++i) {
        utf8_accented += "\xc3\xa9";
    }
    CHECK(harness::truncate_output_head(utf8_accented, harness::OutputLimit{.max_bytes = 4, .max_lines = 2000})
                    .text.empty());

    // 12 bytes: three U+1F642 (4 bytes each).
    const std::string emojis = "\xf0\x9f\x99\x82\xf0\x9f\x99\x82\xf0\x9f\x99\x82";
    CHECK(harness::truncate_output_head(emojis, harness::OutputLimit{.max_bytes = 5, .max_lines = 2000}).text.empty());
}

TEST_CASE("truncate_output_tail keeps the last lines that fit", "[harness][output-limiter][issue823][spec]") {
    const auto result =
            harness::truncate_output_tail("a\nb\nc\nd\n", harness::OutputLimit{.max_bytes = 1024, .max_lines = 2});

    CHECK(result.truncated);
    CHECK(result.truncated_by == harness::OutputTruncationKind::Lines);
    CHECK(result.text == "c\nd");
    CHECK(result.output_lines == 2);
    CHECK(result.total_lines == 4);
    CHECK_FALSE(result.last_line_partial);
}

TEST_CASE("truncate_output_tail keeps the tail of an oversized final line and says so",
        "[harness][output-limiter][issue823][spec]") {
    // A 10-byte line, then a 20-byte final line; a 15-byte budget fits no whole
    // line, so the tail of the final line survives.
    const std::string content = std::string(10, 'x') + "\n" + std::string(20, 'y');
    const auto result =
            harness::truncate_output_tail(content, harness::OutputLimit{.max_bytes = 15, .max_lines = 2000});

    CHECK(result.truncated);
    CHECK(result.truncated_by == harness::OutputTruncationKind::Bytes);
    CHECK(result.last_line_partial);
    CHECK(result.text == std::string(15, 'y'));
    CHECK(result.output_lines == 1);
    CHECK(result.output_bytes == 15);
    CHECK(result.total_lines == 2);
    CHECK(result.total_bytes == 31);
}

TEST_CASE("truncate_output_tail starts on a UTF-8 character boundary", "[harness][output-limiter][issue823][spec]") {
    const auto result = harness::truncate_output_tail(
            std::string("xx\xc3\xa9\xc3\xa9"), harness::OutputLimit{.max_bytes = 5, .max_lines = 2000});

    CHECK(result.truncated);
    CHECK(result.last_line_partial);
    CHECK(result.text == "x\xc3\xa9\xc3\xa9");
    CHECK(result.output_bytes == 5);
}

TEST_CASE("format_output_size matches pi formatSize including its decimal places",
        "[harness][output-limiter][issue823][spec]") {
    CHECK(harness::format_output_size(0) == "0B");
    CHECK(harness::format_output_size(512) == "512B");
    CHECK(harness::format_output_size(1023) == "1023B");
    CHECK(harness::format_output_size(1024) == "1.0KB");
    CHECK(harness::format_output_size(50 * 1024) == "50.0KB");
    CHECK(harness::format_output_size(60 * 1024) == "60.0KB");
    // 78.0KB: a value whose tenths digit comes from real division.
    CHECK(harness::format_output_size(79872) == "78.0KB");
    // 1536 bytes is exactly 1.5KB, the tie JavaScript's toFixed rounds away
    // from zero where a round-half-to-even formatter would round down.
    CHECK(harness::format_output_size(1536) == "1.5KB");
    CHECK(harness::format_output_size(1024 * 1024) == "1.0MB");
    CHECK(harness::format_output_size(3 * 1024 * 1024 / 2) == "1.5MB");
    CHECK(harness::format_output_size(3 * 1024 * 1024) == "3.0MB");
}
