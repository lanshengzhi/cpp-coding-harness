#include "UsageNormalization.hpp"

#include <algorithm>
#include <cstdint>

namespace cch::ai::api {

/// Normalize a Chat Completions usage report: prompt tokens minus the cached
/// share are the billable input, reasoning tokens are informational, and the
/// shared cost rules price the result.
Usage normalize_completions_usage(const Model& model, const CompletionsUsageFields& fields) {
    const auto cache_read = fields.cached_tokens.value_or(fields.prompt_cache_hit_tokens.value_or(0));
    const auto cache_write = fields.cache_write_tokens.value_or(0);
    Usage usage{
            .input = std::max<std::int64_t>(0, fields.prompt_tokens - cache_read - cache_write),
            .output = fields.completion_tokens,
            .cache_read = cache_read,
            .cache_write = cache_write,
            .cache_write_1h = std::nullopt,
            .reasoning = fields.reasoning_tokens.value_or(0),
            .total_tokens = 0,
            .cost = {},
    };
    usage.total_tokens = usage.input + usage.output + usage.cache_read + usage.cache_write;
    usage.cost = calculate_cost(model, usage);
    return usage;
}

} // namespace cch::ai::api
