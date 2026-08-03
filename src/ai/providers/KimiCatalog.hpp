#pragma once

#include <cch/ai/Model.hpp>

#include <vector>

namespace cch::ai::providers {

/// Frozen Kimi Coding catalog from pi-ai@0.83.0.
[[nodiscard]] std::vector<Model> kimi_coding_models();

} // namespace cch::ai::providers
