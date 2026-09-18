#pragma once

#include "MessageConversion.hpp"

#include <algorithm>
#include <vector>

namespace cch::ai::api {

[[nodiscard]] inline bool supports_images(const Model& model) {
    return std::ranges::find(model.input, ModelInput::Image) != model.input.end();
}

[[nodiscard]] inline bool reasoning_off_supported(const Model& model) {
    if (!model.thinking_level_map) {
        return true;
    }
    const auto found = model.thinking_level_map->find(ModelThinkingLevel::Off);
    return found == model.thinking_level_map->end() || found->second.has_value();
}

/// Normalized cross-protocol history: synthesized user/tool scaffolding,
/// image downgrades, and per-adapter tool-call id rewriting.
[[nodiscard]] std::vector<MessageVariant> normalize_history(
        AdapterKind adapter, const Model& model, const AiContext& context);

} // namespace cch::ai::api
