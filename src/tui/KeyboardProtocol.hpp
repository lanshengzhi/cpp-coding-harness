#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace cch::tui::detail {

enum class KeyboardProtocolResponseKind {
    KittyFlags,
    DeviceAttributes,
};

struct KeyboardProtocolResponse {
    KeyboardProtocolResponseKind kind{KeyboardProtocolResponseKind::DeviceAttributes};
    unsigned int flags{0};
};

struct KeyboardProtocolInputResult {
    std::string pending;
    std::string forwarded_input;
    std::vector<KeyboardProtocolResponse> responses;
};

[[nodiscard]] KeyboardProtocolInputResult parse_keyboard_protocol_input(
    std::string pending,
    std::string_view input);

} // namespace cch::tui::detail
