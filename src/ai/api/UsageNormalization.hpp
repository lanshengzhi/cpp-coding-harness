#pragma once

#include <cch/ai/Model.hpp>
#include <cch/ai/Usage.hpp>

#include <cstdint>
#include <optional>

namespace cch::ai::api {

struct ResponsesUsageFields {
    std::int64_t input_tokens{0};
    std::int64_t output_tokens{0};
    std::int64_t cached_tokens{0};
    std::int64_t cache_write_tokens{0};
    std::optional<std::int64_t> reasoning_tokens{std::nullopt};
    std::int64_t total_tokens{0};
};

struct AnthropicUsageUpdate {
    std::optional<std::int64_t> input{std::nullopt};
    std::optional<std::int64_t> output{std::nullopt};
    std::optional<std::int64_t> cache_read{std::nullopt};
    std::optional<std::int64_t> cache_write{std::nullopt};
    std::optional<std::int64_t> cache_write_1h{std::nullopt};
    std::optional<std::int64_t> reasoning{std::nullopt};
};

[[nodiscard]] Usage normalize_responses_usage(
    const Model& model,
    const ResponsesUsageFields& fields);
[[nodiscard]] Usage normalize_deepseek_usage(
    const Model& model,
    std::int64_t input_tokens,
    std::int64_t output_tokens,
    std::int64_t cached_tokens,
    std::optional<std::int64_t> reasoning_tokens);
void apply_anthropic_usage_start(
    const Model& model,
    Usage& usage,
    const AnthropicUsageUpdate& update);
void apply_anthropic_usage_delta(
    const Model& model,
    Usage& usage,
    const AnthropicUsageUpdate& update);

} // namespace cch::ai::api
