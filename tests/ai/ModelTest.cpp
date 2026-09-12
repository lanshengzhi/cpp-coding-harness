#include <cch/agent/Agent.hpp>
#include <cch/ai/Model.hpp>
#include "support/FakeModelStream.hpp"
#include "support/ModelFixture.hpp"
#include "support/PiFixture.hpp"

#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <memory>
#include <string>
#include <string_view>

using namespace cch;

namespace {

[[nodiscard]] const support::JsonValue& require_member(
        const support::JsonValue::object_t& object, std::string_view key) {
    const auto found = object.find(std::string{key});
    REQUIRE(found != object.end());
    return found->second;
}

} // namespace

TEST_CASE("Agent starts with the concrete frozen default Model", "[ai][model][issue336][compat-pi]") {
    // The vendored pi DEFAULT_MODEL golden stays the authority for the Agent's
    // initial `ai::Model`, compared member by member: the placeholder has no
    // `models.json` parse path (an empty baseUrl and zeroed capabilities fail
    // the coding-agent schema) and production never writes model JSON.
    auto runtime = std::make_shared<tests::FakeModelStream>();
    agent::Agent instance(runtime->factory(), agent::ToolRegistry{});
    const auto& model = instance.state().model;

    const auto golden = tests::read_pi_fixture("models/default-model.json");
    REQUIRE(golden);
    const auto* object = golden->get_if<support::JsonValue::object_t>();
    REQUIRE(object != nullptr);

    CHECK(model.id == require_member(*object, "id").get_string());
    CHECK(model.name == require_member(*object, "name").get_string());
    CHECK(model.api == require_member(*object, "api").get_string());
    CHECK(model.provider == require_member(*object, "provider").get_string());
    CHECK(model.base_url == require_member(*object, "baseUrl").get_string());
    CHECK(model.reasoning == require_member(*object, "reasoning").get_boolean());
    CHECK(require_member(*object, "input").get_array().empty());
    CHECK(model.input.empty());
    const auto& golden_cost = require_member(*object, "cost").get_object();
    CHECK(model.cost.input == golden_cost.at("input").get_number());
    CHECK(model.cost.output == golden_cost.at("output").get_number());
    CHECK(model.cost.cache_read == golden_cost.at("cacheRead").get_number());
    CHECK(model.cost.cache_write == golden_cost.at("cacheWrite").get_number());
    CHECK_FALSE(model.cost.tiers.has_value());
    CHECK(model.context_window == require_member(*object, "contextWindow").get_number());
    CHECK(model.max_tokens == require_member(*object, "maxTokens").get_number());
    // Omitted optional members mean no thinking map, headers, or API compat.
    CHECK_FALSE(object->contains("thinkingLevelMap"));
    CHECK_FALSE(model.thinking_level_map.has_value());
    CHECK_FALSE(object->contains("headers"));
    CHECK_FALSE(model.headers.has_value());
    CHECK_FALSE(object->contains("compat"));
    CHECK_FALSE(model.compat.has_value());
}

TEST_CASE("Model validation rejects partial identity invalid cost and incompatible compat",
        "[ai][model][issue336][compat-pi]") {
    auto model = tests::make_model("model-1");

    model.id.clear();
    CHECK_FALSE(ai::validate_model(model));

    model = tests::make_model("model-1");
    model.name.clear();
    CHECK_FALSE(ai::validate_model(model));

    model = tests::make_model("model-1");
    model.api.clear();
    CHECK_FALSE(ai::validate_model(model));

    model = tests::make_model("model-1");
    model.provider.clear();
    CHECK_FALSE(ai::validate_model(model));

    model = tests::make_model("model-1");
    model.cost.output = -0.01;
    CHECK_FALSE(ai::validate_model(model));

    model = tests::make_model("model-1");
    model.cost.input = std::numeric_limits<double>::infinity();
    CHECK_FALSE(ai::validate_model(model));

    model = tests::make_model("model-1");
    model.compat = ai::AnthropicMessagesCompat{.force_adaptive_thinking = true};
    CHECK_FALSE(ai::validate_model(model));

    model.api = "anthropic-messages";
    CHECK(ai::validate_model(model));
}
