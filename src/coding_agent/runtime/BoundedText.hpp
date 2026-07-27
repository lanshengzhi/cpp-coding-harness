#pragma once

#include "coding_agent/BoundedText.hpp"

#include <cstddef>
#include <string>
#include <utility>

namespace cch::coding_agent::runtime {

constexpr std::size_t kMaxPayloadStringBytes = kMaxPresentationPayloadBytes;
constexpr std::size_t kSecretBoundaryLookaheadBytes = cch::coding_agent::kSecretBoundaryLookaheadBytes;

[[nodiscard]] inline std::string bounded_redacted(
    std::string text,
    std::size_t max_bytes = kMaxPayloadStringBytes) {
    return bounded_redacted_presentation(std::move(text), max_bytes);
}

} // namespace cch::coding_agent::runtime
