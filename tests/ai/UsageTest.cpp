#include <cch/ai/Usage.hpp>
#include "ai/api/UsageNormalization.hpp"
#include "support/ModelFixture.hpp"
#include "support/PiFixture.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

using namespace cch;

namespace {

void check_cost(const ai::UsageCost& cost, const support::JsonValue& expected) {
    const auto& object = expected.get_object();
    constexpr double kCostTolerance = 1e-12;
    CHECK(std::abs(cost.input - object.at("input").get_number()) < kCostTolerance);
    CHECK(std::abs(cost.output - object.at("output").get_number()) < kCostTolerance);
    CHECK(std::abs(cost.cache_read - object.at("cache_read").get_number()) < kCostTolerance);
    CHECK(std::abs(cost.cache_write - object.at("cache_write").get_number()) < kCostTolerance);
    CHECK(std::abs(cost.total - object.at("total").get_number()) < kCostTolerance);
}

void check_usage_fixture(const ai::Usage& usage, const support::JsonValue& expected) {
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

    check_cost(usage.cost, object.at("cost"));
}

} // namespace

TEST_CASE("Chat Completions usage splits uncached cached and reasoning tokens", "[ai][usage][issue339][compat-pi]") {
    auto model = tests::make_model("priced");
    model.cost = ai::ModelCost{.input = 2, .output = 4, .cache_read = 1, .cache_write = 3};

    const auto usage = ai::api::normalize_completions_usage(model,
            ai::api::CompletionsUsageFields{
                    .prompt_tokens = 120,
                    .completion_tokens = 30,
                    .cached_tokens = 20,
                    .reasoning_tokens = 7,
            });

    const auto fixture = tests::read_pi_fixture("usage/normalization-and-cost.json");
    REQUIRE(fixture);
    check_usage_fixture(usage, fixture->at("deepseek"));

    // The cached and cache-write shares clamp the billable input at zero.
    const auto clamped = ai::api::normalize_completions_usage(model,
            ai::api::CompletionsUsageFields{
                    .prompt_tokens = 5,
                    .cached_tokens = 4,
                    .cache_write_tokens = 3,
            });
    CHECK(clamped.input == 0);
    CHECK(clamped.reasoning == 0);
    CHECK(clamped.total_tokens == 7);

    // Relays that report DeepSeek's `prompt_cache_hit_tokens` instead of the
    // OpenAI `prompt_tokens_details.cached_tokens` split the same way.
    const auto fallback = ai::api::normalize_completions_usage(model,
            ai::api::CompletionsUsageFields{
                    .prompt_tokens = 50,
                    .completion_tokens = 10,
                    .prompt_cache_hit_tokens = 4,
            });
    CHECK(fallback.input == 46);
    CHECK(fallback.cache_read == 4);
    CHECK(fallback.total_tokens == 60);
}

TEST_CASE("shared cost pricing applies tiers and one-hour cache writes", "[ai][usage][issue339][compat-pi]") {
    // The fixture's Anthropic entry prices a request above the tier threshold
    // with four of its ten cache-write tokens at the one-hour rate: the tier
    // rates (10/20/5/15) replace the base rates (2/4/1/3), and one-hour writes
    // bill at twice the tier's input rate — short 6x15 + long 4x20 = 0.00017.
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
    usage.input = 80;
    usage.output = 9;
    usage.cache_read = 20;
    usage.cache_write = 10;
    usage.cache_write_1h = 4;
    usage.reasoning = 6;
    usage.total_tokens = 119;

    const auto fixture = tests::read_pi_fixture("usage/normalization-and-cost.json");
    REQUIRE(fixture);
    check_cost(ai::calculate_cost(model, usage), fixture->at("anthropic").at("cost"));

    // The threshold is exclusive: at exactly 100 input tokens (80 uncached +
    // 20 cached) the tier prices do not apply yet.
    usage.cache_write = 0;
    usage.cache_write_1h = std::nullopt;
    const auto at_threshold = ai::calculate_cost(model, usage);
    CHECK(std::abs(at_threshold.input - 80 * 2 / 1'000'000.0) < 1e-12);
    CHECK(std::abs(at_threshold.output - 9 * 4 / 1'000'000.0) < 1e-12);
    CHECK(std::abs(at_threshold.cache_read - 20 * 1 / 1'000'000.0) < 1e-12);
    CHECK(std::abs(at_threshold.cache_write) < 1e-12);

    usage.input = 10;
    usage.cache_read = 0;
    const auto below = ai::calculate_cost(model, usage);
    CHECK(std::abs(below.input - 10 * 2 / 1'000'000.0) < 1e-12);
}
