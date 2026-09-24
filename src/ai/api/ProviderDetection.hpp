#pragma once

#include <cch/ai/Model.hpp>

#include <algorithm>
#include <cctype>
#include <string_view>

namespace cch::ai::api {

/// Case-insensitive substring test over ASCII URL text. std tolower applies
/// per byte through an unsigned char cast; an empty needle never matches.
[[nodiscard]] inline bool contains_case_insensitive(std::string_view value, std::string_view needle) {
    const auto equal_ignoring_case = [](char left, char right) {
        return std::tolower(static_cast<unsigned char>(left)) == std::tolower(static_cast<unsigned char>(right));
    };
    return !std::ranges::search(value, needle, equal_ignoring_case).empty();
}

/// True when the model identifies as OpenRouter by wire provider id or by an
/// openrouter.ai base URL. The URL test is case-insensitive: URL hosts are
/// case-insensitive, so "https://OpenRouter.AI/api/v1" still detects.
[[nodiscard]] inline bool is_openrouter(const Model& model) {
    return model.provider == "openrouter" || contains_case_insensitive(model.base_url, "openrouter.ai");
}

/// True when the model identifies as DeepSeek by wire provider id or by a
/// deepseek.com base URL, matched case-insensitively like is_openrouter.
[[nodiscard]] inline bool is_deepseek(const Model& model) {
    return model.provider == "deepseek" || contains_case_insensitive(model.base_url, "deepseek.com");
}

// debt: replace URL heuristics with explicit provider metadata when custom
// provider composition carries that identity through the model contract.

} // namespace cch::ai::api
