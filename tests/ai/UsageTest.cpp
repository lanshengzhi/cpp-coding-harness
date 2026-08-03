#include <cch/ai/Usage.hpp>
#include "ai/api/UsageNormalization.hpp"
#include "support/ModelFixture.hpp"
#include "support/PiFixture.hpp"

#include "../../third_party/catch2/catch_test_macros.hpp"

#include <cmath>
#include <cstdint>

using namespace cch;

namespace {

void check_usage_fixture(const ai::Usage& usage, const util::JsonValue& expected) {
    const auto& object = expected.get_object();
    CHECK(usage.input == static_cast<std::int64_t>(object.at("input").get_number()));
    CHECK(usage.output == static_cast<std::int64_t>(object.at("output").get_number()));
    CHECK(usage.cache_read ==
          static_cast<std::int64_t>(object.at("cache_read").get_number()));
    CHECK(usage.cache_write ==
          static_cast<std::int64_t>(object.at("cache_write").get_number()));
    CHECK(usage.reasoning ==
          static_cast<std::int64_t>(object.at("reasoning").get_number()));
    CHECK(usage.total_tokens ==
          static_cast<std::int64_t>(object.at("total_tokens").get_number()));

    const auto& cost = object.at("cost").get_object();
    constexpr double kCostTolerance = 1e-12;
    CHECK(std::abs(usage.cost.input - cost.at("input").get_number()) < kCostTolerance);
    CHECK(std::abs(usage.cost.output - cost.at("output").get_number()) < kCostTolerance);
    CHECK(std::abs(usage.cost.cache_read - cost.at("cache_read").get_number()) <
          kCostTolerance);
    CHECK(std::abs(usage.cost.cache_write - cost.at("cache_write").get_number()) <
          kCostTolerance);
    CHECK(std::abs(usage.cost.total - cost.at("total").get_number()) < kCostTolerance);
}

} // namespace

TEST_CASE("Responses and DeepSeek usage split uncached cached and reasoning tokens", "[ai][usage][issue339]") {
    auto model = tests::make_model("priced");
    model.cost = ai::ModelCost{.input = 2, .output = 4, .cache_read = 1, .cache_write = 3};

    const auto responses = ai::api::normalize_responses_usage(
        model,
        ai::api::ResponsesUsageFields{
            .input_tokens = 120,
            .output_tokens = 30,
            .cached_tokens = 20,
            .cache_write_tokens = 10,
            .reasoning_tokens = 7,
            .total_tokens = 150,
        });
    const auto deepseek = ai::api::normalize_deepseek_usage(
        model, 120, 30, 20, 7);

    const auto fixture = tests::read_pi_fixture("usage/normalization-and-cost.json");
    REQUIRE(fixture);
    check_usage_fixture(responses, fixture->at("responses"));
    check_usage_fixture(deepseek, fixture->at("deepseek"));

    const auto clamped = ai::api::normalize_responses_usage(
        model,
        ai::api::ResponsesUsageFields{
            .input_tokens = 5,
            .cached_tokens = 4,
            .cache_write_tokens = 3,
        });
    CHECK(clamped.input == 0);
    CHECK(clamped.reasoning == 0);
}

TEST_CASE(
    "Anthropic usage deltas preserve omitted fields and shared cost uses tiers and one hour writes",
    "[ai][usage][issue339]") {
    auto model = tests::make_model("tiered");
    model.cost = ai::ModelCost{
        .input = 2,
        .output = 4,
        .cache_read = 1,
        .cache_write = 3,
        .tiers = std::vector<ai::ModelCostTier>{ai::ModelCostTier{
            .input = 10,
            .output = 20,
            .cache_read = 5,
            .cache_write = 15,
            .input_tokens_above = 100,
        }},
    };
    ai::Usage usage;
    ai::api::apply_anthropic_usage_start(
        model,
        usage,
        ai::api::AnthropicUsageUpdate{
            .input = 80,
            .output = 1,
            .cache_read = 20,
            .cache_write = 10,
            .cache_write_1h = 4,
        });
    ai::api::apply_anthropic_usage_delta(
        model,
        usage,
        ai::api::AnthropicUsageUpdate{
            .output = 9,
            .reasoning = 6,
        });

    const auto fixture = tests::read_pi_fixture("usage/normalization-and-cost.json");
    REQUIRE(fixture);
    check_usage_fixture(usage, fixture->at("anthropic"));
    CHECK(usage.cache_write_1h == 4);
}
