#pragma once

#include <cch/ai/Model.hpp>
#include <cch/ai/Usage.hpp>

#include <cstdint>
#include <optional>

namespace cch::ai::api {

struct CompletionsUsageFields {
    std::int64_t prompt_tokens{0};
    std::int64_t completion_tokens{0};
    std::optional<std::int64_t> cached_tokens{std::nullopt};
    std::optional<std::int64_t> prompt_cache_hit_tokens{std::nullopt};
    std::optional<std::int64_t> cache_write_tokens{std::nullopt};
    std::optional<std::int64_t> reasoning_tokens{std::nullopt};
};

[[nodiscard]] Usage normalize_completions_usage(const Model& model, const CompletionsUsageFields& fields);

} // namespace cch::ai::api
