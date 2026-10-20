#include <cch/tui/ProgramStatus.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace cch::tui {
namespace {

constexpr std::string_view kReplyPrefix = "\x1b]7501;?";
/// Decoded `msg` limit (pi `MAX_MESSAGE_BYTES`). Its base64 encoding stays
/// under the 2732-byte encoded limit.
constexpr std::size_t kMaxMessageBytes = 2048;

[[nodiscard]] std::string_view state_name(ProgramStatus::State state) {
    switch (state) {
    case ProgramStatus::State::Idle:
        return "idle";
    case ProgramStatus::State::Working:
        return "working";
    case ProgramStatus::State::Blocked:
        return "blocked";
    case ProgramStatus::State::Done:
        return "done";
    case ProgramStatus::State::Error:
        return "error";
    case ProgramStatus::State::Clear:
        return "clear";
    }
    return "idle";
}

[[nodiscard]] std::string_view kind_name(ProgramStatus::Kind kind) {
    switch (kind) {
    case ProgramStatus::Kind::Permission:
        return "permission";
    case ProgramStatus::Kind::Question:
        return "question";
    case ProgramStatus::Kind::Auth:
        return "auth";
    }
    return "question";
}

/// `[A-Za-z0-9_.+-]{1,32}` (pi `APP_PATTERN`).
[[nodiscard]] bool valid_app_name(std::string_view app) {
    if (app.empty() || app.size() > 32) return false;
    for (const auto value : app) {
        const bool allowed = (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
                             (value >= '0' && value <= '9') || value == '_' || value == '.' || value == '+' ||
                             value == '-';
        if (!allowed) return false;
    }
    return true;
}

/// pi `CONTROL_CHARACTERS` `[\u0000-\u001f\u007f-\u009f]+`: every run becomes
/// one space so a terminal cannot discard the report.
[[nodiscard]] std::string sanitize_message(std::string_view message) {
    std::string sanitized;
    sanitized.reserve(message.size());
    bool replacing = false;
    for (const auto raw : message) {
        const auto value = static_cast<unsigned char>(raw);
        const bool control = value <= 0x1f || (value >= 0x7f && value <= 0x9f);
        if (control) {
            if (!replacing) sanitized.push_back(' ');
            replacing = true;
            continue;
        }
        replacing = false;
        sanitized.push_back(static_cast<char>(value));
    }
    return sanitized;
}

[[nodiscard]] bool ascii_whitespace(char value) {
    return value == ' ' || value == '\t' || value == '\n' || value == '\r' || value == '\v' || value == '\f';
}

[[nodiscard]] std::string trim_ascii(std::string value) {
    std::size_t begin = 0;
    while (begin < value.size() && ascii_whitespace(value[begin]))
        ++begin;
    std::size_t end = value.size();
    while (end > begin && ascii_whitespace(value[end - 1]))
        --end;
    return value.substr(begin, end - begin);
}

/// Cut at a UTF-8 code-point boundary within `max_bytes` (pi `truncateUtf8`).
[[nodiscard]] std::string truncate_utf8(std::string_view text, std::size_t max_bytes) {
    std::size_t bytes = 0;
    std::size_t offset = 0;
    while (offset < text.size()) {
        const auto lead = static_cast<unsigned char>(text[offset]);
        std::size_t length = 1;
        if ((lead & 0x80) == 0) {
            length = 1;
        } else if ((lead & 0xe0) == 0xc0) {
            length = 2;
        } else if ((lead & 0xf0) == 0xe0) {
            length = 3;
        } else if ((lead & 0xf8) == 0xf0) {
            length = 4;
        }
        if (offset + length > text.size()) length = 1;
        if (bytes + length > max_bytes) break;
        bytes += length;
        offset += length;
    }
    return std::string(text.substr(0, offset));
}

/// Standard base64 with padding (pi `Buffer.toString("base64")`).
[[nodiscard]] std::string base64_encode(std::string_view value) {
    constexpr std::string_view kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve((value.size() + 2) / 3 * 4);
    for (std::size_t offset = 0; offset < value.size(); offset += 3) {
        const auto remaining = value.size() - offset;
        const auto first = static_cast<std::uint32_t>(static_cast<unsigned char>(value[offset]));
        const auto second =
                remaining > 1 ? static_cast<std::uint32_t>(static_cast<unsigned char>(value[offset + 1])) : 0;
        const auto third =
                remaining > 2 ? static_cast<std::uint32_t>(static_cast<unsigned char>(value[offset + 2])) : 0;
        const auto grouped = (first << 16) | (second << 8) | third;
        result.push_back(kAlphabet[(grouped >> 18) & 0x3f]);
        result.push_back(kAlphabet[(grouped >> 12) & 0x3f]);
        result.push_back(remaining > 1 ? kAlphabet[(grouped >> 6) & 0x3f] : '=');
        result.push_back(remaining > 2 ? kAlphabet[grouped & 0x3f] : '=');
    }
    return result;
}

} // namespace

std::string format_program_status(const ProgramStatus& status) {
    std::string pairs = "state=";
    pairs += state_name(status.state);
    if (status.app && valid_app_name(*status.app)) {
        pairs += ":app=";
        pairs += *status.app;
    }
    if (status.state == ProgramStatus::State::Blocked && status.kind) {
        pairs += ":kind=";
        pairs += kind_name(*status.kind);
    }
    const auto message =
            truncate_utf8(trim_ascii(sanitize_message(status.message.value_or(std::string{}))), kMaxMessageBytes);
    if (!message.empty()) {
        pairs += ":msg=";
        pairs += base64_encode(message);
    }
    return "\x1b]7501;" + pairs + "\x1b\\";
}

bool is_program_status_reply(std::string_view sequence) {
    if (!sequence.starts_with(kReplyPrefix)) return false;
    const auto body = sequence.substr(kReplyPrefix.size());
    std::string_view pairs;
    if (body.ends_with("\x1b\\")) {
        pairs = body.substr(0, body.size() - 2);
    } else if (body.ends_with("\x07")) {
        pairs = body.substr(0, body.size() - 1);
    } else {
        return false;
    }
    for (const auto value : pairs) {
        if (value == '\x07' || value == '\x1b') return false;
    }
    return true;
}

} // namespace cch::tui
