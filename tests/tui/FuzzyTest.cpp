#include <cch/tui/Fuzzy.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace cch::tui;

namespace {

// é, ß, É; U+0130; U+0301/U+0307; U+00A0; U+1F600; Greek capitals U+0391/U+03A3.
constexpr std::string_view kLowerAccentE = "\xc3\xa9";
constexpr std::string_view kUpperAccentE = "\xc3\x89";
constexpr std::string_view kSharpS = "\xc3\x9f";
constexpr std::string_view kDottedI = "\xc4\xb0";
constexpr std::string_view kCombiningAcute = "\xcc\x81";
constexpr std::string_view kNoBreakSpace = "\xc2\xa0";
constexpr std::string_view kGrinningFace = "\xf0\x9f\x98\x80";
constexpr std::string_view kCapitalAlpha = "\xce\x91";
constexpr std::string_view kCapitalSigma = "\xce\xa3";

} // namespace

TEST_CASE("fuzzy_match empty query matches everything with score 0", "[tui][fuzzy][issue52][spec]") {
    const auto result = fuzzy_match("", "anything");
    CHECK(result.matches);
    CHECK(result.score == 0.0);
}

TEST_CASE("fuzzy_match query longer than text does not match", "[tui][fuzzy][issue52][spec]") {
    CHECK_FALSE(fuzzy_match("longquery", "short").matches);
}

TEST_CASE("fuzzy_match exact match scores below zero", "[tui][fuzzy][issue52][spec]") {
    const auto result = fuzzy_match("test", "test");
    CHECK(result.matches);
    CHECK(result.score < 0.0); // consecutive and exact-match bonuses
}

TEST_CASE("fuzzy_match characters must appear in order", "[tui][fuzzy][issue52][spec]") {
    CHECK(fuzzy_match("abc", "aXbXc").matches);
    CHECK_FALSE(fuzzy_match("abc", "cba").matches);
}

TEST_CASE("fuzzy_match is case insensitive", "[tui][fuzzy][issue52][spec]") {
    CHECK(fuzzy_match("ABC", "abc").matches);
    CHECK(fuzzy_match("abc", "ABC").matches);
}

TEST_CASE("fuzzy_match consecutive matches score better than scattered", "[tui][fuzzy][issue52][spec]") {
    const auto consecutive = fuzzy_match("foo", "foobar");
    const auto scattered = fuzzy_match("foo", "f_o_o_bar");
    REQUIRE(consecutive.matches);
    REQUIRE(scattered.matches);
    CHECK(consecutive.score < scattered.score);
}

TEST_CASE("fuzzy_match word boundary matches score better", "[tui][fuzzy][issue52][spec]") {
    const auto at_boundary = fuzzy_match("fb", "foo-bar");
    const auto not_at_boundary = fuzzy_match("fb", "afbx");
    REQUIRE(at_boundary.matches);
    REQUIRE(not_at_boundary.matches);
    CHECK(at_boundary.score < not_at_boundary.score);
}

TEST_CASE("fuzzy_match matches swapped alpha numeric tokens", "[tui][fuzzy][issue52][spec]") {
    const auto result = fuzzy_match("codex52", "gpt-5.2-codex");
    CHECK(result.matches);
}

TEST_CASE("fuzzy_match handles accented text through case folding", "[tui][fuzzy][issue52][spec]") {
    const std::string upper_accented = "\xc3\x89"; // É
    const std::string lower_accented = "\xc3\xa9"; // é
    CHECK(fuzzy_match(upper_accented, lower_accented).matches);
    CHECK(fuzzy_match(lower_accented, upper_accented).matches);
}

TEST_CASE("fuzzy_match_indices reports matched byte offsets", "[tui][fuzzy][issue52][spec]") {
    const auto indices = fuzzy_match_indices("abc", "aXbXc");
    REQUIRE(indices);
    const std::vector<std::size_t> expected{0, 2, 4};
    CHECK(*indices == expected);
}

TEST_CASE("fuzzy_match_indices reports the exact-match offset run", "[tui][fuzzy][issue52][spec]") {
    const auto indices = fuzzy_match_indices("foo", "foobar");
    REQUIRE(indices);
    const std::vector<std::size_t> expected{0, 1, 2};
    CHECK(*indices == expected);
}

TEST_CASE("fuzzy_match_indices is case insensitive and uses swapped queries", "[tui][fuzzy][issue52][spec]") {
    const auto indices = fuzzy_match_indices("CODE", "codex");
    REQUIRE(indices);
    const std::vector<std::size_t> expected{0, 1, 2, 3};
    CHECK(*indices == expected);

    const auto swapped = fuzzy_match_indices("codex52", "gpt-5.2-codex");
    REQUIRE(swapped);
    CHECK_FALSE(swapped->empty());
}

TEST_CASE("fuzzy_match_indices returns nullopt on no match and empty on empty query", "[tui][fuzzy][issue52][spec]") {
    CHECK_FALSE(fuzzy_match_indices("abc", "cba"));
    const auto empty = fuzzy_match_indices("", "anything");
    REQUIRE(empty);
    CHECK(empty->empty());
}

TEST_CASE("fuzzy_match_indices offsets point into the original text", "[tui][fuzzy][issue52][spec]") {
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

TEST_CASE("fuzzy_filter empty query returns all items unchanged", "[tui][fuzzy][issue52][spec]") {
    const std::vector<std::string> items{"apple", "banana", "cherry"};
    const auto result = fuzzy_filter(items, "", [](const std::string& value) { return value; });
    CHECK(result == items);
}

TEST_CASE("fuzzy_filter filters out non-matching items", "[tui][fuzzy][issue52][spec]") {
    const std::vector<std::string> items{"apple", "banana", "cherry"};
    const auto result = fuzzy_filter(items, "an", [](const std::string& value) { return value; });
    REQUIRE(result.size() == 1);
    CHECK(result[0] == "banana");
}

TEST_CASE("fuzzy_filter sorts results by match quality", "[tui][fuzzy][issue52][spec]") {
    const std::vector<std::string> items{"a_p_p", "app", "application"};
    const auto result = fuzzy_filter(items, "app", [](const std::string& value) { return value; });
    REQUIRE(result.size() == 3);
    CHECK(result[0] == "app");
}

TEST_CASE("fuzzy_filter prioritizes exact matches over longer prefixes", "[tui][fuzzy][issue52][spec]") {
    const std::vector<std::string> items{"clone", "cl"};
    const auto result = fuzzy_filter(items, "cl", [](const std::string& value) { return value; });
    REQUIRE(result.size() == 2);
    CHECK(result[0] == "cl");
    CHECK(result[1] == "clone");
}

TEST_CASE("fuzzy_filter works with a custom projection", "[tui][fuzzy][issue52][spec]") {
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

TEST_CASE("fuzzy_filter matches slash-separated tokens against reordered text", "[tui][fuzzy][issue52][spec]") {
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

TEST_CASE("fuzzy_match leaves sharp s unexpanded so ss does not match it", "[tui][fuzzy][issue958][spec]") {
    // Full casefolding maps ß to "ss" and would match; frozen toLowerCase does
    // not, in either direction or through an ASCII spelling of the query.
    CHECK_FALSE(fuzzy_match("ss", kSharpS).matches);
    CHECK_FALSE(fuzzy_match("SS", kSharpS).matches);
    CHECK_FALSE(fuzzy_match(kSharpS, "ss").matches);
}

TEST_CASE("fuzzy_match scores count UTF-16 units rather than UTF-8 bytes", "[tui][fuzzy][issue958][spec]") {
    // One ASCII prefix character, one two-byte prefix, and one supplementary
    // prefix: the scored position is the UTF-16 index, so the last two score
    // the same 0.2 a byte-indexed scan would report as 0.3 and 0.4.
    const auto accented = fuzzy_match("a", std::string{kLowerAccentE} + "a");
    const auto ascii = fuzzy_match("a", "xxa");
    const auto supplementary = fuzzy_match("a", std::string{kGrinningFace} + "a");
    REQUIRE(accented.matches);
    REQUIRE(ascii.matches);
    REQUIRE(supplementary.matches);
    CHECK(accented.score == 0.1);
    CHECK(ascii.score == 0.2);
    CHECK(supplementary.score == 0.2);

    // A two-byte character whose lowercase form is one unit, and a decomposed
    // pair NFC would compose into a single unit, both score from the raw units.
    const auto sharp = fuzzy_match("a", std::string{kSharpS} + "a");
    REQUIRE(sharp.matches);
    CHECK(sharp.score == 0.1);
    const auto decomposed = fuzzy_match("a", std::string{"e"} + std::string{kCombiningAcute} + "a");
    REQUIRE(decomposed.matches);
    CHECK(decomposed.score == 0.2);

    // A scored pair keeps the frozen index arithmetic over UTF-16 units.
    const auto pair = fuzzy_match("ab", std::string{kLowerAccentE} + "ab");
    REQUIRE(pair.matches);
    CHECK(pair.score == -4.7);
}

TEST_CASE("fuzzy_match applies frozen lowercase context for dotted capital I and final sigma",
        "[tui][fuzzy][issue958][spec]") {
    // İ lowercases to "i" + U+0307 (two units), so the following "a" is at
    // UTF-16 index 2; NFC-style composition would collapse it to index 1.
    const auto dotted = fuzzy_match("a", std::string{kDottedI} + "a");
    REQUIRE(dotted.matches);
    CHECK(dotted.score == 0.2);
    CHECK(fuzzy_match(std::string{kUpperAccentE} + "A", std::string{kLowerAccentE} + "a").matches);

    // Final_Sigma is context sensitive: trailing Σ becomes ς, leading Σ does not.
    CHECK_FALSE(fuzzy_match("\xcf\x82", kCapitalSigma).matches);
    CHECK(fuzzy_match("\xcf\x83", kCapitalSigma).matches);
    CHECK_FALSE(fuzzy_match("\xcf\x83", std::string{kCapitalAlpha} + std::string{kCapitalSigma}).matches);
    const auto leading = fuzzy_match("\xcf\x83", std::string{kCapitalSigma} + std::string{kCapitalAlpha});
    REQUIRE(leading.matches);
    CHECK(leading.score == -15.0);
}

TEST_CASE("fuzzy_match treats non-ASCII whitespace as a word boundary", "[tui][fuzzy][issue958][spec]") {
    // pi's /[\s\-_./:]/ includes U+00A0; the C locale's isspace does not.
    const auto boundary = fuzzy_match("b", std::string{"a"} + std::string{kNoBreakSpace} + "b");
    REQUIRE(boundary.matches);
    CHECK(boundary.score == -9.8);
}

TEST_CASE("fuzzy_match_indices map UTF-16 match positions back to original UTF-8 offsets",
        "[tui][fuzzy][issue958][spec]") {
    // The scored positions above are UTF-16 indices; the reported offsets are
    // the original UTF-8 byte offsets of the matched characters.
    const auto dotted = fuzzy_match_indices("a", std::string{kDottedI} + "a");
    REQUIRE(dotted);
    CHECK(*dotted == std::vector<std::size_t>{2});
    CHECK((std::string{kDottedI} + "a")[(*dotted)[0]] == 'a');

    const auto sharp = fuzzy_match_indices("a", std::string{kSharpS} + "a");
    REQUIRE(sharp);
    CHECK(*sharp == std::vector<std::size_t>{2});

    const auto supplementary = fuzzy_match_indices("a", std::string{kGrinningFace} + "a");
    REQUIRE(supplementary);
    CHECK(*supplementary == std::vector<std::size_t>{4});

    const auto decomposed = fuzzy_match_indices("a", std::string{"e"} + std::string{kCombiningAcute} + "a");
    REQUIRE(decomposed);
    CHECK(*decomposed == std::vector<std::size_t>{3});

    // Multi-character queries keep one original offset per matched unit.
    const auto pair = fuzzy_match_indices("ab", std::string{kGrinningFace} + "a-b");
    REQUIRE(pair);
    CHECK(*pair == std::vector<std::size_t>{4, 6});
    const auto text = std::string{kGrinningFace} + "a-b";
    CHECK(text[(*pair)[0]] == 'a');
    CHECK(text[(*pair)[1]] == 'b');
}

TEST_CASE("fuzzy_filter ranks by UTF-16 score and keeps input order on ties", "[tui][fuzzy][issue958][spec]") {
    const std::string accented = std::string{kLowerAccentE} + "a";
    const std::string supplementary = std::string{kGrinningFace} + "a";
    // Frozen ranking of this set: exact match, then consecutive, then 0.1 for
    // the one-unit accented prefix, then the 0.2 tie in input order. Byte-index
    // scoring ties the accented item with "xxa" and lets input order decide.
    const std::vector<std::string> items{"xxa", accented, supplementary, "a", "aa"};
    const auto ranked = fuzzy_filter(items, "a", [](const std::string& value) { return value; });
    REQUIRE(ranked.size() == items.size());
    CHECK(ranked[0] == "a");
    CHECK(ranked[1] == "aa");
    CHECK(ranked[2] == accented);
    CHECK(ranked[3] == "xxa");
    CHECK(ranked[4] == supplementary);

    // Non-matching and non-ASCII spellings stay excluded: neither "SS" nor "ß"
    // contains the queried character.
    const std::vector<std::string> mixed{"xxa", accented, supplementary, "SS", std::string{kSharpS}};
    const auto filtered = fuzzy_filter(mixed, "a", [](const std::string& value) { return value; });
    REQUIRE(filtered.size() == 3);
    CHECK(filtered[0] == accented);
    CHECK(filtered[1] == "xxa");
    CHECK(filtered[2] == supplementary);

    // Context-sensitive final sigma also drives ranking: only the leading-Σ
    // entry contains a plain sigma.
    const std::vector<std::string> greek{std::string{kCapitalAlpha} + std::string{kCapitalSigma},
            std::string{kCapitalSigma} + std::string{kCapitalAlpha}};
    const auto sigma_ranked = fuzzy_filter(greek, "\xcf\x83", [](const std::string& value) { return value; });
    REQUIRE(sigma_ranked.size() == 1);
    CHECK(sigma_ranked[0] == (std::string{kCapitalSigma} + std::string{kCapitalAlpha}));
}

TEST_CASE("fuzzy_filter splits queries on the frozen whitespace and slash class", "[tui][fuzzy][issue958][spec]") {
    const auto get_text = [](const std::string& value) { return value; };
    const std::vector<std::string> paths{"a/b/c"};
    // U+00A0 separates tokens like any ASCII whitespace, and leading U+00A0 is
    // trimmed rather than becoming part of the first token.
    CHECK(fuzzy_filter(paths, "a" + std::string{kNoBreakSpace} + "c", get_text).size() == 1);
    const std::vector<std::string> items{"alpha", "beta"};
    const auto trimmed = fuzzy_filter(items, std::string{kNoBreakSpace} + "alpha", get_text);
    REQUIRE(trimmed.size() == 1);
    CHECK(trimmed[0] == "alpha");
}
