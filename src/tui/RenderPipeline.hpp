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
#include <cch/tui/Terminal.hpp>

#include <cch/support/Error.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace cch::tui::detail {

/// Owns the state and terminal effects for differential frame rendering.
class RenderPipeline final {
public:
    explicit RenderPipeline(Terminal& terminal);
    /// Arm a fresh differential baseline; the committed dimensions start at
    /// the terminal's dimensions so the first render is not misread as a
    /// resize reflow.
    void start(TerminalDimensions dimensions) noexcept;
    void begin_frame() noexcept;
    void note_prepared_line() noexcept;
    [[nodiscard]] std::size_t frame_prepare_call_count() const noexcept;
    /// The dimensions of the last committed frame: the geometry basis the
    /// compositor's overlay dispatch and focus queries read between frames.
    [[nodiscard]] TerminalDimensions committed_dimensions() const noexcept {
        return previous_dimensions_;
    }
    [[nodiscard]] support::ExpectedVoid stop(std::optional<CursorPosition> stop_cursor);
    [[nodiscard]] support::ExpectedVoid clear_screen();
    [[nodiscard]] support::ExpectedVoid render(RenderResult materialized,
            TerminalDimensions dimensions,
            TerminalCapabilities capabilities,
            bool compose_overlays,
            std::optional<CursorPosition> cursor_location);

private:
    struct ActiveImage {
        TerminalImageHandle handle;
        CellRegion region;
        std::uint64_t resource_id{0};
        std::uint64_t revision{0};
    };

    struct AdmittedFrame {
        std::size_t rows{0};
        TerminalDimensions dimensions{};
        std::size_t viewport_height{0};
        bool cleared{false};
        std::size_t stale_below{0};
        std::size_t stale_cleared{0};
    };

    [[nodiscard]] support::ExpectedVoid remove_active_images();
    [[nodiscard]] support::ExpectedVoid remove_images_intersecting(const CellRegion& region);
    [[nodiscard]] support::ExpectedVoid remove_stale_images(
            const std::vector<InlineImageRenderRegion>& desired_images);
    [[nodiscard]] support::ExpectedVoid place_images(const std::vector<InlineImageRenderRegion>& desired_images);
    [[nodiscard]] std::size_t admitted_prefix(TerminalDimensions dimensions, std::size_t viewport_height) const;

    Terminal& terminal_; // must outlive this pipeline.
    bool first_render_{true};
    std::vector<std::string> previous_lines_;
    std::vector<std::string> previous_dock_lines_;
    /// Composed rows before frame-level preparation, used to reuse finalized rows.
    std::vector<std::string> previous_raw_lines_;
    std::vector<std::string> previous_raw_dock_lines_;
    std::size_t frame_prepare_call_count_{0};
    std::size_t previous_viewport_height_{0};
    std::size_t viewport_top_{0};
    /// The last committed frame's dimensions: set at start, committed with a
    /// successful render, and reset by clear_screen.
    TerminalDimensions previous_dimensions_{};
    AdmittedFrame admitted_;
    std::vector<ActiveImage> active_images_;
};

} // namespace cch::tui::detail
