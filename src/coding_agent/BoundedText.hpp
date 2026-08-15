#pragma once

#include "ai/BoundedText.hpp"

#include <cstddef>
#include <string>
#include <string_view>
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
    return ai::bounded_redacted_text(
        std::move(text),
        max_bytes,
        {},
        kSecretBoundaryLookaheadBytes);
}

/// Bounds presentation text without redaction. User Bash command, output, and
/// error-diagnostic values pass through raw like pi (ADR 0028); bounding alone
/// keeps unbounded text from overwhelming the transcript.
[[nodiscard]] inline std::string bounded_presentation(
    std::string_view text,
    std::size_t max_bytes = kMaxPresentationPayloadBytes) {
    return ai::bounded_text(text, max_bytes);
}

} // namespace cch::coding_agent
