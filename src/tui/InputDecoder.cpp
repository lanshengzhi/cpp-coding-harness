#include "InputDecoder.hpp"

#include <cch/tui/Keys.hpp>
#include "tui/InputInternal.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace cch::tui::detail {
namespace {

constexpr std::string_view kPasteStart = "\x1b[200~";
constexpr std::string_view kPasteEnd = "\x1b[201~";
constexpr std::size_t kMaxPendingBytes = 256;
constexpr unsigned int kShiftModifier = 1;
constexpr unsigned int kAltModifier = 2;
constexpr unsigned int kCtrlModifier = 4;
constexpr unsigned int kLockModifiers = 64 + 128;

struct ParsedModifiers {
    bool ctrl{false};
    bool shift{false};
    bool alt{false};
};

struct ParsedNumber {
    unsigned int value{0};
    bool valid{false};
};

ParsedNumber parse_number(std::string_view text) {
    if (text.empty()) return {};
    unsigned int value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return ParsedNumber{
        .value = value,
        .valid = error == std::errc{} && end == text.data() + text.size(),
    };
}

std::vector<std::string_view> split(std::string_view text, char separator) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (true) {
        const auto position = text.find(separator, start);
        parts.push_back(text.substr(start, position == std::string_view::npos
            ? text.size() - start
            : position - start));
        if (position == std::string_view::npos) return parts;
        start = position + 1;
    }
}

std::optional<ParsedModifiers> parse_modifiers(unsigned int protocol_value) {
    if (protocol_value == 0) return std::nullopt;
    const auto modifier = (protocol_value - 1) & ~kLockModifiers;
    if ((modifier & ~(kShiftModifier | kAltModifier | kCtrlModifier)) != 0) return std::nullopt;
    return ParsedModifiers{
        .ctrl = (modifier & kCtrlModifier) != 0,
        .shift = (modifier & kShiftModifier) != 0,
        .alt = (modifier & kAltModifier) != 0,
    };
}

KeyEventType parse_event_type(std::string_view text) {
    const auto parsed = parse_number(text);
    if (!parsed.valid || parsed.value == 1) return KeyEventType::Press;
    if (parsed.value == 2) return KeyEventType::Repeat;
    if (parsed.value == 3) return KeyEventType::Release;
    return KeyEventType::Press;
}

std::string encode_utf8(unsigned int codepoint) {
    std::string encoded;
    if (codepoint <= 0x7f) {
        encoded.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7ff) {
        encoded.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
        encoded.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    } else if (codepoint <= 0xffff && (codepoint < 0xd800 || codepoint > 0xdfff)) {
        encoded.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
        encoded.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
        encoded.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    } else if (codepoint <= 0x10ffff) {
        encoded.push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
        encoded.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
        encoded.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
        encoded.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    }
    return encoded;
}

std::optional<std::string> key_for_codepoint(unsigned int codepoint, bool shift) {
    constexpr std::array<std::pair<unsigned int, std::string_view>, 28> kKeypadKeys{{
        {57399, "0"}, {57400, "1"}, {57401, "2"}, {57402, "3"}, {57403, "4"},
        {57404, "5"}, {57405, "6"}, {57406, "7"}, {57407, "8"}, {57408, "9"},
        {57409, "."}, {57410, "/"}, {57411, "*"}, {57412, "-"}, {57413, "+"},
        {57414, "enter"}, {57415, "="}, {57416, ","}, {57417, "left"}, {57418, "right"},
        {57419, "up"}, {57420, "down"}, {57421, "pageUp"}, {57422, "pageDown"},
        {57423, "home"}, {57424, "end"}, {57425, "insert"}, {57426, "delete"},
    }};
    for (const auto& [candidate, key] : kKeypadKeys) {
        if (candidate == codepoint) return std::string(key);
    }

    if (codepoint == 27) return "escape";
    if (codepoint == 9) return "tab";
    if (codepoint == 13) return "enter";
    if (codepoint == 32) return "space";
    if (codepoint == 127) return "backspace";
    if (shift && codepoint >= 'A' && codepoint <= 'Z') codepoint += 'a' - 'A';
    if ((codepoint >= 'a' && codepoint <= 'z') || (codepoint >= '0' && codepoint <= '9') ||
        (codepoint <= 0x7f && is_baseline_symbol(static_cast<char>(codepoint)))) {
        return std::string(1, static_cast<char>(codepoint));
    }
    auto encoded = encode_utf8(codepoint);
    if (encoded.empty()) return std::nullopt;
    return encoded;
}

std::optional<KeyEvent> make_key_event(
    unsigned int codepoint,
    ParsedModifiers modifiers,
    KeyEventType type,
    std::optional<unsigned int> base_layout_key = std::nullopt) {
    auto key = key_for_codepoint(codepoint, modifiers.shift);
    const bool authoritative = key && key->size() == 1 &&
        (((*key)[0] >= 'a' && (*key)[0] <= 'z') || ((*key)[0] >= '0' && (*key)[0] <= '9') ||
         is_baseline_symbol((*key)[0]));
    if (!authoritative && base_layout_key) key = key_for_codepoint(*base_layout_key, modifiers.shift);
    if (!key) return std::nullopt;
    return KeyEvent{
        .key = std::move(*key),
        .ctrl = modifiers.ctrl,
        .shift = modifiers.shift,
        .alt = modifiers.alt,
        .type = type,
    };
}

std::optional<KeyEvent> parse_kitty_csi_u(std::string_view sequence) {
    if (!sequence.starts_with("\x1b[") || !sequence.ends_with('u')) return std::nullopt;
    const auto body = sequence.substr(2, sequence.size() - 3);
    const auto semicolon = body.find(';');
    const auto key_part = body.substr(0, semicolon);
    const auto modifier_part = semicolon == std::string_view::npos
        ? std::string_view{}
        : body.substr(semicolon + 1);
    if (semicolon != std::string_view::npos && modifier_part.find(';') != std::string_view::npos) {
        return std::nullopt;
    }

    const auto key_parts = split(key_part, ':');
    if (key_parts.empty() || key_parts.size() > 3) return std::nullopt;
    const auto codepoint = parse_number(key_parts[0]);
    if (!codepoint.valid) return std::nullopt;

    std::optional<unsigned int> base_layout_key;
    if (key_parts.size() == 3 && !key_parts[2].empty()) {
        const auto parsed_base = parse_number(key_parts[2]);
        if (!parsed_base.valid) return std::nullopt;
        base_layout_key = parsed_base.value;
    }

    unsigned int modifier_value = 1;
    KeyEventType type = KeyEventType::Press;
    if (!modifier_part.empty()) {
        const auto modifier_parts = split(modifier_part, ':');
        if (modifier_parts.empty() || modifier_parts.size() > 2) return std::nullopt;
        const auto parsed_modifier = parse_number(modifier_parts[0]);
        if (!parsed_modifier.valid) return std::nullopt;
        modifier_value = parsed_modifier.value;
        if (modifier_parts.size() == 2) type = parse_event_type(modifier_parts[1]);
    }
    const auto modifiers = parse_modifiers(modifier_value);
    if (!modifiers) return std::nullopt;
    return make_key_event(codepoint.value, *modifiers, type, base_layout_key);
}

std::optional<KeyEvent> parse_modify_other_keys(std::string_view sequence) {
    if (!sequence.starts_with("\x1b[27;") || !sequence.ends_with('~')) return std::nullopt;
    const auto parts = split(sequence.substr(5, sequence.size() - 6), ';');
    if (parts.size() != 2) return std::nullopt;
    const auto modifier_value = parse_number(parts[0]);
    const auto codepoint = parse_number(parts[1]);
    if (!modifier_value.valid || !codepoint.valid) return std::nullopt;
    const auto modifiers = parse_modifiers(modifier_value.value);
    if (!modifiers) return std::nullopt;
    return make_key_event(codepoint.value, *modifiers, KeyEventType::Press);
}

std::optional<KeyEvent> parse_kitty_navigation(std::string_view sequence) {
    if (!sequence.starts_with("\x1b[1;") || sequence.size() < 6) return std::nullopt;
    const auto final = sequence.back();
    if (std::string_view("ABCDHF").find(final) == std::string_view::npos) return std::nullopt;
    const auto parts = split(sequence.substr(4, sequence.size() - 5), ':');
    if (parts.empty() || parts.size() > 2) return std::nullopt;
    const auto modifier_value = parse_number(parts[0]);
    if (!modifier_value.valid) return std::nullopt;
    const auto modifiers = parse_modifiers(modifier_value.value);
    if (!modifiers) return std::nullopt;
    const auto type = parts.size() == 2 ? parse_event_type(parts[1]) : KeyEventType::Press;
    const auto key = final == 'A' ? "up" : final == 'B' ? "down" : final == 'C' ? "right" :
        final == 'D' ? "left" : final == 'H' ? "home" : "end";
    return KeyEvent{
        .key = key,
        .ctrl = modifiers->ctrl,
        .shift = modifiers->shift,
        .alt = modifiers->alt,
        .type = type,
    };
}

std::optional<KeyEvent> parse_kitty_functional(std::string_view sequence) {
    if (!sequence.starts_with("\x1b[") || !sequence.ends_with('~')) return std::nullopt;
    const auto parts = split(sequence.substr(2, sequence.size() - 3), ';');
    if (parts.empty() || parts.size() > 2) return std::nullopt;
    const auto key_number = parse_number(parts[0]);
    if (!key_number.valid) return std::nullopt;

    std::string key;
    if (key_number.value == 2) key = "insert";
    else if (key_number.value == 3) key = "delete";
    else if (key_number.value == 5) key = "pageUp";
    else if (key_number.value == 6) key = "pageDown";
    else if (key_number.value == 7) key = "home";
    else if (key_number.value == 8) key = "end";
    else return std::nullopt;

    unsigned int modifier_value = 1;
    KeyEventType type = KeyEventType::Press;
    if (parts.size() == 2) {
        const auto modifier_parts = split(parts[1], ':');
        if (modifier_parts.empty() || modifier_parts.size() > 2) return std::nullopt;
        const auto parsed_modifier = parse_number(modifier_parts[0]);
        if (!parsed_modifier.valid) return std::nullopt;
        modifier_value = parsed_modifier.value;
        if (modifier_parts.size() == 2) type = parse_event_type(modifier_parts[1]);
    }
    const auto modifiers = parse_modifiers(modifier_value);
    if (!modifiers) return std::nullopt;
    return KeyEvent{
        .key = std::move(key),
        .ctrl = modifiers->ctrl,
        .shift = modifiers->shift,
        .alt = modifiers->alt,
        .type = type,
    };
}

std::optional<KeyEvent> parse_legacy_sequence(std::string_view sequence) {
    constexpr std::array<std::pair<std::string_view, std::string_view>, 65> kSequences{{
        {"\x1b[A", "up"}, {"\x1bOA", "up"}, {"\x1b[B", "down"}, {"\x1bOB", "down"},
        {"\x1b[C", "right"}, {"\x1bOC", "right"}, {"\x1b[D", "left"}, {"\x1bOD", "left"},
        {"\x1b[H", "home"}, {"\x1bOH", "home"}, {"\x1b[1~", "home"}, {"\x1b[7~", "home"},
        {"\x1b[F", "end"}, {"\x1bOF", "end"}, {"\x1b[4~", "end"}, {"\x1b[8~", "end"},
        {"\x1b[2~", "insert"}, {"\x1b[3~", "delete"}, {"\x1b[5~", "pageUp"},
        {"\x1b[[5~", "pageUp"}, {"\x1b[6~", "pageDown"}, {"\x1b[[6~", "pageDown"},
        {"\x1b[E", "clear"}, {"\x1bOE", "clear"}, {"\x1bOP", "f1"}, {"\x1b[11~", "f1"},
        {"\x1b[[A", "f1"}, {"\x1bOQ", "f2"}, {"\x1b[12~", "f2"}, {"\x1b[[B", "f2"},
        {"\x1bOR", "f3"}, {"\x1b[13~", "f3"}, {"\x1b[[C", "f3"}, {"\x1bOS", "f4"},
        {"\x1b[14~", "f4"}, {"\x1b[[D", "f4"}, {"\x1b[15~", "f5"}, {"\x1b[[E", "f5"},
        {"\x1b[17~", "f6"}, {"\x1b[18~", "f7"}, {"\x1b[19~", "f8"}, {"\x1b[20~", "f9"},
        {"\x1b[21~", "f10"}, {"\x1b[23~", "f11"}, {"\x1b[24~", "f12"},
        {"\x1b[Z", "shift+tab"}, {"\x1b[a", "shift+up"}, {"\x1b[b", "shift+down"},
        {"\x1b[c", "shift+right"}, {"\x1b[d", "shift+left"}, {"\x1bOa", "ctrl+up"},
        {"\x1bOb", "ctrl+down"}, {"\x1bOc", "ctrl+right"}, {"\x1bOd", "ctrl+left"},
        {"\x1b[2$", "shift+insert"}, {"\x1b[3$", "shift+delete"}, {"\x1b[5$", "shift+pageUp"},
        {"\x1b[6$", "shift+pageDown"}, {"\x1b[7$", "shift+home"}, {"\x1b[8$", "shift+end"},
        {"\x1b[2^", "ctrl+insert"}, {"\x1b[3^", "ctrl+delete"}, {"\x1b[5^", "ctrl+pageUp"},
        {"\x1b[6^", "ctrl+pageDown"},
    }};
    for (const auto& [raw, identifier] : kSequences) {
        if (sequence == raw) {
            if (auto parsed = parse_key_id(identifier); parsed) return *parsed;
            return std::nullopt;
        }
    }
    if (sequence == "\x1b[7^") return *parse_key_id("ctrl+home");
    if (sequence == "\x1b[8^") return *parse_key_id("ctrl+end");
    if (sequence == "\x1b[e") return *parse_key_id("shift+clear");
    if (sequence == "\x1bOe") return *parse_key_id("ctrl+clear");
    if (sequence == "\x1b" "B" || sequence == "\x1b" "b") return *parse_key_id("alt+left");
    if (sequence == "\x1b" "F" || sequence == "\x1b" "f") return *parse_key_id("alt+right");
    if (sequence == "\x1bp") return *parse_key_id("alt+up");
    if (sequence == "\x1bn") return *parse_key_id("alt+down");
    if (sequence == "\x1bOM") return *parse_key_id("enter");
    return std::nullopt;
}

std::optional<KeyEvent> parse_raw_sequence(std::string_view sequence) {
    if (auto parsed = parse_modify_other_keys(sequence)) return parsed;
    if (auto parsed = parse_kitty_csi_u(sequence)) return parsed;
    if (auto parsed = parse_kitty_navigation(sequence)) return parsed;
    if (auto parsed = parse_kitty_functional(sequence)) return parsed;
    if (auto parsed = parse_legacy_sequence(sequence)) return parsed;

    if (sequence == "\x1b") return *parse_key_id("escape");
    if (sequence == "\t") return *parse_key_id("tab");
    if (sequence == "\r" || sequence == "\n") return *parse_key_id("enter");
    // The `\x00` literal in a C++ string is empty, so compare the byte
    // explicitly (pi keys.ts: `data === "\x00"` → ctrl+space).
    if (sequence.size() == 1 && sequence.front() == '\0') {
        return *parse_key_id("ctrl+space");
    }
    if (sequence == " ") return *parse_key_id("space");
    if (sequence == "\x7f" || sequence == "\x08") return *parse_key_id("backspace");
    if (sequence == "\x1b\x1b") return *parse_key_id("ctrl+alt+[");
    if (sequence == "\x1b\x1c") return *parse_key_id("ctrl+alt+\\");
    if (sequence == "\x1b\x1d") return *parse_key_id("ctrl+alt+]");
    if (sequence == "\x1b\x1f") return *parse_key_id("ctrl+alt+-");
    if (sequence == "\x1b\x7f" || sequence == "\x1b\x08") return *parse_key_id("alt+backspace");

    if (sequence.size() == 1) {
        const auto value = static_cast<unsigned char>(sequence.front());
        if (value >= 1 && value <= 26) {
            return KeyEvent{.key = std::string(1, static_cast<char>('a' + value - 1)), .ctrl = true};
        }
        if (value == 28) return *parse_key_id("ctrl+\\");
        if (value == 29) return *parse_key_id("ctrl+]");
        if (value == 31) return *parse_key_id("ctrl+-");
        if (value >= 'A' && value <= 'Z') {
            return KeyEvent{.key = std::string(1, static_cast<char>(value - 'A' + 'a')), .shift = true};
        }
        if (value >= 32 && value <= 126) return KeyEvent{.key = std::string(sequence)};
        if (value >= 128) return KeyEvent{.key = std::string(sequence)};
    }

    if (sequence.size() > 1 && sequence.front() != '\x1b') {
        const auto lead = static_cast<unsigned char>(sequence.front());
        const auto expected_length = (lead & 0xe0) == 0xc0 ? 2U :
            (lead & 0xf0) == 0xe0 ? 3U : (lead & 0xf8) == 0xf0 ? 4U : 0U;
        if (expected_length == sequence.size()) {
            bool valid = true;
            for (std::size_t index = 1; index < sequence.size(); ++index) {
                if ((static_cast<unsigned char>(sequence[index]) & 0xc0) != 0x80) valid = false;
            }
            if (valid) return KeyEvent{.key = std::string(sequence)};
        }
    }

    if (sequence.size() >= 2 && sequence.front() == '\x1b') {
        const auto key = parse_raw_sequence(sequence.substr(1));
        if (!key) return std::nullopt;
        auto modified = *key;
        modified.alt = true;
        return modified;
    }
    return std::nullopt;
}

std::size_t utf8_sequence_length(unsigned char lead) {
    if (lead < 0x80) return 1;
    if ((lead & 0xe0) == 0xc0) return 2;
    if ((lead & 0xf0) == 0xe0) return 3;
    if ((lead & 0xf8) == 0xf0) return 4;
    return 0;
}

bool valid_utf8_prefix(std::string_view input, std::size_t length) {
    if (length == 0 || input.size() < length) return false;
    for (std::size_t index = 1; index < length; ++index) {
        if ((static_cast<unsigned char>(input[index]) & 0xc0) != 0x80) return false;
    }
    return true;
}

bool starts_with_prefix(std::string_view value, std::string_view complete) {
    return value.size() <= complete.size() && complete.substr(0, value.size()) == value;
}

std::optional<std::size_t> escape_sequence_length(std::string_view pending, bool end_of_feed) {
    if (pending.empty() || pending.front() != '\x1b') return std::nullopt;
    if (pending.size() == 1) return std::nullopt;

    const auto kind = pending[1];
    if (kind == '[') {
        if (pending.starts_with("\x1b[[")) {
            if (pending.size() < 4) return std::nullopt;
            if (pending[3] >= 'A' && pending[3] <= 'E') return 4;
            const auto final = pending.find('~', 3);
            return final == std::string_view::npos ? std::nullopt : std::optional<std::size_t>{final + 1};
        }
        for (std::size_t index = 2; index < pending.size(); ++index) {
            const auto value = static_cast<unsigned char>(pending[index]);
            if (value >= 0x40 && value <= 0x7e) return index + 1;
        }
        // The legacy shift-modifier sequences (\x1b[2$ shift+insert,
        // \x1b[3$ shift+delete, \x1b[5$ shift+pageUp, \x1b[6$
        // shift+pageDown, \x1b[7$ shift+home, \x1b[8$ shift+end) end in
        // '$' — an ECMA-48 intermediate byte, not a final — so the scan
        // above never completes them. Accept exactly pi's six '$'-final
        // forms (keys.ts LEGACY_SEQUENCE_KEY_IDS); any other '$' stays an
        // intermediate byte of a longer CSI.
        if (pending.size() == 4 && pending[2] >= '2' && pending[2] <= '8' &&
            pending[2] != '4' && pending[3] == '$') {
            return 4;
        }
        return std::nullopt;
    }
    if (kind == 'O') return pending.size() >= 3 ? std::optional<std::size_t>{3} : std::nullopt;
    if (kind == ']') {
        const auto bell = pending.find('\x07', 2);
        const auto terminator = pending.find("\x1b\\", 2);
        if (bell != std::string_view::npos) return bell + 1;
        if (terminator != std::string_view::npos) return terminator + 2;
        return std::nullopt;
    }
    if (kind == 'P' || kind == '_') {
        const auto terminator = pending.find("\x1b\\", 2);
        return terminator == std::string_view::npos
            ? std::nullopt
            : std::optional<std::size_t>{terminator + 2};
    }
    if (kind == '\x1b' && pending.size() >= 3 && std::string_view("[]OP_").find(pending[2]) != std::string_view::npos) {
        return 1;
    }
    if (pending.size() >= 3 || end_of_feed) return 2;
    return std::nullopt;
}

// --- Out-of-band terminal response grammars ---
//
// The response parsers below are the deep-module consolidation of the
// pre-unification sidecar consumers: the CPR answer (ProcessTerminal's
// parse_cursor_position_response, ADR 0041), the cell-size answer
// (TerminalImage's consume_cell_size_response grammar), keyboard-protocol
// negotiation (KeyboardProtocol's parse_response; pi 83114817
// packages/tui/src/terminal.ts), and the appearance reports (pi 83114817
// packages/tui/src/terminal-colors.ts). Complete sequences only: split
// fragments reassemble in the decoder's single pending buffer before any of
// these run.

constexpr std::string_view kColorSchemeResponsePrefix{"\x1b[?997;"};
constexpr std::string_view kOscBackgroundResponsePrefix{"\x1b]11;"};
constexpr std::string_view kCellSizeResponsePrefix{"\x1b[6;"};

/// Parse a complete CPR response (`ESC [ rows ; cols R`, 1-based) into a
/// 0-based cursor position.
[[nodiscard]] std::optional<CursorPosition> parse_cursor_position_response(std::string_view response) {
    if (!response.starts_with("\x1b[") || !response.ends_with("R")) return std::nullopt;
    const auto body = response.substr(2, response.size() - 3);
    const auto separator = body.find(';');
    if (separator == std::string_view::npos) return std::nullopt;
    std::size_t rows = 0;
    std::size_t columns = 0;
    const auto [rows_end, rows_error] = std::from_chars(body.data(), body.data() + separator, rows);
    if (rows_error != std::errc{} || rows_end != body.data() + separator) return std::nullopt;
    const auto [columns_end, columns_error] = std::from_chars(
        body.data() + separator + 1,
        body.data() + body.size(),
        columns);
    if (columns_error != std::errc{} || columns_end != body.data() + body.size()) {
        return std::nullopt;
    }
    if (rows == 0 || columns == 0) return std::nullopt;
    return CursorPosition{.column = columns - 1, .row = rows - 1};
}

/// Parse a complete `ESC [ 6 ; h ; w t` cell-size answer (pi
/// terminal-image.ts consumeCellSizeResponse).
[[nodiscard]] std::optional<CellSizeResponse> parse_cell_size_response(std::string_view response) {
    if (!response.starts_with(kCellSizeResponsePrefix) || response.back() != 't') {
        return std::nullopt;
    }
    const auto body = response.substr(
        kCellSizeResponsePrefix.size(),
        response.size() - kCellSizeResponsePrefix.size() - 1);
    const auto separator = body.find(';');
    if (separator == std::string_view::npos) return std::nullopt;
    const auto height = body.substr(0, separator);
    const auto width = body.substr(separator + 1);

    std::size_t height_px = 0;
    std::size_t width_px = 0;
    const auto [height_end, height_error] =
        std::from_chars(height.data(), height.data() + height.size(), height_px);
    const auto [width_end, width_error] =
        std::from_chars(width.data(), width.data() + width.size(), width_px);
    if (height_error != std::errc{} || width_error != std::errc{} ||
        height_end != height.data() + height.size() ||
        width_end != width.data() + width.size()) {
        return std::nullopt;
    }
    return CellSizeResponse{
        .height_px = height_px,
        .width_px = width_px,
    };
}

/// Parse a complete keyboard-protocol negotiation answer: `CSI ? flags u`
/// (Kitty flags) or `CSI ? ... c` (device attributes, the modifyOtherKeys
/// fallback sentinel).
[[nodiscard]] std::optional<KeyboardProtocolResponse> parse_keyboard_protocol_response(
    std::string_view response) {
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

struct EnvironmentRgb {
    int red{0};
    int green{0};
    int blue{0};
};

[[nodiscard]] double linear_channel(int channel) {
    const auto value = static_cast<double>(channel) / 255.0;
    return value <= 0.03928 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
}

[[nodiscard]] TerminalAppearance appearance_for_rgb(const EnvironmentRgb& rgb) {
    const auto luminance = 0.2126 * linear_channel(rgb.red) +
        0.7152 * linear_channel(rgb.green) + 0.0722 * linear_channel(rgb.blue);
    return luminance >= 0.5 ? TerminalAppearance::Light : TerminalAppearance::Dark;
}

[[nodiscard]] std::optional<int> parse_osc_hex_channel(std::string_view channel) {
    if (channel.empty() || channel.size() > 8) return std::nullopt;
    std::uint64_t value = 0;
    std::uint64_t maximum = 0;
    for (const auto character : channel) {
        int digit = 0;
        if (character >= '0' && character <= '9') {
            digit = character - '0';
        } else if (character >= 'a' && character <= 'f') {
            digit = 10 + character - 'a';
        } else if (character >= 'A' && character <= 'F') {
            digit = 10 + character - 'A';
        } else {
            return std::nullopt;
        }
        value = value * 16 + static_cast<std::uint64_t>(digit);
        maximum = maximum * 16 + 15;
    }
    if (maximum == 0) return std::nullopt;
    return static_cast<int>(std::round(static_cast<double>(value) / static_cast<double>(maximum) * 255.0));
}

[[nodiscard]] std::optional<EnvironmentRgb> parse_osc_background(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return std::nullopt;
    const auto last = value.find_last_not_of(" \t\r\n");
    value = value.substr(first, last - first + 1);
    if (value.starts_with('#')) {
        value.remove_prefix(1);
        if (value.size() != 6 && value.size() != 12) return std::nullopt;
        const auto channel_width = value.size() / 3;
        const auto red = parse_osc_hex_channel(value.substr(0, channel_width));
        const auto green = parse_osc_hex_channel(value.substr(channel_width, channel_width));
        const auto blue = parse_osc_hex_channel(value.substr(channel_width * 2, channel_width));
        if (!red || !green || !blue) return std::nullopt;
        return EnvironmentRgb{.red = *red, .green = *green, .blue = *blue};
    }

    auto lower = std::string(value.substr(0, std::min<std::size_t>(5, value.size())));
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    if (lower.starts_with("rgba:")) {
        value.remove_prefix(5);
    } else if (lower.starts_with("rgb:")) {
        value.remove_prefix(4);
    } else {
        return std::nullopt;
    }
    const auto first_separator = value.find('/');
    if (first_separator == std::string_view::npos) return std::nullopt;
    const auto second_separator = value.find('/', first_separator + 1);
    if (second_separator == std::string_view::npos) return std::nullopt;
    const auto third_separator = value.find('/', second_separator + 1);
    const auto red = parse_osc_hex_channel(value.substr(0, first_separator));
    const auto green = parse_osc_hex_channel(value.substr(
        first_separator + 1,
        second_separator - first_separator - 1));
    const auto blue = parse_osc_hex_channel(value.substr(
        second_separator + 1,
        third_separator == std::string_view::npos
            ? std::string_view::npos
            : third_separator - second_separator - 1));
    if (!red || !green || !blue) return std::nullopt;
    return EnvironmentRgb{.red = *red, .green = *green, .blue = *blue};
}

/// How one complete escape sequence classifies in the demux: an out-of-band
/// terminal response, a response-shaped sequence to consume silently, or
/// ordinary in-band input to forward and key-decode.
struct ResponseScan {
    std::optional<TerminalResponseVariant> response{std::nullopt};
    /// Response-shaped sequences are never forwarded as input: the appearance
    /// grammars consume their bytes even when the body is malformed, exactly
    /// as the pre-unification appearance sidecar did.
    bool consumed{false};
};

[[nodiscard]] ResponseScan scan_terminal_response(std::string_view sequence) {
    // `CSI ? 997 ; 1|2 n` color-scheme report (pi terminal-colors.ts
    // parseTerminalColorSchemeReport): consumed even when the value is
    // unrecognized.
    if (sequence.starts_with(kColorSchemeResponsePrefix) && sequence.back() == 'n') {
        ResponseScan scan{.response = std::nullopt, .consumed = true};
        const auto value = sequence.substr(
            kColorSchemeResponsePrefix.size(),
            sequence.size() - kColorSchemeResponsePrefix.size() - 1);
        if (value == "1" || value == "2") {
            scan.response = ColorSchemeResponse{
                .kind = ColorSchemeResponseKind::ColorScheme,
                .appearance = value == "1" ? TerminalAppearance::Dark : TerminalAppearance::Light,
            };
        }
        return scan;
    }
    // OSC 11 background-color report (pi terminal-colors.ts
    // parseOsc11BackgroundColor): consumed even when the body is malformed.
    if (sequence.starts_with(kOscBackgroundResponsePrefix)) {
        ResponseScan scan{.response = std::nullopt, .consumed = true};
        auto body = sequence.substr(kOscBackgroundResponsePrefix.size());
        if (body.ends_with('\x07')) {
            body.remove_suffix(1);
        } else if (body.ends_with("\x1b\\")) {
            body.remove_suffix(2);
        }
        if (const auto rgb = parse_osc_background(body)) {
            scan.response = ColorSchemeResponse{
                .kind = ColorSchemeResponseKind::Background,
                .appearance = appearance_for_rgb(*rgb),
            };
        }
        return scan;
    }
    // Cell size, CPR, and keyboard negotiation emit only on a fully valid
    // parse; a malformed lookalike stays ordinary in-band input.
    if (auto cell_size = parse_cell_size_response(sequence)) {
        return ResponseScan{.response = std::move(*cell_size), .consumed = true};
    }
    if (auto position = parse_cursor_position_response(sequence)) {
        return ResponseScan{.response = *position, .consumed = true};
    }
    if (auto keyboard = parse_keyboard_protocol_response(sequence)) {
        return ResponseScan{.response = *keyboard, .consumed = true};
    }
    return {};
}

/// Whether a buffered fragment can only complete into an appearance
/// response: such fragments are dropped at the flush deadline (the
/// pre-unification appearance sidecar dropped them at its 150 ms deadline)
/// instead of leaking into the user key stream.
[[nodiscard]] bool is_appearance_response_fragment(std::string_view pending) {
    return pending.starts_with(kColorSchemeResponsePrefix) ||
        pending.starts_with(kOscBackgroundResponsePrefix);
}

} // namespace

StreamDecodeResult TerminalStreamDecoder::feed(std::string_view input) {
    StreamDecodeResult result;
    for (const auto byte : input) {
        if (paste_mode_) {
            consume_paste_byte(byte, result);
            continue;
        }
        if (discard_escape_byte(byte)) continue;
        pending_.push_back(byte);
        drain(result, false);
    }
    drain(result, true);
    return result;
}

bool TerminalStreamDecoder::discard_escape_byte(char byte) {
    if (discard_mode_ == EscapeDiscardMode::None) return false;

    if (discard_mode_ == EscapeDiscardMode::Csi) {
        if (byte == '\x1b') {
            discard_mode_ = EscapeDiscardMode::None;
            return false;
        }
        const auto value = static_cast<unsigned char>(byte);
        if (value >= 0x40 && value <= 0x7e) discard_mode_ = EscapeDiscardMode::None;
        return true;
    }

    if (discard_mode_ == EscapeDiscardMode::Osc && byte == '\x07') {
        discard_mode_ = EscapeDiscardMode::None;
        discard_saw_escape_ = false;
        return true;
    }
    if (discard_saw_escape_ && byte == '\\') {
        discard_mode_ = EscapeDiscardMode::None;
        discard_saw_escape_ = false;
        return true;
    }
    discard_saw_escape_ = byte == '\x1b';
    return true;
}

StreamDecodeResult TerminalStreamDecoder::flush() {
    StreamDecodeResult result;
    if (paste_mode_) {
        reset();
        return result;
    }
    if (!pending_.empty()) {
        if (!is_appearance_response_fragment(pending_)) {
            // The fragment window ended: event consumers get the best-effort
            // key decode (a lone ESC resolves to the escape key, pi's
            // fragment timeout) and byte-level consumers get the fragment
            // verbatim so no input byte is lost.
            if (pending_.front() == '\x1b') {
                if (auto key = parse_raw_sequence(pending_)) {
                    result.events.emplace_back(std::move(*key));
                }
            }
            result.forwarded_input = std::move(pending_);
        }
        pending_.clear();
    }
    discard_mode_ = EscapeDiscardMode::None;
    discard_saw_escape_ = false;
    return result;
}

void TerminalStreamDecoder::reset() {
    pending_.clear();
    discard_mode_ = EscapeDiscardMode::None;
    discard_saw_escape_ = false;
    paste_mode_ = false;
    paste_text_.clear();
    paste_end_candidate_.clear();
    paste_original_bytes_ = 0;
    paste_lines_ = 1;
}

void TerminalStreamDecoder::drain(StreamDecodeResult& result, bool end_of_feed) {
    while (!pending_.empty()) {
        if (pending_.size() <= kPasteStart.size() && starts_with_prefix(pending_, kPasteStart)) {
            if (pending_ == kPasteStart) {
                enter_paste(result);
            }
            return;
        }

        std::size_t length = 0;
        if (pending_.front() == '\x1b') {
            const auto sequence_length = escape_sequence_length(pending_, end_of_feed);
            if (!sequence_length) {
                if (pending_.size() >= kMaxPendingBytes) {
                    if (pending_.size() > 1 && pending_[1] == ']') {
                        discard_mode_ = EscapeDiscardMode::Osc;
                    } else if (pending_.size() > 1 && (pending_[1] == 'P' || pending_[1] == '_')) {
                        discard_mode_ = EscapeDiscardMode::StringTerminated;
                    } else {
                        discard_mode_ = EscapeDiscardMode::Csi;
                    }
                    pending_.clear();
                }
                return;
            }
            length = *sequence_length;
        } else {
            length = utf8_sequence_length(static_cast<unsigned char>(pending_.front()));
            if (length == 0) {
                // Invalid UTF-8 lead byte: no event, but the byte still
                // forwards verbatim for byte-level consumers.
                result.forwarded_input.push_back(pending_.front());
                pending_.erase(0, 1);
                continue;
            }
            if (pending_.size() < length) return;
            if (!valid_utf8_prefix(pending_, length)) {
                result.forwarded_input.push_back(pending_.front());
                pending_.erase(0, 1);
                continue;
            }
        }

        const auto sequence = pending_.substr(0, length);
        pending_.erase(0, length);
        if (sequence.front() == '\x1b') {
            auto scan = scan_terminal_response(sequence);
            if (scan.consumed) {
                if (scan.response) result.responses.push_back(std::move(*scan.response));
                continue;
            }
        }
        result.forwarded_input += sequence;
        if (auto key = parse_raw_sequence(sequence)) result.events.emplace_back(std::move(*key));
    }
}

void TerminalStreamDecoder::enter_paste(StreamDecodeResult& result) {
    // Byte-level consumers receive the complete bracketed-paste framing so
    // the downstream input decoder re-derives the same paste (pi's
    // StdinBuffer re-wraps paste content with its markers).
    result.forwarded_input += pending_;
    pending_.clear();
    paste_mode_ = true;
    paste_text_.clear();
    paste_end_candidate_.clear();
    paste_original_bytes_ = 0;
    paste_lines_ = 1;
}

void TerminalStreamDecoder::consume_paste_byte(char byte, StreamDecodeResult& result) {
    result.forwarded_input.push_back(byte);
    paste_end_candidate_.push_back(byte);
    while (!starts_with_prefix(paste_end_candidate_, kPasteEnd)) {
        commit_paste_byte(paste_end_candidate_.front());
        paste_end_candidate_.erase(0, 1);
    }
    if (paste_end_candidate_ != kPasteEnd) return;

    result.events.emplace_back(PasteEvent{
        .text = std::move(paste_text_),
        .original_bytes = paste_original_bytes_,
        .lines = paste_lines_,
        .truncated = paste_original_bytes_ > kMaxPasteBytes,
    });
    paste_mode_ = false;
    paste_text_.clear();
    paste_end_candidate_.clear();
    paste_original_bytes_ = 0;
    paste_lines_ = 1;
}

void TerminalStreamDecoder::commit_paste_byte(char byte) {
    if (paste_original_bytes_ != std::numeric_limits<std::size_t>::max()) ++paste_original_bytes_;
    if (byte == '\n' && paste_lines_ != std::numeric_limits<std::size_t>::max()) ++paste_lines_;
    if (paste_text_.size() < kMaxPasteBytes) paste_text_.push_back(byte);
}

} // namespace cch::tui::detail
