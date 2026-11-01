#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace cch::durable {

/// Incremental UTF-8 decoder for text that arrives in chunks: well-formed
/// sequences pass through unchanged, and every invalid or incomplete sequence
/// becomes U+FFFD. A character split across `decode` calls survives because the
/// incomplete tail is kept until the next call or `finish`.
class Utf8Decoder {
public:
    [[nodiscard]] std::string decode(std::string_view chunk, bool finish = false) {
        pending_.append(chunk);
        std::string result;
        std::size_t i = 0;
        while (i < pending_.size()) {
            auto lead = static_cast<unsigned char>(pending_[i]);
            if (lead < 0x80) {
                result.push_back(pending_[i++]);
                continue;
            }
            std::size_t length = lead >= 0xc2 && lead <= 0xdf   ? 2
                                 : lead >= 0xe0 && lead <= 0xef ? 3
                                 : lead >= 0xf0 && lead <= 0xf4 ? 4
                                                                : 0;
            if (length == 0) {
                result += "\xef\xbf\xbd";
                ++i;
                continue;
            }
            std::size_t valid = 1;
            while (valid < length && i + valid < pending_.size()) {
                auto byte = static_cast<unsigned char>(pending_[i + valid]);
                if (byte < 0x80 || byte > 0xbf ||
                        (valid == 1 && ((lead == 0xe0 && byte < 0xa0) || (lead == 0xed && byte > 0x9f) ||
                                               (lead == 0xf0 && byte < 0x90) || (lead == 0xf4 && byte > 0x8f))))
                    break;
                ++valid;
            }
            if (valid == length) {
                result.append(pending_, i, length);
                i += length;
            } else if (i + valid == pending_.size() && !finish) {
                break;
            } else {
                result += "\xef\xbf\xbd";
                i += valid;
            }
        }
        pending_.erase(0, i);
        return result;
    }

private:
    std::string pending_;
};

/// One whole buffer decoded as text; a leading or trailing partial character
/// becomes U+FFFD, so a byte range that starts or ends inside a sequence still
/// yields valid UTF-8.
[[nodiscard]] inline std::string decode_utf8(std::string_view bytes) {
    Utf8Decoder decoder;
    return decoder.decode(bytes, true);
}

} // namespace cch::durable
