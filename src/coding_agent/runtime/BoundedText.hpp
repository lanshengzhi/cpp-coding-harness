#pragma once

#include "util/BoundedText.hpp"

#include <cstddef>
#include <string>
#include <utility>

namespace cch::coding_agent::runtime {

// Shared bounded-output policy for frontend presentation of provider-owned
// text (terminal diagnostics, partial content, message payloads): secrets are
// redacted before bounding, the redaction marker is never split by the byte
// budget, and output stays valid UTF-8.
constexpr std::size_t kMaxPayloadStringBytes = 8192;
constexpr std::size_t kSecretBoundaryLookaheadBytes = 256;

[[nodiscard]] inline std::string bounded_redacted(
    std::string text,
    std::size_t max_bytes = kMaxPayloadStringBytes) {
    return util::bounded_redacted_text(
        std::move(text),
        max_bytes,
        {},
        kSecretBoundaryLookaheadBytes);
}

} // namespace cch::coding_agent::runtime
