#include "KeyboardProtocol.hpp"

#include <charconv>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace cch::tui::detail {
namespace {

[[nodiscard]] bool is_response_prefix(std::string_view input) {
    if (input == "\x1b" || input == "\x1b[" || input == "\x1b[?") return true;
    if (!input.starts_with("\x1b[?")) return false;
    for (const auto& byte : input.substr(3)) {
        if ((byte < '0' || byte > '9') && byte != ';') return false;
    }
    return true;
}

[[nodiscard]] std::optional<KeyboardProtocolResponse> parse_response(std::string_view response) {
    if (!response.starts_with("\x1b[?") || response.size() < 5) return std::nullopt;
    const auto final = response.back();
    const auto body = response.substr(3, response.size() - 4);
    if (final == 'u') {
        unsigned int flags = 0;
        const auto [end, error] = std::from_chars(body.data(), body.data() + body.size(), flags);
        if (error != std::errc{} || end != body.data() + body.size()) return std::nullopt;
        return KeyboardProtocolResponse{
            .kind = KeyboardProtocolResponseKind::KittyFlags,
            .flags = flags,
        };
    }
    if (final != 'c') return std::nullopt;
    for (const auto& byte : body) {
        if ((byte < '0' || byte > '9') && byte != ';') return std::nullopt;
    }
    return KeyboardProtocolResponse{
        .kind = KeyboardProtocolResponseKind::DeviceAttributes,
    };
}

} // namespace

KeyboardProtocolInputResult parse_keyboard_protocol_input(
    std::string pending,
    std::string_view input) {
    KeyboardProtocolInputResult result{
        .pending = std::move(pending),
        .forwarded_input = {},
        .responses = {},
    };
    result.pending += input;
    while (!result.pending.empty()) {
        if (!result.pending.starts_with("\x1b")) {
            const auto escape = result.pending.find('\x1b');
            const auto count = escape == std::string::npos ? result.pending.size() : escape;
            result.forwarded_input += result.pending.substr(0, count);
            result.pending.erase(0, count);
            continue;
        }
        if (!result.pending.starts_with("\x1b[?")) {
            if (is_response_prefix(result.pending)) return result;
            result.forwarded_input += result.pending;
            result.pending.clear();
            return result;
        }
        const auto final = result.pending.find_first_of("uc", 3);
        if (final == std::string::npos) {
            if (is_response_prefix(result.pending)) return result;
            result.forwarded_input += result.pending;
            result.pending.clear();
            return result;
        }
        const auto response_text = result.pending.substr(0, final + 1);
        auto response = parse_response(response_text);
        if (!response) {
            result.forwarded_input += result.pending;
            result.pending.clear();
            return result;
        }
        result.responses.push_back(*response);
        result.pending.erase(0, final + 1);
    }
    return result;
}

} // namespace cch::tui::detail
