#pragma once

#include <cch/ai/Context.hpp>
#include <cch/ai/Model.hpp>

#include <algorithm>
#include <vector>

namespace cch::ai::api {

[[nodiscard]] inline bool supports_images(const Model& model) {
    return std::ranges::find(model.input, ModelInput::Image) != model.input.end();
}

/// Normalized provider history: synthesized user/tool scaffolding and image
/// downgrades. Tool-call ids pass through unchanged (ADR 0074).
[[nodiscard]] std::vector<MessageVariant> normalize_history(const Model& model, const AiContext& context);

} // namespace cch::ai::api
