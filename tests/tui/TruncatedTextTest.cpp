#include <cch/tui/TruncatedText.hpp>
#include <cch/tui/Utils.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

// #966's expected bytes were independently run against pi-v1.0.4 at
// 7c10bd4337495ee613f2224843ecdf349b80d1df (Node v26.11.1). A 5-column
// `hello world` yields `he\x1b[0m...\x1b[0m`; padding 2 at width 4 still
// yields the one-column ellipsis between both full padding runs. These cases
// distinguish pi's default ellipsis and max(1, width - 2 * padding) from a
// hard cut or a padding validation error.
TEST_CASE("TruncatedText passes through short text", "[tui][truncated][issue966][spec]") {
    cch::tui::TruncatedText text("hello");
    const auto result = text.render(20);
    REQUIRE(result);
    REQUIRE(result->lines.size() == 1);
    CHECK(result->lines[0] == "hello               ");
}

TEST_CASE("TruncatedText appends the frozen pi ellipsis at the width boundary", "[tui][truncated][issue966][spec]") {
    cch::tui::TruncatedText text("hello world");
    const auto result = text.render(5);
    REQUIRE(result);
    REQUIRE(result->lines.size() == 1);
    CHECK(result->lines[0] == "he\x1b[0m...\x1b[0m");
    CHECK(cch::tui::visible_width(result->lines[0]) == 5);
}

TEST_CASE("TruncatedText appends the frozen pi ellipsis to ANSI-bearing text", "[tui][truncated][issue966][spec]") {
    cch::tui::TruncatedText text("\x1b[31mhello world\x1b[0m");
    const auto result = text.render(5);
    REQUIRE(result);
    REQUIRE(result->lines.size() == 1);
    CHECK(result->lines[0] == "\x1b[31mhe\x1b[0m...\x1b[0m");
    CHECK(cch::tui::visible_width(result->lines[0]) == 5);
}

TEST_CASE("TruncatedText only renders first line", "[tui][truncated][issue966][spec]") {
    cch::tui::TruncatedText text("hello\nworld");
    const auto result = text.render(20);
    REQUIRE(result);
    REQUIRE(result->lines.size() == 1);
    CHECK(result->lines[0].starts_with("hello"));
    CHECK(result->lines[0].find("world") == std::string::npos);
}

TEST_CASE("TruncatedText applies padding", "[tui][truncated][issue966][spec]") {
    cch::tui::TruncatedText text("hi", 1, 1);
    const auto result = text.render(10);
    REQUIRE(result);
    REQUIRE(result->lines.size() == 3);
    CHECK(result->lines[1] == " hi       ");
}

TEST_CASE("TruncatedText clamps padding to a one-column content width like frozen pi",
        "[tui][truncated][issue966][spec]") {
    cch::tui::TruncatedText text("hello world", 2, 1);
    const auto result = text.render(4);
    REQUIRE(result);
    REQUIRE(result->lines.size() == 3);
    CHECK(result->lines[1] == "  \x1b[0m.\x1b[0m  ");
    CHECK(cch::tui::visible_width(result->lines[1]) == 5);
}

TEST_CASE("TruncatedText renders at zero width like frozen pi", "[tui][truncated][issue966][spec]") {
    cch::tui::TruncatedText text("hi");
    const auto result = text.render(0);
    REQUIRE(result);
    REQUIRE(result->lines.size() == 1);
    CHECK(result->lines[0] == "\x1b[0m.\x1b[0m");
}
