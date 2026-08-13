#include <cch/tui/Fuzzy.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace cch::tui;

TEST_CASE("fuzzy_match empty query matches everything with score 0", "[tui][fuzzy][issue52]") {
    const auto result = fuzzy_match("", "anything");
    CHECK(result.matches);
    CHECK(result.score == 0.0);
}

TEST_CASE("fuzzy_match query longer than text does not match", "[tui][fuzzy][issue52]") {
    CHECK_FALSE(fuzzy_match("longquery", "short").matches);
}

TEST_CASE("fuzzy_match exact match scores below zero", "[tui][fuzzy][issue52]") {
    const auto result = fuzzy_match("test", "test");
    CHECK(result.matches);
    CHECK(result.score < 0.0); // consecutive and exact-match bonuses
}

TEST_CASE("fuzzy_match characters must appear in order", "[tui][fuzzy][issue52]") {
    CHECK(fuzzy_match("abc", "aXbXc").matches);
    CHECK_FALSE(fuzzy_match("abc", "cba").matches);
}

TEST_CASE("fuzzy_match is case insensitive", "[tui][fuzzy][issue52]") {
    CHECK(fuzzy_match("ABC", "abc").matches);
    CHECK(fuzzy_match("abc", "ABC").matches);
}

TEST_CASE("fuzzy_match consecutive matches score better than scattered", "[tui][fuzzy][issue52]") {
    const auto consecutive = fuzzy_match("foo", "foobar");
    const auto scattered = fuzzy_match("foo", "f_o_o_bar");
    REQUIRE(consecutive.matches);
    REQUIRE(scattered.matches);
    CHECK(consecutive.score < scattered.score);
}

TEST_CASE("fuzzy_match word boundary matches score better", "[tui][fuzzy][issue52]") {
    const auto at_boundary = fuzzy_match("fb", "foo-bar");
    const auto not_at_boundary = fuzzy_match("fb", "afbx");
    REQUIRE(at_boundary.matches);
    REQUIRE(not_at_boundary.matches);
    CHECK(at_boundary.score < not_at_boundary.score);
}

TEST_CASE("fuzzy_match matches swapped alpha numeric tokens", "[tui][fuzzy][issue52]") {
    const auto result = fuzzy_match("codex52", "gpt-5.2-codex");
    CHECK(result.matches);
}

TEST_CASE("fuzzy_match handles accented text through case folding", "[tui][fuzzy][issue52]") {
    const std::string upper_accented = "\xc3\x89"; // É
    const std::string lower_accented = "\xc3\xa9"; // é
    CHECK(fuzzy_match(upper_accented, lower_accented).matches);
    CHECK(fuzzy_match(lower_accented, upper_accented).matches);
}

TEST_CASE("fuzzy_match_indices reports matched byte offsets", "[tui][fuzzy][issue52]") {
    const auto indices = fuzzy_match_indices("abc", "aXbXc");
    REQUIRE(indices);
    const std::vector<std::size_t> expected{0, 2, 4};
    CHECK(*indices == expected);
}

TEST_CASE("fuzzy_match_indices reports the exact-match offset run", "[tui][fuzzy][issue52]") {
    const auto indices = fuzzy_match_indices("foo", "foobar");
    REQUIRE(indices);
    const std::vector<std::size_t> expected{0, 1, 2};
    CHECK(*indices == expected);
}

TEST_CASE("fuzzy_match_indices is case insensitive and uses swapped queries", "[tui][fuzzy][issue52]") {
    const auto indices = fuzzy_match_indices("CODE", "codex");
    REQUIRE(indices);
    const std::vector<std::size_t> expected{0, 1, 2, 3};
    CHECK(*indices == expected);

    const auto swapped = fuzzy_match_indices("codex52", "gpt-5.2-codex");
    REQUIRE(swapped);
    CHECK_FALSE(swapped->empty());
}

TEST_CASE("fuzzy_match_indices returns nullopt on no match and empty on empty query", "[tui][fuzzy][issue52]") {
    CHECK_FALSE(fuzzy_match_indices("abc", "cba"));
    const auto empty = fuzzy_match_indices("", "anything");
    REQUIRE(empty);
    CHECK(empty->empty());
}

TEST_CASE("fuzzy_match_indices offsets point into the original text", "[tui][fuzzy][issue52]") {
    const std::string text = "aa-bb-cc";
    const auto indices = fuzzy_match_indices("abc", text);
    REQUIRE(indices);
    for (const auto index : *indices) {
        REQUIRE(index < text.size());
    }
    CHECK(text[(*indices)[0]] == 'a');
    CHECK(text[(*indices)[1]] == 'b');
    CHECK(text[(*indices)[2]] == 'c');
}

TEST_CASE("fuzzy_filter empty query returns all items unchanged", "[tui][fuzzy][issue52]") {
    const std::vector<std::string> items{"apple", "banana", "cherry"};
    const auto result = fuzzy_filter(items, "", [](const std::string& value) { return value; });
    CHECK(result == items);
}

TEST_CASE("fuzzy_filter filters out non-matching items", "[tui][fuzzy][issue52]") {
    const std::vector<std::string> items{"apple", "banana", "cherry"};
    const auto result = fuzzy_filter(items, "an", [](const std::string& value) { return value; });
    REQUIRE(result.size() == 1);
    CHECK(result[0] == "banana");
}

TEST_CASE("fuzzy_filter sorts results by match quality", "[tui][fuzzy][issue52]") {
    const std::vector<std::string> items{"a_p_p", "app", "application"};
    const auto result = fuzzy_filter(items, "app", [](const std::string& value) { return value; });
    REQUIRE(result.size() == 3);
    CHECK(result[0] == "app");
}

TEST_CASE("fuzzy_filter prioritizes exact matches over longer prefixes", "[tui][fuzzy][issue52]") {
    const std::vector<std::string> items{"clone", "cl"};
    const auto result = fuzzy_filter(items, "cl", [](const std::string& value) { return value; });
    REQUIRE(result.size() == 2);
    CHECK(result[0] == "cl");
    CHECK(result[1] == "clone");
}

TEST_CASE("fuzzy_filter works with a custom projection", "[tui][fuzzy][issue52]") {
    struct Item {
        std::string name;
        int id{0};
    };
    const std::vector<Item> items{{"foo", 1}, {"bar", 2}, {"foobar", 3}};
    const auto result = fuzzy_filter(items, "foo", [](const Item& item) { return item.name; });
    REQUIRE(result.size() == 2);
    CHECK(result[0].id == 1);
    CHECK(result[1].id == 3);
}

TEST_CASE("fuzzy_filter matches slash-separated tokens against reordered text", "[tui][fuzzy][issue52]") {
    struct Item {
        std::string id;
        std::string provider;
    };
    const std::vector<Item> items{{"gpt-5.5", "openai-codex"}};
    const auto result = fuzzy_filter(
        items,
        "openai-codex/gpt-5.5",
        [](const Item& item) { return item.id + " " + item.provider; });
    REQUIRE(result.size() == 1);
    CHECK(result[0].id == "gpt-5.5");
}
