// The AI-side Glaze cases: the AI context JSON surface (`AiContext` /
// `ContextDto`, removed with `src/ai/glaze/` by #652) and the shared Glaze
// reader limits. The pi message JSON cases moved to
// `tests/harness/session/SessionMessageJsonTest.cpp` when the session module
// took ownership of that wire shape (ADR 0056).

#include "ai/glaze/AiJson.hpp"
#include "support/ComplexToolSchemaFixture.hpp"
#include "support/Json.hpp"
#include "support/JsonGlaze.hpp"

#include <cch/support/Error.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <variant>

using namespace cch;

TEST_CASE("context JSON preserves a complete Tool Argument Contract unchanged", "[ai][u2][glaze][issue24][compat-pi]") {
    auto expected_contract = support::read_json(tests::kComplexToolArgumentContract);
    REQUIRE(expected_contract);

    ai::AiContext context;
    context.system_prompt = "sys";
    context.messages.push_back(ai::MessageVariant{ai::user_text_message("hello")});
    context.tools.push_back(ai::Tool{
        "read_file",
        "Read a workspace file",
        *expected_contract,
    });

    auto json = ai::glaze::write_context_json(context);
    REQUIRE(json);
    auto parsed = ai::glaze::read_context_json(*json);
    REQUIRE(parsed);

    REQUIRE(parsed->system_prompt);
    CHECK(*parsed->system_prompt == "sys");
    REQUIRE(parsed->messages.size() == 1);
    REQUIRE(std::holds_alternative<ai::UserMessage>(parsed->messages[0]));
    const auto& user = std::get<ai::UserMessage>(parsed->messages[0]);
    CHECK(ai::text_from_user_message(user) == "hello");
    REQUIRE(parsed->tools.size() == 1);
    CHECK(parsed->tools[0].name == "read_file");

    auto expected_json = support::write_json(*expected_contract);
    auto restored_json = support::write_json(parsed->tools[0].parameters);
    REQUIRE(expected_json);
    REQUIRE(restored_json);
    CHECK(*restored_json == *expected_json);
}

TEST_CASE("Glaze accepts its nesting limit and rejects the next level", "[ai][u2][glaze][compat-pi]") {
    constexpr std::size_t kMaximumDepth = 256;
    std::string accepted_json(kMaximumDepth, '[');
    accepted_json += '0';
    accepted_json.append(kMaximumDepth, ']');
    REQUIRE(support::read_json<glz::generic>(accepted_json));

    std::string rejected_json(kMaximumDepth + 1, '[');
    rejected_json += '0';
    rejected_json.append(kMaximumDepth + 1, ']');
    const auto parsed = support::read_json<glz::generic>(rejected_json);
    REQUIRE_FALSE(parsed);
    CHECK(parsed.error().code == support::ErrorCode::JsonParse);
}
