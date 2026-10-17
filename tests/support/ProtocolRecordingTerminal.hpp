#pragma once

#include <cch/tui/Terminal.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cch::tests {

/// Decorator terminal observing public call and escape boundaries for protocol assertions.
class ProtocolRecordingTerminal final : public cch::tui::Terminal {
public:
    enum class OpKind {
        Write,
        SetCursor,
        SetCursorVisible,
        BeginSync,
        EndSync,
    };

    struct ObservedOp {
        OpKind kind;
        std::string write_bytes{};
        cch::tui::CursorPosition cursor_position{};
        bool visible{false};
    };

    explicit ProtocolRecordingTerminal(cch::tui::VirtualTerminal& inner) : inner_(inner) {}

    [[nodiscard]] cch::support::ExpectedVoid start(
            cch::tui::TerminalInputSink input_sink, cch::tui::TerminalResizeSink resize_sink) override {
        return inner_.start(std::move(input_sink), std::move(resize_sink));
    }

    [[nodiscard]] cch::support::ExpectedVoid stop() override { return inner_.stop(); }
    [[nodiscard]] cch::tui::TerminalDimensions dimensions() const override { return inner_.dimensions(); }
    [[nodiscard]] cch::tui::TerminalCapabilities capabilities() const override { return inner_.capabilities(); }
    [[nodiscard]] cch::tui::TerminalModeState modes() const override { return inner_.modes(); }
    [[nodiscard]] cch::support::ExpectedVoid clear_screen() override { return inner_.clear_screen(); }

    [[nodiscard]] cch::support::ExpectedVoid write(std::string_view output) override {
        ops_.push_back(ObservedOp{
                .kind = OpKind::Write,
                .write_bytes = std::string(output),
        });
        return inner_.write(output);
    }

    [[nodiscard]] cch::support::ExpectedVoid set_cursor(cch::tui::CursorPosition position) override {
        ops_.push_back(ObservedOp{
                .kind = OpKind::SetCursor,
                .cursor_position = position,
        });
        return inner_.set_cursor(position);
    }

    [[nodiscard]] cch::support::ExpectedVoid set_cursor_visible(bool visible) override {
        ops_.push_back(ObservedOp{
                .kind = OpKind::SetCursorVisible,
                .visible = visible,
        });
        return inner_.set_cursor_visible(visible);
    }

    [[nodiscard]] cch::support::ExpectedVoid set_alternate_screen(bool enabled) override {
        return inner_.set_alternate_screen(enabled);
    }

    [[nodiscard]] cch::support::ExpectedVoid set_autowrap(bool enabled) override {
        return inner_.set_autowrap(enabled);
    }

    [[nodiscard]] cch::support::ExpectedVoid set_scroll_margins(std::size_t top_row, std::size_t bottom_row) override {
        return inner_.set_scroll_margins(top_row, bottom_row);
    }

    [[nodiscard]] cch::support::ExpectedVoid reset_scroll_margins() override { return inner_.reset_scroll_margins(); }

    [[nodiscard]] cch::support::ExpectedVoid begin_synchronized_update() override {
        ops_.push_back(ObservedOp{.kind = OpKind::BeginSync});
        return inner_.begin_synchronized_update();
    }

    [[nodiscard]] cch::support::ExpectedVoid end_synchronized_update() override {
        ops_.push_back(ObservedOp{.kind = OpKind::EndSync});
        return inner_.end_synchronized_update();
    }

    [[nodiscard]] cch::support::ExpectedVoid set_title(std::string_view title) override {
        return inner_.set_title(title);
    }

    [[nodiscard]] cch::support::ExpectedVoid apply_cell_pixel_dimensions(
            cch::tui::CellPixelDimensions pixels) override {
        return inner_.apply_cell_pixel_dimensions(pixels);
    }

    [[nodiscard]] cch::support::ExpectedVoid set_progress(bool active) override { return inner_.set_progress(active); }

    [[nodiscard]] cch::support::ExpectedVoid drain_input(
            std::chrono::milliseconds max_ms, std::chrono::milliseconds idle_ms) override {
        return inner_.drain_input(max_ms, idle_ms);
    }

    [[nodiscard]] cch::support::ExpectedVoid enter_output_only() override { return inner_.enter_output_only(); }
    [[nodiscard]] cch::support::ExpectedVoid leave_output_only() override { return inner_.leave_output_only(); }

    [[nodiscard]] cch::support::ExpectedVoid inject_input(std::string input) {
        return inner_.inject_input(std::move(input));
    }
    [[nodiscard]] cch::support::ExpectedVoid flush_input() { return inner_.flush_input(); }
    [[nodiscard]] cch::support::ExpectedVoid inject_resize(cch::tui::TerminalDimensions dimensions) {
        return inner_.inject_resize(dimensions);
    }

    void reset_recording() { ops_.clear(); }
    [[nodiscard]] const std::vector<ObservedOp>& ops() const { return ops_; }
    [[nodiscard]] cch::tui::VirtualTerminal& inner() noexcept { return inner_; }
    [[nodiscard]] const cch::tui::VirtualTerminal& inner() const noexcept { return inner_; }

private:
    cch::tui::VirtualTerminal& inner_;
    std::vector<ObservedOp> ops_;
};

[[nodiscard]] inline bool is_cursor_movement_sequence(std::string_view s) {
    if (s.empty() || !s.starts_with("\x1b[")) return false;
    // Check for MainScreen relative movement e.g. \x1b[1A\x1b[6G or \x1b[6G
    if (s.ends_with('G')) {
        for (std::size_t i = 0; i < s.size();) {
            if (s[i] == '\x1b') {
                if (i + 1 >= s.size() || s[i + 1] != '[') return false;
                i += 2;
                const auto num_start = i;
                while (i < s.size() && s[i] >= '0' && s[i] <= '9')
                    ++i;
                if (i == num_start || i >= s.size()) return false;
                const char cmd = s[i++];
                if (cmd != 'A' && cmd != 'B' && cmd != 'G') return false;
            } else {
                return false;
            }
        }
        return true;
    }
    // Check for AltScreen absolute movement e.g. \x1b[2;6H or \x1b[3;8H
    if (s.ends_with('H')) {
        for (std::size_t i = 0; i < s.size();) {
            if (s[i] == '\x1b') {
                if (i + 1 >= s.size() || s[i + 1] != '[') return false;
                i += 2;
                while (i < s.size() && (s[i] >= '0' && s[i] <= '9'))
                    ++i;
                if (i < s.size() && s[i] == ';') {
                    ++i;
                    while (i < s.size() && (s[i] >= '0' && s[i] <= '9'))
                        ++i;
                }
                if (i >= s.size() || s[i] != 'H') return false;
                ++i;
            } else {
                return false;
            }
        }
        return true;
    }
    return false;
}

enum class ImeTokenKind {
    AbsoluteMove,
    RelativeMove,
    Visibility,
    Unsupported,
};

struct ImeToken {
    ImeTokenKind kind;
    std::size_t row{0};      // 1-indexed for AbsoluteMove
    std::size_t col{0};      // 1-indexed for AbsoluteMove or RelativeMove
    int row_delta{0};        // negative for up (A), positive for down (B)
    bool visible{false};     // for Visibility
    std::string canonical{}; // canonical string representation

    bool operator==(const ImeToken& other) const = default;
};

[[nodiscard]] constexpr bool is_cursor_final_byte(char c) noexcept { return (c >= 'A' && c <= 'H') || c == 'f'; }

[[nodiscard]] inline bool has_cursor_control(std::string_view s) {
    if (s.empty()) return false;
    for (std::size_t i = 0; i < s.size();) {
        if (s.substr(i).starts_with("\x1b[")) {
            std::size_t p = i + 2;
            const bool has_question_25 = s.substr(p).starts_with("?25");
            while (p < s.size() && s[p] >= 0x20 && s[p] <= 0x3F) {
                ++p;
            }
            if (p < s.size()) {
                const char final_byte = s[p];
                if (is_cursor_final_byte(final_byte) || has_question_25) {
                    return true;
                }
                i = p + 1;
            } else {
                if (has_question_25) {
                    return true;
                }
                break;
            }
        } else {
            ++i;
        }
    }
    return false;
}

[[nodiscard]] inline std::optional<std::size_t> parse_uint(std::string_view s) {
    if (s.empty()) return std::nullopt;
    std::size_t value = 0;
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
    if (ec == std::errc{} && ptr == s.data() + s.size()) {
        return value;
    }
    return std::nullopt;
}

[[nodiscard]] inline std::optional<int> parse_int(std::string_view s) {
    if (s.empty()) return std::nullopt;
    int value = 0;
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
    if (ec == std::errc{} && ptr == s.data() + s.size()) {
        return value;
    }
    return std::nullopt;
}

struct RelativeMoveParams {
    int row_delta{0};
    std::size_t col{0};
};

[[nodiscard]] inline std::optional<RelativeMoveParams> parse_relative_move(std::string_view s) {
    if (s.empty() || !s.starts_with("\x1b[")) return std::nullopt;
    int row_delta = 0;
    std::size_t col = 0;
    std::size_t i = 0;
    if (s.starts_with("\x1b[")) {
        std::size_t p = 2;
        const std::size_t num_start = p;
        while (p < s.size() && s[p] >= '0' && s[p] <= '9')
            ++p;
        if (p < s.size() && (s[p] == 'A' || s[p] == 'B')) {
            int val = 1;
            if (p > num_start) {
                const auto parsed = parse_int(s.substr(num_start, p - num_start));
                if (!parsed) return std::nullopt;
                val = *parsed;
            }
            row_delta = (s[p] == 'A') ? -val : val;
            i = p + 1;
        }
    }
    if (i < s.size() && s.substr(i).starts_with("\x1b[")) {
        std::size_t p = i + 2;
        const std::size_t num_start = p;
        while (p < s.size() && s[p] >= '0' && s[p] <= '9')
            ++p;
        if (p < s.size() && s[p] == 'G') {
            std::size_t val = 1;
            if (p > num_start) {
                const auto parsed = parse_uint(s.substr(num_start, p - num_start));
                if (!parsed) return std::nullopt;
                val = *parsed;
            }
            if (p + 1 == s.size()) {
                col = val;
                return RelativeMoveParams{.row_delta = row_delta, .col = col};
            }
        }
    } else if (i == s.size() && row_delta != 0) {
        return RelativeMoveParams{.row_delta = row_delta, .col = col};
    }
    return std::nullopt;
}

struct AbsoluteMoveParams {
    std::size_t row{0};
    std::size_t col{0};
};

[[nodiscard]] inline std::optional<AbsoluteMoveParams> parse_absolute_move(std::string_view s) {
    if (s.empty() || !s.starts_with("\x1b[")) return std::nullopt;
    std::size_t p = 2;
    const std::size_t num1_start = p;
    while (p < s.size() && s[p] >= '0' && s[p] <= '9')
        ++p;
    if (p == num1_start || p >= s.size() || s[p] != ';') return std::nullopt;
    const auto row_opt = parse_uint(s.substr(num1_start, p - num1_start));
    if (!row_opt) return std::nullopt;
    ++p;
    const std::size_t num2_start = p;
    while (p < s.size() && s[p] >= '0' && s[p] <= '9')
        ++p;
    if (p == num2_start || p >= s.size() || s[p] != 'H') return std::nullopt;
    const auto col_opt = parse_uint(s.substr(num2_start, p - num2_start));
    if (!col_opt) return std::nullopt;
    if (p + 1 == s.size()) {
        return AbsoluteMoveParams{.row = *row_opt, .col = *col_opt};
    }
    return std::nullopt;
}

struct FrozenTerminalEvent {
    std::string op;
    std::string bytes{};
};

/// Decodes the final frame IME placement and visibility from frozen raw writes and terminal events.
/// Retains exact ordered sequence within explicit target final frame boundary.
[[nodiscard]] inline std::vector<ImeToken> extract_ime_tokens_from_frozen_evidence(
        std::span<const std::string> writes, std::span<const FrozenTerminalEvent> events, bool is_fullscreen) {
    std::vector<ImeToken> result;
    if (is_fullscreen) {
        // Fullscreen: AltScreen frame write ends in \x1b[?2026l with bundled cursor positioning and visibility.
        // We find the final frame write (last write containing \x1b[?2026l or \x1b[?25).
        std::string_view target_write;
        for (auto it = writes.rbegin(); it != writes.rend(); ++it) {
            if (it->find("\x1b[?2026l") != std::string::npos || it->find("\x1b[?25") != std::string::npos) {
                target_write = *it;
                break;
            }
        }
        if (target_write.empty() && !writes.empty()) {
            target_write = writes.back();
        }

        // In AltScreen, each line paint has \x1b[<row>;1H\x1b[2K...
        // Any CSI command after the last \x1b[2K line paint is an IME sequence.
        // If there are no paint lines (\x1b[2K), then any CSI in the write (excluding \x1b[?2026h/l) is IME.
        std::size_t ime_start = 0;
        const auto last_line_clear = target_write.rfind("\x1b[2K");
        if (last_line_clear != std::string_view::npos) {
            const auto pos_h = target_write.find("\x1b[", last_line_clear + 4);
            if (pos_h != std::string_view::npos) {
                ime_start = pos_h;
            } else {
                ime_start = target_write.size();
            }
        }

        // Parse all CSI sequences from ime_start to end, in original order
        for (std::size_t i = ime_start; i < target_write.size();) {
            if (target_write.substr(i).starts_with("\x1b[")) {
                std::size_t end = i + 2;
                const bool is_q25 = target_write.substr(end).starts_with("?25");
                while (end < target_write.size() && target_write[end] >= 0x20 && target_write[end] <= 0x3F) {
                    ++end;
                }
                if (end < target_write.size()) {
                    const char cmd = target_write[end];
                    const std::string_view csi_seq = target_write.substr(i, end + 1 - i);
                    i = end + 1;

                    if (csi_seq == "\x1b[?2026h" || csi_seq == "\x1b[?2026l") {
                        continue;
                    }
                    if (csi_seq == "\x1b[?25h") {
                        result.push_back(ImeToken{
                                .kind = ImeTokenKind::Visibility,
                                .visible = true,
                                .canonical = "\x1b[?25h",
                        });
                        continue;
                    }
                    if (csi_seq == "\x1b[?25l") {
                        result.push_back(ImeToken{
                                .kind = ImeTokenKind::Visibility,
                                .visible = false,
                                .canonical = "\x1b[?25l",
                        });
                        continue;
                    }
                    if (is_q25) {
                        result.push_back(ImeToken{
                                .kind = ImeTokenKind::Unsupported,
                                .canonical = std::string(csi_seq),
                        });
                        continue;
                    }
                    if (cmd == 'H' || cmd == 'f') {
                        if (const auto parsed = parse_absolute_move(csi_seq)) {
                            result.push_back(ImeToken{
                                    .kind = ImeTokenKind::AbsoluteMove,
                                    .row = parsed->row,
                                    .col = parsed->col,
                                    .canonical = std::string(csi_seq),
                            });
                        } else {
                            result.push_back(ImeToken{
                                    .kind = ImeTokenKind::Unsupported,
                                    .canonical = std::string(csi_seq),
                            });
                        }
                        continue;
                    }
                    if (is_cursor_final_byte(cmd)) {
                        result.push_back(ImeToken{
                                .kind = ImeTokenKind::Unsupported,
                                .canonical = std::string(csi_seq),
                        });
                        continue;
                    }
                    // Noncursor formatting (e.g. SGR 'm', erase 'K') is ignored
                } else {
                    const auto csi_seq = target_write.substr(i);
                    i = target_write.size();
                    if (is_q25 || has_cursor_control(csi_seq)) {
                        result.push_back(ImeToken{
                                .kind = ImeTokenKind::Unsupported,
                                .canonical = std::string(csi_seq),
                        });
                    }
                }
            } else {
                ++i;
            }
        }
    } else {
        // Regular: MainScreen emits standalone relative movement write followed by visibility update in events.
        // Find the target frame paint boundary: the last event that wrote the composed buffer.
        std::size_t boundary_idx = 0;
        for (std::size_t i = 0; i < events.size(); ++i) {
            const auto& ev = events[i];
            if (ev.op == "write" &&
                    (ev.bytes.find("\r\n") != std::string::npos || ev.bytes.find("\x1b[2K") != std::string::npos ||
                            (ev.bytes.starts_with("\x1b[?2026h") && ev.bytes.find("header") != std::string::npos))) {
                boundary_idx = i + 1;
            }
        }

        for (std::size_t i = boundary_idx; i < events.size(); ++i) {
            const auto& ev = events[i];
            if (ev.op == "write") {
                if (const auto parsed = parse_relative_move(ev.bytes)) {
                    result.push_back(ImeToken{
                            .kind = ImeTokenKind::RelativeMove,
                            .col = parsed->col,
                            .row_delta = parsed->row_delta,
                            .canonical = ev.bytes,
                    });
                } else if (has_cursor_control(ev.bytes)) {
                    result.push_back(ImeToken{
                            .kind = ImeTokenKind::Unsupported,
                            .canonical = ev.bytes,
                    });
                }
            } else if (ev.op == "showCursor") {
                if (result.empty() || result.back().kind != ImeTokenKind::Visibility || !result.back().visible) {
                    result.push_back(ImeToken{
                            .kind = ImeTokenKind::Visibility,
                            .visible = true,
                            .canonical = "\x1b[?25h",
                    });
                }
            } else if (ev.op == "hideCursor") {
                if (result.empty() || result.back().kind != ImeTokenKind::Visibility || result.back().visible) {
                    result.push_back(ImeToken{
                            .kind = ImeTokenKind::Visibility,
                            .visible = false,
                            .canonical = "\x1b[?25l",
                    });
                }
            } else if (ev.op == "setCursor") {
                result.push_back(ImeToken{
                        .kind = ImeTokenKind::Unsupported,
                        .canonical = ev.bytes.empty() ? "setCursor" : ev.bytes,
                });
            }
        }
    }
    return result;
}

/// Projects final IME placement and visibility from Pike's observed ops.
/// Retains exact ordered sequence within explicit target final frame boundary.
[[nodiscard]] inline std::vector<ImeToken> project_final_ime_sequence(
        std::span<const ProtocolRecordingTerminal::ObservedOp> ops,
        bool is_fullscreen,
        bool current_terminal_visible = false) {
    std::vector<ImeToken> result;
    if (is_fullscreen) {
        // AltScreen: The target final frame is the last [BeginSync, EndSync] enclosure.
        std::size_t start_idx = 0;
        std::size_t end_idx = ops.size();

        for (std::size_t i = 0; i < ops.size(); ++i) {
            if (ops[i].kind == ProtocolRecordingTerminal::OpKind::BeginSync) {
                start_idx = i;
            }
        }
        for (std::size_t i = start_idx; i < ops.size(); ++i) {
            if (ops[i].kind == ProtocolRecordingTerminal::OpKind::EndSync) {
                end_idx = i;
                break;
            }
        }

        // Paint boundary: paint writes in AltScreen contain \x1b[2K.
        // Post-boundary ops are collected after the last paint write.
        std::size_t paint_boundary = start_idx;
        for (std::size_t i = start_idx; i < end_idx; ++i) {
            if (ops[i].kind == ProtocolRecordingTerminal::OpKind::Write &&
                    ops[i].write_bytes.find("\x1b[2K") != std::string::npos) {
                paint_boundary = i + 1;
            }
        }

        bool has_visibility = false;
        // Collect all post-boundary cursor ops in original order
        for (std::size_t i = paint_boundary; i < end_idx; ++i) {
            const auto& op = ops[i];
            if (op.kind == ProtocolRecordingTerminal::OpKind::SetCursor) {
                result.push_back(ImeToken{
                        .kind = ImeTokenKind::AbsoluteMove,
                        .row = op.cursor_position.row + 1,
                        .col = op.cursor_position.column + 1,
                        .canonical =
                                std::format("\x1b[{};{}H", op.cursor_position.row + 1, op.cursor_position.column + 1),
                });
            } else if (op.kind == ProtocolRecordingTerminal::OpKind::SetCursorVisible) {
                has_visibility = true;
                if (result.empty() || result.back().kind != ImeTokenKind::Visibility ||
                        result.back().visible != op.visible) {
                    result.push_back(ImeToken{
                            .kind = ImeTokenKind::Visibility,
                            .visible = op.visible,
                            .canonical = op.visible ? "\x1b[?25h" : "\x1b[?25l",
                    });
                }
            } else if (op.kind == ProtocolRecordingTerminal::OpKind::Write) {
                if (has_cursor_control(op.write_bytes)) {
                    result.push_back(ImeToken{
                            .kind = ImeTokenKind::Unsupported,
                            .canonical = op.write_bytes,
                    });
                }
            }
        }

        // If AltScreen suppressed redundant cached idempotent visibility update,
        // project the effective terminal visibility state (permitted previous exception).
        if (!has_visibility) {
            result.push_back(ImeToken{
                    .kind = ImeTokenKind::Visibility,
                    .visible = current_terminal_visible,
                    .canonical = current_terminal_visible ? "\x1b[?25h" : "\x1b[?25l",
            });
        }
    } else {
        // MainScreen: Target IME writes follow the last EndSync of paint.
        std::size_t boundary_idx = 0;
        for (std::size_t i = 0; i < ops.size(); ++i) {
            if (ops[i].kind == ProtocolRecordingTerminal::OpKind::EndSync) {
                boundary_idx = i + 1;
            }
        }

        for (std::size_t i = boundary_idx; i < ops.size(); ++i) {
            const auto& op = ops[i];
            if (op.kind == ProtocolRecordingTerminal::OpKind::Write) {
                if (const auto parsed = parse_relative_move(op.write_bytes)) {
                    result.push_back(ImeToken{
                            .kind = ImeTokenKind::RelativeMove,
                            .col = parsed->col,
                            .row_delta = parsed->row_delta,
                            .canonical = op.write_bytes,
                    });
                } else if (has_cursor_control(op.write_bytes)) {
                    result.push_back(ImeToken{
                            .kind = ImeTokenKind::Unsupported,
                            .canonical = op.write_bytes,
                    });
                }
            } else if (op.kind == ProtocolRecordingTerminal::OpKind::SetCursor) {
                result.push_back(ImeToken{
                        .kind = ImeTokenKind::Unsupported,
                        .row = op.cursor_position.row + 1,
                        .col = op.cursor_position.column + 1,
                        .canonical =
                                std::format("\x1b[{};{}H", op.cursor_position.row + 1, op.cursor_position.column + 1),
                });
            } else if (op.kind == ProtocolRecordingTerminal::OpKind::SetCursorVisible) {
                if (result.empty() || result.back().kind != ImeTokenKind::Visibility ||
                        result.back().visible != op.visible) {
                    result.push_back(ImeToken{
                            .kind = ImeTokenKind::Visibility,
                            .visible = op.visible,
                            .canonical = op.visible ? "\x1b[?25h" : "\x1b[?25l",
                    });
                }
            }
        }
    }
    return result;
}

[[nodiscard]] inline std::vector<ImeToken> project_final_ime_sequence(
        const ProtocolRecordingTerminal& terminal, bool is_fullscreen) {
    return project_final_ime_sequence(terminal.ops(), is_fullscreen, terminal.inner().modes().cursor_visible);
}

/// Scoped parser for OSC 9;4 terminal progress escape sequences.
/// Retains exact bytes, order, and duplicates.
/// Supported sequence forms:
/// - \x1b]9;4;<state>\x07 (BEL terminated)
/// - \x1b]9;4;<state>\x1b\\ (ST terminated)
/// Excludes unrelated paint, style, cursor, and query controls (CSI, OSC 0 title, etc.).
/// Does not silently drop malformed OSC 9;4 sequences (e.g., \x1b]9;4;0;\x07 or unterminated).
/// Handles fragmented chunk boundaries.
class Osc94Extractor {
public:
    void feed(std::string_view chunk) {
        buffer_.append(chunk);
        parse_buffer(/*finishing=*/false);
    }

    void feed(std::span<const std::string> chunks) {
        for (const auto& chunk : chunks) {
            feed(chunk);
        }
    }

    void finish() { parse_buffer(/*finishing=*/true); }

    [[nodiscard]] const std::vector<std::string>& commands() const noexcept { return commands_; }
    [[nodiscard]] std::vector<std::string> take_commands() noexcept { return std::move(commands_); }
    void clear() noexcept {
        buffer_.clear();
        commands_.clear();
    }

private:
    void parse_buffer(bool finishing) {
        std::size_t i = 0;
        while (i < buffer_.size()) {
            if (buffer_[i] != '\x1b') {
                ++i;
                continue;
            }
            if (i + 1 >= buffer_.size()) {
                if (!finishing) break;
                ++i;
                continue;
            }
            const char c1 = buffer_[i + 1];
            if (c1 == ']') {
                if (i + 2 >= buffer_.size()) {
                    if (!finishing) break;
                    i += 2;
                    continue;
                }
                const auto osc_payload = std::string_view{buffer_}.substr(i + 2);
                if (osc_payload.starts_with("9;4")) {
                    const auto bel_pos = buffer_.find('\x07', i + 2);
                    const auto st_pos = buffer_.find("\x1b\\", i + 2);
                    std::size_t term_end = std::string::npos;
                    if (bel_pos != std::string::npos && st_pos != std::string::npos) {
                        term_end = std::min(bel_pos + 1, st_pos + 2);
                    } else if (bel_pos != std::string::npos) {
                        term_end = bel_pos + 1;
                    } else if (st_pos != std::string::npos) {
                        term_end = st_pos + 2;
                    }

                    if (term_end != std::string::npos) {
                        commands_.push_back(buffer_.substr(i, term_end - i));
                        i = term_end;
                        continue;
                    }
                    if (!finishing) {
                        break;
                    }
                    // Unterminated OSC 9;4 sequence on finish: do NOT silently drop!
                    commands_.push_back(buffer_.substr(i));
                    i = buffer_.size();
                    break;
                }
                if (osc_payload.size() < 3 && !finishing && (osc_payload == "9" || osc_payload == "9;")) {
                    break;
                }
                const auto bel_pos = buffer_.find('\x07', i + 2);
                const auto st_pos = buffer_.find("\x1b\\", i + 2);
                std::size_t term_end = std::string::npos;
                if (bel_pos != std::string::npos && st_pos != std::string::npos) {
                    term_end = std::min(bel_pos + 1, st_pos + 2);
                } else if (bel_pos != std::string::npos) {
                    term_end = bel_pos + 1;
                } else if (st_pos != std::string::npos) {
                    term_end = st_pos + 2;
                }

                if (term_end != std::string::npos) {
                    i = term_end;
                } else {
                    if (!finishing) break;
                    i += 2;
                }
                continue;
            }
            if (c1 == '[') {
                std::size_t p = i + 2;
                while (p < buffer_.size() && (static_cast<unsigned char>(buffer_[p]) >= 0x20 &&
                                                     static_cast<unsigned char>(buffer_[p]) <= 0x3F)) {
                    ++p;
                }
                if (p < buffer_.size()) {
                    if (static_cast<unsigned char>(buffer_[p]) >= 0x40 &&
                            static_cast<unsigned char>(buffer_[p]) <= 0x7E) {
                        i = p + 1;
                        continue;
                    }
                    i = p + 1;
                    continue;
                }
                if (!finishing) break;
                i = buffer_.size();
                break;
            }
            if (c1 == '(' || c1 == ')' || c1 == '*' || c1 == '+') {
                if (i + 2 < buffer_.size()) {
                    i += 3;
                    continue;
                }
                if (!finishing) break;
                i = buffer_.size();
                break;
            }
            i += 2;
        }
        if (i > 0) {
            buffer_.erase(0, i);
        }
    }

    std::string buffer_{};
    std::vector<std::string> commands_{};
};

[[nodiscard]] inline std::vector<std::string> extract_osc_9_4_commands(std::span<const std::string> chunks) {
    Osc94Extractor extractor;
    extractor.feed(chunks);
    extractor.finish();
    return extractor.take_commands();
}

[[nodiscard]] inline std::vector<std::string> extract_osc_9_4_commands(std::string_view stream) {
    Osc94Extractor extractor;
    extractor.feed(stream);
    extractor.finish();
    return extractor.take_commands();
}

/// Extract theme controls in wire order, retaining duplicates and malformed relevant
/// controls. Ignore paint, styling, titles and other unrelated terminal protocols.
/// finish() retains an incomplete relevant control so truncation cannot pass an oracle.
class ThemeProtocolExtractor final {
public:
    void feed(std::string_view chunk) {
        buffer_.append(chunk);
        parse(false);
    }

    void feed(std::span<const std::string> chunks) {
        for (const auto& chunk : chunks)
            feed(chunk);
    }

    void finish() { parse(true); }

    [[nodiscard]] std::vector<std::string> take_commands() { return std::move(commands_); }

private:
    void parse(bool finishing) {
        std::size_t pos = 0;
        while (pos < buffer_.size()) {
            if (buffer_[pos] != '\x1b') {
                ++pos;
                continue;
            }
            if (pos + 1 == buffer_.size()) {
                if (!finishing) break;
                ++pos;
                continue;
            }
            const auto rest = std::string_view{buffer_}.substr(pos);
            if (rest[1] == ']') {
                const auto bel = buffer_.find('\x07', pos + 2);
                const auto st = buffer_.find("\x1b\\", pos + 2);
                auto end = std::string::npos;
                if (bel != std::string::npos) end = bel + 1;
                if (st != std::string::npos) end = std::min(end, st + 2);
                if (end == std::string::npos && !finishing) break;
                const auto payload = rest.substr(2);
                // Include malformed variants of the three relevant OSC targets.
                const bool relevant =
                        payload.starts_with("10") || payload.starts_with("11") ||
                        (payload.starts_with("4") && (payload.size() == 1 || payload[1] == ';' || payload[1] == ':'));
                if (end == std::string::npos) end = buffer_.size();
                if (relevant) commands_.emplace_back(buffer_.substr(pos, end - pos));
                pos = end;
                continue;
            }
            if (rest[1] == '[') {
                auto end = pos + 2;
                while (end < buffer_.size() && buffer_[end] >= 0x20 && buffer_[end] <= 0x3f)
                    ++end;
                if (end == buffer_.size() && !finishing) break;
                const auto parameters = std::string_view{buffer_}.substr(pos + 2, end - pos - 2);
                const bool relevant = parameters.starts_with("?2031") || (end < buffer_.size() && buffer_[end] == 'c');
                if (end < buffer_.size()) ++end;
                if (relevant) commands_.emplace_back(buffer_.substr(pos, end - pos));
                pos = end;
                continue;
            }
            pos += 2;
        }
        buffer_.erase(0, pos);
    }

    std::string buffer_{};
    std::vector<std::string> commands_{};
};

[[nodiscard]] inline std::vector<std::string> extract_theme_protocol_commands(std::span<const std::string> chunks) {
    ThemeProtocolExtractor extractor;
    extractor.feed(chunks);
    extractor.finish();
    return extractor.take_commands();
}

[[nodiscard]] inline std::vector<std::string> extract_theme_protocol_commands(std::string_view stream) {
    ThemeProtocolExtractor extractor;
    extractor.feed(stream);
    extractor.finish();
    return extractor.take_commands();
}

} // namespace cch::tests
