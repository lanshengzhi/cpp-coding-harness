#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace cch::tests {

/// The RGBA pixels an encoded image decodes to; null when the bytes cannot be
/// decoded. Used to compare image content independently of the encoder.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> decode_rgba(std::span<const std::uint8_t> bytes);

} // namespace cch::tests
