#pragma once

#include <cch/support/Error.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
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

/// The only behavior-bearing per-API compatibility value in the supported
/// adapter surface (ADR 0033). Missing booleans retain provider defaults.
struct AnthropicMessagesCompat {
    std::optional<bool> force_adaptive_thinking{std::nullopt};
    std::optional<bool> allow_empty_signature{std::nullopt};
};

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
    std::optional<AnthropicMessagesCompat> compat{std::nullopt};
};

[[nodiscard]] cch::support::ExpectedVoid validate_model(const Model& model);
[[nodiscard]] std::vector<ModelThinkingLevel> get_supported_thinking_levels(const Model& model);
[[nodiscard]] ModelThinkingLevel clamp_thinking_level(
    const Model& model,
    ModelThinkingLevel requested);

/// Clamp a wire-name thinking level ("off".."max") to the model's supported
/// set and return the clamped wire name (pi-ai `clampThinkingLevel` applied to
/// the agent-level string vocabulary). An unparseable request is returned
/// unchanged so validation owns rejection of invalid level names.
[[nodiscard]] std::string clamp_thinking_level_string(
    const Model& model,
    std::string_view requested);

} // namespace cch::ai
