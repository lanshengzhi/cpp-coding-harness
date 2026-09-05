#pragma once

#include <cch/tui/Terminal.hpp>

#include <cch/support/Error.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cch::tui {

struct VirtualTerminalOptions {
    std::size_t columns{80};
    std::size_t rows{24};
    TerminalCapabilities capabilities{.synchronized_output = true};
};

struct VirtualTerminalStyle {
    bool bold{false};
    bool dim{false};
    bool italic{false};
    bool underline{false};
    bool blink{false};
    bool inverse{false};
    bool hidden{false};
    bool strikethrough{false};
    std::string fg_color;
    std::string bg_color;
    std::string hyperlink;
    std::string hyperlink_params;

    bool operator==(const VirtualTerminalStyle&) const = default;
};

struct VirtualTerminalCell {
    std::string grapheme;
    bool continuation{false};
    VirtualTerminalStyle style;

    bool operator==(const VirtualTerminalCell&) const = default;
};

struct VirtualTerminalImage {
    TerminalImageHandle handle;
    std::uint64_t resource_id{0};
    std::uint64_t revision{0};
    CellRegion region{};
    InlineImageProtocol protocol{InlineImageProtocol::None};
    std::string mime_type{};
    std::optional<std::string> filename{std::nullopt};
    std::size_t pixel_width{0};
    std::size_t pixel_height{0};

    bool operator==(const VirtualTerminalImage&) const = default;
};

class VirtualTerminal final : public Terminal {
public:
    explicit VirtualTerminal(VirtualTerminalOptions options = {});
    VirtualTerminal(VirtualTerminal&&) noexcept;
    VirtualTerminal& operator=(VirtualTerminal&&) noexcept;
    ~VirtualTerminal() override;

    VirtualTerminal(const VirtualTerminal&) = delete;
    VirtualTerminal& operator=(const VirtualTerminal&) = delete;

    [[nodiscard]] support::ExpectedVoid start(TerminalInputSink input_sink, TerminalResizeSink resize_sink) override;
    [[nodiscard]] support::ExpectedVoid stop() override;
    [[nodiscard]] TerminalDimensions dimensions() const override;
    [[nodiscard]] TerminalCapabilities capabilities() const override;
    [[nodiscard]] TerminalModeState modes() const override;
    [[nodiscard]] support::ExpectedVoid clear_screen() override;
    [[nodiscard]] support::ExpectedVoid write(std::string_view output) override;
    [[nodiscard]] support::ExpectedVoid set_cursor(CursorPosition position) override;
    [[nodiscard]] support::ExpectedVoid set_cursor_visible(bool visible) override;
    [[nodiscard]] support::ExpectedVoid set_scroll_margins(
        std::size_t top_row,
        std::size_t bottom_row) override;
    [[nodiscard]] support::ExpectedVoid reset_scroll_margins() override;
    [[nodiscard]] support::ExpectedVoid set_dock_cursor(
        std::size_t dock_row,
        std::size_t column) override;
    [[nodiscard]] support::Expected<TerminalImageHandle> place_image(const TerminalImage& image) override;
    [[nodiscard]] support::ExpectedVoid remove_image(
        TerminalImageHandle handle,
        const CellRegion& region) override;
    [[nodiscard]] support::ExpectedVoid begin_synchronized_update() override;
    [[nodiscard]] support::ExpectedVoid end_synchronized_update() override;
    [[nodiscard]] support::ExpectedVoid set_title(std::string_view title) override;
    [[nodiscard]] support::ExpectedVoid set_progress(bool active) override;
    [[nodiscard]] support::ExpectedVoid drain_input(
        std::chrono::milliseconds max_ms = kDrainInputMaxMs,
        std::chrono::milliseconds idle_ms = kDrainInputIdleMs) override;

    [[nodiscard]] support::ExpectedVoid inject_input(std::string input);
    [[nodiscard]] support::ExpectedVoid flush_input();
    [[nodiscard]] support::ExpectedVoid inject_resize(TerminalDimensions dimensions);
    /// Test scaffolding for the dirty-screen boot scenario (issue #476, ADR
    /// 0041): paints pre-existing shell lines into the visible cells starting
    /// at row 0 and leaves the cursor mid-screen where the shell left it, so
    /// start() anchors the buffer origin at that row. Must be called before
    /// start().
    [[nodiscard]] support::ExpectedVoid seed_shell_content(
        std::vector<std::string> lines,
        CursorPosition cursor);
    [[nodiscard]] const std::vector<std::string>& output() const;
    [[nodiscard]] const std::vector<std::string>& screen() const;
    /// The full written buffer as the terminal models it under the
    /// main-screen scrollback flow (pi `TuiMainScreen`): `scrollback()` holds
    /// the lines that overflowed past the visible viewport (oldest first),
    /// `screen()` the visible viewport, so `scrollback() ++ screen()` is the
    /// full composed buffer. `viewport_top()` is the number of scrolled-out
    /// lines (the first visible buffer line).
    [[nodiscard]] const std::vector<std::string>& scrollback() const;
    [[nodiscard]] std::size_t viewport_top() const;
    [[nodiscard]] const std::vector<std::vector<VirtualTerminalCell>>& cells() const;
    [[nodiscard]] const std::vector<VirtualTerminalImage>& images() const;
    [[nodiscard]] VirtualTerminalStyle final_style() const;
    [[nodiscard]] CursorPosition cursor() const;
    /// Returns true if clear_screen() was called since the last check.
    /// Resets the flag on read.
    [[nodiscard]] bool check_clear_screen_called();
    /// Returns true if the clear-scrollback (`\x1b[3J`) was emitted since the
    /// last check (clear_screen() clears the terminal's scroll history too).
    /// Resets the flag on read.
    [[nodiscard]] bool check_clear_scrollback_called();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cch::tui
