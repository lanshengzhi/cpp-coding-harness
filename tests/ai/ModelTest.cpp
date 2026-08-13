#include <cch/agent/Agent.hpp>
#include <cch/ai/Model.hpp>
#include "ai/glaze/ModelJson.hpp"
#include "support/FakeModelStream.hpp"
#include "support/ModelFixture.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>

using namespace cch;

namespace {

std::string read_fixture(std::string_view name) {
    const auto path = std::filesystem::path(CCH_SOURCE_DIR) / "fixtures" / "pi-ai" / "models" / name;
    std::ifstream input(path);
    REQUIRE(input.is_open());
    std::ostringstream contents;
    contents << input.rdbuf();
    auto text = contents.str();
    if (text.ends_with('\n')) {
        text.pop_back();
    }
    return text;
}

} // namespace

TEST_CASE("complete Model matches the frozen pi shape golden", "[ai][model][issue336]") {
    auto model = tests::make_model("kimi-for-coding", "kimi-coding", "anthropic-messages");
    model.name = "Kimi for Coding";
    model.base_url = "https://api.kimi.com/coding";
    model.reasoning = true;
    model.thinking_level_map = ai::ThinkingLevelMap{
        {ai::ModelThinkingLevel::Minimal, std::string{"low"}},
        {ai::ModelThinkingLevel::Low, std::nullopt},
        {ai::ModelThinkingLevel::High, std::string{"high"}},
    };
    model.input = {ai::ModelInput::Text, ai::ModelInput::Image};
    model.cost = ai::ModelCost{
        .input = 1.0,
        .output = 4.0,
        .cache_read = 0.1,
        .cache_write = 1.25,
        .tiers = std::vector<ai::ModelCostTier>{ai::ModelCostTier{
            .input = 2.0,
            .output = 8.0,
            .cache_read = 0.2,
            .cache_write = 2.5,
            .input_tokens_above = 200000,
        }},
    };
    model.context_window = 262144;
    model.max_tokens = 32768;
    model.headers = ai::ModelHeaders{{"X-Static", "catalog"}};
    model.compat = ai::AnthropicMessagesCompat{
        .force_adaptive_thinking = true,
        .allow_empty_signature = false,
    };

    REQUIRE(ai::validate_model(model));
    auto serialized = ai::glaze::write_model_json(model);
    REQUIRE(serialized);
    CHECK(*serialized == read_fixture("complete-anthropic-model.json"));
}

TEST_CASE("Agent starts with the concrete frozen default Model", "[ai][model][issue336]") {
    auto runtime = std::make_shared<tests::FakeModelStream>();
    agent::Agent instance(runtime->factory(), agent::ToolRegistry{});

    auto serialized = ai::glaze::write_model_json(instance.state().model);
    REQUIRE(serialized);
    CHECK(*serialized == read_fixture("default-model.json"));
}

TEST_CASE("thinking level map distinguishes no map missing key and explicit null", "[ai][model][issue336]") {
    auto parsed = ai::glaze::read_model_json(read_fixture("complete-anthropic-model.json"));
    REQUIRE(parsed);
    REQUIRE(parsed->thinking_level_map.has_value());

    const auto& levels = *parsed->thinking_level_map;
    CHECK(levels.find(ai::ModelThinkingLevel::Off) == levels.end());
    const auto low = levels.find(ai::ModelThinkingLevel::Low);
    REQUIRE(low != levels.end());
    CHECK_FALSE(low->second.has_value());
    const auto high = levels.find(ai::ModelThinkingLevel::High);
    REQUIRE(high != levels.end());
    CHECK(high->second == std::optional<std::string>{"high"});

    auto no_map = ai::glaze::read_model_json(read_fixture("default-model.json"));
    REQUIRE(no_map);
    CHECK_FALSE(no_map->thinking_level_map.has_value());
}

TEST_CASE("Model validation rejects partial identity invalid cost and incompatible compat", "[ai][model][issue336]") {
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
