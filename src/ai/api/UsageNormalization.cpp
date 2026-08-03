#include "UsageNormalization.hpp"

#include <algorithm>
#include <cstdint>

namespace cch::ai::api {
namespace {

void recompute_anthropic_totals(const Model& model, Usage& usage) {
    usage.total_tokens = usage.input + usage.output + usage.cache_read + usage.cache_write;
    usage.cost = calculate_cost(model, usage);
}

} // namespace

Usage normalize_responses_usage(
    const Model& model,
    const ResponsesUsageFields& fields) {
    Usage usage{
        .input = std::max<std::int64_t>(
            0,
            fields.input_tokens - fields.cached_tokens - fields.cache_write_tokens),
        .output = fields.output_tokens,
        .cache_read = fields.cached_tokens,
        .cache_write = fields.cache_write_tokens,
        .cache_write_1h = std::nullopt,
        .reasoning = fields.reasoning_tokens.value_or(0),
        .total_tokens = fields.total_tokens,
        .cost = {},
    };
    usage.cost = calculate_cost(model, usage);
    return usage;
}

Usage normalize_deepseek_usage(
    const Model& model,
    std::int64_t input_tokens,
    std::int64_t output_tokens,
    std::int64_t cached_tokens,
    std::optional<std::int64_t> reasoning_tokens) {
    return normalize_responses_usage(
        model,
        ResponsesUsageFields{
            .input_tokens = input_tokens,
            .output_tokens = output_tokens,
            .cached_tokens = cached_tokens,
            .cache_write_tokens = 0,
            .reasoning_tokens = reasoning_tokens,
            .total_tokens = input_tokens + output_tokens,
        });
}

void apply_anthropic_usage_start(
    const Model& model,
    Usage& usage,
    const AnthropicUsageUpdate& update) {
    usage.input = update.input.value_or(0);
    usage.output = update.output.value_or(0);
    usage.cache_read = update.cache_read.value_or(0);
    usage.cache_write = update.cache_write.value_or(0);
    usage.cache_write_1h = update.cache_write_1h;
    usage.reasoning = update.reasoning;
    recompute_anthropic_totals(model, usage);
}

void apply_anthropic_usage_delta(
    const Model& model,
    Usage& usage,
    const AnthropicUsageUpdate& update) {
    if (update.input) {
        usage.input = *update.input;
    }
    if (update.output) {
        usage.output = *update.output;
    }
    if (update.cache_read) {
        usage.cache_read = *update.cache_read;
    }
    if (update.cache_write) {
        usage.cache_write = *update.cache_write;
    }
    if (update.reasoning) {
        usage.reasoning = update.reasoning;
    }
    recompute_anthropic_totals(model, usage);
}

} // namespace cch::ai::api
