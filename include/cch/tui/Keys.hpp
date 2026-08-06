#pragma once

#include <cch/util/Error.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <variant>

namespace cch::tui {

enum class KeyEventType {
    Press,
    Repeat,
    Release,
};

struct KeyEvent {
    std::string key;
    bool ctrl{false};
    bool shift{false};
    bool alt{false};
    KeyEventType type{KeyEventType::Press};

    bool operator==(const KeyEvent&) const = default;
};

inline constexpr std::size_t kMaxPasteBytes = 1024 * 1024;

struct PasteEvent {
    std::string text;
    std::size_t original_bytes{0};
    std::size_t lines{1};
    bool truncated{false};

    bool operator==(const PasteEvent&) const = default;
};

using InputEventVariant = std::variant<KeyEvent, PasteEvent>;

/// Parse a pi baseline key identifier. Aliases normalize to their canonical key name.
[[nodiscard]] util::Expected<KeyEvent> parse_key_id(std::string_view identifier);
[[nodiscard]] std::string key_id(const KeyEvent& event);
[[nodiscard]] bool matches_key(const KeyEvent& event, std::string_view identifier);

} // namespace cch::tui
