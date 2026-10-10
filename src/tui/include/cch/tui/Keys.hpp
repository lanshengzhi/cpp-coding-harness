#pragma once

#include <cch/support/Error.hpp>

#include <cstddef>
#include <optional>
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
    /// Shortcut identity: the key name a binding matches (pi `Key.id`).
    std::string key{};
    /// The original printable text the terminal produced, kept apart from the
    /// shortcut identity (pi `Key.text`). A non-Latin Kitty layout keeps its
    /// own character here while `key` stays the base-layout key a shortcut
    /// matches; empty when the event carries no printable character.
    std::string text{};
    bool ctrl{false};
    bool shift{false};
    bool alt{false};
    bool super{false};
    KeyEventType type{KeyEventType::Press};

    bool operator==(const KeyEvent&) const = default;
};

inline constexpr std::size_t kMaxPasteBytes = 1024 * 1024;

struct PasteEvent {
    std::string text{};
    std::size_t original_bytes{0};
    std::size_t lines{1};
    bool truncated{false};

    bool operator==(const PasteEvent&) const = default;
};

using InputEventVariant = std::variant<KeyEvent, PasteEvent>;

/// Parse a pi baseline key identifier. Aliases normalize to their canonical key name.
[[nodiscard]] support::Expected<KeyEvent> parse_key_id(std::string_view identifier);
[[nodiscard]] std::string key_id(const KeyEvent& event);
[[nodiscard]] bool matches_key(const KeyEvent& event, std::string_view identifier);

/// Decode one complete key payload (pi `parseKey`). The keyboard protocol
/// state is a caller-held argument, never process-global state: each terminal
/// decodes with its own negotiated state and two callers never interfere.
/// Escape sequences must already be whole; the fragment framing that
/// reassembles split reads belongs to the input edge.
[[nodiscard]] std::optional<KeyEvent> parse_key(std::string_view data, bool kitty_protocol_active);

/// The text a key event inserts (pi `decodePrintableKey`). Nothing when the
/// event carries no printable character: named keys, ctrl/alt/super
/// combinations and key releases never insert. The terminal text wins;
/// the shortcut identity is used only for events decoded without one.
[[nodiscard]] std::optional<std::string> printable_text(const KeyEvent& event);

/// Whether the event carries key press behavior (press or repeat). Paste
/// events (no key payload) and key releases do not; a handler that does not
/// intentionally claim a release returns it unhandled (ADR 0050).
[[nodiscard]] inline bool carries_press_behavior(const KeyEvent* key) {
    return key != nullptr && key->type != KeyEventType::Release;
}

} // namespace cch::tui
