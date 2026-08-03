#include <cch/ai/Usage.hpp>

#include <cch/ai/Model.hpp>

#include <algorithm>
#include <cstdint>

namespace cch::ai {

UsageCost calculate_cost(const Model& model, const Usage& usage) {
    const auto input_tokens = usage.input + usage.cache_read + usage.cache_write;
    double input_rate = model.cost.input;
    double output_rate = model.cost.output;
    double cache_read_rate = model.cost.cache_read;
    double cache_write_rate = model.cost.cache_write;
    std::uint64_t matched_threshold = 0;
    bool matched = false;
    if (model.cost.tiers) {
        for (const auto& tier : *model.cost.tiers) {
            if (input_tokens > static_cast<std::int64_t>(tier.input_tokens_above) &&
                (!matched || tier.input_tokens_above > matched_threshold)) {
                input_rate = tier.input;
                output_rate = tier.output;
                cache_read_rate = tier.cache_read;
                cache_write_rate = tier.cache_write;
                matched_threshold = tier.input_tokens_above;
                matched = true;
            }
        }
    }

    constexpr double kTokensPerMillion = 1'000'000.0;
    const auto long_write = usage.cache_write_1h.value_or(0);
    const auto short_write = usage.cache_write - long_write;
    UsageCost cost;
    cost.input = input_rate * static_cast<double>(usage.input) / kTokensPerMillion;
    cost.output = output_rate * static_cast<double>(usage.output) / kTokensPerMillion;
    cost.cache_read =
        cache_read_rate * static_cast<double>(usage.cache_read) / kTokensPerMillion;
    cost.cache_write =
        (cache_write_rate * static_cast<double>(short_write) +
         input_rate * 2 * static_cast<double>(long_write)) /
        kTokensPerMillion;
    cost.total = cost.input + cost.output + cost.cache_read + cost.cache_write;
    return cost;
}

} // namespace cch::ai
