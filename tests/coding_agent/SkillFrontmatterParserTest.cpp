#include "coding_agent/SkillFrontmatterParser.hpp"
#include "../../include/cch/util/Error.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace cch;

namespace {

TEST_CASE("parseFrontmatter extracts name and description", "[coding_agent][skill][u2]") {
    std::string_view content =
        "---\n"
        "name: my-skill\n"
        "description: Does useful things.\n"
        "---\n"
        "# Body content\n"
        "Instructions here.\n";

    auto result = coding_agent::parseFrontmatter(content);
    REQUIRE(result.has_value());

    CHECK(result->fields.at("name") == "my-skill");
    CHECK(result->fields.at("description") == "Does useful things.");
    CHECK(result->body == "# Body content\nInstructions here.");
}

TEST_CASE("parseFrontmatter handles boolean disable-model-invocation", "[coding_agent][skill][u2]") {
    std::string_view content =
        "---\n"
        "name: hidden-skill\n"
        "description: Hidden.\n"
        "disable-model-invocation: true\n"
        "---\n"
        "Body.\n";

    auto result = coding_agent::parseFrontmatter(content);
    REQUIRE(result.has_value());

    CHECK(result->fields.at("disable-model-invocation") == "true");
}

TEST_CASE("parseFrontmatter tolerates unknown keys", "[coding_agent][skill][u2]") {
    std::string_view content =
        "---\n"
        "name: my-skill\n"
        "description: Does things.\n"
        "license: MIT\n"
        "metadata: {}\n"
        "---\n"
        "Body.\n";

    auto result = coding_agent::parseFrontmatter(content);
    REQUIRE(result.has_value());

    CHECK(result->fields.at("license") == "MIT");
    CHECK(result->fields.at("metadata") == "{}");
}

TEST_CASE("parseFrontmatter returns empty frontmatter for content without ---", "[coding_agent][skill][u2]") {
    std::string_view content = "# Just a heading\n\nSome text.\n";

    auto result = coding_agent::parseFrontmatter(content);
    REQUIRE(result.has_value());

    CHECK(result->fields.empty());
    CHECK(result->body == "# Just a heading\n\nSome text.");
}

TEST_CASE("parseFrontmatter treats no closing --- as body-only", "[coding_agent][skill][u2]") {
    std::string_view content =
        "---\n"
        "name: my-skill\n"
        "description: No closing delimiter\n";

    auto result = coding_agent::parseFrontmatter(content);
    REQUIRE(result.has_value());

    CHECK(result->fields.empty());
    CHECK(result->body == "---\nname: my-skill\ndescription: No closing delimiter");
}

TEST_CASE("parseFrontmatter handles empty YAML block", "[coding_agent][skill][u2]") {
    std::string_view content =
        "---\n"
        "---\n"
        "Body content here.\n";

    auto result = coding_agent::parseFrontmatter(content);
    REQUIRE(result.has_value());

    CHECK(result->fields.empty());
    CHECK(result->body == "Body content here.");
}

TEST_CASE("parseFrontmatter normalizes \\r\\n line endings", "[coding_agent][skill][u2]") {
    std::string_view content =
        "---\r\n"
        "name: my-skill\r\n"
        "description: Has CRLF.\r\n"
        "---\r\n"
        "Body with CRLF.\r\n";

    auto result = coding_agent::parseFrontmatter(content);
    REQUIRE(result.has_value());

    CHECK(result->fields.at("name") == "my-skill");
    CHECK(result->fields.at("description") == "Has CRLF.");
    CHECK(result->body == "Body with CRLF.");
}

TEST_CASE("parseFrontmatter strips quotes from values", "[coding_agent][skill][u2]") {
    std::string_view content =
        "---\n"
        "name: my-skill\n"
        "description: \"Does things with quotes.\"\n"
        "---\n"
        "Body.\n";

    auto result = coding_agent::parseFrontmatter(content);
    REQUIRE(result.has_value());

    CHECK(result->fields.at("description") == "Does things with quotes.");
}

TEST_CASE("parseFrontmatter preserves value with colon", "[coding_agent][skill][u2]") {
    std::string_view content =
        "---\n"
        "name: my-skill\n"
        "description: Check this: important detail\n"
        "---\n"
        "Body.\n";

    auto result = coding_agent::parseFrontmatter(content);
    REQUIRE(result.has_value());

    CHECK(result->fields.at("description") == "Check this: important detail");
}

TEST_CASE("parseFrontmatter fails on malformed line with no colon", "[coding_agent][skill][u2]") {
    std::string_view content =
        "---\n"
        "name: my-skill\n"
        "this line has no colon\n"
        "description: Should fail.\n"
        "---\n"
        "Body.\n";

    auto result = coding_agent::parseFrontmatter(content);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == util::ErrorCode::Validation);
}

TEST_CASE("parseFrontmatter handles content starting with --- but not \\n", "[coding_agent][skill][u2]") {
    // "---" without a following newline on the first line is not valid frontmatter
    std::string_view content = "---some markdown\nNot frontmatter.\n";

    auto result = coding_agent::parseFrontmatter(content);
    REQUIRE(result.has_value());

    CHECK(result->fields.empty());
    CHECK(result->body == "---some markdown\nNot frontmatter.");
}

TEST_CASE("parseFrontmatter trims whitespace around body", "[coding_agent][skill][u2]") {
    std::string_view content =
        "---\n"
        "name: my-skill\n"
        "description: D.\n"
        "---\n"
        "\n"
        "Body after blank line.\n"
        "\n";

    auto result = coding_agent::parseFrontmatter(content);
    REQUIRE(result.has_value());

    CHECK(result->body == "Body after blank line.");
}

} // namespace
