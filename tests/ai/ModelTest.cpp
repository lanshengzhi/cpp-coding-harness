#include <cch/agent/Agent.hpp>
#include <cch/ai/Model.hpp>
#include "support/FakeModelStream.hpp"
#include "support/ModelFixture.hpp"
#include "support/PiFixture.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

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
    model.cost.input = -1'000'000.0;
    CHECK(ai::validate_model(model));

    model = tests::make_model("model-1");
    model.cost.input = std::numeric_limits<double>::infinity();
    CHECK_FALSE(ai::validate_model(model));

    model = tests::make_model("model-1");
    model.compat = ai::ModelCompatVariant{ai::AnthropicMessagesCompat{.force_adaptive_thinking = true}};
    CHECK_FALSE(ai::validate_model(model));

    model.api = "anthropic-messages";
    CHECK(ai::validate_model(model));
}

TEST_CASE("Model compatibility alternatives validate against their API identity", "[ai][model][issue759][spec]") {
    auto model = tests::make_model("model-1", "deepseek", "openai-responses");
    model.compat = ai::ModelCompatVariant{ai::OpenAICompletionsCompat{
            .supports_store = false,
            .thinking_format = ai::OpenAICompletionsThinkingFormat::DeepSeek,
    }};

    auto invalid = ai::validate_model(model);
    REQUIRE_FALSE(invalid);
    CHECK(invalid.error().message == "invalid model compat");
    CHECK(invalid.error().detail.find("OpenAICompletionsCompat") != std::string::npos);
    CHECK(invalid.error().detail.find("openai-completions") != std::string::npos);
    CHECK(invalid.error().detail.find("openai-responses") != std::string::npos);

    model.api = "openai-completions";
    CHECK(ai::validate_model(model));
    const auto* completions = std::get_if<ai::OpenAICompletionsCompat>(&*model.compat);
    REQUIRE(completions != nullptr);
    CHECK(completions->supports_store == false);
    CHECK(completions->thinking_format == ai::OpenAICompletionsThinkingFormat::DeepSeek);
}

TEST_CASE("Model compatibility preserves absent fields and explicit false overrides", "[ai][model][issue759][spec]") {
    auto model = tests::make_model("gpt-5.6-luna", "openai", "openai-responses");
    model.compat = ai::ModelCompatVariant{ai::OpenAIResponsesCompat{
            .supports_strict_mode = false,
            .supports_explicit_prompt_cache_mode = true,
    }};

    REQUIRE(ai::validate_model(model));
    const auto* responses = std::get_if<ai::OpenAIResponsesCompat>(&*model.compat);
    REQUIRE(responses != nullptr);
    CHECK(responses->supports_strict_mode == false);
    CHECK(responses->supports_explicit_prompt_cache_mode == true);

    model.compat = ai::ModelCompatVariant{ai::OpenAICompletionsCompat{}};
    model.api = "openai-completions";
    REQUIRE(ai::validate_model(model));
    const auto* completions = std::get_if<ai::OpenAICompletionsCompat>(&*model.compat);
    REQUIRE(completions != nullptr);
    CHECK_FALSE(completions->thinking_format.has_value());
    CHECK_FALSE(completions->supports_strict_mode.has_value());
    CHECK(completions->thinking_format == std::nullopt);

    model.compat = ai::ModelCompatVariant{ai::OpenAICompletionsCompat{
            .supports_strict_mode = false,
    }};
    REQUIRE(ai::validate_model(model));
    completions = std::get_if<ai::OpenAICompletionsCompat>(&*model.compat);
    REQUIRE(completions != nullptr);
    CHECK(completions->supports_strict_mode == false);
}

TEST_CASE("Thinking level wire vocabulary is public on the Model interface", "[ai][model][issue651][spec]") {
    // The published table holds the seven wire names in the order
    // `clamp_thinking_level` walks when it widens or narrows a request.
    const std::array<std::pair<ai::ModelThinkingLevel, std::string_view>, 7> expected{{
            {ai::ModelThinkingLevel::Off, "off"},
            {ai::ModelThinkingLevel::Minimal, "minimal"},
            {ai::ModelThinkingLevel::Low, "low"},
            {ai::ModelThinkingLevel::Medium, "medium"},
            {ai::ModelThinkingLevel::High, "high"},
            {ai::ModelThinkingLevel::XHigh, "xhigh"},
            {ai::ModelThinkingLevel::Max, "max"},
    }};
    CHECK(ai::kModelThinkingLevels == expected);
    for (const auto& [level, name] : expected) {
        CHECK(ai::model_thinking_level_name(level) == name);
        CHECK(ai::parse_model_thinking_level(name) == level);
    }
    CHECK_FALSE(ai::parse_model_thinking_level("turbo").has_value());
}
