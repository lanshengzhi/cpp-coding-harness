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
#include <cch/tui/TuiDiagnostics.hpp>

#include <cch/support/Error.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace cch::tui::detail {

/// Frozen offscreen Kitty cache caps (pi tui-alt-screen.ts
/// `MAX_CACHED_OFFSCREEN_KITTY_*`): entries kept offscreen before the oldest
/// are deleted by image id.
inline constexpr std::size_t kMaxCachedOffscreenKittyImages = 16;
inline constexpr std::size_t kMaxCachedOffscreenKittyTransmissionBytes = 32 * 1024 * 1024;
inline constexpr std::size_t kMaxCachedOffscreenKittyDecodedBytes = 64 * 1024 * 1024;

/// One prepared fullscreen frame: the row buffer with image anchors, plus the
/// frozen cleanup bytes that precede the row writes.
struct FullscreenImageFrame {
    std::vector<std::string> lines;
    /// Frozen `clearImages`: delete every cached resource on a full redraw
    /// when nothing was uploaded yet, every placement once something was, and
    /// placements only on an image-anchor redraw. Empty when the frame keeps
    /// its placements.
    std::string clear_bytes;
    /// Image-id deletions for cache entries evicted past the frozen caps.
    std::string deletion_bytes;
    /// Whether changed image anchors force a full row rewrite.
    bool images_need_redraw{false};
};

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
    /// Full redraws this pipeline attempted (pi `fullRender`/`fullRedrawCount`),
    /// including a first render and a redraw that backpressure refused.
    [[nodiscard]] std::size_t full_redraws() const noexcept { return full_redraws_; }
    /// Terminal write accounting of the most recent frame.
    [[nodiscard]] TuiWriteAccounting write_accounting() const noexcept { return write_accounting_; }
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

    /// The fullscreen root's frozen image stage (#1012): replace each visible
    /// placement's row with its Kitty transmission or `a=p` placement-only
    /// reuse, evict offscreen uploads past the frozen caps, and report the
    /// clear bytes and full-row rewrite a changed anchor requires. A non-Kitty
    /// session (including the fullscreen iTerm2 fallback, which suppresses
    /// images for the whole session) writes the fallback text at each anchor
    /// row and places nothing. `full_redraw` maps the root's full repaint.
    [[nodiscard]] FullscreenImageFrame prepare_fullscreen_images(std::vector<std::string> lines,
            const std::vector<InlineImageRenderRegion>& images,
            InlineImageProtocol protocol,
            bool full_redraw);
    /// Drop the uploaded-placement cache at a session boundary
    /// (pi `beforeTerminalStart`/`beforeTerminalStop`).
    void clear_image_placements() noexcept;

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

    /// One cached fullscreen upload: the session upload id, the anchor command
    /// of its last prepared frame, and the frozen eviction accounting.
    struct FullscreenImagePlacement {
        TerminalImageHandle handle;
        std::uint64_t revision{0};
        std::size_t transmission_bytes{0};
        std::size_t decoded_bytes{0};
        std::size_t row{0};
        std::size_t column{0};
        std::string anchor;
        /// Visible in the previous prepared frame: an anchor that stops being
        /// visible forces a redraw so its old cells are rewritten.
        bool anchored{false};
        bool visible{false};
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
    std::unordered_map<std::uint64_t, FullscreenImagePlacement> fullscreen_image_placements_;
    /// Upload ids in frozen LRU order (oldest first): visible placements move
    /// to the back on each redraw frame, exactly like the frozen Map order.
    std::vector<std::uint64_t> fullscreen_image_order_;
    bool restored_state_pending_start_{false};
    std::size_t full_redraws_{0};
    TuiWriteAccounting write_accounting_{};

    /// Issue one frame write and account for its admission: a refused write is
    /// attempted but not committed.
    [[nodiscard]] support::ExpectedVoid write_frame(std::string_view output);
    void position_hardware_cursor(const std::optional<CursorPosition>& cursor_pos, std::size_t total_lines);
};

} // namespace cch::tui::detail
