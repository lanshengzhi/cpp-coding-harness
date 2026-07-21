#pragma once

#include "util/Redactor.hpp"

#include <algorithm>
#include <string>
#include <string_view>

namespace cch::coding_agent::runtime {

// Shared bounded-output policy for frontend presentation of provider-owned
// text (terminal diagnostics, partial content, message payloads): secrets are
// redacted before bounding, the redaction marker is never split by the byte
// budget, and output stays valid UTF-8.
constexpr std::size_t kMaxPayloadStringBytes = 8192;
constexpr std::size_t kSecretBoundaryLookaheadBytes = 256;

[[nodiscard]] inline std::string bounded_utf8(std::string_view text, std::size_t max_bytes) {
    std::string safe;
    safe.reserve(std::min(text.size(), max_bytes));
    std::size_t index = 0;
    while (index < text.size() && safe.size() < max_bytes) {
        const auto lead = static_cast<unsigned char>(text[index]);
        std::size_t length = 0;
        if (lead <= 0x7f) {
            length = 1;
        } else if (lead >= 0xc2 && lead <= 0xdf) {
            length = 2;
        } else if (lead >= 0xe0 && lead <= 0xef) {
            length = 3;
        } else if (lead >= 0xf0 && lead <= 0xf4) {
            length = 4;
        }

        bool valid = length != 0 && index + length <= text.size();
        for (std::size_t offset = 1; valid && offset < length; ++offset) {
            const auto continuation = static_cast<unsigned char>(text[index + offset]);
            valid = continuation >= 0x80 && continuation <= 0xbf;
        }
        if (valid && length == 3) {
            const auto second = static_cast<unsigned char>(text[index + 1]);
            valid = !((lead == 0xe0 && second < 0xa0) || (lead == 0xed && second >= 0xa0));
        }
        if (valid && length == 4) {
            const auto second = static_cast<unsigned char>(text[index + 1]);
            valid = !((lead == 0xf0 && second < 0x90) || (lead == 0xf4 && second >= 0x90));
        }

        if (!valid) {
            constexpr std::string_view replacement{"\xef\xbf\xbd"};
            if (safe.size() + replacement.size() > max_bytes) {
                break;
            }
            safe.append(replacement);
            ++index;
            continue;
        }
        if (safe.size() + length > max_bytes) {
            break;
        }
        safe.append(text, index, length);
        index += length;
    }
    return safe;
}

[[nodiscard]] inline std::string bounded_redacted(
    std::string text,
    std::size_t max_bytes = kMaxPayloadStringBytes) {
    if (max_bytes == 0) {
        return {};
    }
    const auto inspected_bytes = max_bytes > text.max_size() - kSecretBoundaryLookaheadBytes
        ? max_bytes
        : max_bytes + kSecretBoundaryLookaheadBytes;
    if (text.size() > inspected_bytes) {
        text.resize(inspected_bytes);
    }
    text = util::redact_text(std::move(text));

    constexpr std::string_view marker{"[REDACTED]"};
    if (text.size() > max_bytes && max_bytes >= marker.size()) {
        const auto crossing_marker = text.rfind(marker, max_bytes);
        if (crossing_marker != std::string::npos && crossing_marker + marker.size() > max_bytes) {
            text = text.substr(0, max_bytes - marker.size()) + std::string(marker);
        }
    }
    return bounded_utf8(text, max_bytes);
}

} // namespace cch::coding_agent::runtime
