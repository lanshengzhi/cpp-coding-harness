#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace cch::tests {

inline constexpr std::string_view kTinyPngBase64 =
    "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACAQMAAABIeJ9nAAAAIGNIUk0AAHomAACAhAAA+gAAAIDoAAB1MAAA6mAAADqYAAAXcJy6UTwAAAAGUExURf8AAP///0EdNBEAAAABYktHRAH/Ai3eAAAAB3RJTUUH6gEOADM5Ddoh/wAAAAxJREFUCNdjYGBgAAAABAABJzQnCgAAACV0RVh0ZGF0ZTpjcmVhdGUAMjAyNi0wMS0xNFQwMDo1MTo1NyswMDowMOnKzHgAAAAldEVYdGRhdGU6bW9kaWZ5ADIwMjYtMDEtMTRUMDA6NTE6NTcrMDA6MDCYl3TEAAAAKHRFWHRkYXRlOnRpbWVzdGFtcAAyMDI2LTAxLTE0VDAwOjUxOjU3KzAwOjAwz4JVGwAAAABJRU5ErkJggg==";
inline constexpr std::string_view kTinyJpegBase64 =
    "/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAMCAgMCAgMDAwMEAwMEBQgFBQQEBQoHBwYIDAoMDAsKCwsNDhIQDQ4RDgsLEBYQERMUFRUVDA8XGBYUGBIUFRT/2wBDAQMEBAUEBQkFBQkUDQsNFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBT/wAARCAACAAIDAREAAhEBAxEB/8QAFAABAAAAAAAAAAAAAAAAAAAACf/EABQQAQAAAAAAAAAAAAAAAAAAAAD/xAAVAQEBAAAAAAAAAAAAAAAAAAAGCf/EABQRAQAAAAAAAAAAAAAAAAAAAAD/2gAMAwEAAhEDEQA/AD3VTB3/2Q==";
inline constexpr std::string_view kTinyGifBase64 =
    "R0lGODlhAQABAIAAAAAAAP///ywAAAAAAQABAAACAUwAOw==";
inline constexpr std::string_view kTinyWebpBase64 =
    "UklGRhwAAABXRUJQVlA4TA8AAAAvAUAAAAcQ/Y/+ByKi/wEA";
inline constexpr std::string_view kOrientedJpegBase64 =
    "/9j/4AAQSkZJRgABAQAAAQABAAD/4QAiRXhpZgAATU0AKgAAAAgAAQESAAMAAAABAAYAAAAAAAD/2wBDAAgGBgcGBQgHBwcJCQgKDBQNDAsLDBkSEw8UHRofHh0aHBwgJC4nICIsIxwcKDcpLDAxNDQ0Hyc5PTgyPC4zNDL/2wBDAQkJCQwLDBgNDRgyIRwhMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjL/wAARCAACAAMDASIAAhEBAxEB/8QAHwAAAQUBAQEBAQEAAAAAAAAAAAECAwQFBgcICQoL/8QAtRAAAgEDAwIEAwUFBAQAAAF9AQIDAAQRBRIhMUEGE1FhByJxFDKBkaEII0KxwRVS0fAkM2JyggkKFhcYGRolJicoKSo0NTY3ODk6Q0RFRkdISUpTVFVWV1hZWmNkZWZnaGlqc3R1dnd4eXqDhIWGh4iJipKTlJWWl5iZmqKjpKWmp6ipqrKztLW2t7i5usLDxMXGx8jJytLT1NXW19jZ2uHi4+Tl5ufo6erx8vP09fb3+Pn6/8QAHwEAAwEBAQEBAQEBAQAAAAAAAAECAwQFBgcICQoL/8QAtREAAgECBAQDBAcFBAQAAQJ3AAECAxEEBSExBhJBUQdhcRMiMoEIFEKRobHBCSMzUvAVYnLRChYkNOEl8RcYGRomJygpKjU2Nzg5OkNERUZHSElKU1RVVldYWVpjZGVmZ2hpanN0dXZ3eHl6goOEhYaHiImKkpOUlZaXmJmaoqOkpaanqKmqsrO0tba3uLm6wsPExcbHyMnK0tPU1dbX2Nna4uPk5ebn6Onq8vP09fb3+Pn6/9oADAMBAAIRAxEAPwDi6KKK+ZP3E//Z";
inline constexpr std::string_view kPatternedJpegBase64 =
        "/9j/4AAQSkZJRgABAQAAAQABAAD/4QAiRXhpZgAATU0AKgAAAAgAAQESAAMAAAABAAYAAAAAAAD/"
        "2wBDAAMCAgMCAgMDAwMEAwMEBQgFBQQEBQoHBwYIDAoMDAsKCwsNDhIQDQ4RDgsLEBYQERMUFRUVDA8XGBYUGBIUFRT/"
        "2wBDAQMEBAUEBQkFBQkUDQsNFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBT/"
        "wAARCAAIABADAREAAhEBAxEB/8QAHwAAAQUBAQEBAQEAAAAAAAAAAAECAwQFBgcICQoL/"
        "8QAtRAAAgEDAwIEAwUFBAQAAAF9AQIDAAQRBRIhMUEGE1FhByJxFDKBkaEII0KxwRVS0fAkM2JyggkKFhcYGRolJicoKSo0NTY3ODk6Q0RFRkd"
        "ISUpTVFVWV1hZWmNkZWZnaGlqc3R1dnd4eXqDhIWGh4iJipKTlJWWl5iZmqKjpKWmp6ipqrKztLW2t7i5usLDxMXGx8jJytLT1NXW19jZ2uHi4"
        "+Tl5ufo6erx8vP09fb3+Pn6/8QAHwEAAwEBAQEBAQEBAQAAAAAAAAECAwQFBgcICQoL/"
        "8QAtREAAgECBAQDBAcFBAQAAQJ3AAECAxEEBSExBhJBUQdhcRMiMoEIFEKRobHBCSMzUvAVYnLRChYkNOEl8RcYGRomJygpKjU2Nzg5OkNERUZ"
        "HSElKU1RVVldYWVpjZGVmZ2hpanN0dXZ3eHl6goOEhYaHiImKkpOUlZaXmJmaoqOkpaanqKmqsrO0tba3uLm6wsPExcbHyMnK0tPU1dbX2Nna4"
        "uPk5ebn6Onq8vP09fb3+Pn6/9oADAMBAAIRAxEAPwD42/4Wr/1C/wDyY/8Asa/cP+JZf+px/wCUP/ux/Rv/ABMN/wBSr/yt/"
        "wDcj9af+Gdf+pg/8kv/ALZX+On+tH/Tn/yb/wC1P8//APiBf/Uy/wDKX/3U/9k=";
inline constexpr std::string_view kOrientedWebpBase64 =
    "UklGRnAAAABXRUJQVlA4WAoAAAAIAAAAAgAAAQAAVlA4IDAAAADQAQCdASoDAAIAAUAmJaACdLoB+AADsAD+8ut//NgVzXPv9//S4P0uD9Lg/9KQAABFWElGGgAAAE1NACoAAAAIAAEBEgADAAAAAQAGAAAAAAAA";

[[nodiscard]] inline std::vector<std::uint8_t> decode_base64(std::string_view encoded) {
    constexpr std::string_view kAlphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::array<int, 256> values{};
    values.fill(-1);
    for (std::size_t index = 0; index < kAlphabet.size(); ++index) {
        values[static_cast<unsigned char>(kAlphabet[index])] = static_cast<int>(index);
    }

    std::vector<std::uint8_t> decoded;
    std::uint32_t accumulator = 0;
    int bits = 0;
    for (const auto character : encoded) {
        if (character == '=') break;
        const auto value = values[static_cast<unsigned char>(character)];
        if (value < 0) continue;
        accumulator = (accumulator << 6U) | static_cast<std::uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            decoded.push_back(static_cast<std::uint8_t>(accumulator >> bits));
            accumulator &= (std::uint32_t{1} << bits) - 1U;
        }
    }
    return decoded;
}

[[nodiscard]] inline std::string bytes_as_string(const std::vector<std::uint8_t>& bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

/// A 2x2 24-bit bottom-up BMP. Its format is not one providers take inline, so
/// reading it requires a configured image processor.
[[nodiscard]] inline std::vector<std::uint8_t> tiny_bmp() {
    std::vector<std::uint8_t> bmp(70, 0);
    bmp[0] = 'B';
    bmp[1] = 'M';
    const auto put32 = [&bmp](std::size_t offset, std::uint32_t value) {
        bmp[offset] = static_cast<std::uint8_t>(value);
        bmp[offset + 1] = static_cast<std::uint8_t>(value >> 8U);
        bmp[offset + 2] = static_cast<std::uint8_t>(value >> 16U);
        bmp[offset + 3] = static_cast<std::uint8_t>(value >> 24U);
    };
    const auto put16 = [&bmp](std::size_t offset, std::uint16_t value) {
        bmp[offset] = static_cast<std::uint8_t>(value);
        bmp[offset + 1] = static_cast<std::uint8_t>(value >> 8U);
    };
    put32(2, 70);
    put32(10, 54);
    put32(14, 40);
    put32(18, 2);
    put32(22, 2);
    put16(26, 1);
    put16(28, 24);
    put32(34, 16);
    // Two rows of two BGR pixels, bottom row first.
    constexpr std::array<std::uint8_t, 16> kPixels{0, 0, 255, 0, 255, 0, 255, 0, 0, 0, 0, 255, 0, 255, 0, 255};
    for (std::size_t index = 0; index < kPixels.size(); ++index)
        bmp[54 + index] = kPixels[index];
    return bmp;
}

/// The tiny PNG with an animation-control chunk inserted before `IDAT`.
[[nodiscard]] inline std::vector<std::uint8_t> animated_png() {
    auto png = decode_base64(kTinyPngBase64);
    constexpr std::array<std::uint8_t, 12> kAnimationChunk{0, 0, 0, 0, 'a', 'c', 'T', 'L', 0, 0, 0, 0};
    png.insert(png.begin() + 33, kAnimationChunk.begin(), kAnimationChunk.end());
    return png;
}

} // namespace cch::tests
