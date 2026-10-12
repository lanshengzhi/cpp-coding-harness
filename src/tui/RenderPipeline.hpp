#pragma once

// The cch_tui frame renderer (#749): the differential frame pipeline lifted
// out of Tui. It owns the committed-frame state — the previous prepared and
// raw composed rows, the viewport top over the buffer, the admitted
// (partially flushed) frame watermark, the active image placements, and the
// committed frame dimensions — behind the narrow per-frame seam Tui drives.
//
// Repository-private `cch_tui` header: not an Owner Interface, not installed,
// never exported.

#include <cch/tui/Component.hpp>
#include <cch/tui/ITui.hpp>
#include <cch/tui/Terminal.hpp>
#include <cch/tui/TerminalImage.hpp>
#include <cch/tui/Tui.hpp>

#include <cch/support/Error.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace cch::tui::detail {

/// Owns the state and terminal effects for differential frame rendering.
class RenderPipeline final {
public:
    explicit RenderPipeline(Terminal& terminal);
    /// Arm a fresh differential baseline. The committed dimensions start
    /// unset, so the first render is neither misread as a resize reflow nor
    /// cleared (frozen `previousWidth == 0` semantics).
    void start(TerminalDimensions dimensions) noexcept;
    void begin_frame() noexcept;
    void note_prepared_line() noexcept;
    [[nodiscard]] std::size_t frame_prepare_call_count() const noexcept;
    /// The dimensions of the last committed frame: the geometry basis the
    /// compositor's overlay dispatch and focus queries read between frames.
    [[nodiscard]] TerminalDimensions committed_dimensions() const noexcept {
        return previous_dimensions_.value_or(TerminalDimensions{});
    }
    [[nodiscard]] std::size_t admitted_prefix(TerminalDimensions dimensions) const noexcept;
    [[nodiscard]] support::ExpectedVoid stop(std::optional<CursorPosition> stop_cursor, TuiStopOptions options);
    [[nodiscard]] TuiMainScreenRenderState capture_render_state() const;
    [[nodiscard]] support::ExpectedVoid restore_render_state(TuiMainScreenRenderState state);
    void reset_render_state() noexcept;
    [[nodiscard]] support::ExpectedVoid clear_screen();
    [[nodiscard]] support::ExpectedVoid render(RenderResult materialized,
            TerminalDimensions dimensions,
            TerminalCapabilities capabilities,
            bool compose_overlays,
            bool has_overlay_entries,
            bool clear_on_shrink,
            std::optional<CursorPosition> cursor_location);

private:
    void compose_images(std::vector<std::string>& lines,
            const std::vector<InlineImageRenderRegion>& images,
            InlineImageProtocol protocol);

    struct AdmittedFrame {
        std::size_t rows{0};
        TerminalDimensions dimensions{};
        bool cleared{false};
    };

    struct ImagePlacementState {
        TerminalImageHandle handle;
        CellRegion region;
        std::uint64_t revision{0};
    };

    Terminal& terminal_; // must outlive this pipeline.
    bool first_render_{true};
    std::vector<std::string> previous_lines_;
    /// Composed rows before frame-level preparation, used to reuse finalized rows.
    std::vector<std::string> previous_raw_lines_;
    std::size_t frame_prepare_call_count_{0};
    std::size_t cursor_row_{0};
    std::size_t hardware_cursor_row_{0};
    std::size_t max_lines_rendered_{0};
    std::size_t previous_viewport_top_{0};
    /// The last committed frame's dimensions: reset at start, committed with a
    /// successful render, and reset by clear_screen.
    std::optional<TerminalDimensions> previous_dimensions_{};
    AdmittedFrame admitted_{};
    std::vector<std::uint32_t> previous_kitty_image_ids_;
    std::unordered_map<std::uint64_t, ImagePlacementState> image_placements_;
    bool restored_state_pending_start_{false};

    void position_hardware_cursor(const std::optional<CursorPosition>& cursor_pos, std::size_t total_lines);
};

} // namespace cch::tui::detail
