#pragma once

#include <cch/tui/ITui.hpp>
#include <cch/tui/Keybindings.hpp>
#include <cch/tui/OverlayHandle.hpp>
#include <cch/tui/Mouse.hpp>
#include <cch/tui/RawInputListener.hpp>
#include <cch/tui/ScrollView.hpp>
#include <cch/tui/TuiHost.hpp>

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace cch::tui {

namespace detail {
class OverlayCompositor;
struct LayoutFrame;
struct MousePacket;
struct OverlayHandleContext;
} // namespace detail

/// Absolute-viewport render state of an alternate-screen root, for transfer
/// between roots (#985 shape). `width == 0` means no baseline was captured;
/// use `reset_render_state()` for the -1 sentinel that forces the next frame
/// to clear and repaint.
struct TuiAltScreenRenderState {
    std::vector<std::string> viewport_rows;
    std::size_t width{0};
    std::size_t height{0};
    std::optional<CursorPosition> cursor;
};

/// Synchronous selection clipboard operation. A failure keeps its specific
/// error so a host can report it through its own feedback surface.
using SelectionCopySink = std::move_only_function<support::ExpectedVoid(std::string_view)>;
/// Best-effort navigation for a clicked OSC 8 link.
using LinkOpenSink = std::move_only_function<void(std::string_view url) noexcept>;
/// Synchronous right-click paste request. The fullscreen root consumes a
/// declined request without starting text selection.
using RightClickPasteSink = std::move_only_function<support::ExpectedVoid()>;

/// The frozen multi-click selection interval (pi `DOUBLE_CLICK_INTERVAL_MS`):
/// consecutive presses on the same word within this window count up from
/// character to word to line granularity.
inline constexpr auto kSelectionDoubleClickInterval = std::chrono::milliseconds{500};
/// The edge auto-scroll cadence while a drag selection is held at the
/// viewport edge (pi `setInterval(..., 50)` in `updateSelectionAutoScroll`).
inline constexpr auto kSelectionAutoScrollInterval = std::chrono::milliseconds{50};

struct AutoWheelScrollLines {};
using WheelScrollLines = std::variant<std::size_t, AutoWheelScrollLines>;
using WheelScrollClock = std::move_only_function<std::chrono::steady_clock::time_point()>;

struct AltScreenOptions {
    /// Copy a non-empty text selection when its mouse gesture ends.
    bool copy_on_select{true};
    /// An empty sink selects the toolkit's default OSC 52 clipboard operation.
    SelectionCopySink copy_selection{};
    /// Render a clickable label on the primary follow-end viewport's last row
    /// while it is scrolled away from the end (pi `scrollToEndIndicator`).
    std::function<std::string()> scroll_to_end_indicator{};
    /// Logical rows per wheel event; the alternate-screen frozen default is one.
    /// `AutoWheelScrollLines` enables the frozen velocity-based accelerator.
    /// Frozen pi additionally gates auto on `!terminalAcceleratesWheel()` so
    /// darwin/local terminals get one line per event even in "auto"
    /// (pi `wheel-scroll.ts:35-44`); that platform gate is deliberately
    /// deferred in this Linux-only port and is opt-in here.
    WheelScrollLines wheel_scroll_lines{std::size_t{1}};
    /// Monotonic input clock for automatic wheel acceleration. Tests inject a
    /// deterministic sequence; production defaults to `steady_clock`.
    WheelScrollClock wheel_clock{};
};

/// Alternate-screen (fullscreen) root: owns the terminal's alternate buffer
/// for exactly the lifetime between a successful `start()` and `stop()`.
///
/// The viewport is absolute: every frame repaints only the rows that differ
/// from the committed baseline, each row is written at its own screen position
/// and rows past the terminal height are clipped instead of appended, so
/// content never enters scrollback. `stop()` exits the alternate buffer so the
/// main buffer returns, whether it is called normally or after a failed
/// start; `TuiStopOptions::preserve_screen` does not apply to this root.
///
/// Pointer routing is this root's own stage (pi `TuiAltScreen.handleViewportInput`):
/// complete mouse reports are normalized into `MouseEvent` values, dispatched to
/// the component the last rendered frame placed under the pointer, and otherwise
/// offered to the frame's viewports. The frame itself is the cached geometry the
/// hit test runs against, so no hit test renders; keyboard focus, capture and
/// render decisions follow the component's `MouseEventResult`.
///
/// Without a layout root the attached children are one implicitly scrolled
/// document (`implicitDocument` + `implicitScrollView`, pi
/// `tui-alt-screen.ts:262-270`): the viewport shows the document's end while it
/// follows appended rows, and `viewport_top`/`is_following_output` report where
/// it settled.
class AltScreen final : public ITui, public detail::TuiHost, private detail::OverlayHost {
public:
    explicit AltScreen(Terminal& terminal, AltScreenOptions options = {});
    ~AltScreen() override;

    AltScreen(const AltScreen&) = delete;
    AltScreen& operator=(const AltScreen&) = delete;

    [[nodiscard]] Terminal& terminal() noexcept override { return terminal_; }
    [[nodiscard]] const Terminal& terminal() const noexcept override { return terminal_; }
    void set_keybinding_context(std::shared_ptr<KeybindingsContext> context) override;

    [[nodiscard]] support::Expected<std::reference_wrapper<Component>> add_child(
            std::unique_ptr<Component> component) override;
    [[nodiscard]] support::ExpectedVoid remove_child(Component* component) override;
    void clear_children() override;

    /// Install the component that fills the viewport as the root of a
    /// constrained layout (pi `TuiAltScreen.setLayoutRoot`). While one is
    /// installed it is this root's only mounted component: `owns()` and every
    /// frame follow it, and the child composition is ignored. Passing null
    /// restores the sequential composition of the attached children.
    void set_layout_root(std::unique_ptr<Component> root);

    /// pi `viewportTop`: the top row of the primary scroll viewport.
    [[nodiscard]] std::size_t viewport_top() const;
    /// pi `isFollowingOutput`: whether appended rows move the primary viewport.
    [[nodiscard]] bool is_following_output() const;
    /// Whether the last rendered viewport contains a non-empty mouse selection.
    [[nodiscard]] bool has_active_selection() const;
    /// Copy the current selection. An empty selection returns false without
    /// invoking the configured sink; sink errors are returned unchanged.
    [[nodiscard]] support::Expected<bool> copy_active_selection_to_clipboard();
    [[nodiscard]] bool copy_on_select() const noexcept { return copy_on_select_; }
    void set_copy_on_select(bool enabled) noexcept { copy_on_select_ = enabled; }
    void set_selection_copy_sink(SelectionCopySink sink);
    void set_link_open_sink(LinkOpenSink sink);
    /// Install the host action for an unmodified secondary-button press.
    void set_right_click_paste_sink(RightClickPasteSink sink);
    /// Override the clock that times multi-click selection presses; the default
    /// reads the system clock in milliseconds like pi `Date.now()`. Tests
    /// inject a deterministic clock here.
    void set_selection_click_clock(std::move_only_function<std::chrono::milliseconds()> clock);

    /// Scroll the primary viewport by a row delta and request a frame
    /// (pi `TuiAltScreen.scrollBy`).
    void scroll_by(std::ptrdiff_t lines);
    void scroll_to_top();
    void scroll_to_bottom();
    void set_wheel_scroll_lines(WheelScrollLines lines);
    void set_scroll_to_end_indicator(std::function<std::string()> indicator);

    [[nodiscard]] support::ExpectedVoid set_focus(Component* component) override;
    [[nodiscard]] Component* focus() const noexcept override { return focused_; }

    [[nodiscard]] support::ExpectedVoid start() override;
    [[nodiscard]] support::ExpectedVoid stop(TuiStopOptions options = {}) override;
    [[nodiscard]] support::ExpectedVoid render() override;
    [[nodiscard]] support::ExpectedVoid clear_screen() override;
    void request_render(bool force = false) override;
    void request_immediate_render() override;
    void invalidate() override;
    void set_clear_on_shrink(bool enabled) noexcept override;
    [[nodiscard]] bool clear_on_shrink() const noexcept override;
    void set_scheduler(TuiScheduler* scheduler) override;
    void set_render_prepare_sink(TuiRenderPrepareSink sink) override;
    void set_render_complete_sink(TuiRenderCompleteSink sink) override;
    void set_on_debug(TuiDebugHook hook) override { detail::TuiHost::set_debug_hook(std::move(hook)); }
    [[nodiscard]] std::size_t full_redraws() const noexcept override { return full_redraws_; }
    [[nodiscard]] TuiWriteAccounting write_accounting() const noexcept override { return write_accounting_; }

    [[nodiscard]] RawInputListenerHandle add_raw_input_listener(RawInputListener listener) override;
    [[nodiscard]] support::ExpectedVoid remove_raw_input_listener(RawInputListenerId id) override;
    [[nodiscard]] TerminalColorSchemeListenerHandle on_terminal_color_scheme_change(
            TerminalColorSchemeListener listener) override;
    void set_terminal_color_scheme_notifications(bool enabled) override;
    [[nodiscard]] std::future<TerminalColors> query_terminal_colors(TerminalColorsQueryOptions options) override;

    [[nodiscard]] support::Expected<OverlayHandle> show_overlay(std::unique_ptr<Overlay> overlay) override;
    [[nodiscard]] support::Expected<std::reference_wrapper<Overlay>> add_overlay(
            std::unique_ptr<Overlay> overlay) override;
    [[nodiscard]] support::ExpectedVoid remove_overlay(Overlay* overlay) override;
    [[nodiscard]] support::ExpectedVoid hide_overlay(Overlay* overlay) override;
    [[nodiscard]] support::ExpectedVoid restore_overlay(Overlay* overlay) override;

    /// Committed viewport baseline: the prepared rows and cursor of the last
    /// admitted frame.
    [[nodiscard]] TuiAltScreenRenderState capture_render_state() const;
    /// Import a captured baseline. Rejected while the root is running; a state
    /// with `width == 0` restores the construction-time "no baseline" case.
    [[nodiscard]] support::ExpectedVoid restore_render_state(const TuiAltScreenRenderState& state);
    /// Drop the baseline and force the next frame through the clearing
    /// full-redraw path (the -1 sentinel; distinct from a fresh root).
    void reset_render_state() noexcept;

public:
    /// Debug-only snapshots of live selection endpoints for evidence tests:
    /// pi capture reads `tui.selectionAnchor` and `tui.selectionFocus` directly.
    [[nodiscard]] std::optional<std::pair<std::ptrdiff_t, std::ptrdiff_t>> selection_anchor_trace() const;
    [[nodiscard]] std::optional<std::pair<std::ptrdiff_t, std::ptrdiff_t>> selection_focus_trace() const;
    [[nodiscard]] std::optional<std::string> selection_text_trace() const;

private:
    friend class OverlayHandle;

    struct RenderScheduleState {
        AltScreen* owner{nullptr};
    };

    /// The document behind the implicit viewport: the attached children
    /// composed in mount order, which is what a frame without a layout root
    /// shows (pi `TuiAltScreen.implicitDocument`, `tui-alt-screen.ts:262-267`).
    class ImplicitDocument;

    [[nodiscard]] bool owns(const Component* component) const;
    /// Compose the attached children into one document result, pushing the
    /// frame's rows into any viewport-aware child first.
    [[nodiscard]] support::Expected<RenderResult> render_implicit_document(std::size_t width);
    /// Offer a mouse event to the implicitly composed document: the row ranges
    /// of its last composition decide which mounted child owns the event (pi
    /// `implicitDocument.handleMouse`, `tui-alt-screen.ts:260-267`).
    [[nodiscard]] std::optional<MouseDispatchResult> dispatch_mouse_to_implicit_document(const MouseEvent& event);
    /// The alternate-screen input stage (pi `handleViewportInput`): focus reports
    /// and mouse reports are consumed here, everything else passes unchanged.
    [[nodiscard]] bool handle_viewport_input(std::string_view data);
    [[nodiscard]] bool handle_viewport_key(const KeyEvent& event);
    /// Route a pointer report to the gesture captured or started by an earlier
    /// press, or return false when no live gesture owns it.
    bool handle_gesture_mouse_report(const detail::MousePacket& packet, const MouseEvent& event);
    void handle_mouse_report(const detail::MousePacket& packet);
    [[nodiscard]] bool handle_right_click_paste(const detail::MousePacket& packet);
    [[nodiscard]] bool handle_selection_mouse_report(const detail::MousePacket& packet);
    [[nodiscard]] bool handle_scroll_to_end_indicator_mouse_report(const detail::MousePacket& packet);
    [[nodiscard]] bool handle_scrollbar_mouse_report(const detail::MousePacket& packet);
    void stop_scrollbar_drag() noexcept;
    void validate_scrollbar_interaction(const detail::LayoutFrame& frame);
    void handle_wheel_report(const detail::MousePacket& packet);
    [[nodiscard]] std::optional<MouseDispatchResult> dispatch_mouse_to_layout(const MouseEvent& event);
    [[nodiscard]] std::optional<MouseDispatchResult> dispatch_mouse_to_target(
            const MouseEvent& event, const MouseDispatchTarget& target);
    [[nodiscard]] bool apply_mouse_dispatch_result(const MouseEvent& event, const MouseDispatchResult& result);
    /// The click count of a click at `x`/`y` on `target`: consecutive clicks on
    /// the same component and cell within the double-click interval count up and
    /// wrap after a triple click (pi `getComponentClickCount`).
    [[nodiscard]] std::size_t next_component_click_count(
            const MouseDispatchTarget& target, std::ptrdiff_t x, std::ptrdiff_t y);
    /// Offer a wheel delta to the viewports under the pointer, then to the
    /// primary viewport (pi `routeWheel`).
    void route_wheel(const MouseEvent& event, std::ptrdiff_t delta);
    [[nodiscard]] std::ptrdiff_t next_wheel_scroll_lines(std::ptrdiff_t direction);
    void reset_wheel_scroll_acceleration() noexcept;
    [[nodiscard]] support::ExpectedVoid composite_scroll_to_end_indicator(std::vector<std::string>& lines);
    /// Move the scrollbar hover to the viewport under a frame cell, or clear it
    /// (pi `updateScrollbarHover`).
    void update_scrollbar_hover(std::ptrdiff_t x, std::ptrdiff_t y);
    /// Give pointer presence to one viewport (pi `setScrollbarHover`): the
    /// previous bar leaves active, the next one enters it. A viewport the root
    /// no longer owns — a stale frame can name one — is treated as no hover.
    void set_scrollbar_hover(ScrollView* scroll_view);
    void stop_scrollbar_hover();
    /// Drop the cached frame geometry and document rows because the composition
    /// they name has changed (a removed component may already be destroyed).
    void invalidate_composition_cache() noexcept;
    void clear_mouse_gesture() noexcept;
    void clear_text_selection() noexcept;
    [[nodiscard]] std::optional<std::string> active_selection_text() const;
    [[nodiscard]] bool selection_anchor_precedes_focus() const noexcept;
    [[nodiscard]] std::vector<std::string> apply_text_selection(const std::vector<std::string>& lines) const;
    [[nodiscard]] MouseEvent create_mouse_event(const detail::MousePacket& packet,
            MouseEventType type,
            std::optional<std::ptrdiff_t> wheel_delta = std::nullopt,
            std::optional<std::size_t> click_count = std::nullopt) const;
    /// The primary viewport of the last frame, or the implicit viewport before
    /// the first frame and when no installed root declares one (pi
    /// `getPrimaryScrollView`).
    [[nodiscard]] ScrollView& primary_scroll_view() noexcept;
    [[nodiscard]] const ScrollView& primary_scroll_view() const noexcept;
    [[nodiscard]] support::ExpectedVoid render_now();
    [[nodiscard]] support::ExpectedVoid render_frame();
    [[nodiscard]] support::ExpectedVoid render_viewport(
            const std::vector<std::string>& lines, std::optional<CursorPosition> cursor, TerminalDimensions dimensions);
    void dispatch_input(const InputEventVariant& event);
    void handle_input(std::string input);
    void handle_resize(TerminalDimensions dimensions);
    void apply_focus(Component* component);
    void set_focus_internal(Component* component, bool preserve_restore);
    void remove_overlay_internal(Overlay* overlay) override;
    void hide_overlay_internal(Overlay* overlay, bool hidden) override;
    void focus_overlay_internal(Overlay* overlay) override;
    void unfocus_overlay_internal(Overlay* overlay, std::optional<Component*> target) override;
    [[nodiscard]] bool is_overlay_focused_internal(const Overlay* overlay) const override;
    [[nodiscard]] std::optional<OverlayBounds> overlay_bounds_internal(const Overlay* overlay) const override;
    void clear_overlay_restore_for(Overlay* overlay);
    [[nodiscard]] Component* resolve_overlay_restore();
    void reconcile_overlay_focus(TerminalDimensions dimensions);
    [[nodiscard]] bool focus_target_available(Component* component) const;
    void fallback_focus();
    void schedule_render();
    void cancel_scheduled_render() noexcept;
    void execute_scheduled_render(std::size_t generation);

    std::shared_ptr<RenderScheduleState> render_schedule_state_;
    std::shared_ptr<detail::OverlayHandleContext> overlay_handle_context_;
    std::unique_ptr<detail::OverlayCompositor> compositor_;
    /// One click sequence of the pointer (pi `lastComponentClick`): the number a
    /// repeated click on the same component and cell reports.
    struct MouseClick {
        std::chrono::steady_clock::time_point at{};
        std::size_t count{1};
        const Component* component{nullptr};
        std::ptrdiff_t x{0};
        std::ptrdiff_t y{0};
    };
    /// The frame stage: reports reach it through the raw-input listener chain
    /// (pi `addInputListener` in the constructor).
    RawInputListenerHandle mouse_listener_;
    /// The last rendered frame, the geometry every hit test runs against (pi
    /// `currentLayout`); the rows are painted away, the boxes stay.
    std::unique_ptr<detail::LayoutFrame> cached_frame_;
    /// Rendered row count per mounted child of the implicit document, in mount
    /// order (pi `implicitDocument.mouseLayout.children`).
    std::vector<std::pair<Component*, std::size_t>> implicit_document_rows_;
    // Pointer gesture state (pi `mouseCapture`, `mousePressTarget`,
    // `mousePressPoint`, `mousePressMoved`): a capture keeps drag and release
    // events with the component a press landed on, even outside its bounds.
    std::optional<MouseDispatchTarget> mouse_capture_{};
    std::optional<MouseDispatchTarget> mouse_press_target_{};
    std::optional<std::pair<std::ptrdiff_t, std::ptrdiff_t>> mouse_press_point_{};
    bool mouse_press_moved_{false};
    std::optional<MouseClick> last_mouse_click_{};
    struct SelectionPoint {
        std::ptrdiff_t row{0};
        std::ptrdiff_t column{0};
        ScrollView* scroll_view{nullptr};
        /// pi `SelectionPoint.boundary`: word/line range ends sit on the
        /// boundary after their last cell, so text extraction skips the
        /// end-column grapheme snap a character-drag endpoint gets.
        bool boundary{false};
    };
    /// What a multi-click selection expands by (pi `SelectionGranularity`).
    enum class SelectionGranularity { Character, Word, Line };
    struct SelectionRange {
        SelectionPoint start;
        SelectionPoint end;
    };
    /// The last word-selection press (pi `ClickTarget`): consecutive presses on
    /// the same word, row and viewport inside the interval count up.
    struct SelectionWordClick {
        std::chrono::milliseconds at{};
        std::size_t count{1};
        std::ptrdiff_t row{0};
        ScrollView* scroll_view{nullptr};
        std::ptrdiff_t word_start{0};
        std::ptrdiff_t word_end{0};
    };
    [[nodiscard]] std::optional<SelectionPoint> selection_point_at(
            std::ptrdiff_t x, std::ptrdiff_t y, ScrollView* scroll_view = nullptr) const;
    /// The source row for `point` without clamping: missing rows select
    /// nothing, where text extraction would keep reading the nearest row
    /// (pi `getSelectionSourceLine` returns undefined outside the content).
    [[nodiscard]] std::optional<std::string_view> selection_source_line_at(const SelectionPoint& point) const;
    [[nodiscard]] std::string_view selection_source_line(const SelectionPoint& point) const;
    /// pi `getWordSelection`: the word-segmenter segment containing `point`,
    /// extended over joiner-adjacent selectable segments. Joiners are
    /// selectable segments, selectability only gates joining, and any segment
    /// the click lands on yields a range (pi returns undefined only past the
    /// last segment, so the press stays a character-granularity click).
    [[nodiscard]] std::optional<SelectionRange> word_selection_at(const SelectionPoint& point) const;
    /// pi `getLineSelection`: the whole rendered source row. Infallible: the
    /// clamped source lookup yields "" outside the content, so the range is
    /// zero-width there while granularity and the initial range still land.
    [[nodiscard]] SelectionRange line_selection_at(const SelectionPoint& point) const;
    /// pi `getClickCount`: `(previous.count % 3) + 1` when the click lands on
    /// the same word on the same row and viewport within
    /// `kSelectionDoubleClickInterval`, else 1. The recorded word comes from
    /// the current press, so clicking a different word (or past the line,
    /// where there is no word) starts a fresh cycle.
    [[nodiscard]] std::size_t next_selection_click_count(
            const SelectionPoint& point, const std::optional<SelectionRange>& word);
    /// pi `updateSelectionFocus`: character drags follow the pointer; word and
    /// line drags snap to whole units of the range under the pointer while the
    /// initial range's other end stays anchored, reversing the anchor when the
    /// drag moves before the initial range.
    void update_selection_focus(const SelectionPoint& point);
    /// Start or stop the edge auto-scroll timer from the last drag report:
    /// the pointer at the viewport's visible top/bottom edge scrolls one line
    /// per tick while the gesture is held outside the viewport, and anywhere
    /// else stops it (pi `updateSelectionAutoScroll`, `tui-alt-screen.ts:1262-1306`).
    void update_selection_auto_scroll(const detail::MousePacket& packet);
    /// One interval tick: scroll the selection's viewport one line, extend the
    /// focus to the clipped pointer, and stop when the scroll is consumed or
    /// the gesture state vanished (pi `autoScrollSelection`, `:1287-1306`).
    void auto_scroll_selection();
    /// Run a tick only when its arm generation is still current: a stale queued
    /// interval item must never change a new selection (pi `clearInterval`).
    void execute_auto_scroll_tick(std::size_t generation);
    /// Cancel the interval, the drag pointer, and the direction — every exit
    /// path and the generation guard make a stale queued tick a no-op
    /// (pi `stopSelectionAutoScroll`, `:1308-1314` and `clearInterval`).
    void stop_selection_auto_scroll() noexcept;
    void clear_selection_click_history() noexcept { last_word_click_.reset(); }
    [[nodiscard]] std::pair<std::ptrdiff_t, std::ptrdiff_t> selection_columns(
            std::string_view line, std::ptrdiff_t row, const SelectionPoint& start, const SelectionPoint& end) const;
    std::optional<SelectionPoint> selection_anchor_{};
    std::optional<SelectionPoint> selection_focus_{};
    bool selection_press_active_{false};
    bool selection_dragged_{false};
    SelectionGranularity selection_granularity_{SelectionGranularity::Character};
    std::optional<SelectionRange> selection_initial_range_{};
    std::optional<SelectionWordClick> last_word_click_{};
    /// The last drag report's pointer and the armed direction while a held
    /// drag rides an edge (pi `selectionDragPointer`/`selectionAutoScrollDirection`).
    std::optional<std::pair<std::ptrdiff_t, std::ptrdiff_t>> selection_drag_pointer_{};
    std::ptrdiff_t selection_auto_scroll_direction_{0};
    bool selection_auto_scroll_armed_{false};
    /// Incremented on every arm/stop: an interval work item queued by an
    /// earlier generation no-ops, so a stale tick can never change a new
    /// selection (pi `clearInterval` semantics).
    std::size_t selection_auto_scroll_generation_{0};
    std::optional<std::string> pressed_url_{};
    std::move_only_function<std::chrono::milliseconds()> selection_click_clock_{};
    WheelScrollLines wheel_scroll_lines_{std::size_t{1}};
    WheelScrollClock wheel_clock_{};
    std::optional<std::chrono::steady_clock::time_point> wheel_last_time_{};
    std::ptrdiff_t wheel_last_direction_{0};
    std::optional<double> wheel_average_gap_ms_{};
    double wheel_carry_{0};
    /// Resolved viewport key bindings. Viewport keys (pageUp/pageDown/
    /// home/end) stay inert until `set_keybinding_context` installs a
    /// context; frozen pi always has global keybindings, and the frontend
    /// (`InteractiveEngine`) always wires this.
    std::shared_ptr<KeybindingsContext> keybinding_context_{};
    std::function<std::string()> scroll_to_end_indicator_{};
    struct ScrollToEndIndicatorRect {
        std::ptrdiff_t row{0};
        std::ptrdiff_t column{0};
        std::size_t width{0};
    };
    std::optional<ScrollToEndIndicatorRect> scroll_to_end_indicator_rect_{};
    LinkOpenSink link_open_sink_{};
    RightClickPasteSink right_click_paste_sink_{};
    bool copy_on_select_{true};
    SelectionCopySink selection_copy_sink_{};
    /// Captured scrollbar pointer interaction (pi `scrollbarDrag`). The weak
    /// token prevents a detached or destroyed viewport from being dereferenced.
    struct ScrollbarDrag {
        ScrollView* scroll_view{nullptr};
        std::ptrdiff_t grab_offset{0};
        std::weak_ptr<void> lifetime{};
    };
    std::optional<ScrollbarDrag> scrollbar_drag_{};
    /// The viewport whose scrollbar the pointer is on (pi `scrollbarHover`); a
    /// hovered bar keeps its transient reveal and draws the active thumb.
    ScrollView* scrollbar_hover_{nullptr};
    std::weak_ptr<void> scrollbar_hover_lifetime_{};
    // Fullscreen layout authority (#998): when set, every frame is one
    // constrained composition of this component instead of a flow of children.
    std::unique_ptr<Component> layout_root_;
    // The implicit viewport the attached children are shown through when no
    // layout root is installed (pi `implicitScrollView`).
    std::unique_ptr<ScrollView> implicit_scroll_view_;
    // The primary viewport of the last frame (pi `currentLayout.primaryScrollView`).
    ScrollView* layout_primary_scroll_view_{nullptr};
    TuiScheduler* scheduler_{nullptr};
    TuiRenderPrepareSink render_prepare_sink_;
    TuiRenderCompleteSink render_complete_sink_;
    bool started_{false};
    bool stopping_{false};
    bool pending_render_{false};
    bool render_scheduled_{false};
    bool clear_on_shrink_{false};
    std::size_t render_generation_{0};
    std::size_t overlay_focus_order_counter_{0};
    detail::TuiHost::OverlayFocusRestoreState overlay_focus_restore_{};

    // Absolute-viewport baseline. Construction starts with no baseline and
    // paints every row without clearing; `reset_pending_` is the sentinel set
    // by `reset_render_state()` that additionally clears the buffer first.
    bool first_render_{true};
    bool reset_pending_{false};
    // Set by restore_render_state() so the next start() keeps the imported
    // baseline instead of resetting it for the freshly entered buffer.
    bool baseline_from_restore_{false};
    std::optional<TerminalDimensions> previous_dimensions_;
    std::vector<std::string> previous_lines_;
    std::vector<std::string> selection_source_lines_;
    std::optional<CursorPosition> cursor_;
    bool cursor_visible_{false};
    std::size_t full_redraws_{0};
    TuiWriteAccounting write_accounting_{};
};

} // namespace cch::tui
