#pragma once

#include "util/BoundedText.hpp"

#include <cstddef>
#include <string>
#include <utility>

namespace cch::ai::providers {

inline constexpr std::size_t kMaxProviderErrorDetailBytes = 4096;

[[nodiscard]] inline std::string bounded_provider_error_detail(std::string detail) {
    return util::bounded_redacted_text(
        std::move(detail),
        kMaxProviderErrorDetailBytes);
}

} // namespace cch::ai::providers
