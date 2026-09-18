#pragma once

#include "support/BoundedText.hpp"

#include <cch/support/Error.hpp>

#include <cstddef>
#include <string>
#include <utility>

namespace cch::ai::providers {

inline constexpr std::size_t kMaxProviderErrorDetailBytes = 4096;

[[nodiscard]] inline std::string bounded_provider_error_detail(std::string detail) {
    return support::bounded_redacted_text(std::move(detail), kMaxProviderErrorDetailBytes);
}

/// One provider-facing Stream error with bounded, redacted message and detail.
[[nodiscard]] inline support::Error make_stream_error(std::string message, std::string detail = {}) {
    return support::make_error(support::ErrorCode::Stream,
            bounded_provider_error_detail(std::move(message)),
            bounded_provider_error_detail(std::move(detail)));
}

} // namespace cch::ai::providers
