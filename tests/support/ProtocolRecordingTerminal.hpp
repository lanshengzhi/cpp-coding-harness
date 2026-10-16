#pragma once

#include <cch/tui/Terminal.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include <chrono>
#include <cstddef>
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
        return inner_.begin_synchronized_update();
    }

    [[nodiscard]] cch::support::ExpectedVoid end_synchronized_update() override {
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

    [[nodiscard]] cch::support::ExpectedVoid inject_input(std::string input) {
        return inner_.inject_input(std::move(input));
    }
    [[nodiscard]] cch::support::ExpectedVoid flush_input() { return inner_.flush_input(); }

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

} // namespace cch::tests
