#include "coding_agent/runtime/UserBashOutputAccumulator.hpp"
#include "support/EnvVarGuard.hpp"
#include "support/TempWorkspace.hpp"
#include "agent/harness/OutputLimiter.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>

using namespace cch;
namespace runtime = cch::coding_agent::runtime;

namespace {

void append_in_chunks(
    runtime::UserBashOutputAccumulator& accumulator,
    std::string_view content,
    std::size_t chunk_size) {
    for (std::size_t offset = 0; offset < content.size(); offset += chunk_size) {
        accumulator.append(content.substr(offset, chunk_size));
    }
}

} // namespace

TEST_CASE(
    "User Bash output accumulator presents one sanitized stream in callback-arrival order",
    "[coding_agent][runtime][issue86][issue97]") {
    runtime::UserBashOutputAccumulator accumulator;
    accumulator.append("\x1b[31mfirst\x1b[0m\r\n");
    accumulator.append("sec\x1b[2Kond\rthird");
    accumulator.finish();
    CHECK(accumulator.tail() == "first\nsecondthird");
    CHECK_FALSE(accumulator.truncated());
    CHECK_FALSE(accumulator.full_output_path().has_value());
    CHECK_FALSE(accumulator.artifact_error().has_value());
}

TEST_CASE(
    "User Bash output accumulator makes invalid UTF-8 and binary bytes safe across chunk boundaries",
    "[coding_agent][runtime][issue86][issue97]") {
    runtime::UserBashOutputAccumulator accumulator;
    // é (U+00E9) is 0xC3 0xA9: a valid sequence split across two callbacks.
    accumulator.append("h\xc3");
    accumulator.append("\xa9llo\ttab");
    accumulator.append("\x07" "bell"); // BEL is a binary control and is dropped
    accumulator.append("\x7f" "del"); // DEL is preserved, matching pi
    // U+FFF9–U+FFFB are dropped, even split across chunk boundaries.
    accumulator.append("\xef\xbf");
    accumulator.append("\xb9");
    accumulator.append("\xef\xbf\xba\xef\xbf\xbb");
    accumulator.append("\x80\xff"); // stray continuation and invalid lead bytes
    accumulator.finish();
    CHECK(accumulator.tail() ==
        "h\xc3\xa9llo\ttabbell\x7f" "del\xef\xbf\xbd\xef\xbf\xbd");
    CHECK_FALSE(accumulator.truncated());
}

TEST_CASE(
    "User Bash output accumulator filters C0 controls to pi's recipe",
    "[coding_agent][runtime][issue97]") {
    runtime::UserBashOutputAccumulator accumulator;
    // Pi's sanitizeBinaryOutput keeps tab, LF, and CR; the later
    // .replace(/\r/g, "") removes CR, so only tab and LF survive.
    std::string controls;
    for (char ch = 0x00; ch < 0x20; ++ch) {
        controls.push_back(ch);
    }
    accumulator.append(controls);
    accumulator.finish();
    CHECK(accumulator.tail() == "\t\n");
    CHECK_FALSE(accumulator.truncated());
}

TEST_CASE(
    "User Bash output accumulator strips ANSI controls split across chunk boundaries",
    "[coding_agent][runtime][issue86]") {
    runtime::UserBashOutputAccumulator accumulator;
    accumulator.append("a\x1b[");
    accumulator.append("31mRED\x1b");
    accumulator.append("[0m b\x1b]0;title");
    accumulator.append("\a done");
    accumulator.finish();
    CHECK(accumulator.tail() == "aRED b done");
    CHECK_FALSE(accumulator.truncated());
}

TEST_CASE(
    "User Bash output accumulator removes carriage returns like pi",
    "[coding_agent][runtime][issue97]") {
    runtime::UserBashOutputAccumulator accumulator;
    // Pi's .replace(/\r/g, ""): CRLF collapses to LF and a lone CR
    // disappears, across chunk boundaries and at end of stream.
    accumulator.append("a\r");
    accumulator.append("\nb\rc");
    accumulator.append("\r\nd\r");
    accumulator.finish();
    CHECK(accumulator.tail() == "a\nbc\nd");
    CHECK_FALSE(accumulator.truncated());
}

TEST_CASE(
    "User Bash output accumulator passes secret-bearing output through unchanged",
    "[coding_agent][runtime][issue97]") {
    // ADR 0028: no secret redaction is applied anywhere in the output path.
    const std::string secret = "sk-abcdefghijklmnopqrstuvwxyz123456";
    runtime::UserBashOutputAccumulator accumulator;
    accumulator.append("launch api_");
    accumulator.append("key=" + secret.substr(0, 5));
    accumulator.append(secret.substr(5, 10));
    accumulator.append(secret.substr(15) + " done");
    accumulator.finish();
    CHECK(accumulator.tail() == "launch api_key=" + secret + " done");
}

TEST_CASE(
    "User Bash output accumulator passes quoted secret values through unchanged",
    "[coding_agent][runtime][issue97]") {
    runtime::UserBashOutputAccumulator accumulator;
    accumulator.append("{\"token\": \"");
    accumulator.append("quoted sec");
    accumulator.append("ret value\"}");
    accumulator.finish();
    CHECK(accumulator.tail() == "{\"token\": \"quoted secret value\"}");
}

TEST_CASE(
    "User Bash output accumulator keeps content exactly at the line and byte limits",
    "[coding_agent][runtime][issue86]") {
    const harness::OutputLimit limit{.max_bytes = 8, .max_lines = 3};
    runtime::UserBashOutputAccumulator accumulator{limit};
    accumulator.append("l1\nl2\nl3");
    accumulator.finish();
    CHECK(accumulator.tail() == "l1\nl2\nl3");
    CHECK_FALSE(accumulator.truncated());
}

TEST_CASE(
    "User Bash output accumulator keeps the tail when lines exceed the limit",
"[coding_agent][runtime][issue86]") {
    tests::TempWorkspace workspace;
    tests::EnvVarGuard tmpdir{"TMPDIR", workspace.path().string()};
    const harness::OutputLimit limit{.max_bytes = 100, .max_lines = 3};
    runtime::UserBashOutputAccumulator accumulator{limit};
    accumulator.append("l1\nl2\nl3\nl4");
    accumulator.finish();
    CHECK(accumulator.tail() == "l2\nl3\nl4");
    CHECK(accumulator.truncated());
}

TEST_CASE(
    "User Bash output accumulator keeps the tail when bytes exceed the limit",
"[coding_agent][runtime][issue86]") {
    tests::TempWorkspace workspace;
    tests::EnvVarGuard tmpdir{"TMPDIR", workspace.path().string()};
    const harness::OutputLimit limit{.max_bytes = 8, .max_lines = 100};
    runtime::UserBashOutputAccumulator accumulator{limit};
    accumulator.append("xxl1\nl2\nl3");
    accumulator.finish();
    CHECK(accumulator.tail() == "l1\nl2\nl3");
    CHECK(accumulator.truncated());
}

TEST_CASE(
    "User Bash output accumulator never splits a multibyte sequence at the tail cut",
"[coding_agent][runtime][issue86]") {
    tests::TempWorkspace workspace;
    tests::EnvVarGuard tmpdir{"TMPDIR", workspace.path().string()};
    const harness::OutputLimit limit{.max_bytes = 4, .max_lines = 100};
    runtime::UserBashOutputAccumulator accumulator{limit};
    accumulator.append("x\xc3\xa9" "abc");
    accumulator.finish();
    CHECK(accumulator.tail() == "abc");
    CHECK(accumulator.truncated());
}

TEST_CASE(
    "User Bash output accumulator matches whole-buffer tail semantics for arbitrary chunkings",
"[coding_agent][runtime][issue86][issue97]") {
    tests::TempWorkspace workspace;
    tests::EnvVarGuard tmpdir{"TMPDIR", workspace.path().string()};
    const harness::OutputLimit limit{.max_bytes = 64, .max_lines = 5};
    const std::string content =
        "alpha api_key=split-secret\nbeta\n\x1b[32mgamma\x1b[0m\r\ndelta\nepsilon\nzeta\neta";
    const std::string sanitized =
        "alpha api_key=split-secret\nbeta\ngamma\ndelta\nepsilon\nzeta\neta";
    const auto expected = harness::limit_output_tail(sanitized, limit);
    REQUIRE(expected.truncated);
    for (const std::size_t chunk_size : {std::size_t{1}, std::size_t{3}, std::size_t{17}}) {
        runtime::UserBashOutputAccumulator accumulator{limit};
        append_in_chunks(accumulator, content, chunk_size);
        accumulator.finish();
        CHECK(accumulator.tail() == expected.text);
        CHECK(accumulator.truncated());
    }
}

TEST_CASE(
    "User Bash output accumulator bounds retained memory under arbitrarily large output",
"[coding_agent][runtime][issue86]") {
    tests::TempWorkspace workspace;
    tests::EnvVarGuard tmpdir{"TMPDIR", workspace.path().string()};
    runtime::UserBashOutputAccumulator accumulator;
    const std::string chunk(4096, 'x');
    for (int index = 0; index < 64; ++index) {
        accumulator.append(chunk);
        accumulator.append("\n");
    }
    accumulator.finish();
    CHECK(accumulator.tail().size() <= 50 * 1024);
    CHECK(accumulator.truncated());
}

TEST_CASE(
    "User Bash output accumulator spills the complete sanitized stream to an owner-only temp file",
    "[coding_agent][runtime][issue86][issue97]") {
    tests::TempWorkspace workspace;
    const auto spill_dir = workspace.path() / "spill";
    std::filesystem::create_directories(spill_dir);
    tests::EnvVarGuard tmpdir{"TMPDIR", spill_dir.string()};

    const harness::OutputLimit limit{.max_bytes = 64, .max_lines = 4};
    const std::string secret = "sk-abcdefghijklmnopqrstuvwxyz123456";
    const std::string sanitized =
        "one api_key=" + secret + "\ntwo\nthree\nfour\nfive\nsix\nseven\n";
    runtime::UserBashOutputAccumulator accumulator{limit};
    accumulator.append("one api_");
    accumulator.append("key=" + secret + "\r\n");
    accumulator.append("two\nthree\nfour\n");
    accumulator.append("five\nsix\nseven\n");
    accumulator.finish();

    CHECK(accumulator.truncated());
    CHECK_FALSE(accumulator.artifact_error().has_value());
    REQUIRE(accumulator.full_output_path().has_value());
    const std::filesystem::path spill_path{*accumulator.full_output_path()};
    CHECK(spill_path.parent_path() == spill_dir);

    std::ifstream input{spill_path, std::ios::binary};
    const std::string spilled{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
    CHECK(spilled == sanitized);
    // Secret-bearing output spills unchanged: no redaction anywhere (ADR 0028).
    CHECK(spilled.find(secret) != std::string::npos);

    const auto expected_tail = harness::limit_output_tail(sanitized, limit);
    REQUIRE(expected_tail.truncated);
    CHECK(accumulator.tail() == expected_tail.text);

    const auto permissions = std::filesystem::status(spill_path).permissions();
    CHECK((permissions & std::filesystem::perms::group_all) == std::filesystem::perms::none);
    CHECK((permissions & std::filesystem::perms::others_all) == std::filesystem::perms::none);

    std::filesystem::remove(spill_path);
}

TEST_CASE(
    "User Bash output accumulator records no spill path while output fits the retained limits",
    "[coding_agent][runtime][issue86]") {
    tests::TempWorkspace workspace;
    const auto spill_dir = workspace.path() / "spill";
    std::filesystem::create_directories(spill_dir);
    tests::EnvVarGuard tmpdir{"TMPDIR", spill_dir.string()};

    const harness::OutputLimit limit{.max_bytes = 64, .max_lines = 4};
    runtime::UserBashOutputAccumulator accumulator{limit};
    accumulator.append("one\ntwo\nthree\n");
    accumulator.finish();

    CHECK(accumulator.tail() == "one\ntwo\nthree\n");
    CHECK_FALSE(accumulator.truncated());
    CHECK_FALSE(accumulator.full_output_path().has_value());
    CHECK_FALSE(accumulator.artifact_error().has_value());
    CHECK(std::filesystem::is_empty(spill_dir));
}

TEST_CASE(
    "User Bash output accumulator spill failure preserves the bounded truncated result without a path",
    "[coding_agent][runtime][issue86]") {
    tests::TempWorkspace workspace;
    tests::EnvVarGuard tmpdir{
        "TMPDIR",
        (workspace.path() / "missing" / "deeper").string()};

    const harness::OutputLimit limit{.max_bytes = 64, .max_lines = 4};
    const std::string sanitized = "one\ntwo\nthree\nfour\nfive\nsix\nseven\n";
    runtime::UserBashOutputAccumulator accumulator{limit};
    accumulator.append(sanitized);
    accumulator.finish();

    const auto expected_tail = harness::limit_output_tail(sanitized, limit);
    REQUIRE(expected_tail.truncated);
    CHECK(accumulator.truncated());
    CHECK(accumulator.tail() == expected_tail.text);
    CHECK_FALSE(accumulator.full_output_path().has_value());
    REQUIRE(accumulator.artifact_error().has_value());
    CHECK(accumulator.artifact_error()->message.size() <= 8192);
}

TEST_CASE(
    "User Bash output accumulator passes Authorization values through unchanged",
    "[coding_agent][runtime][issue97]") {
    runtime::UserBashOutputAccumulator accumulator;
    accumulator.append("Authorization: Bearer abc");
    accumulator.append(" def ghi\nnext");
    accumulator.finish();
    CHECK(accumulator.tail() == "Authorization: Bearer abc def ghi\nnext");
}

TEST_CASE(
    "User Bash output accumulator drops an over-long unterminated escape sequence",
    "[coding_agent][runtime][issue86]") {
    runtime::UserBashOutputAccumulator accumulator;
    accumulator.append("\x1b[" + std::string(5000, '0'));
    accumulator.append("plain");
    accumulator.finish();
    CHECK(accumulator.tail() == "plain");
    CHECK(accumulator.tail().find('\x1b') == std::string::npos);
}
