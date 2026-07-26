#include "../../third_party/catch2/catch_test_macros.hpp"

#include <cch/tui/TruncatedText.hpp>

#include <string>
#include <vector>

TEST_CASE("TruncatedText passes through short text", "[tui][issue46][truncated]") {
    cch::tui::TruncatedText text("hello");
    auto result = text.render(20);
    REQUIRE(result);
    REQUIRE(result->size() >= 1);
    CHECK((*result)[0].find("hello") != std::string::npos);
}

TEST_CASE("TruncatedText truncates long text with ellipsis", "[tui][issue46][truncated]") {
    cch::tui::TruncatedText text("hello world");
    auto result = text.render(5);
    REQUIRE(result);
    REQUIRE(result->size() >= 1);
    CHECK((*result)[0].find("...") != std::string::npos);
}

TEST_CASE("TruncatedText only renders first line", "[tui][issue46][truncated]") {
    cch::tui::TruncatedText text("hello\nworld");
    auto result = text.render(20);
    REQUIRE(result);
    REQUIRE(result->size() >= 1);
    CHECK((*result)[0].find("hello") != std::string::npos);
    CHECK((*result)[0].find("world") == std::string::npos);
}

TEST_CASE("TruncatedText applies padding", "[tui][issue46][truncated]") {
    cch::tui::TruncatedText text("hi", "...", 1, 1);
    auto result = text.render(10);
    REQUIRE(result);
    REQUIRE(result->size() == 3);
    CHECK((*result)[1].size() == 10);
}

TEST_CASE("TruncatedText rejects width consumed by padding", "[tui][issue46][truncated]") {
    cch::tui::TruncatedText text("hi", "...", 2, 1);
    const auto result = text.render(4);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == cch::util::ErrorCode::Validation);
}

TEST_CASE("TruncatedText rejects zero width", "[tui][issue46][truncated]") {
    cch::tui::TruncatedText text("hi");
    auto result = text.render(0);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == cch::util::ErrorCode::Validation);
}

TEST_CASE("TruncatedText handles custom ellipsis", "[tui][issue46][truncated]") {
    cch::tui::TruncatedText text("hello world", ">");
    auto result = text.render(5);
    REQUIRE(result);
    REQUIRE_FALSE(result->empty());
    CHECK((*result)[0].find(">") != std::string::npos);
}
