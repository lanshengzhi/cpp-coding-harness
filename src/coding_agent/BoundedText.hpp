#pragma once

#include "util/BoundedText.hpp"

#include <cstddef>
#include <string>
#include <utility>

namespace cch::coding_agent {

// Shared bounded-output policy for coding-agent presentation text. Secrets are
// redacted before bounding, the redaction marker is never split, and output
// remains valid UTF-8.
constexpr std::size_t kMaxPresentationPayloadBytes = 8192;
constexpr std::size_t kSecretBoundaryLookaheadBytes = 256;

[[nodiscard]] inline std::string bounded_redacted_presentation(
    std::string text,
    std::size_t max_bytes = kMaxPresentationPayloadBytes) {
    return util::bounded_redacted_text(
        std::move(text),
        max_bytes,
        {},
        kSecretBoundaryLookaheadBytes);
}

} // namespace cch::coding_agent
