#pragma once

#include <cch/ai/Context.hpp>
#include <cch/ai/Model.hpp>
#include <cch/ai/RequestOptions.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace cch::ai::detail {

struct ContextTokenEstimate {
    std::uint64_t tokens{0};
    std::uint64_t usage_tokens{0};
    std::uint64_t trailing_tokens{0};
    std::optional<std::size_t> last_usage_index{std::nullopt};
};

[[nodiscard]] ContextTokenEstimate estimate_context_tokens(const AiContext& context);
[[nodiscard]] std::uint64_t clamp_max_tokens_to_context(
    const Model& model,
    const AiContext& context,
    std::uint64_t max_tokens);
[[nodiscard]] std::string clamp_openai_prompt_cache_key(std::string_view key);
[[nodiscard]] CacheRetention resolve_cache_retention(
    std::optional<CacheRetention> requested,
    const ProviderEnv& env);

} // namespace cch::ai::detail
