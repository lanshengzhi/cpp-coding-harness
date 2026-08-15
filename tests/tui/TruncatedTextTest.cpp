#include <cch/tui/TruncatedText.hpp>
#include <cch/tui/Utils.hpp>

#include <cch/support/Error.hpp>
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

TEST_CASE("TruncatedText passes through short text", "[tui][truncated][issue381]") {
    cch::tui::TruncatedText text("hello");
    auto result = text.render(20);
    REQUIRE(result);
    REQUIRE(result->lines.size() >= 1);
    CHECK(result->lines[0].find("hello") != std::string::npos);
    CHECK(result->lines[0].find("...") == std::string::npos);
}

TEST_CASE("TruncatedText hard-cuts long text at the width boundary", "[tui][truncated][issue381]") {
    cch::tui::TruncatedText text("hello world");
    auto result = text.render(5);
    REQUIRE(result);
    REQUIRE(result->lines.size() >= 1);
    CHECK(result->lines[0].find("hello") != std::string::npos);
    CHECK(result->lines[0].find("world") == std::string::npos);
    CHECK(result->lines[0].find("...") == std::string::npos);
    CHECK(cch::tui::visible_width(result->lines[0]) == 5);
}

TEST_CASE("TruncatedText hard-cuts ANSI-bearing text retaining escapes", "[tui][truncated][issue381]") {
    cch::tui::TruncatedText text("\x1b[31mhello world\x1b[0m");
    auto result = text.render(5);
    REQUIRE(result);
    REQUIRE(result->lines.size() >= 1);
    CHECK(result->lines[0].find("\x1b[31m") != std::string::npos);
    CHECK(result->lines[0].find("hello") != std::string::npos);
    CHECK(result->lines[0].find("world") == std::string::npos);
    CHECK(result->lines[0].find("...") == std::string::npos);
    CHECK(cch::tui::visible_width(result->lines[0]) == 5);
}

TEST_CASE("TruncatedText only renders first line", "[tui][truncated][issue381]") {
    cch::tui::TruncatedText text("hello\nworld");
    auto result = text.render(20);
    REQUIRE(result);
    REQUIRE(result->lines.size() >= 1);
    CHECK(result->lines[0].find("hello") != std::string::npos);
    CHECK(result->lines[0].find("world") == std::string::npos);
}

TEST_CASE("TruncatedText applies padding", "[tui][truncated][issue381]") {
    cch::tui::TruncatedText text("hi", 1, 1);
    auto result = text.render(10);
    REQUIRE(result);
    REQUIRE(result->lines.size() == 3);
    CHECK(result->lines[1].size() == 10);
}

TEST_CASE("TruncatedText rejects width consumed by padding", "[tui][truncated][issue381]") {
    cch::tui::TruncatedText text("hi", 2, 1);
    const auto result = text.render(4);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == cch::support::ErrorCode::Validation);
}

TEST_CASE("TruncatedText rejects zero width", "[tui][truncated][issue381]") {
    cch::tui::TruncatedText text("hi");
    auto result = text.render(0);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == cch::support::ErrorCode::Validation);
}
