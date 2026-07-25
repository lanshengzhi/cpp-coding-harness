#pragma once

#include "Redactor.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace cch::util {

[[nodiscard]] inline std::string bounded_utf8(
    std::string_view text,
    std::size_t max_bytes) {
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
            valid = !((lead == 0xe0 && second < 0xa0) ||
                      (lead == 0xed && second >= 0xa0));
        }
        if (valid && length == 4) {
            const auto second = static_cast<unsigned char>(text[index + 1]);
            valid = !((lead == 0xf0 && second < 0x90) ||
                      (lead == 0xf4 && second >= 0x90));
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

[[nodiscard]] inline std::string bounded_text(
    std::string_view text,
    std::size_t max_bytes,
    std::string_view suffix = {},
    bool force_truncated = false) {
    if (max_bytes == 0) {
        return {};
    }
    if (!force_truncated && text.size() <= max_bytes) {
        return bounded_utf8(text, max_bytes);
    }
    if (max_bytes <= suffix.size()) {
        return bounded_utf8(suffix, max_bytes);
    }

    const auto content_bytes = max_bytes - suffix.size();
    auto safe = bounded_utf8(text, content_bytes);
    safe += suffix;
    return safe;
}

[[nodiscard]] inline std::string bounded_redacted_text(
    std::string text,
    std::size_t max_bytes,
    std::string_view suffix = {},
    std::size_t boundary_lookahead_bytes = 256) {
    if (max_bytes == 0) {
        return {};
    }
    const auto inspected_bytes = max_bytes > text.max_size() - boundary_lookahead_bytes
        ? max_bytes
        : max_bytes + boundary_lookahead_bytes;
    const bool input_truncated = text.size() > inspected_bytes;
    if (input_truncated) {
        text.resize(inspected_bytes);
    }
    text = redact_text(std::move(text));

    const auto content_bytes = max_bytes > suffix.size()
        ? max_bytes - suffix.size()
        : 0;
    if (content_bytes >= kRedactionMarker.size() && text.size() > content_bytes) {
        const auto crossing_marker = text.rfind(kRedactionMarker, content_bytes);
        if (crossing_marker != std::string::npos &&
            crossing_marker + kRedactionMarker.size() > content_bytes) {
            auto safe = bounded_utf8(text, content_bytes - kRedactionMarker.size());
            safe += kRedactionMarker;
            safe += suffix;
            return safe;
        }
    }
    return bounded_text(text, max_bytes, suffix, input_truncated);
}

} // namespace cch::util
