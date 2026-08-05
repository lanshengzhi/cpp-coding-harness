#include <cch/ai/Model.hpp>
#include <cch/ai/RequestOptions.hpp>
#include "ai/SimpleOptions.hpp"
#include "support/ModelFixture.hpp"

#include "../../third_party/catch2/catch_test_macros.hpp"

#include <cstdint>
#include <optional>
#include <type_traits>
#include <vector>

using namespace cch;

TEST_CASE(
    "Thinking levels preserve pi null and explicit extended mapping semantics",
    "[ai][simple-options][issue339]") {
    auto model = tests::make_model("reasoning", "deepseek", "openai-responses");
    model.reasoning = true;
    model.thinking_level_map = ai::ThinkingLevelMap{
        {ai::ModelThinkingLevel::Off, std::nullopt},
        {ai::ModelThinkingLevel::Low, "low"},
        {ai::ModelThinkingLevel::XHigh, "xhigh"},
    };

    const auto supported = ai::get_supported_thinking_levels(model);

    CHECK((supported == std::vector<ai::ModelThinkingLevel>{
        ai::ModelThinkingLevel::Minimal,
        ai::ModelThinkingLevel::Low,
        ai::ModelThinkingLevel::Medium,
        ai::ModelThinkingLevel::High,
        ai::ModelThinkingLevel::XHigh,
    }));
    CHECK(ai::clamp_thinking_level(model, ai::ModelThinkingLevel::Off) ==
          ai::ModelThinkingLevel::Minimal);
    CHECK(ai::clamp_thinking_level(model, ai::ModelThinkingLevel::Max) ==
          ai::ModelThinkingLevel::XHigh);
}

TEST_CASE(
    "string-level clamp matches pi clampThinkingLevel across the seven-level set",
    "[ai][simple-options][issue352]") {
    auto partial = tests::make_model("partial", "deepseek", "openai-responses");
    partial.reasoning = true;
    partial.thinking_level_map = ai::ThinkingLevelMap{
        {ai::ModelThinkingLevel::Off, std::nullopt},
        {ai::ModelThinkingLevel::Low, "low"},
        {ai::ModelThinkingLevel::XHigh, "xhigh"},
    };

    // Off is explicitly unsupported (null); xhigh is mapped; max is absent
    // and therefore unsupported. The supported set is minimal..xhigh.
    CHECK(ai::clamp_thinking_level_string(partial, "off") == "minimal");
    CHECK(ai::clamp_thinking_level_string(partial, "max") == "xhigh");
    CHECK(ai::clamp_thinking_level_string(partial, "medium") == "medium");

    // A non-reasoning model supports only "off"; every other request clamps
    // down to it (pi `getSupportedThinkingLevels` returns ["off"]).
    const auto basic = tests::make_model("basic");
    CHECK(ai::clamp_thinking_level_string(basic, "medium") == "off");
    CHECK(ai::clamp_thinking_level_string(basic, "off") == "off");
    CHECK(ai::clamp_thinking_level_string(basic, "max") == "off");

    // Invalid names pass through unchanged: validation owns rejection.
    CHECK(ai::clamp_thinking_level_string(basic, "turbo") == "turbo");
}

TEST_CASE(
    "Simple request options are move-only and clamp output to context",
    "[ai][simple-options][issue339]") {
    static_assert(!std::is_copy_constructible_v<ai::SimpleStreamOptions>);
    static_assert(std::is_move_constructible_v<ai::SimpleStreamOptions>);
    static_assert(std::is_same_v<
                  decltype(ai::SimpleStreamOptions::reasoning),
                  std::optional<ai::ThinkingLevel>>);

    auto model = tests::make_model("small");
    model.context_window = 5000;
    model.max_tokens = 2000;
    ai::AiContext context;
    context.system_prompt = std::string(4000, 'x');

    CHECK(ai::detail::clamp_max_tokens_to_context(model, context, 2000) == 1);
}
