#pragma once

#include <cch/support/Error.hpp>

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace cch::ai {

enum class ModelInput { Text, Image };

/// Caller reasoning requests exclude Off; omission disables reasoning.
enum class ThinkingLevel { Minimal, Low, Medium, High, XHigh, Max };
/// Model capability maps include Off so catalogs can mark it unsupported.
enum class ModelThinkingLevel { Off, Minimal, Low, Medium, High, XHigh, Max };

using ThinkingLevelMap = std::map<ModelThinkingLevel, std::optional<std::string>>;
using ModelHeaders = std::map<std::string, std::string, std::less<>>;

struct ModelCostTier {
    double input{0};
    double output{0};
    double cache_read{0};
    double cache_write{0};
    std::uint64_t input_tokens_above{0};
};

struct ModelCost {
    double input{0};
    double output{0};
    double cache_read{0};
    double cache_write{0};
    std::optional<std::vector<ModelCostTier>> tiers{std::nullopt};
};

enum class OpenAICompletionsMaxTokensField { MaxCompletionTokens, MaxTokens };
enum class OpenAICompletionsThinkingFormat { OpenAI, OpenRouter, DeepSeek, Qwen };
enum class OpenAICompletionsCacheControlFormat { Anthropic };
enum class OpenAIResponsesSessionAffinityFormat { OpenAI, OpenAINoSession, OpenRouter };

/// Typed compatibility values populated by the shipped openai-completions
/// catalog. An absent member keeps the adapter's scoped upstream detection.
struct OpenAICompletionsCompat {
    std::optional<bool> supports_store{std::nullopt};
    std::optional<bool> supports_developer_role{std::nullopt};
    std::optional<bool> supports_strict_mode{std::nullopt};
    std::optional<OpenAICompletionsMaxTokensField> max_tokens_field{std::nullopt};
    std::optional<bool> requires_reasoning_content_on_assistant_messages{std::nullopt};
    std::optional<OpenAICompletionsThinkingFormat> thinking_format{std::nullopt};
    std::optional<OpenAICompletionsCacheControlFormat> cache_control_format{std::nullopt};
    std::optional<bool> supports_long_cache_retention{std::nullopt};
    std::optional<bool> supports_reasoning_effort{std::nullopt};
};

/// Typed compatibility values populated by the shipped openai-responses
/// catalog and consumed by the current Responses payload seam. The pinned
/// snapshot has `sessionAffinityFormat` only on OpenCode Go and has no
/// `supportsMaxOutputTokens`; the latter remains unrepresented.
struct OpenAIResponsesCompat {
    std::optional<OpenAIResponsesSessionAffinityFormat> session_affinity_format{std::nullopt};
    std::optional<bool> supports_strict_mode{std::nullopt};
    std::optional<bool> supports_explicit_prompt_cache_mode{std::nullopt};
};

/// Typed compatibility values populated by the shipped
/// anthropic-messages catalog. Missing booleans retain provider defaults.
struct AnthropicMessagesCompat {
    std::optional<bool> force_adaptive_thinking{std::nullopt};
    std::optional<bool> allow_empty_signature{std::nullopt};
    std::optional<bool> supports_temperature{std::nullopt};
};

using ModelCompatVariant = std::variant<AnthropicMessagesCompat, OpenAICompletionsCompat, OpenAIResponsesCompat>;

/// Complete passive, credential-free identity and capability value for one
/// model (ADR 0019). Provider and API identities are independent: provider
/// selects runtime/auth ownership while API selects protocol execution.
struct Model {
    std::string id{};
    std::string name{};
    std::string api{};
    std::string provider{};
    std::string base_url{};
    bool reasoning{false};
    /// No map means the catalog supplied no mapping. Within a present map, a
    /// missing key means provider default and a present null means unsupported.
    std::optional<ThinkingLevelMap> thinking_level_map{std::nullopt};
    std::vector<ModelInput> input{};
    ModelCost cost{};
    std::uint64_t context_window{0};
    std::uint64_t max_tokens{0};
    std::optional<ModelHeaders> headers{std::nullopt};
    std::optional<ModelCompatVariant> compat{std::nullopt};
};

[[nodiscard]] cch::support::ExpectedVoid validate_model(const Model& model);
[[nodiscard]] std::vector<ModelThinkingLevel> get_supported_thinking_levels(const Model& model);
[[nodiscard]] ModelThinkingLevel clamp_thinking_level(
    const Model& model,
    ModelThinkingLevel requested);

/// The seven Thinking Level wire names paired with their levels, in the order
/// `clamp_thinking_level` walks when it widens or narrows a request.
inline constexpr std::array<std::pair<ModelThinkingLevel, std::string_view>, 7> kModelThinkingLevels{{
        {ModelThinkingLevel::Off, "off"},
        {ModelThinkingLevel::Minimal, "minimal"},
        {ModelThinkingLevel::Low, "low"},
        {ModelThinkingLevel::Medium, "medium"},
        {ModelThinkingLevel::High, "high"},
        {ModelThinkingLevel::XHigh, "xhigh"},
        {ModelThinkingLevel::Max, "max"},
}};

/// The wire name of `level`, or `std::nullopt` for a level outside the
/// seven-level vocabulary.
[[nodiscard]] inline std::optional<std::string_view> model_thinking_level_name(ModelThinkingLevel level) {
    for (const auto& [known_level, name] : kModelThinkingLevels) {
        if (known_level == level) {
            return name;
        }
    }
    return std::nullopt;
}

/// The level named by `name`, or `std::nullopt` when `name` is not one of the
/// seven wire names; validation owns rejection of invalid names.
[[nodiscard]] inline std::optional<ModelThinkingLevel> parse_model_thinking_level(std::string_view name) {
    for (const auto& [level, known_name] : kModelThinkingLevels) {
        if (known_name == name) {
            return level;
        }
    }
    return std::nullopt;
}

/// The stream `ThinkingLevel` named by a model thinking-level wire name:
/// "minimal".."max" map to the stream level; "off", empty, or unknown names
/// forward no reasoning (pi `createLoopConfig` derives the per-turn stream
/// `reasoning` option from the thinking level: `off` → undefined).
[[nodiscard]] inline std::optional<ThinkingLevel> parse_stream_thinking_level(std::string_view name) {
    const auto level = parse_model_thinking_level(name);
    if (!level) {
        return std::nullopt;
    }
    switch (*level) {
    case ModelThinkingLevel::Minimal:
        return ThinkingLevel::Minimal;
    case ModelThinkingLevel::Low:
        return ThinkingLevel::Low;
    case ModelThinkingLevel::Medium:
        return ThinkingLevel::Medium;
    case ModelThinkingLevel::High:
        return ThinkingLevel::High;
    case ModelThinkingLevel::XHigh:
        return ThinkingLevel::XHigh;
    case ModelThinkingLevel::Max:
        return ThinkingLevel::Max;
    case ModelThinkingLevel::Off:
        return std::nullopt;
    }
    return std::nullopt;
}

/// Clamp a wire-name thinking level ("off".."max") to the model's supported
/// set and return the clamped wire name (pi-ai `clampThinkingLevel` applied to
/// the agent-level string vocabulary). An unparseable request is returned
/// unchanged so validation owns rejection of invalid level names.
[[nodiscard]] std::string clamp_thinking_level_string(
    const Model& model,
    std::string_view requested);

} // namespace cch::ai
