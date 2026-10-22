#include <cch/tui/AltScreen.hpp>

#include <cch/tui/Base64.hpp>
#include "tui/Layout.hpp"
#include "tui/LineCompositor.hpp"
#include "tui/MouseRouting.hpp"
#include "tui/OverlayCompositor.hpp"
#include "tui/RenderPipeline.hpp"
#include "tui/RenderUtils.hpp"
#include "tui/UnicodeWidth.hpp"
#include "tui/WordNavigation.hpp"

#include <cch/support/Error.hpp>
#include <cch/tui/CursorMarkers.hpp>
#include <cch/tui/TerminalImage.hpp>
#include <cch/tui/Utils.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <format>
#include <limits>
#include <ranges>
#include <utility>

namespace cch::tui {
namespace {

// Terminal reports the alternate-screen root reacts to (pi
// `tui-alt-screen.ts:67-71`).
constexpr std::string_view kFocusIn = "\x1b[I";
constexpr std::string_view kFocusOut = "\x1b[O";
// Any-motion tracking reports hover and drag without the caller asking for it.
// A multiplexer lags on every forwarded movement, so it gets button-motion
// tracking instead; both keep clicks, wheel reports and drags (pi
// `tui-alt-screen.ts:363-378`).
constexpr std::string_view kEnableMouseAllMotion = "\x1b[?1000h\x1b[?1002h\x1b[?1003h\x1b[?1004h\x1b[?1006h";
constexpr std::string_view kEnableMouseButtonMotion = "\x1b[?1000h\x1b[?1002h\x1b[?1004h\x1b[?1006h";
constexpr std::string_view kDisableMouse = "\x1b[?1006l\x1b[?1004l\x1b[?1003l\x1b[?1002l\x1b[?1000l";
// Consecutive clicks on one component and cell within this window count up (pi
// `DOUBLE_CLICK_INTERVAL_MS`).
constexpr auto kComponentDoubleClickInterval = std::chrono::milliseconds{500};

[[nodiscard]] std::string describe_error(const support::Error& error) {
    if (error.detail.empty()) return error.message;
    return std::format("{} [{}]", error.message, error.detail);
}

[[nodiscard]] support::Error combine_errors(
        support::Error primary, const support::Error& secondary, std::string message) {
    return support::make_error(primary.code,
            std::move(message),
            std::format("primary: {}; secondary: {}", describe_error(primary), describe_error(secondary)));
}

void retain_error(support::ExpectedVoid& accumulated, support::ExpectedVoid candidate) {
    if (candidate) return;
    if (accumulated) {
        accumulated = std::unexpected(candidate.error());
        return;
    }
    accumulated = std::unexpected(combine_errors(
            std::move(accumulated.error()), candidate.error(), "AltScreen stop encountered multiple failures"));
}

/// Whether every pointer movement reaches the terminal: never inside the
/// multiplexers pi reduces to button-motion tracking.
[[nodiscard]] bool mouse_all_motion_enabled() {
    if (std::getenv("TMUX") != nullptr || std::getenv("ZELLIJ") != nullptr || std::getenv("STY") != nullptr)
        return false;
    const auto* term = std::getenv("TERM");
    if (term == nullptr) return true;
    const std::string_view value{term};
    return !value.starts_with("tmux") && !value.starts_with("screen");
}


[[nodiscard]] std::string apply_selection_inverse(std::string_view text) {
    std::string result{"\x1b[7m"};
    for (std::size_t index = 0; index < text.size();) {
        if (text[index] == '\x1b') {
            const auto ansi = detail::extract_ansi_code(text, index);
            if (ansi) {
                result += ansi->code;
                if (ansi->code.ends_with('m')) result += "\x1b[7m";
                index += ansi->length;
                continue;
            }
        }
        const auto [codepoint, size] = detail::decode_utf8(text, index);
        (void)codepoint;
        if (size == 0) break;
        result.append(text.substr(index, size));
        index += size;
    }
    result += "\x1b[27m";
    return result;
}

[[nodiscard]] std::pair<std::size_t, std::size_t> grapheme_cell_range(std::string_view line, std::size_t column) {
    auto tokens = detail::tokenize_terminal_output(line);
    if (!tokens) return {column, column + 1};
    std::size_t current = 0;
    for (const auto& token : *tokens) {
        if (token.kind != detail::TerminalTokenKind::Grapheme) continue;
        if (column < current + token.width) return {current, current + token.width};
        current += token.width;
    }
    return {std::min(column, current), std::min(column + 1, current)};
}

[[nodiscard]] std::string trim_end(std::string text) {
    while (!text.empty()) {
        const auto ch = static_cast<unsigned char>(text.back());
        if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r' && ch != '\f' && ch != '\v') break;
        text.pop_back();
    }
    return text;
}

struct SearchHighlightRange {
    std::size_t start_col{0};
    std::size_t end_col{0};
    bool current{false};
};

/// One word segment of a rendered source row in visible columns (pi
/// `Intl.Segmenter` word segment mapped to terminal cells).
struct SelectionWordSegment {
    std::ptrdiff_t start{0};
    std::ptrdiff_t end{0};
    bool word_like{false};
    bool joiner{false};
    [[nodiscard]] bool selectable() const { return word_like || joiner; }
};

/// The word segments of one rendered source row in visible columns: pinned
/// ICU `UBRK_WORD` boundaries via `detail::segment_words` (the same engine
/// class as pi's `Intl.Segmenter`), segment edges advanced by the segment's
/// visible width. Zero-width segments occupy no cells and are unclickable,
/// exactly like pi (a click selects the next segment instead).
[[nodiscard]] std::optional<std::vector<SelectionWordSegment>> selection_word_segments(std::string_view line) {
    auto words = detail::segment_words(line);
    // A mouse press cannot surface an error, so a segmentation failure
    // degrades to "no word here": the press selects by character, the same
    // practical fallback the pre-existing word navigation uses when its
    // segmenter fails.
    if (!words) return std::nullopt;
    std::vector<SelectionWordSegment> segments;
    segments.reserve(words->size());
    std::ptrdiff_t column = 0;
    for (const auto& word : *words) {
        const auto segment = line.substr(word.byte_start, word.byte_end - word.byte_start);
        const auto width = static_cast<std::ptrdiff_t>(visible_width(segment));
        segments.push_back(SelectionWordSegment{.start = column,
                .end = column + width,
                .word_like = word.word_like,
                .joiner = segment == "/" || segment == "-"});
        column += width;
    }
    return segments;
}

[[nodiscard]] support::ExpectedVoid write_osc52(Terminal& terminal, std::string_view text) {
    return terminal.write(std::format("\x1b]52;c;{}\x07", base64_encode(text)));
}

void scroll_scrollbar_to_pointer(ScrollView& scroll_view,
        const detail::ScrollbarGeometry& geometry,
        std::ptrdiff_t pointer_y,
        std::ptrdiff_t grab_offset) {
    const auto max_thumb_offset = geometry.track_height - geometry.thumb_height;
    const auto requested_offset = pointer_y - geometry.track_top - grab_offset;
    const auto thumb_offset =
            std::clamp(requested_offset, std::ptrdiff_t{0}, static_cast<std::ptrdiff_t>(max_thumb_offset));
    const auto scroll_top =
            max_thumb_offset == 0 ? std::size_t{0}
                                  : static_cast<std::size_t>(std::llround(
                                            static_cast<double>(thumb_offset) / static_cast<double>(max_thumb_offset) *
                                            static_cast<double>(geometry.max_scroll_top)));
    scroll_view.scroll_to(static_cast<std::ptrdiff_t>(scroll_top));
}

} // namespace

/// The mounted-children document behind the implicit viewport (pi
/// `TuiAltScreen.implicitDocument`): the children compose in mount order, push
/// the frame's rows into a viewport-aware child, and report one leaf result, so
/// the frame engine owns clipping and scrolling. `cursor_location` reports the
/// composed cursor at the document row it was rendered at, which is the row the
/// engine translates into the frame.
class AltScreen::ImplicitDocument final : public Component, public MouseHandler {
public:
    explicit ImplicitDocument(AltScreen& owner) : owner_(owner) {}

    [[nodiscard]] support::Expected<RenderResult> render(std::size_t width) override {
        auto rendered = owner_.render_implicit_document(width);
        if (!rendered) {
            cursor_.reset();
            return rendered;
        }
        cursor_ = rendered->cursor;
        return rendered;
    }

    void invalidate() override {
        for (const auto& child : owner_.children_)
            child->invalidate();
    }

    [[nodiscard]] std::optional<CursorPosition> cursor_location() const override { return cursor_; }

    [[nodiscard]] bool contains(const Component* target) const override { return owner_.owns(target); }

    [[nodiscard]] std::optional<MouseDispatchResult> handle_mouse_forwarded(const MouseEvent& event) override {
        return owner_.dispatch_mouse_to_implicit_document(event);
    }

private:
    AltScreen& owner_;
    std::optional<CursorPosition> cursor_{};
};

AltScreen::AltScreen(Terminal& terminal, AltScreenOptions options)
    : detail::TuiHost(terminal), render_schedule_state_(std::make_shared<RenderScheduleState>()),
      overlay_handle_context_(std::make_shared<detail::OverlayHandleContext>()),
      compositor_(std::make_unique<detail::OverlayCompositor>()),
      image_pipeline_(std::make_unique<detail::RenderPipeline>(terminal)), selection_click_clock_([] {
          return std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::system_clock::now().time_since_epoch());
      }),
      wheel_scroll_lines_(std::move(options.wheel_scroll_lines)), wheel_clock_(std::move(options.wheel_clock)),
      scroll_to_end_indicator_(std::move(options.scroll_to_end_indicator)), copy_on_select_(options.copy_on_select),
      selection_copy_sink_(std::move(options.copy_selection)),
      search_match_style_(std::move(options.search_match_style)),
      search_current_match_style_(std::move(options.search_current_match_style)),
      search_navigation_button_style_(std::move(options.search_navigation_button_style)),
      show_hardware_cursor_(options.show_hardware_cursor) {
    flashes_.set_style(std::move(options.flash_style));
    render_schedule_state_->owner = this;
    overlay_handle_context_->owner = this;
    if (!selection_copy_sink_) {
        selection_copy_sink_ = [this](std::string_view text) { return write_osc52(terminal_, text); };
    }
    if (!wheel_clock_) wheel_clock_ = [] { return std::chrono::steady_clock::now(); };
    if (!search_match_style_) search_match_style_ = [](std::string text) { return "\x1b[4m" + text + "\x1b[24m"; };
    if (!search_current_match_style_)
        search_current_match_style_ = [](std::string text) { return "\x1b[1;7m" + text + "\x1b[22;27m"; };
    if (!search_navigation_button_style_) search_navigation_button_style_ = [](std::string text, bool) { return text; };
    implicit_scroll_view_ = std::make_unique<ScrollView>(
            std::make_unique<ImplicitDocument>(*this), ScrollViewOptions{.follow_end = true, .primary = true});
    // pi registers the viewport input stage with the instance
    // (`tui-alt-screen.ts:281`), so mouse reports are consumed before any
    // listener registered afterwards observes them.
    mouse_listener_ = add_raw_input_listener([this](std::string_view data) {
        return handle_viewport_input(data) ? RawInputListenerResult::consume() : RawInputListenerResult::pass();
    });
}

void AltScreen::set_keybinding_context(std::shared_ptr<KeybindingsContext> context) {
    keybinding_context_ = std::move(context);
}

void AltScreen::set_wheel_scroll_lines(WheelScrollLines lines) {
    wheel_scroll_lines_ = std::move(lines);
    reset_wheel_scroll_acceleration();
}

void AltScreen::set_scroll_to_end_indicator(std::move_only_function<std::string()> indicator) {
    scroll_to_end_indicator_ = std::move(indicator);
    request_render();
}

void AltScreen::flash(std::string message, std::chrono::milliseconds duration) {
    flashes_.flash(std::move(message), duration);
}

void AltScreen::set_selection_copy_sink(SelectionCopySink sink) {
    if (sink) {
        selection_copy_sink_ = std::move(sink);
        return;
    }
    selection_copy_sink_ = [this](std::string_view text) { return write_osc52(terminal_, text); };
}

void AltScreen::set_link_open_sink(LinkOpenSink sink) { link_open_sink_ = std::move(sink); }

void AltScreen::set_right_click_paste_sink(RightClickPasteSink sink) { right_click_paste_sink_ = std::move(sink); }

void AltScreen::set_selection_click_clock(std::move_only_function<std::chrono::milliseconds()> clock) {
    // An empty callable would be invoked on the next press (undefined
    // behavior); ignoring it keeps the current clock.
    if (!clock) return;
    selection_click_clock_ = std::move(clock);
}

AltScreen::~AltScreen() {
    (void)stop();
    flashes_.dispose();
    render_schedule_state_->owner = nullptr;
    overlay_handle_context_->owner = nullptr;
}

bool AltScreen::owns(const Component* component) const {
    if (component == nullptr) return false;
    if (layout_root_) return layout_root_->contains(component);
    // The implicit document and its viewport are live routing nodes in the
    // child composition, though they are not themselves mounted children.
    if (component == implicit_scroll_view_.get() || component == implicit_scroll_view_->child_.get()) return true;
    for (const auto& child : children_) {
        if (child && child->contains(component)) return true;
    }
    return false;
}

void AltScreen::set_layout_root(std::unique_ptr<Component> root) {
    ChildOperation operation(*this);
    // The active interaction may live in the outgoing tree; its pointer would
    // dangle once the root is destroyed.
    stop_scrollbar_drag();
    stop_scrollbar_hover();
    // Focus that lives in the outgoing tree keeps the tree alive until the
    // operation ends, then falls back to a mounted target.
    const bool was_focused = focused_ != nullptr && layout_root_ && layout_root_->contains(focused_);
    if (was_focused) retired_children_.push_back(std::move(layout_root_));
    layout_root_ = std::move(root);
    // The recorded primary viewport belongs to the tree that was just replaced.
    layout_primary_scroll_view_ = nullptr;
    invalidate_composition_cache();
    if (was_focused) {
        apply_focus(nullptr);
        fallback_focus();
    }
    request_render();
}

std::unique_ptr<Component> AltScreen::take_layout_root() {
    ChildOperation operation(*this);
    stop_scrollbar_drag();
    stop_scrollbar_hover();
    if (layout_root_ == nullptr) return nullptr;
    const bool was_focused = focused_ != nullptr && layout_root_->contains(focused_);
    if (was_focused) apply_focus(nullptr);
    auto root = std::move(layout_root_);
    layout_primary_scroll_view_ = nullptr;
    invalidate_composition_cache();
    if (was_focused) fallback_focus();
    request_render();
    return root;
}

support::Expected<std::reference_wrapper<Component>> AltScreen::add_child(std::unique_ptr<Component> component) {
    auto attached = detail::attach_child(children_, std::move(component), "");
    if (!attached) return std::unexpected(attached.error());
    invalidate_composition_cache();
    request_render();
    return *attached;
}

support::ExpectedVoid AltScreen::remove_child(Component* component) {
    if (component == nullptr) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation, "TUI child cannot be null"));
    }

    ChildOperation operation(*this);
    auto found = std::find_if(
            children_.begin(), children_.end(), [component](const auto& child) { return child.get() == component; });
    if (found == children_.end()) {
        return std::unexpected(
                support::make_error(support::ErrorCode::Validation, "Component is not attached to this TUI"));
    }

    const bool was_focused = focused_ == component;
    const bool was_blocker = detail::TuiHost::overlay_focus_restore_.status == OverlayFocusRestoreStatus::Blocked &&
                             detail::TuiHost::overlay_focus_restore_.blocked_by == component;
    const auto restore_state = detail::TuiHost::overlay_focus_restore_;
    compositor_->forget_component(component);
    if (was_blocker) {
        detail::TuiHost::overlay_focus_restore_ = {};
        if (restore_state.resume_to_overlay && compositor_->owns(restore_state.overlay)) {
            set_focus_internal(restore_state.overlay, false);
        } else if (!restore_state.resume_to_overlay && restore_state.resume_target != component &&
                   focus_target_available(restore_state.resume_target)) {
            set_focus_internal(restore_state.resume_target, false);
        } else if (focused_ == component) {
            apply_focus(nullptr);
        }
    } else if (detail::TuiHost::overlay_focus_restore_.status == OverlayFocusRestoreStatus::Blocked &&
               !detail::TuiHost::overlay_focus_restore_.resume_to_overlay &&
               detail::TuiHost::overlay_focus_restore_.resume_target == component) {
        detail::TuiHost::overlay_focus_restore_.resume_target = nullptr;
    }
    if (scrollbar_drag_ != std::nullopt && component->contains(scrollbar_drag_->scroll_view)) {
        stop_scrollbar_drag();
        stop_scrollbar_hover();
    } else if (scrollbar_hover_ != nullptr && component->contains(scrollbar_hover_)) {
        stop_scrollbar_hover();
    }
    retired_children_.push_back(std::move(*found));
    children_.erase(found);
    invalidate_composition_cache();
    if (was_focused && !was_blocker) {
        set_focus_internal(nullptr, false);
        if (focused_ == nullptr) fallback_focus();
    }
    request_render();
    return {};
}

void AltScreen::clear_children() { static_cast<void>(take_children()); }

std::vector<std::unique_ptr<Component>> AltScreen::take_children() {
    ChildOperation operation(*this);
    const bool was_focused = focused_ != nullptr && owns(focused_);
    const bool was_blocker = detail::TuiHost::overlay_focus_restore_.status == OverlayFocusRestoreStatus::Blocked &&
                             detail::TuiHost::overlay_focus_restore_.blocked_by != nullptr &&
                             owns(detail::TuiHost::overlay_focus_restore_.blocked_by);
    const auto restore_state = detail::TuiHost::overlay_focus_restore_;
    const bool removed_restore_target =
            restore_state.status == OverlayFocusRestoreStatus::Blocked && !restore_state.resume_to_overlay &&
            std::any_of(children_.begin(), children_.end(), [&restore_state](const auto& child) {
                return child.get() == restore_state.resume_target;
            });
    // Without a layout root, the attached children are the whole composition,
    // so clearing them invalidates any viewport interaction with that document.
    if (layout_root_ == nullptr) {
        stop_scrollbar_drag();
        stop_scrollbar_hover();
    }
    for (const auto& child : children_)
        compositor_->forget_component(child.get());
    if (was_blocker) {
        detail::TuiHost::overlay_focus_restore_ = {};
        if (restore_state.resume_to_overlay && compositor_->owns(restore_state.overlay)) {
            set_focus_internal(restore_state.overlay, false);
        } else if (!restore_state.resume_to_overlay && !removed_restore_target &&
                   focus_target_available(restore_state.resume_target)) {
            set_focus_internal(restore_state.resume_target, false);
        } else if (focused_ != nullptr && owns(focused_)) {
            apply_focus(nullptr);
        }
    } else if (removed_restore_target) {
        detail::TuiHost::overlay_focus_restore_.resume_target = nullptr;
    }
    auto taken = std::move(children_);
    children_.clear();
    invalidate_composition_cache();
    if (was_focused && !was_blocker) set_focus_internal(nullptr, false);
    request_render();
    return taken;
}

bool AltScreen::has_overlay_entries() const noexcept { return compositor_->has_overlay_entries(); }

void AltScreen::clear_overlays() {
    while (compositor_->has_overlay_entries()) {
        auto* overlay = compositor_->topmost_overlay();
        if (overlay == nullptr) break;
        remove_overlay_internal(overlay);
    }
}

bool AltScreen::focus_target_available(Component* component) const {
    if (component == nullptr) return false;
    if (owns(component)) return dynamic_cast<Focusable*>(component) != nullptr;
    return compositor_->focus_target_available(component, terminal_.dimensions());
}

void AltScreen::apply_focus(Component* component) {
    detail::TuiHost::apply_focus(component, [this](Component* candidate) { return focus_target_available(candidate); });
}

// This root's overlay transitions share the TuiHost restore state, while the
// six OverlayHost implementations still own their duplicated focus behavior.
void AltScreen::set_focus_internal(Component* component, bool preserve_restore) {
    const auto previous = focused_;
    const auto restore_before = detail::TuiHost::overlay_focus_restore_;
    const auto is_overlay = [this](Component* candidate) {
        auto* overlay = dynamic_cast<Overlay*>(candidate);
        return overlay != nullptr && compositor_->owns(overlay);
    };
    auto* previous_overlay = is_overlay(previous) ? dynamic_cast<Overlay*>(previous) : nullptr;
    auto* next_overlay = is_overlay(component) ? dynamic_cast<Overlay*>(component) : nullptr;

    if (component != nullptr && next_overlay == nullptr) {
        if (detail::TuiHost::overlay_focus_restore_.status == OverlayFocusRestoreStatus::Blocked &&
                detail::TuiHost::overlay_focus_restore_.blocked_by == previous) {
            if (!detail::TuiHost::overlay_focus_restore_.resume_to_overlay) {
                component = detail::TuiHost::overlay_focus_restore_.resume_target;
                detail::TuiHost::overlay_focus_restore_ = {};
            } else if (previous == nullptr || !owns(previous)) {
                component = resolve_overlay_restore();
            } else {
                detail::TuiHost::overlay_focus_restore_.blocked_by = component;
            }
        } else if (!preserve_restore && previous_overlay != nullptr &&
                   detail::TuiHost::overlay_focus_restore_.status == OverlayFocusRestoreStatus::Eligible &&
                   detail::TuiHost::overlay_focus_restore_.overlay == previous_overlay) {
            detail::TuiHost::overlay_focus_restore_ = OverlayFocusRestoreState{
                    .status = OverlayFocusRestoreStatus::Blocked,
                    .overlay = previous_overlay,
                    .blocked_by = component,
            };
        }
    } else if (component == nullptr) {
        if (detail::TuiHost::overlay_focus_restore_.status == OverlayFocusRestoreStatus::Blocked &&
                detail::TuiHost::overlay_focus_restore_.blocked_by == previous) {
            component = resolve_overlay_restore();
        } else if (detail::TuiHost::overlay_focus_restore_.status == OverlayFocusRestoreStatus::Blocked &&
                   !detail::TuiHost::overlay_focus_restore_.resume_to_overlay &&
                   previous == detail::TuiHost::overlay_focus_restore_.resume_target) {
            detail::TuiHost::overlay_focus_restore_ = {};
        } else if (!preserve_restore) {
            detail::TuiHost::overlay_focus_restore_ = {};
        }
    }

    apply_focus(component);
    auto* focused_overlay = dynamic_cast<Overlay*>(focused_);
    if (focused_overlay != nullptr && compositor_->visible(focused_overlay, terminal_.dimensions())) {
        detail::TuiHost::overlay_focus_restore_ = OverlayFocusRestoreState{
                .status = OverlayFocusRestoreStatus::Eligible,
                .overlay = focused_overlay,
        };
    } else if (preserve_restore && restore_before.status != OverlayFocusRestoreStatus::Inactive &&
               detail::TuiHost::overlay_focus_restore_.status == OverlayFocusRestoreStatus::Inactive) {
        detail::TuiHost::overlay_focus_restore_ = restore_before;
    }
}

support::ExpectedVoid AltScreen::set_focus(Component* component) {
    ChildOperation operation(*this);
    if (component != nullptr && stopping_) {
        return std::unexpected(
                support::make_error(support::ErrorCode::Validation, "TUI cannot focus components while stopping"));
    }
    if (component != nullptr && !owns(component)) {
        auto* overlay = dynamic_cast<Overlay*>(component);
        if (overlay == nullptr || !compositor_->owns(overlay)) {
            return std::unexpected(support::make_error(
                    support::ErrorCode::Validation, "TUI focus target is not attached to this root"));
        }
    }
    if (component != nullptr && dynamic_cast<Focusable*>(component) == nullptr) {
        return std::unexpected(
                support::make_error(support::ErrorCode::Validation, "TUI focus target does not participate in focus"));
    }
    if (component == focused_) return {};
    set_focus_internal(component, false);
    return {};
}

void AltScreen::fallback_focus() {
    ChildOperation operation(*this);
    if (auto* overlay = compositor_->topmost_focusable(terminal_.dimensions())) {
        set_focus_internal(overlay, false);
        return;
    }
    for (const auto& child : children_) {
        auto* focusable = dynamic_cast<Focusable*>(child.get());
        if (focusable == nullptr) continue;
        auto* as_component = dynamic_cast<Component*>(focusable);
        if (as_component != focused_) set_focus_internal(as_component, false);
        return;
    }
}

support::ExpectedVoid AltScreen::start() {
    if (started_) return {};

    started_ = true;
    // No pointer interaction survives the session boundary (pi
    // `beforeTerminalStart`).
    stop_scrollbar_drag();
    stop_scrollbar_hover();
    clear_selection_click_history();
    pressed_url_.reset();
    // Frozen `beforeTerminalStart`: the session's image protocol decides
    // whether the root places Kitty images or shows the text fallback for its
    // whole lifetime, and a previous session's uploads are forgotten.
    session_image_protocol_ = terminal_.capabilities().inline_images;
    image_pipeline_->clear_image_placements();
    if (focused_ == nullptr) fallback_focus();
    if (auto result = terminal_.start(
                [this](std::string input) -> support::ExpectedVoid {
                    handle_input(std::move(input));
                    return {};
                },
                [this](TerminalDimensions dimensions) -> support::ExpectedVoid {
                    handle_resize(dimensions);
                    return {};
                });
            !result) {
        started_ = false;
        return std::unexpected(result.error());
    }

    // Every frame repaints onto a just-cleared buffer, so a restart never
    // diffs against rows the previous session left behind. A baseline imported
    // through restore_render_state() is the #985 transfer case: the terminal
    // already shows those rows, so it survives start().
    if (baseline_from_restore_) {
        baseline_from_restore_ = false;
    } else {
        first_render_ = true;
        previous_dimensions_.reset();
        previous_lines_.clear();
        cursor_.reset();
        cursor_visible_ = false;
    }

    if (auto entered = terminal_.set_alternate_screen(true); !entered) {
        started_ = false;
        if (auto stopped = terminal_.stop(); !stopped) {
            return std::unexpected(stopped.error());
        }
        return std::unexpected(entered.error());
    }
    if (auto autowrap = terminal_.set_autowrap(false); !autowrap) {
        started_ = false;
        (void)terminal_.set_alternate_screen(false);
        if (auto stopped = terminal_.stop(); !stopped) {
            return std::unexpected(stopped.error());
        }
        return std::unexpected(autowrap.error());
    }
    if (auto cursor = terminal_.set_cursor_visible(false); !cursor) {
        started_ = false;
        (void)terminal_.set_autowrap(true);
        (void)terminal_.set_alternate_screen(false);
        if (auto stopped = terminal_.stop(); !stopped) {
            return std::unexpected(stopped.error());
        }
        return std::unexpected(cursor.error());
    }
    cursor_visible_ = false;

    pending_render_ = false;
    flashes_.set_scheduler(scheduler_);
    if (color_scheme_notifications_enabled_) {
        (void)terminal_.write("\x1b[?2031h");
    }
    // The fullscreen root owns pointer reporting for its lifetime: SGR reports
    // arrive with the coordinates and button bits the router decodes, and focus
    // reporting is what lets the terminal tell this root that a gesture left the
    // window (pi `tui-alt-screen.ts:363-378`).
    (void)terminal_.write(mouse_all_motion_enabled() ? kEnableMouseAllMotion : kEnableMouseButtonMotion);
    request_render();
    return {};
}

support::ExpectedVoid AltScreen::stop(TuiStopOptions options) {
    if (!started_ || stopping_) return {};
    cancel_scheduled_render();
    ++render_generation_;
    render_scheduled_ = false;
    flashes_.dispose();
    stop_selection_auto_scroll();
    ChildOperation operation(*this);
    stopping_ = true;
    started_ = false;
    apply_focus(nullptr);
    detail::TuiHost::overlay_focus_restore_ = {};
    // A gesture never survives the session that captured it, and neither does
    // the scrollbar hover (pi `beforeTerminalStop`).
    clear_mouse_gesture();
    if (selection_press_active_) clear_text_selection();
    pressed_url_.reset();
    last_mouse_click_.reset();
    clear_selection_click_history();
    stop_scrollbar_drag();
    stop_scrollbar_hover();

    support::ExpectedVoid first_error;
    if (color_scheme_notifications_enabled_) {
        retain_error(first_error, terminal_.write("\x1b[?2031l"));
    }
    if (options.preserve_screen) {
        retain_error(first_error, terminal_.begin_synchronized_update());
        if (session_image_protocol_ == InlineImageProtocol::Kitty) {
            retain_error(first_error, terminal_.write(delete_all_kitty_images()));
        }
        image_pipeline_->clear_image_placements();
        retain_error(first_error, terminal_.write(kDisableMouse));
        retain_error(first_error, terminal_.set_autowrap(true));
        retain_error(first_error, terminal_.end_synchronized_update());

        cancel_color_queries(options.preserve_pending_color_query_completion);

        retain_error(first_error, terminal_.set_cursor_visible(true));
        reset_decoder();
        pending_render_ = false;
        auto stop_result = terminal_.stop(TerminalStopOptions{.final_alternate_restore = true});
        if (!stop_result) {
            retain_error(first_error, std::move(stop_result));
            retain_error(first_error, terminal_.stop());
        }
        stopping_ = false;
        return first_error;
    }

    // Frozen `beforeTerminalStop`: a Kitty session drops every uploaded image
    // before the alternate buffer is left, and forgets the placements with it.
    if (session_image_protocol_ == InlineImageProtocol::Kitty) {
        retain_error(first_error, terminal_.write(delete_all_kitty_images()));
    }
    image_pipeline_->clear_image_placements();
    // Give pointer reporting back to the terminal before the alternate buffer
    // is left (pi `beforeTerminalStop`).
    retain_error(first_error, terminal_.write(kDisableMouse));

    cancel_color_queries(options.preserve_pending_color_query_completion);

    // Restore the mode stack in reverse order of entry.
    retain_error(first_error, terminal_.set_cursor_visible(true));
    retain_error(first_error, terminal_.set_autowrap(true));
    retain_error(first_error, terminal_.set_alternate_screen(false));
    reset_decoder();
    pending_render_ = false;
    retain_error(first_error, terminal_.stop());
    stopping_ = false;
    return first_error;
}

support::ExpectedVoid AltScreen::render() { return render_now(); }

support::ExpectedVoid AltScreen::render_final_presentation() {
    return std::unexpected(
            support::make_error(support::ErrorCode::Validation, "AltScreen does not support final presentation"));
}

void AltScreen::toggle_search() {
    if (active_search_) {
        close_search();
        return;
    }
    auto component = std::make_unique<AltScreenSearchComponent>(
            [this](std::string query) { update_search_query(std::move(query)); },
            *default_tui_keybindings(),
            keybinding_context_,
            [this](std::string text, bool hovered) {
                return search_navigation_button_style_(std::move(text), hovered);
            });
    auto* component_pointer = component.get();
    auto overlay = std::make_unique<Overlay>(OverlayOptions{
            .width = SizeValue::percent(40),
            .min_width = 32,
            .anchor = OverlayAnchor::TopRight,
            .margins = OverlayMargins{.left = 1, .right = 1, .top = 1, .bottom = 1},
    });
    auto added = overlay->add_child(std::move(component));
    if (!added) return;
    auto shown = show_overlay(std::move(overlay));
    if (!shown) return;
    active_search_ = ActiveSearch{
            .component = component_pointer,
            .index = std::make_unique<SearchIndex>(),
            .overlay = std::move(*shown),
            .query = {},
            .matches = {},
            .selected_index = -1,
            .selected_key = std::nullopt,
            .anchor_row = primary_scroll_view().scroll_top(),
            .selection_mode = ActiveSearch::SelectionMode::Query,
    };
    request_render();
}

void AltScreen::close_search() {
    if (!active_search_) return;
    auto overlay = std::move(active_search_->overlay);
    active_search_.reset();
    if (overlay) overlay->hide();
    request_render();
}

void AltScreen::update_search_query(std::string query) {
    if (!active_search_ || query == active_search_->query) return;
    auto& search = *active_search_;
    if (search.selected_index >= 0 && static_cast<std::size_t>(search.selected_index) < search.matches.size())
        search.anchor_row = search.matches[static_cast<std::size_t>(search.selected_index)].segments.front().row;
    else
        search.anchor_row = primary_scroll_view().scroll_top();
    search.query = std::move(query);
    search.selection_mode = ActiveSearch::SelectionMode::Query;
    search.component->set_result(-1, 0);
    request_render();
}

void AltScreen::navigate_search(int direction) {
    if (!active_search_ || active_search_->query.empty()) return;
    active_search_->selection_mode =
            direction < 0 ? ActiveSearch::SelectionMode::Previous : ActiveSearch::SelectionMode::Next;
    request_render();
}

bool AltScreen::refresh_search(const detail::LayoutFrame& frame) {
    if (!active_search_) return false;
    auto& search = *active_search_;
    auto* scroll_view = frame.primary_scroll_view != nullptr ? frame.primary_scroll_view : implicit_scroll_view_.get();
    const auto* box = detail::get_scroll_view_box(frame, scroll_view);
    if (!box || search.query.empty()) {
        search.matches.clear();
        search.selected_index = -1;
        search.selected_key.reset();
        search.selection_mode = ActiveSearch::SelectionMode::Retain;
        search.component->set_result(-1, 0);
        return false;
    }

    const bool should_reveal = search.selection_mode != ActiveSearch::SelectionMode::Retain;
    const auto result = search.index->search(box->scroll_content_lines, search.query);
    search.matches = result.matches;
    if (!result.changed && search.selection_mode == ActiveSearch::SelectionMode::Retain) return false;

    std::ptrdiff_t exact_index = -1;
    if (search.selected_key) {
        const auto found =
                std::find_if(search.matches.begin(), search.matches.end(), [&search](const SearchMatch& match) {
                    return alt_screen_search_match_key(match) == *search.selected_key;
                });
        if (found != search.matches.end()) exact_index = std::distance(search.matches.begin(), found);
    }
    std::ptrdiff_t selected_index = -1;
    if (!search.matches.empty()) {
        switch (search.selection_mode) {
        case ActiveSearch::SelectionMode::Query: {
            const auto found = std::lower_bound(search.matches.begin(),
                    search.matches.end(),
                    search.anchor_row,
                    [](const SearchMatch& match, std::size_t row) { return match.segments.front().row < row; });
            selected_index = found == search.matches.end() ? 0 : std::distance(search.matches.begin(), found);
            break;
        }
        case ActiveSearch::SelectionMode::Next: {
            const auto base = exact_index >= 0 ? exact_index
                                               : std::min(search.selected_index,
                                                         static_cast<std::ptrdiff_t>(search.matches.size()) - 1);
            selected_index = base < 0 ? 0 : (base + 1) % static_cast<std::ptrdiff_t>(search.matches.size());
            break;
        }
        case ActiveSearch::SelectionMode::Previous: {
            const auto base = exact_index >= 0 ? exact_index
                                               : std::min(search.selected_index,
                                                         static_cast<std::ptrdiff_t>(search.matches.size()) - 1);
            selected_index = base < 0 ? static_cast<std::ptrdiff_t>(search.matches.size()) - 1
                                      : (base - 1 + static_cast<std::ptrdiff_t>(search.matches.size())) %
                                                static_cast<std::ptrdiff_t>(search.matches.size());
            break;
        }
        case ActiveSearch::SelectionMode::Retain:
            selected_index = exact_index >= 0 ? exact_index
                                              : std::clamp<std::ptrdiff_t>(search.selected_index,
                                                        0,
                                                        static_cast<std::ptrdiff_t>(search.matches.size()) - 1);
            break;
        }
    }

    search.selected_index = selected_index;
    search.selected_key = selected_index >= 0 ? std::optional<std::string>{alt_screen_search_match_key(
                                                        search.matches[static_cast<std::size_t>(selected_index)])}
                                              : std::nullopt;
    search.selection_mode = ActiveSearch::SelectionMode::Retain;
    search.component->set_result(selected_index, search.matches.size());
    if (!should_reveal || selected_index < 0 || scroll_view->viewport_height() == 0) return false;
    const auto& selected = search.matches[static_cast<std::size_t>(selected_index)];
    const auto before = scroll_view->scroll_top();
    const auto visible_bottom = before + scroll_view->viewport_height() - 1;
    auto target = static_cast<std::ptrdiff_t>(before);
    if (selected.segments.front().row < before || selected.segments.back().row > visible_bottom) {
        target = static_cast<std::ptrdiff_t>(selected.segments.front().row) -
                 static_cast<std::ptrdiff_t>(scroll_view->viewport_height() / 3);
    }
    scroll_view->scroll_to(target, true);
    return scroll_view->scroll_top() != before;
}

std::vector<std::string> AltScreen::apply_search_highlights(
        const std::vector<std::string>& screen, const detail::LayoutFrame& frame) const {
    if (!active_search_ || active_search_->selected_index < 0 || active_search_->matches.empty()) return screen;
    auto* scroll_view = frame.primary_scroll_view != nullptr ? frame.primary_scroll_view : implicit_scroll_view_.get();
    const auto* box = detail::get_scroll_view_box(frame, scroll_view);
    if (!box) return screen;

    std::vector<std::vector<SearchHighlightRange>> ranges(screen.size());
    const auto scrollbar = detail::get_scrollbar_geometry(*box);
    const auto min_row = std::max<std::ptrdiff_t>({0, box->rect.y, box->clip.y});
    const auto max_row = std::min<std::ptrdiff_t>({static_cast<std::ptrdiff_t>(screen.size()),
            box->rect.y + static_cast<std::ptrdiff_t>(box->rect.height),
            box->clip.y + static_cast<std::ptrdiff_t>(box->clip.height)});
    const auto min_column = std::max<std::ptrdiff_t>({0, box->rect.x, box->clip.x});
    const auto max_column = std::min<std::ptrdiff_t>({static_cast<std::ptrdiff_t>(terminal_.dimensions().columns),
            box->rect.x + static_cast<std::ptrdiff_t>(box->rect.width),
            box->clip.x + static_cast<std::ptrdiff_t>(box->clip.width),
            scrollbar ? scrollbar->column : std::numeric_limits<std::ptrdiff_t>::max()});
    const auto min_content_row = scroll_view->scroll_top() + static_cast<std::size_t>(min_row - box->rect.y);
    const auto max_content_row = scroll_view->scroll_top() + static_cast<std::size_t>(max_row - box->rect.y - 1);
    auto first_match = std::lower_bound(active_search_->matches.begin(),
            active_search_->matches.end(),
            min_content_row,
            [](const SearchMatch& match, std::size_t row) { return match.segments.back().row < row; });
    for (auto iterator = first_match; iterator != active_search_->matches.end(); ++iterator) {
        const auto match_index = static_cast<std::ptrdiff_t>(std::distance(active_search_->matches.begin(), iterator));
        if (iterator->segments.front().row > max_content_row) break;
        for (const auto& segment : iterator->segments) {
            const auto row = box->rect.y + static_cast<std::ptrdiff_t>(segment.row) -
                             static_cast<std::ptrdiff_t>(scroll_view->scroll_top());
            if (row < min_row || row >= max_row || row < 0 || static_cast<std::size_t>(row) >= ranges.size()) continue;
            const auto start_col = std::max(min_column, box->rect.x + static_cast<std::ptrdiff_t>(segment.start_col));
            const auto end_col = std::min(max_column, box->rect.x + static_cast<std::ptrdiff_t>(segment.end_col));
            if (end_col <= start_col) continue;
            ranges[static_cast<std::size_t>(row)].push_back(SearchHighlightRange{
                    .start_col = static_cast<std::size_t>(start_col),
                    .end_col = static_cast<std::size_t>(end_col),
                    .current = match_index == active_search_->selected_index,
            });
        }
    }

    auto result = screen;
    for (std::size_t row = 0; row < ranges.size(); ++row) {
        if (ranges[row].empty() || is_image_line(result[row])) continue;
        std::sort(ranges[row].begin(), ranges[row].end(), [](const auto& left, const auto& right) {
            return left.start_col > right.start_col;
        });
        auto line = result[row];
        const auto line_width = visible_width(line);
        for (const auto& range : ranges[row]) {
            const auto start = std::min(range.start_col, line_width);
            const auto end = std::min(range.end_col, line_width);
            if (end <= start) continue;
            auto before = slice_by_column(line, 0, start, true);
            auto highlighted = slice_by_column(line, start, end - start, true);
            auto after = slice_by_column(line, end, line_width - end, true);
            if (!before || !highlighted || !after) continue;
            auto& style = range.current ? search_current_match_style_ : search_match_style_;
            std::string styled;
            std::size_t plain_start = 0;
            for (std::size_t position = 0; position < highlighted->size();) {
                const auto ansi = detail::extract_ansi_code(*highlighted, position);
                if (!ansi) {
                    const auto [codepoint, size] = detail::decode_utf8(*highlighted, position);
                    (void)codepoint;
                    position += size == 0 ? 1 : size;
                    continue;
                }
                if (position > plain_start) styled += style(highlighted->substr(plain_start, position - plain_start));
                styled += ansi->code;
                position += ansi->length;
                plain_start = position;
            }
            if (plain_start < highlighted->size()) styled += style(highlighted->substr(plain_start));
            line = *before + styled + *after;
        }
        result[row] = std::move(line);
    }
    return result;
}

std::optional<int> AltScreen::search_navigation_direction_at(std::ptrdiff_t x, std::ptrdiff_t y) const {
    if (!active_search_ || !active_search_->overlay) return std::nullopt;
    const auto bounds = active_search_->overlay->get_bounds();
    if (!bounds || x < static_cast<std::ptrdiff_t>(bounds->col) ||
            x >= static_cast<std::ptrdiff_t>(bounds->col + bounds->width) ||
            y < static_cast<std::ptrdiff_t>(bounds->row) ||
            y >= static_cast<std::ptrdiff_t>(bounds->row + bounds->height))
        return std::nullopt;
    return active_search_->component->navigation_direction_at(
            static_cast<std::size_t>(y - static_cast<std::ptrdiff_t>(bounds->row)),
            static_cast<std::size_t>(x - static_cast<std::ptrdiff_t>(bounds->col)));
}

bool AltScreen::handle_search_mouse_report(const detail::MousePacket& packet) {
    if (!active_search_) return false;
    const auto direction = search_navigation_direction_at(packet.x, packet.y);
    if (active_search_->component->set_hovered_navigation_direction(direction)) request_render();
    if (direction && !packet.release && packet.kind == detail::MousePacket::Kind::Pointer &&
            (packet.button & 32) == 0 && (packet.button & 3) == 0) {
        navigate_search(*direction);
        return true;
    }
    return false;
}

bool AltScreen::should_defer_viewport_input_to_overlay() const {
    return dynamic_cast<Overlay*>(focused_) != nullptr && !search_overlay_focused();
}

bool AltScreen::search_overlay_focused() const {
    return active_search_ && active_search_->overlay && active_search_->overlay->is_focused();
}

support::ExpectedVoid AltScreen::render_now() {
    if (!started_) {
        return std::unexpected(
                support::make_error(support::ErrorCode::Validation, "TUI must be started before rendering"));
    }

    cancel_scheduled_render();
    ++render_generation_;
    render_scheduled_ = false;

    if (render_prepare_sink_) {
        auto prepared = render_prepare_sink_();
        if (!prepared) {
            support::ExpectedVoid result = std::unexpected(prepared.error());
            if (render_complete_sink_) render_complete_sink_(result);
            return result;
        }
        if (!*prepared) return {};
    }

    auto result = render_frame();
    if (result) pending_render_ = false;
    if (render_complete_sink_) render_complete_sink_(result);
    if (!result && result.error().code == support::ErrorCode::Busy) request_render();
    return result;
}

support::ExpectedVoid AltScreen::render_frame() {
    ChildOperation operation(*this);
    const auto dimensions = terminal_.dimensions();
    reconcile_overlay_focus(dimensions);
    if (dimensions.columns == 0 || dimensions.rows == 0) {
        return std::unexpected(
                support::make_error(support::ErrorCode::Validation, "TUI requires positive terminal dimensions"));
    }

    std::vector<std::string> lines;
    std::optional<CursorPosition> cursor;
    // One constrained composition either way (pi `doRender`): an installed
    // layout root fills the viewport, and the implicit viewport shows the
    // mounted children as one document. `set_available_height` is not pushed
    // into a layout child; its allocation is the bound.
    Component& root = layout_root_ ? *layout_root_ : *implicit_scroll_view_;
    auto frame = detail::LayoutEngine::render(root, dimensions.columns, dimensions.rows, [this] { request_render(); });
    if (!frame) return std::unexpected(frame.error());
    if (refresh_search(*frame)) {
        frame = detail::LayoutEngine::render(root, dimensions.columns, dimensions.rows, [this] { request_render(); });
        if (!frame) return std::unexpected(frame.error());
    }
    layout_primary_scroll_view_ = frame->primary_scroll_view;
    // A mode change can hide the captured bar without another pointer report.
    // Validate against the new frame before retaining the active drag.
    cached_frame_ = std::make_unique<detail::LayoutFrame>(std::move(*frame));
    validate_scrollbar_interaction(*cached_frame_);
    // The frame outlives the paint: it is the geometry every later hit test runs
    // against, so a pointer event never renders to find out where a component is
    // (pi `currentLayout`).
    // Frame image placements are emitted below (#1012): the row buffer keeps
    // the image sidecars so the fullscreen image stage owns crop, reuse and
    // cleanup.
    lines = std::move(cached_frame_->lines);
    cursor = cached_frame_->cursor;
    std::vector<InlineImageRenderRegion> images = std::move(cached_frame_->images);
    // The fake-cursor markers resolve before the search highlighting and the
    // overlay compositing, because both track SGR codes rather than the
    // zero-width marker bytes (pi `tui-alt-screen.ts` `resolveFakeCursors`
    // runs before `applySearchHighlights` and `compositeOverlays`).
    lines = resolve_fake_cursors(lines, show_hardware_cursor_);
    lines = apply_search_highlights(lines, *cached_frame_);
    auto display_lines = strip_osc133_zone_prefixes(lines);
    if (auto indicator = composite_scroll_to_end_indicator(display_lines); !indicator) return indicator;
    const bool compose_overlays = compositor_->has_visible_overlays(dimensions);
    if (compose_overlays) {
        RenderResult output{.lines = std::move(display_lines), .images = std::move(images)};
        auto materialized = detail::OverlayCompositor::materialize_images(
                std::move(output), terminal_.capabilities(), dimensions.columns, dimensions.rows);
        if (auto overlaid = compositor_->composite(
                    dimensions, terminal_.capabilities(), materialized, show_hardware_cursor_);
                !overlaid)
            return overlaid;
        display_lines = std::move(materialized.lines);
        images = std::move(materialized.images);
        if (materialized.cursor) cursor = materialized.cursor;
    }
    if (auto flashed = composite_flashes(display_lines, dimensions.columns, dimensions.rows); !flashed) return flashed;
    // The cursor position the marker carried is the terminal cursor position
    // (pi `extractCursorPosition`), measured on the text before the marker
    // and removed from the emitted line.
    const auto marker_cursor = extract_cursor_position(display_lines, dimensions.rows);
    if (marker_cursor) cursor = marker_cursor;
    return render_viewport(std::move(display_lines), cursor, dimensions, std::move(images));
}

support::Expected<RenderResult> AltScreen::render_implicit_document(std::size_t width) {
    const auto rows = terminal_.dimensions().rows;
    RenderResult output;
    std::size_t row_offset = 0;
    std::vector<Component*> children;
    children.reserve(children_.size());
    for (const auto& child : children_)
        children.push_back(child.get());
    // The row each child occupies decides which child owns a pointer event, so
    // the composition that produced the last frame is the one a hit test reads
    // (pi `implicitDocument.mouseLayout`, `tui-alt-screen.ts:260-267`).
    std::vector<std::pair<Component*, std::size_t>> document_rows;
    document_rows.reserve(children.size());
    for (auto* child : children) {
        if (!owns(child)) continue;
        if (auto* viewport_aware = dynamic_cast<ViewportAware*>(child)) {
            viewport_aware->set_available_height(rows);
        }
        if (!owns(child)) continue;
        auto rendered = child->render(width);
        if (!rendered) return std::unexpected(rendered.error());
        if (!owns(child)) continue;
        const auto child_row_offset = row_offset;
        row_offset += rendered->lines.size();
        document_rows.emplace_back(child, rendered->lines.size());
        for (auto& image : rendered->images) {
            image.region.row += child_row_offset;
            output.images.push_back(std::move(image));
        }
        for (auto& line : rendered->lines)
            output.lines.push_back(std::move(line));
        if (!output.cursor) {
            if (const auto location = child->cursor_location()) {
                output.cursor = CursorPosition{.column = location->column, .row = child_row_offset + location->row};
            }
        }
    }
    implicit_document_rows_ = std::move(document_rows);

    if (!output.cursor) {
        auto* focusable = dynamic_cast<Focusable*>(focused_);
        if (focusable != nullptr && focusable->focused()) output.cursor = focusable->cursor_location();
    }
    return output;
}

ScrollView& AltScreen::primary_scroll_view() noexcept {
    return layout_primary_scroll_view_ != nullptr ? *layout_primary_scroll_view_ : *implicit_scroll_view_;
}

const ScrollView& AltScreen::primary_scroll_view() const noexcept {
    return layout_primary_scroll_view_ != nullptr ? *layout_primary_scroll_view_ : *implicit_scroll_view_;
}

std::size_t AltScreen::viewport_top() const { return primary_scroll_view().scroll_top(); }

bool AltScreen::is_following_output() const { return primary_scroll_view().following_end(); }

bool AltScreen::has_active_selection() const { return active_selection_text().has_value(); }

void AltScreen::update_selection_auto_scroll(const detail::MousePacket& packet) {
    auto* scroll_view = selection_anchor_ ? selection_anchor_->scroll_view : nullptr;
    const auto* box = scroll_view != nullptr && cached_frame_ ? detail::get_scroll_view_box(*cached_frame_, scroll_view)
                                                              : nullptr;
    if (box == nullptr || box->rect.height == 0 || box->clip.height == 0) {
        stop_selection_auto_scroll();
        return;
    }
    const auto visible_top = std::max<std::ptrdiff_t>({0, box->rect.y, box->clip.y});
    const auto visible_bottom = std::min<std::ptrdiff_t>({static_cast<std::ptrdiff_t>(terminal_.dimensions().rows) - 1,
            box->rect.y + static_cast<std::ptrdiff_t>(box->rect.height) - 1,
            box->clip.y + static_cast<std::ptrdiff_t>(box->clip.height) - 1});
    selection_drag_pointer_ = std::pair{packet.x, packet.y};
    selection_auto_scroll_direction_ = packet.y <= visible_top ? -1 : packet.y >= visible_bottom ? 1 : 0;
    if (selection_auto_scroll_direction_ == 0) {
        stop_selection_auto_scroll();
        return;
    }
    if (selection_auto_scroll_armed_) return;
    // Without a scheduler the interval cannot run, matching how
    // `schedule_render` no-ops: the arm is skipped silently.
    if (scheduler_ == nullptr) return;
    selection_auto_scroll_armed_ = true;
    ++selection_auto_scroll_generation_;
    const auto generation = selection_auto_scroll_generation_;
    const std::weak_ptr<RenderScheduleState> weak_state = render_schedule_state_;
    scheduler_->schedule_interval(kSelectionAutoScrollInterval, [weak_state, generation] {
        if (const auto state = weak_state.lock(); state && state->owner != nullptr)
            state->owner->execute_auto_scroll_tick(generation);
    });
}

void AltScreen::execute_auto_scroll_tick(std::size_t generation) {
    if (!selection_auto_scroll_armed_ || generation != selection_auto_scroll_generation_) return;
    auto_scroll_selection();
}

void AltScreen::auto_scroll_selection() {
    auto* scroll_view = selection_anchor_ ? selection_anchor_->scroll_view : nullptr;
    const auto pointer = selection_drag_pointer_;
    const auto direction = selection_auto_scroll_direction_;
    if (scroll_view == nullptr || !pointer || direction == 0) {
        stop_selection_auto_scroll();
        return;
    }
    // `scroll_by` returns the lines it could not consume (pi `scrollBy`,
    // ScrollView.cpp). A following viewport scrolls from the content end, so
    // the first upward step consumes one line and reports `direction - 1`;
    // nothing moved reports exactly `direction` (frozen `remaining ===
    // direction`, tui-alt-screen.ts:1295) and stops the interval.
    const auto remaining = scroll_view->scroll_by(direction);
    if (remaining == direction) {
        stop_selection_auto_scroll();
        return;
    }
    if (auto point = selection_point_at(pointer->first, pointer->second, scroll_view)) update_selection_focus(*point);
    request_render();
}

void AltScreen::stop_selection_auto_scroll() noexcept {
    if (selection_auto_scroll_armed_) {
        selection_auto_scroll_armed_ = false;
        if (scheduler_ != nullptr) scheduler_->cancel_interval();
    }
    selection_drag_pointer_.reset();
    selection_auto_scroll_direction_ = 0;
    ++selection_auto_scroll_generation_;
}

void AltScreen::clear_text_selection() noexcept {
    stop_selection_auto_scroll();
    pressed_url_.reset();
    selection_press_active_ = false;
    selection_dragged_ = false;
    selection_anchor_.reset();
    selection_focus_.reset();
    selection_granularity_ = SelectionGranularity::Character;
    selection_initial_range_.reset();
}

void AltScreen::reset_text_selection() noexcept {
    clear_text_selection();
    // pi `resetTextSelection` also drops `lastClick`, so the next press is a
    // character click again instead of continuing the word/line cycle.
    clear_selection_click_history();
}

std::optional<std::string_view> AltScreen::selection_source_line_at(const SelectionPoint& point) const {
    if (point.scroll_view != nullptr && cached_frame_ != nullptr) {
        const auto* box = detail::get_scroll_view_box(*cached_frame_, point.scroll_view);
        if (box != nullptr && box->has_scroll_content_lines && !box->scroll_content_lines.empty()) {
            if (point.row < 0 || point.row >= static_cast<std::ptrdiff_t>(box->scroll_content_lines.size()))
                return std::nullopt;
            return std::string_view{box->scroll_content_lines[static_cast<std::size_t>(point.row)]};
        }
    }
    // pi falls back to the fullscreen rows (its previousScreen) when a scroll
    // view has no captured content lines yet.
    if (point.row < 0 || point.row >= static_cast<std::ptrdiff_t>(selection_source_lines_.size())) return std::nullopt;
    return std::string_view{selection_source_lines_[static_cast<std::size_t>(point.row)]};
}

std::string_view AltScreen::selection_source_line(const SelectionPoint& point) const {
    if (point.scroll_view != nullptr && cached_frame_ != nullptr) {
        const auto* box = detail::get_scroll_view_box(*cached_frame_, point.scroll_view);
        if (box != nullptr && box->has_scroll_content_lines && !box->scroll_content_lines.empty()) {
            const auto row = std::clamp<std::ptrdiff_t>(
                    point.row, 0, static_cast<std::ptrdiff_t>(box->scroll_content_lines.size() - 1));
            return box->scroll_content_lines[static_cast<std::size_t>(row)];
        }
    }
    if (point.row < 0 || static_cast<std::size_t>(point.row) >= selection_source_lines_.size()) return {};
    return selection_source_lines_[static_cast<std::size_t>(point.row)];
}

std::optional<AltScreen::SelectionRange> AltScreen::word_selection_at(const SelectionPoint& point) const {
    const auto line = selection_source_line_at(point);
    if (!line || line->empty()) return std::nullopt;
    const auto segments = selection_word_segments(strip_terminal_sequences(*line));
    if (!segments) return std::nullopt;
    const auto clicked =
            std::find_if(segments->begin(), segments->end(), [&point](const SelectionWordSegment& segment) {
                return point.column >= segment.start && point.column < segment.end;
            });
    if (clicked == segments->end()) return std::nullopt;
    const auto can_join = [&segments](const std::size_t a, const std::size_t b) {
        // pi `canJoin`: selectability gates joining and a joiner on either
        // side chains the segments; unlike words, punctuation does not stop a
        // joiner chain (`-a` selects with the `-`).
        return (*segments)[a].selectable() && (*segments)[b].selectable() &&
               ((*segments)[a].joiner || (*segments)[b].joiner);
    };
    auto start = clicked->start;
    auto end = clicked->end;
    std::size_t start_index = static_cast<std::size_t>(clicked - segments->begin());
    std::size_t end_index = start_index;
    for (std::ptrdiff_t index = static_cast<std::ptrdiff_t>(start_index) - 1; index >= 0; --index) {
        const auto current = static_cast<std::size_t>(index);
        if (!can_join(current, start_index)) break;
        start = (*segments)[current].start;
        start_index = current;
    }
    for (std::size_t index = end_index + 1; index < segments->size(); ++index) {
        if (!can_join(end_index, index)) break;
        end = (*segments)[index].end;
        end_index = index;
    }
    return SelectionRange{.start = SelectionPoint{.row = point.row, .column = start, .scroll_view = point.scroll_view},
            .end = SelectionPoint{.row = point.row, .column = end, .scroll_view = point.scroll_view, .boundary = true}};
}

AltScreen::SelectionRange AltScreen::line_selection_at(const SelectionPoint& point) const {
    // Infallible like pi `getLineSelection`: the clamped source lookup yields
    // "" outside the content, so the range is zero-width there and still
    // carries line granularity and the initial range. Count 3 is only
    // reachable where a word existed at press, so the range is effectively
    // never zero-width in practice (pi:1213-1219, 1233-1256).
    const auto width =
            static_cast<std::ptrdiff_t>(visible_width(strip_terminal_sequences(selection_source_line(point))));
    return SelectionRange{.start = SelectionPoint{.row = point.row, .column = 0, .scroll_view = point.scroll_view},
            .end = SelectionPoint{
                    .row = point.row, .column = width, .scroll_view = point.scroll_view, .boundary = true}};
}

std::size_t AltScreen::next_selection_click_count(
        const SelectionPoint& point, const std::optional<SelectionRange>& word) {
    const auto now = selection_click_clock_();
    auto count = std::size_t{1};
    if (word && last_word_click_ && now - last_word_click_->at <= kSelectionDoubleClickInterval &&
            last_word_click_->scroll_view == point.scroll_view && last_word_click_->row == point.row &&
            last_word_click_->word_start == word->start.column && last_word_click_->word_end == word->end.column) {
        // pi `(previous.count % 3) + 1`: the fourth consecutive press reports
        // as a fresh character click and re-anchors the cycle.
        count = (last_word_click_->count % 3) + 1;
    }
    // pi keeps `lastClick` only for word presses; a press past the line
    // content resets the cycle.
    if (word) {
        last_word_click_ = SelectionWordClick{.at = now,
                .count = count,
                .row = point.row,
                .scroll_view = point.scroll_view,
                .word_start = word->start.column,
                .word_end = word->end.column};
    } else {
        last_word_click_.reset();
    }
    return count;
}

void AltScreen::update_selection_focus(const SelectionPoint& point) {
    if (!selection_anchor_) return;
    // Granularity Character is the only focus that follows the pointer
    // directly. Word and line both resolve through their range like pi
    // `updateSelectionFocus`, which never takes the character-focus branch
    // for them (pi:1253-1295).
    if (selection_granularity_ == SelectionGranularity::Character) {
        const auto& anchor = *selection_anchor_;
        selection_focus_ = (point.row == anchor.row && point.column == anchor.column) ? anchor : point;
        return;
    }
    if (!selection_initial_range_) return;
    // pi keeps word/line granularity even where the range lookup misses and
    // expands through the initial range instead; count 2/3 was only reachable
    // with an initial range, so it is always present here (pi:1257-1270).
    const auto range =
            selection_granularity_ == SelectionGranularity::Line ? line_selection_at(point) : word_selection_at(point);
    if (!range) return;
    const auto& initial = *selection_initial_range_;
    const auto before_initial = range->start.row < initial.start.row ||
                                (range->start.row == initial.start.row && range->start.column < initial.start.column);
    if (before_initial) {
        selection_anchor_ = initial.end;
        selection_focus_ = range->start;
    } else {
        selection_anchor_ = initial.start;
        selection_focus_ = range->end;
    }
}

std::optional<AltScreen::SelectionPoint> AltScreen::selection_point_at(
        std::ptrdiff_t x, std::ptrdiff_t y, ScrollView* scroll_view) const {
    if (scroll_view == nullptr && cached_frame_) {
        const auto candidates = detail::scroll_views_at(*cached_frame_, x, y);
        if (!candidates.empty()) scroll_view = candidates.front();
    }
    if (scroll_view != nullptr && cached_frame_ != nullptr) {
        const auto* box = detail::get_scroll_view_box(*cached_frame_, scroll_view);
        // A degenerate scroll-view box clamps to row 0 column 0 like pi's
        // content-less scroll view (the probe test harness) instead of
        // suppressing the point (pi:347-352).
        if (box == nullptr) return std::nullopt;
        if (box->rect.height == 0 || box->clip.height == 0)
            return SelectionPoint{.row = 0, .column = 0, .scroll_view = scroll_view};
        const auto visible_top = std::max<std::ptrdiff_t>({0, box->rect.y, box->clip.y});
        const auto visible_bottom =
                std::min<std::ptrdiff_t>({static_cast<std::ptrdiff_t>(terminal_.dimensions().rows) - 1,
                        box->rect.y + static_cast<std::ptrdiff_t>(box->rect.height) - 1,
                        box->clip.y + static_cast<std::ptrdiff_t>(box->clip.height) - 1});
        if (visible_bottom < visible_top) return std::nullopt;
        const auto pointer_row = std::clamp(y, visible_top, visible_bottom);
        const auto content_height = scroll_view->content_height();
        const auto row = std::clamp<std::ptrdiff_t>(
                static_cast<std::ptrdiff_t>(scroll_view->scroll_top()) + pointer_row - box->rect.y,
                0,
                content_height == 0 ? 0 : static_cast<std::ptrdiff_t>(content_height - 1));
        const auto column = std::clamp<std::ptrdiff_t>(
                x - box->rect.x, 0, box->rect.width == 0 ? 0 : static_cast<std::ptrdiff_t>(box->rect.width - 1));
        return SelectionPoint{.row = row, .column = column, .scroll_view = scroll_view};
    }
    const auto dimensions = terminal_.dimensions();
    if (dimensions.rows == 0 || dimensions.columns == 0) return std::nullopt;
    return SelectionPoint{.row = std::clamp<std::ptrdiff_t>(y, 0, static_cast<std::ptrdiff_t>(dimensions.rows - 1)),
            .column = std::clamp<std::ptrdiff_t>(x, 0, static_cast<std::ptrdiff_t>(dimensions.columns - 1)),
            .scroll_view = nullptr};
}

std::pair<std::ptrdiff_t, std::ptrdiff_t> AltScreen::selection_columns(
        std::string_view line, std::ptrdiff_t row, const SelectionPoint& start, const SelectionPoint& end) const {
    const auto width = static_cast<std::ptrdiff_t>(visible_width(line));
    auto first = std::ptrdiff_t{0};
    auto last = width;
    if (row == start.row) {
        const auto [cell_start, cell_end] =
                grapheme_cell_range(line, static_cast<std::size_t>(std::max<std::ptrdiff_t>(0, start.column)));
        (void)cell_end;
        first = std::min<std::ptrdiff_t>(static_cast<std::ptrdiff_t>(cell_start), width);
    }
    if (row == end.row) {
        if (end.boundary) {
            last = std::min(end.column, width);
        } else {
            const auto [cell_start, cell_end] =
                    grapheme_cell_range(line, static_cast<std::size_t>(std::max<std::ptrdiff_t>(0, end.column)));
            (void)cell_start;
            last = std::min<std::ptrdiff_t>(static_cast<std::ptrdiff_t>(cell_end), width);
        }
    }
    return {first, std::max(first, last)};
}

bool AltScreen::selection_anchor_precedes_focus() const noexcept {
    if (!selection_anchor_ || !selection_focus_) return false;
    const auto& anchor = *selection_anchor_;
    const auto& focus = *selection_focus_;
    return anchor.row < focus.row || (anchor.row == focus.row && anchor.column < focus.column);
}

std::optional<std::string> AltScreen::active_selection_text() const {
    if (!selection_anchor_ || !selection_focus_ || selection_anchor_->scroll_view != selection_focus_->scroll_view)
        return std::nullopt;
    const auto& anchor = *selection_anchor_;
    const auto& focus = *selection_focus_;
    if (anchor.row == focus.row && anchor.column == focus.column) return std::nullopt;
    const auto& start = selection_anchor_precedes_focus() ? anchor : focus;
    const auto& end = selection_anchor_precedes_focus() ? focus : anchor;
    const auto first_row = std::min(start.row, end.row);
    const auto last_row = std::max(start.row, end.row);
    std::vector<std::string> selected;
    for (auto row = first_row; row <= last_row; ++row) {
        auto point = start;
        point.row = row;
        const auto line = selection_source_line(point);
        const auto [first, last] = selection_columns(line, row, start, end);
        if (last > first) {
            auto sliced = slice_by_column(
                    line, static_cast<std::size_t>(first), static_cast<std::size_t>(last - first), true);
            selected.push_back(trim_end(strip_terminal_sequences(sliced ? *sliced : std::string{})));
        } else {
            selected.emplace_back();
        }
    }
    auto text = std::string{};
    for (std::size_t index = 0; index < selected.size(); ++index) {
        if (index > 0) text.push_back('\n');
        text += selected[index];
    }
    if (text.empty()) return std::nullopt;
    return text;
}

std::vector<std::string> AltScreen::apply_text_selection(const std::vector<std::string>& lines) const {
    const auto text = active_selection_text();
    if (!text || !selection_anchor_ || !selection_focus_) return lines;
    const auto& anchor = *selection_anchor_;
    const auto& focus = *selection_focus_;
    if (anchor.scroll_view != focus.scroll_view) return lines;
    const auto& start = selection_anchor_precedes_focus() ? anchor : focus;
    const auto& end = selection_anchor_precedes_focus() ? focus : anchor;
    auto result = lines;
    auto first_visible = std::ptrdiff_t{0};
    auto last_visible = static_cast<std::ptrdiff_t>(result.size()) - 1;
    auto column_offset = std::ptrdiff_t{0};
    auto row_offset = std::ptrdiff_t{0};
    if (start.scroll_view != nullptr && cached_frame_) {
        const auto* box = detail::get_scroll_view_box(*cached_frame_, start.scroll_view);
        if (box == nullptr) return lines;
        first_visible = std::max<std::ptrdiff_t>({0, box->rect.y, box->clip.y});
        last_visible = std::min<std::ptrdiff_t>({static_cast<std::ptrdiff_t>(result.size()) - 1,
                box->rect.y + static_cast<std::ptrdiff_t>(box->rect.height) - 1,
                box->clip.y + static_cast<std::ptrdiff_t>(box->clip.height) - 1});
        column_offset = box->rect.x;
        row_offset = box->rect.y - static_cast<std::ptrdiff_t>(start.scroll_view->scroll_top());
    }
    for (auto screen_row = first_visible; screen_row <= last_visible; ++screen_row) {
        const auto source_row = screen_row - row_offset;
        if (source_row < start.row || source_row > end.row ||
                is_image_line(result[static_cast<std::size_t>(screen_row)]))
            continue;
        SelectionPoint source_point = start;
        source_point.row = source_row;
        const auto source_line = selection_source_line(source_point);
        const auto [source_first, source_last] = selection_columns(source_line, source_row, start, end);
        const auto first = std::max<std::ptrdiff_t>(0, column_offset + source_first);
        const auto last = std::min<std::ptrdiff_t>(
                static_cast<std::ptrdiff_t>(visible_width(result[static_cast<std::size_t>(screen_row)])),
                column_offset + source_last);
        if (last <= first) continue;
        auto& line = result[static_cast<std::size_t>(screen_row)];
        auto before = slice_by_column(line, 0, static_cast<std::size_t>(first), true);
        auto selected =
                slice_by_column(line, static_cast<std::size_t>(first), static_cast<std::size_t>(last - first), true);
        auto after = slice_by_column(line,
                static_cast<std::size_t>(last),
                static_cast<std::size_t>(
                        std::max<std::ptrdiff_t>(0, static_cast<std::ptrdiff_t>(visible_width(line)) - last)),
                true);
        line = (before ? *before : std::string{}) + apply_selection_inverse(selected ? *selected : std::string{}) +
               (after ? *after : std::string{});
    }
    return result;
}

support::Expected<bool> AltScreen::copy_active_selection_to_clipboard() {
    const auto text = active_selection_text();
    if (!text) return false;
    if (auto copied = selection_copy_sink_(*text); !copied) return std::unexpected(copied.error());
    return true;
}

void AltScreen::scroll_by(std::ptrdiff_t lines) {
    // The viewport's unconsumed lines have no cross-viewport consumer here:
    // input routing hands a wheel delta to `detail::scroll_views_at` callers
    // instead (pi `TuiAltScreen.scrollBy` also drops the remainder).
    (void)primary_scroll_view().scroll_by(lines);
    request_render();
}

void AltScreen::scroll_to_top() {
    primary_scroll_view().scroll_to_start();
    request_render();
}

void AltScreen::scroll_to_bottom() {
    primary_scroll_view().scroll_to_end();
    request_render();
}

std::vector<std::string> AltScreen::strip_osc133_zone_prefixes(const std::vector<std::string>& lines) const {
    constexpr std::string_view kStart = "\x1b]133;";
    std::vector<std::string> result;
    result.reserve(lines.size());
    for (const auto& line : lines) {
        std::size_t position = 0;
        while (line.compare(position, kStart.size(), kStart) == 0 && position + kStart.size() < line.size() &&
                (line[position + kStart.size()] == 'A' || line[position + kStart.size()] == 'B' ||
                        line[position + kStart.size()] == 'C')) {
            const auto start = position;
            position += kStart.size() + 1;
            if (position < line.size() && line[position] == '\x07') {
                ++position;
            } else if (position + 1 < line.size() && line[position] == '\x1b' && line[position + 1] == '\\') {
                position += 2;
            } else {
                position = start;
                break;
            }
        }
        result.push_back(line.substr(position));
    }
    return result;
}

support::ExpectedVoid AltScreen::composite_flashes(
        std::vector<std::string>& lines, std::size_t width, std::size_t height) {
    auto rendered_flashes = flashes_.render(width);
    if (!rendered_flashes) return std::unexpected(rendered_flashes.error());
    auto& flashes = rendered_flashes->lines;
    if (flashes.size() > height) {
        flashes.erase(flashes.begin(), flashes.end() - static_cast<std::ptrdiff_t>(height));
    }
    if (flashes.empty()) return {};
    if (lines.size() < height) lines.resize(height);
    for (std::size_t row = 0; row < flashes.size(); ++row) {
        const auto flash_width = visible_width(flashes[row]);
        if (flash_width == 0) continue;
        auto composed = detail::replace_line_region(lines[row], flashes[row], width - flash_width, flash_width, width);
        if (!composed) return std::unexpected(composed.error());
        lines[row] = std::move(*composed);
    }
    return {};
}

support::ExpectedVoid AltScreen::composite_scroll_to_end_indicator(std::vector<std::string>& lines) {
    scroll_to_end_indicator_rect_.reset();
    if (!scroll_to_end_indicator_) return {};
    auto* scroll_view = cached_frame_ && cached_frame_->primary_scroll_view != nullptr
                                ? cached_frame_->primary_scroll_view
                                : implicit_scroll_view_.get();
    if (!scroll_view->follow_end() || scroll_view->following_end() || !cached_frame_) return {};
    const auto* box = detail::get_scroll_view_box(*cached_frame_, scroll_view);
    if (box == nullptr || box->clip.width == 0 || box->clip.height == 0) return {};
    const auto row = box->clip.y + static_cast<std::ptrdiff_t>(box->clip.height) - 1;
    if (row < 0 || static_cast<std::size_t>(row) >= lines.size() || is_image_line(lines[static_cast<std::size_t>(row)]))
        return {};
    const auto scrollbar = detail::get_scrollbar_geometry(*box);
    const auto right_edge = scrollbar ? scrollbar->column : box->clip.x + static_cast<std::ptrdiff_t>(box->clip.width);
    auto label = scroll_to_end_indicator_();
    auto clipped = truncate_text(label, box->clip.width, "", false);
    if (!clipped) return std::unexpected(clipped.error());
    const auto label_width = static_cast<std::ptrdiff_t>(visible_width(*clipped));
    const auto column = box->clip.x + (static_cast<std::ptrdiff_t>(box->clip.width) - label_width) / 2;
    const auto available = static_cast<std::size_t>(std::max<std::ptrdiff_t>(0, right_edge - column));
    auto text = truncate_text(*clipped, available, "", false);
    if (!text) return std::unexpected(text.error());
    const auto text_width = visible_width(*text);
    if (text_width == 0) return {};
    auto composed = detail::replace_line_region(lines[static_cast<std::size_t>(row)],
            *text,
            static_cast<std::size_t>(column),
            text_width,
            terminal_.dimensions().columns);
    if (!composed) return std::unexpected(composed.error());
    lines[static_cast<std::size_t>(row)] = std::move(*composed);
    scroll_to_end_indicator_rect_ = ScrollToEndIndicatorRect{.row = row, .column = column, .width = text_width};
    return {};
}

support::ExpectedVoid AltScreen::render_viewport(std::vector<std::string> lines,
        std::optional<CursorPosition> cursor,
        TerminalDimensions dimensions,
        std::vector<InlineImageRenderRegion> images) {
    const auto visible_rows = dimensions.rows;
    const bool dimensions_changed = !previous_dimensions_ || previous_dimensions_->columns != dimensions.columns ||
                                    previous_dimensions_->rows != dimensions.rows;
    const bool full_repaint = first_render_ || reset_pending_ || dimensions_changed;
    const bool clearing_repaint = reset_pending_ || (clear_on_shrink_ && previous_dimensions_ &&
                                                            (dimensions.columns < previous_dimensions_->columns ||
                                                                    dimensions.rows < previous_dimensions_->rows));

    // Accounting windows one frame, and the repaint attempt is counted where pi
    // decides it (tui-alt-screen.ts:1724-1725), before any row is written.
    write_accounting_ = {};
    if (full_repaint) ++full_redraws_;

    // The frozen fullscreen image stage (tui-alt-screen.ts
    // `prepareKittyScreen`): changed image anchors force the full row rewrite
    // and its placement cleanup, and offscreen uploads past the frozen caps
    // are deleted by image id.
    auto prepared =
            image_pipeline_->prepare_fullscreen_images(std::move(lines), images, session_image_protocol_, full_repaint);
    lines = std::move(prepared.lines);
    const bool rewrite_all_rows = full_repaint || prepared.images_need_redraw;

    std::optional<CursorPosition> resolved;
    if (cursor) {
        resolved = CursorPosition{
                .column = std::min(cursor->column, dimensions.columns), .row = std::min(cursor->row, visible_rows - 1)};
    }

    if (auto begun = terminal_.begin_synchronized_update(); !begun) return std::unexpected(begun.error());
    if (clearing_repaint) {
        if (auto cleared = terminal_.clear_screen(); !cleared) {
            (void)terminal_.end_synchronized_update();
            return std::unexpected(cleared.error());
        }
    }
    for (const auto& cleanup : {prepared.clear_bytes, prepared.deletion_bytes}) {
        if (cleanup.empty()) continue;
        ++write_accounting_.attempted_writes;
        if (auto written = terminal_.write(cleanup); !written) {
            (void)terminal_.end_synchronized_update();
            return std::unexpected(written.error());
        }
        ++write_accounting_.committed_writes;
        write_accounting_.committed_bytes += cleanup.size();
    }

    selection_source_lines_ = lines;
    const auto selected_lines = apply_text_selection(lines);
    for (std::size_t row = 0; row < visible_rows; ++row) {
        const std::string empty;
        const auto& desired = row < selected_lines.size() ? selected_lines[row] : empty;
        if (!rewrite_all_rows && row < previous_lines_.size() && previous_lines_[row] == desired) continue;
        // Absolute row placement with an explicit line erase: the viewport is
        // not a flow, so a shorter row must not leave the previous tail behind.
        auto output = std::format("\x1b[{};1H\x1b[2K{}", row + 1, desired);
        ++write_accounting_.attempted_writes;
        if (auto written = terminal_.write(output); !written) {
            (void)terminal_.end_synchronized_update();
            return std::unexpected(written.error());
        }
        ++write_accounting_.committed_writes;
        write_accounting_.committed_bytes += output.size();
    }

    const bool show_cursor = resolved.has_value() && show_hardware_cursor_;
    if (resolved) {
        if (auto moved = terminal_.set_cursor(*resolved); !moved) {
            (void)terminal_.end_synchronized_update();
            return std::unexpected(moved.error());
        }
    }
    if (show_cursor != cursor_visible_) {
        if (auto visible = terminal_.set_cursor_visible(show_cursor); !visible) {
            (void)terminal_.end_synchronized_update();
            return std::unexpected(visible.error());
        }
        cursor_visible_ = show_cursor;
    }

    if (auto ended = terminal_.end_synchronized_update(); !ended) return std::unexpected(ended.error());

    // Commit the frame only after it fully reached the terminal, so a failed
    // frame repaints instead of being skipped by the differential.
    previous_lines_.assign(visible_rows, std::string{});
    for (std::size_t row = 0; row < visible_rows && row < selected_lines.size(); ++row)
        previous_lines_[row] = selected_lines[row];
    selection_source_lines_ = lines;
    previous_dimensions_ = dimensions;
    cursor_ = resolved;
    first_render_ = false;
    reset_pending_ = false;
    return {};
}

support::ExpectedVoid AltScreen::clear_screen() {
    if (!started_) {
        return std::unexpected(
                support::make_error(support::ErrorCode::Validation, "TUI must be started before clearing the screen"));
    }
    if (auto cleared = terminal_.clear_screen(); !cleared) return std::unexpected(cleared.error());
    previous_lines_.clear();
    reset_pending_ = true;
    return {};
}

void AltScreen::request_render(bool force) {
    ChildOperation operation(*this);
    pending_render_ = true;
    if (!started_ || stopping_ || scheduler_ == nullptr) return;
    if (force) {
        cancel_scheduled_render();
        ++render_generation_;
        render_scheduled_ = false;
        (void)render_now();
        return;
    }
    schedule_render();
}

void AltScreen::request_immediate_render() {
    ChildOperation operation(*this);
    pending_render_ = true;
    if (!started_ || stopping_ || scheduler_ == nullptr) return;
    cancel_scheduled_render();
    const auto generation = ++render_generation_;
    render_scheduled_ = true;
    const std::weak_ptr<RenderScheduleState> weak_state = render_schedule_state_;
    scheduler_->post([weak_state, generation] {
        if (const auto state = weak_state.lock(); state && state->owner != nullptr) {
            state->owner->execute_scheduled_render(generation);
        }
    });
}

void AltScreen::invalidate() {
    ChildOperation operation(*this);
    // pi `TuiBase.invalidate` (`tui.ts:911-913`) walks `getMountedRoots`
    // (`tui-alt-screen.ts:332-334`): while a layout root is installed it is the
    // only mounted root, otherwise the mounted roots are the children.
    if (layout_root_) {
        layout_root_->invalidate();
    } else {
        std::vector<Component*> children;
        children.reserve(children_.size());
        for (const auto& child : children_)
            children.push_back(child.get());
        for (auto* child : children) {
            if (owns(child)) child->invalidate();
        }
    }
    request_render();
}

namespace {

AltScreenOptions with_hardware_cursor(AltScreenOptions options, bool show_hardware_cursor) {
    options.show_hardware_cursor = show_hardware_cursor;
    return options;
}

} // namespace

AltScreen::AltScreen(Terminal& terminal, bool show_hardware_cursor, AltScreenOptions options)
    : AltScreen(terminal, with_hardware_cursor(std::move(options), show_hardware_cursor)) {}

void AltScreen::set_clear_on_shrink(bool enabled) noexcept { clear_on_shrink_ = enabled; }

bool AltScreen::clear_on_shrink() const noexcept { return clear_on_shrink_; }

void AltScreen::set_show_hardware_cursor(bool enabled) {
    if (show_hardware_cursor_ == enabled) return;
    show_hardware_cursor_ = enabled;
    if (!enabled && started_ && !stopping_) {
        (void)terminal_.set_cursor_visible(false);
        cursor_visible_ = false;
    }
    request_render();
}

void AltScreen::set_scheduler(TuiScheduler* scheduler) {
    if (scheduler_ == scheduler) return;
    // The interval belongs to the previous scheduler; cancel through it before
    // the pointer moves so the cancellation reaches the right host.
    stop_selection_auto_scroll();
    cancel_scheduled_render();
    ++render_generation_;
    render_scheduled_ = false;
    scheduler_ = scheduler;
    flashes_.set_scheduler(scheduler_);
    if (started_ && pending_render_) schedule_render();
}

void AltScreen::set_render_prepare_sink(TuiRenderPrepareSink sink) { render_prepare_sink_ = std::move(sink); }

void AltScreen::set_render_complete_sink(TuiRenderCompleteSink sink) { render_complete_sink_ = std::move(sink); }

void AltScreen::schedule_render() {
    if (!started_ || stopping_ || scheduler_ == nullptr || render_scheduled_) return;
    const auto generation = ++render_generation_;
    render_scheduled_ = true;
    const std::weak_ptr<RenderScheduleState> weak_state = render_schedule_state_;
    scheduler_->schedule(std::chrono::steady_clock::duration::zero(), [weak_state, generation] {
        if (const auto state = weak_state.lock(); state && state->owner != nullptr) {
            state->owner->execute_scheduled_render(generation);
        }
    });
}

void AltScreen::cancel_scheduled_render() noexcept {
    if (scheduler_ != nullptr) scheduler_->cancel();
}

void AltScreen::execute_scheduled_render(std::size_t generation) {
    if (!started_ || stopping_ || !render_scheduled_ || generation != render_generation_) return;
    (void)render_now();
}

std::optional<std::pair<std::ptrdiff_t, std::ptrdiff_t>> AltScreen::selection_anchor_trace() const {
    if (!selection_anchor_) return std::nullopt;
    return std::pair{selection_anchor_->row, selection_anchor_->column};
}

std::optional<std::pair<std::ptrdiff_t, std::ptrdiff_t>> AltScreen::selection_focus_trace() const {
    if (!selection_focus_) return std::nullopt;
    return std::pair{selection_focus_->row, selection_focus_->column};
}

std::optional<std::string> AltScreen::selection_text_trace() const { return active_selection_text(); }

void AltScreen::handle_input(std::string input) {
    if (!started_) return;
    detail::TuiHost::handle_input(std::move(input), [this](const InputEventVariant& event) { dispatch_input(event); });
}

void AltScreen::dispatch_input(const InputEventVariant& event) {
    ChildOperation operation(*this);
    reconcile_overlay_focus(terminal_.dimensions());
    const auto* key = std::get_if<KeyEvent>(&event);
    const bool defer_viewport_key = key != nullptr && keybinding_context_ && should_defer_viewport_input_to_overlay() &&
                                    keybinding_context_->first_match(*key,
                                            std::array<std::string_view, 6>{"tui.altScreen.pageUp",
                                                    "tui.altScreen.pageDown",
                                                    "tui.altScreen.top",
                                                    "tui.altScreen.bottom",
                                                    "tui.altScreen.previousPrompt",
                                                    "tui.altScreen.nextPrompt"});
    if (key != nullptr && !defer_viewport_key && handle_viewport_key(*key)) return;
    auto* input_handler = dynamic_cast<InputHandler*>(focused_);
    if (input_handler == nullptr) return;
    static_cast<void>(input_handler->handle_input(event));
    request_immediate_render();
}

void AltScreen::handle_resize(TerminalDimensions dimensions) {
    (void)dimensions;
    if (!started_) return;
    request_render();
}

bool AltScreen::handle_viewport_input(std::string_view data) {
    if (data == kFocusOut) {
        if (active_search_ && active_search_->component->set_hovered_navigation_direction(std::nullopt))
            request_render();
        // An unfinished selection is a gesture and ends with focus loss; a
        // released selection remains visible across focus changes (pi #1006).
        // A pending multi-click dies with the gesture too (pi `lastClick`).
        if (selection_press_active_) clear_text_selection();
        clear_mouse_gesture();
        last_mouse_click_.reset();
        clear_selection_click_history();
        pressed_url_.reset();
        stop_scrollbar_drag();
        stop_scrollbar_hover();
        return true;
    }
    if (data == kFocusIn) return true;

    if (const auto packet = detail::parse_mouse_packet(data)) {
        if (packet->kind == detail::MousePacket::Kind::Wheel) {
            if (!handle_wheel_report(*packet)) return false;
        } else {
            const auto event = create_mouse_event(*packet, detail::pointer_event_type(*packet));
            if (handle_gesture_mouse_report(*packet, event)) return true;
            if (handle_search_mouse_report(*packet)) return true;
            // Frozen defers only the wheel and the viewport keys to a focused
            // overlay (pi `tui-alt-screen.ts:710,741`). A pointer report always
            // reaches the overlay walk, so a click inside an overlay is a
            // component event even while that overlay owns keyboard focus.
            handle_mouse_report(*packet);
        }
        return true;
    }
    // A fragment that can only be a mouse report never becomes typed input, even
    // when its payload is malformed (pi `isMouseSequence`).
    if (detail::is_mouse_sequence(data)) return true;

    const auto key = parse_key(data, terminal_.capabilities().keyboard_protocol == KeyboardProtocol::Kitty);
    if (key && keybinding_context_) {
        constexpr std::array<std::string_view, 1> toggle_action{"tui.altScreen.search"};
        if (keybinding_context_->first_match(*key, toggle_action)) {
            if (key->type != KeyEventType::Release) toggle_search();
            return true;
        }
        if (search_overlay_focused()) {
            constexpr std::array<std::string_view, 1> next_action{"tui.altScreen.searchNext"};
            constexpr std::array<std::string_view, 1> previous_action{"tui.altScreen.searchPrevious"};
            constexpr std::array<std::string_view, 1> close_action{"tui.altScreen.searchClose"};
            if (keybinding_context_->first_match(*key, next_action)) {
                if (key->type != KeyEventType::Release) navigate_search(1);
                return true;
            }
            if (keybinding_context_->first_match(*key, previous_action)) {
                if (key->type != KeyEventType::Release) navigate_search(-1);
                return true;
            }
            if (keybinding_context_->first_match(*key, close_action)) {
                if (key->type != KeyEventType::Release) close_search();
                return true;
            }
        }
        if (should_defer_viewport_input_to_overlay()) return false;
        if (handle_viewport_key(*key)) return true;
    }
    return false;
}

MouseEvent AltScreen::create_mouse_event(const detail::MousePacket& packet,
        MouseEventType type,
        std::optional<std::ptrdiff_t> wheel_delta,
        std::optional<std::size_t> click_count) const {
    const auto dimensions = terminal_.dimensions();
    return detail::create_mouse_event(packet, type, dimensions.columns, dimensions.rows, wheel_delta, click_count);
}

bool AltScreen::handle_wheel_report(const detail::MousePacket& packet) {
    const auto lines = next_wheel_scroll_lines(packet.direction);
    // Frozen applies the Alt multiplier to the accelerated line count before
    // layout dispatch (pi `tui-alt-screen.ts:699-704`, SGR bit 3).
    const auto multiplier = (packet.button & 8) != 0 ? std::ptrdiff_t{5} : std::ptrdiff_t{1};
    const auto delta = packet.direction * lines * multiplier;
    const auto event = create_mouse_event(packet, MouseEventType::Wheel, delta);
    // Frozen offers the wheel to the overlay under the pointer first, then to
    // the layout, and only routes it to the viewports when neither answered
    // (pi `tui-alt-screen.ts:705-712`).
    const auto overlay = dispatch_mouse_to_overlay(event);
    auto result = overlay.result;
    if (!result && !overlay.hit) result = dispatch_mouse_to_layout(event);
    if (result) {
        if (apply_mouse_dispatch_result(event, *result)) request_render();
        return true;
    }
    if (should_defer_viewport_input_to_overlay()) return false;
    route_wheel(event, delta);
    return true;
}

void AltScreen::handle_mouse_report(const detail::MousePacket& packet) {
    const auto type = detail::pointer_event_type(packet);
    const auto event = create_mouse_event(packet, type);
    if (selection_press_active_ && handle_selection_mouse_report(packet)) return;
    if (handle_gesture_mouse_report(packet, event)) return;

    // Frozen offers the report to the overlay under the pointer first; the
    // scroll-to-end indicator and the scrollbars only see a report no overlay
    // covers, and a covered pointer outside every scrollbar ends the hover
    // (pi `tui-alt-screen.ts:920-940`).
    const auto overlay = dispatch_mouse_to_overlay(event);
    if (!overlay.hit) {
        if (handle_scroll_to_end_indicator_mouse_report(packet)) return;
        // A report no gesture owns moves the scrollbar hover before any
        // component sees it (pi `handleMouseEvent`).
        const bool scrollbar_handled = handle_scrollbar_mouse_report(packet);
        if (!scrollbar_drag_) update_scrollbar_hover(packet.x, packet.y);
        if (scrollbar_handled) return;
    } else {
        stop_scrollbar_hover();
    }

    auto result = overlay.result;
    if (!result && !overlay.hit) result = dispatch_mouse_to_layout(event);
    if (result) {
        const bool render = apply_mouse_dispatch_result(event, *result);
        if (type == MouseEventType::Press) {
            clear_text_selection();
            mouse_press_target_ = anchor_gesture_target(result->target, result->host_target);
            mouse_press_point_ = std::pair{packet.x, packet.y};
            mouse_press_moved_ = false;
        }
        if (render) request_render();
        return;
    }

    if (handle_right_click_paste(packet)) return;
    if (handle_selection_mouse_report(packet)) return;
    // Unclaimed pointer reports are consumed by the fullscreen root, which is
    // also the owner of text selection on terminal-rendered content.
}

AltScreen::OverlayMouseDispatch AltScreen::dispatch_mouse_to_overlay(const MouseEvent& event) {
    if (compositor_ == nullptr) return {};
    auto* overlay = compositor_->overlay_at(event.screen_x, event.screen_y, terminal_.dimensions());
    if (overlay == nullptr) return {};
    const auto bounds = compositor_->bounds(overlay);
    if (!bounds) return {};

    auto local = event;
    local.x = event.screen_x - static_cast<std::ptrdiff_t>(bounds->col);
    local.y = event.screen_y - static_cast<std::ptrdiff_t>(bounds->row);
    local.width = bounds->width;
    local.height = bounds->height;
    auto result = dispatch_mouse_event(*overlay, local);
    if (!result) return OverlayMouseDispatch{.hit = true};
    // Frozen pins the hit overlay as the focus target of any focus answer, so
    // the container keeps keyboard focus while a nested control is clicked.
    if (result->focus) result->focus_target = overlay;
    return OverlayMouseDispatch{.hit = true, .result = std::move(result)};
}

bool AltScreen::handle_right_click_paste(const detail::MousePacket& packet) {
    if (packet.release || packet.kind != detail::MousePacket::Kind::Pointer || packet.button != 2 ||
            !right_click_paste_sink_) {
        return false;
    }
    // A handler decline still owns this secondary press, matching pi's
    // best-effort clipboard route and preventing it from starting selection.
    static_cast<void>(right_click_paste_sink_());
    return true;
}

bool AltScreen::handle_selection_mouse_report(const detail::MousePacket& packet) {
    const auto button = packet.button & 3;
    if (button != 0 && !(packet.release && button == 3)) return false;

    if (packet.release) {
        if (!selection_press_active_) return false;
        selection_press_active_ = false;
        stop_selection_auto_scroll();
        const auto point =
                selection_point_at(packet.x, packet.y, selection_anchor_ ? selection_anchor_->scroll_view : nullptr);
        if (!point || !selection_anchor_) {
            clear_text_selection();
            request_render();
            return true;
        }
        update_selection_focus(*point);
        const bool is_click = !selection_dragged_ && selection_anchor_->scroll_view == point->scroll_view &&
                              selection_anchor_->row == point->row && selection_anchor_->column == point->column;
        const auto clicked_url = is_click ? std::move(pressed_url_) : std::nullopt;
        pressed_url_.reset();
        if (clicked_url && link_open_sink_) {
            clear_text_selection();
            link_open_sink_(*clicked_url);
            request_render();
            return true;
        }
        if (is_click) {
            // The release ends a click only when the press never selected:
            // after a word/line selection the release lands inside the
            // selection (anchor moved to the word start), so the press point
            // differs and the release must not re-dispatch a component click
            // (pi:1168-1190). The count is the one the press recorded in the
            // click history; a press without a word reports 1 (pi
            // `clickCount: this.lastClick?.count ?? 1`, 1343-1346).
            const auto count = last_word_click_ ? last_word_click_->count : std::size_t{1};
            auto click = create_mouse_event(packet, MouseEventType::Click, std::nullopt, count);
            if (auto result = dispatch_mouse_to_layout(click)) {
                clear_text_selection();
                if (apply_mouse_dispatch_result(click, *result)) request_render();
                return true;
            }
        }
        if (copy_on_select_) (void)copy_active_selection_to_clipboard();
        request_render();
        return true;
    }

    if ((packet.button & 32) != 0) {
        if (!selection_press_active_ || !selection_anchor_) return true;
        selection_dragged_ = true;
        pressed_url_.reset();
        clear_selection_click_history();
        if (auto point = selection_point_at(packet.x, packet.y, selection_anchor_->scroll_view))
            update_selection_focus(*point);
        update_selection_auto_scroll(packet);
        request_render();
        return true;
    }

    // A fresh press arms a new gesture; an interval a previous drag still has
    // queued must not extend it (pi `stopSelectionAutoScroll`, :1369).
    stop_selection_auto_scroll();
    selection_press_active_ = true;
    selection_dragged_ = false;
    selection_granularity_ = SelectionGranularity::Character;
    pressed_url_.reset();
    selection_initial_range_.reset();
    const auto scroll_views =
            cached_frame_ ? detail::scroll_views_at(*cached_frame_, packet.x, packet.y) : std::vector<ScrollView*>{};
    const auto scroll_view = scroll_views.empty() ? nullptr : scroll_views.front();
    auto anchor = selection_point_at(packet.x, packet.y, scroll_view);
    if (!anchor) {
        clear_text_selection();
        return true;
    }
    const auto word = word_selection_at(*anchor);
    const auto count = next_selection_click_count(*anchor, word);
    if (count == 2 && word) {
        pressed_url_.reset();
        selection_anchor_ = word->start;
        selection_focus_ = word->end;
        selection_granularity_ = SelectionGranularity::Word;
        selection_initial_range_ = *word;
    } else if (count == 3) {
        // Infallible: even an empty source row yields a zero-width range,
        // retaining line granularity during the drag like pi (pi:1213-1219).
        const auto line = line_selection_at(*anchor);
        selection_anchor_ = line.start;
        selection_focus_ = line.end;
        selection_granularity_ = SelectionGranularity::Line;
        selection_initial_range_ = line;
    } else {
        // The first press and any press past the triple click select by
        // character: the stored count wrapped after three, so the fourth
        // consecutive press is a plain click again and the fifth is the next
        // word selection (frozen capture `multiclick-word-line-cycle`).
        selection_anchor_ = *anchor;
        selection_focus_ = *anchor;
    }
    if (selection_granularity_ == SelectionGranularity::Character && anchor->column >= 0) {
        if (const auto source_line = selection_source_line_at(*anchor))
            pressed_url_ = osc8_link_at_column(*source_line, static_cast<std::size_t>(anchor->column));
    }
    // The component click history belongs to the dispatch layer; a root-level
    // selection press does not clear it (pi keeps selection and component
    // clicks independent, frozen sequence in ScrollbarTest issue1005).
    request_render();
    return true;
}

bool AltScreen::handle_gesture_mouse_report(const detail::MousePacket& packet, const MouseEvent& event) {
    if (!mouse_capture_ && !mouse_press_target_) return false;
    // The gesture belongs to a component that may have left the tree since the
    // press, and a removed C++ component is already destroyed. pi keeps the JS
    // object alive and only documents the focus consequence (`tui.ts:82-86`);
    // here the gesture ends instead of reaching freed memory.
    const auto target = mouse_capture_ ? *mouse_capture_ : *mouse_press_target_;
    if (target.component == nullptr || !owns(target.component)) {
        clear_mouse_gesture();
        return false;
    }

    // A captured target keeps receiving the gesture, wherever the pointer is.
    if (mouse_press_point_ && (packet.x != mouse_press_point_->first || packet.y != mouse_press_point_->second)) {
        mouse_press_moved_ = true;
        last_mouse_click_.reset();
    }
    bool render = false;
    if (auto result = dispatch_mouse_to_target(event, target)) {
        render = apply_mouse_dispatch_result(event, *result);
    }
    if (packet.release) {
        // A release on the press cell without movement is a click, and
        // consecutive clicks count up (pi `handleMouseEvent`).
        if (!mouse_press_moved_ && mouse_press_point_ && mouse_press_point_->first == packet.x &&
                mouse_press_point_->second == packet.y) {
            const auto click = create_mouse_event(packet,
                    MouseEventType::Click,
                    std::nullopt,
                    next_component_click_count(target, packet.x, packet.y));
            if (auto result = dispatch_mouse_to_target(click, target)) {
                render = apply_mouse_dispatch_result(click, *result) || render;
            }
        }
        clear_mouse_gesture();
    }
    if (render) request_render();
    return true;
}

// Velocity-based wheel acceleration (pi `wheel-scroll.ts:35-74`).
std::ptrdiff_t AltScreen::next_wheel_scroll_lines(std::ptrdiff_t direction) {
    if (const auto* fixed = std::get_if<std::size_t>(&wheel_scroll_lines_))
        return static_cast<std::ptrdiff_t>(std::max<std::size_t>(1, *fixed));
    const auto now = wheel_clock_();
    if (!wheel_last_time_) {
        wheel_last_time_ = now;
        wheel_last_direction_ = direction;
        wheel_average_gap_ms_.reset();
        wheel_carry_ = 0;
        return 1;
    }
    const auto gap = std::chrono::duration<double, std::milli>(now - *wheel_last_time_).count();
    const bool same_gesture = direction == wheel_last_direction_ && gap <= 200.0;
    wheel_last_time_ = now;
    wheel_last_direction_ = direction;
    if (!same_gesture) {
        wheel_average_gap_ms_.reset();
        wheel_carry_ = 0;
        return 1;
    }
    if (gap < 5.0) return 1;
    wheel_average_gap_ms_ = wheel_average_gap_ms_ ? (*wheel_average_gap_ms_ + gap) / 2.0 : gap;
    const auto scaled = std::min(6.0, std::max(1.0, 100.0 / *wheel_average_gap_ms_)) + wheel_carry_;
    const auto whole = static_cast<std::ptrdiff_t>(std::floor(scaled));
    wheel_carry_ = scaled - static_cast<double>(whole);
    return whole;
}

void AltScreen::reset_wheel_scroll_acceleration() noexcept {
    wheel_last_time_.reset();
    wheel_last_direction_ = 0;
    wheel_average_gap_ms_.reset();
    wheel_carry_ = 0;
}

bool AltScreen::handle_viewport_key(const KeyEvent& event) {
    if (!keybinding_context_) return false;
    // Frozen dispatch order (tui-alt-screen.ts:~720-777): pageUp, pageDown,
    // previousPrompt, nextPrompt, top, bottom. A single ordered pass keeps
    // conflict resolution identical to frozen when user overrides collide.
    constexpr std::array<std::string_view, 6> actions{"tui.altScreen.pageUp",
            "tui.altScreen.pageDown",
            "tui.altScreen.previousPrompt",
            "tui.altScreen.nextPrompt",
            "tui.altScreen.top",
            "tui.altScreen.bottom"};
    const auto matched = keybinding_context_->first_match(event, actions);
    if (!matched) return false;
    if (event.type == KeyEventType::Release) return true;
    if (*matched == "tui.altScreen.pageUp") {
        scroll_by(
                -std::max<std::ptrdiff_t>(1, static_cast<std::ptrdiff_t>(primary_scroll_view().viewport_height()) - 4));
    } else if (*matched == "tui.altScreen.pageDown") {
        scroll_by(
                std::max<std::ptrdiff_t>(1, static_cast<std::ptrdiff_t>(primary_scroll_view().viewport_height()) - 4));
    } else if (*matched == "tui.altScreen.previousPrompt") {
        scroll_to_prompt(-1);
    } else if (*matched == "tui.altScreen.nextPrompt") {
        scroll_to_prompt(1);
    } else if (*matched == "tui.altScreen.top") {
        scroll_to_top();
    } else {
        scroll_to_bottom();
    }
    return true;
}

void AltScreen::scroll_to_prompt(int direction) {
    if (!cached_frame_) return;
    const auto* box = detail::get_scroll_view_box(*cached_frame_, &primary_scroll_view());
    if (box == nullptr || !box->has_scroll_content_lines) return;
    const auto& rows = box->scroll_content_lines;
    auto row = static_cast<std::ptrdiff_t>(primary_scroll_view().scroll_top()) + direction;
    while (row >= 0 && row < static_cast<std::ptrdiff_t>(rows.size())) {
        if (rows[static_cast<std::size_t>(row)].starts_with("\x1b]133;A\x07") ||
                rows[static_cast<std::size_t>(row)].starts_with("\x1b]133;A\x1b\\")) {
            primary_scroll_view().scroll_to(row);
            request_render();
            break;
        }
        row += direction;
    }
}

std::optional<MouseDispatchResult> AltScreen::dispatch_mouse_to_layout(const MouseEvent& event) {
    if (!cached_frame_) return std::nullopt;
    struct Candidate {
        Component* component;
        detail::LayoutRect rect;
    };
    std::vector<Candidate> candidates;
    std::vector<const Component*> visited;
    for (const auto* box : detail::layout_boxes_at(*cached_frame_, event.screen_x, event.screen_y)) {
        if (box->component == nullptr) continue;
        if (std::find(visited.begin(), visited.end(), box->component) != visited.end()) continue;
        visited.push_back(box->component);
        candidates.push_back(Candidate{.component = box->component, .rect = box->rect});
    }
    for (const auto& candidate : candidates) {
        if (!owns(candidate.component)) continue;
        auto target_event = event;
        target_event.x = event.screen_x - candidate.rect.x;
        target_event.y = event.screen_y - candidate.rect.y;
        target_event.width = candidate.rect.width;
        target_event.height = candidate.rect.height;
        if (auto result = dispatch_mouse_event(*candidate.component, target_event)) return result;
    }
    return std::nullopt;
}

std::optional<MouseDispatchResult> AltScreen::dispatch_mouse_to_implicit_document(const MouseEvent& event) {
    // pi `implicitDocument.handleMouse` maps the composed rows the document had
    // in the last frame back to the child that occupied them.
    return detail::dispatch_mouse_to_cached_rows(
            event,
            implicit_document_rows_.size(),
            [this](std::size_t index) -> Component& { return *implicit_document_rows_[index].first; },
            [this](std::size_t index) { return implicit_document_rows_[index].second; });
}

std::optional<MouseDispatchResult> AltScreen::dispatch_mouse_to_target(
        const MouseEvent& event, const MouseDispatchTarget& target) {
    if (target.component == nullptr) return std::nullopt;
    return dispatch_mouse_event(*target.component, retarget_mouse_event(event, target));
}

bool AltScreen::apply_mouse_dispatch_result(const MouseEvent& event, const MouseDispatchResult& result) {
    auto* focus_target = result.focus_target != nullptr ? result.focus_target : result.target.component;
    const bool can_focus = focus_target != nullptr && owns(focus_target);
    const bool focus_changed = result.focus && can_focus && focused_ != focus_target;
    if (result.focus && can_focus) apply_focus(focus_target);
    if (result.capture) mouse_capture_ = anchor_gesture_target(result.target, result.host_target);
    // Move and release default to no render; press, click, drag and wheel default
    // to one, and a handler that answered `render` overrides both (pi
    // `tui-alt-screen.ts:847-856`).
    return result.render.value_or(focus_changed || mouse_event_renders_by_default(event.type));
}

MouseDispatchTarget AltScreen::anchor_gesture_target(
        const MouseDispatchTarget& target, const MouseDispatchTarget& host) const {
    if (owns(target.component)) return target;
    if (host.component != nullptr && owns(host.component)) return host;
    return target;
}

std::size_t AltScreen::next_component_click_count(
        const MouseDispatchTarget& target, std::ptrdiff_t x, std::ptrdiff_t y) {
    const auto now = std::chrono::steady_clock::now();
    const auto previous = last_mouse_click_;
    const auto count = previous && now - previous->at <= kComponentDoubleClickInterval &&
                                       previous->component == target.component && previous->x == x && previous->y == y
                               ? (previous->count % 3) + 1
                               : std::size_t{1};
    last_mouse_click_ = MouseClick{.at = now, .count = count, .component = target.component, .x = x, .y = y};
    return count;
}

void AltScreen::route_wheel(const MouseEvent& event, std::ptrdiff_t delta) {
    auto remaining = delta;
    std::vector<ScrollView*> seen;
    const auto viewports =
            cached_frame_ ? detail::scroll_views_at(*cached_frame_, event.x, event.y) : std::vector<ScrollView*>{};
    for (auto* viewport : viewports) {
        if (viewport == nullptr) continue;
        seen.push_back(viewport);
        remaining = viewport->scroll_by(remaining);
        if (remaining == 0 || viewport->overscroll() == ScrollOverscroll::Contain) break;
    }
    auto& primary = primary_scroll_view();
    if (remaining != 0 && std::find(seen.begin(), seen.end(), &primary) == seen.end()) {
        (void)primary.scroll_by(remaining);
    }
    // A wheel moves the pointer without a motion report, so the hover follows
    // the wheel cell (pi `routeWheel`).
    update_scrollbar_hover(event.x, event.y);
    request_render();
}

bool AltScreen::handle_scroll_to_end_indicator_mouse_report(const detail::MousePacket& packet) {
    const auto& rect = scroll_to_end_indicator_rect_;
    if (!rect || packet.kind == detail::MousePacket::Kind::Wheel || packet.release || (packet.button & 32) != 0 ||
            (packet.button & 3) != 0)
        return false;
    if (packet.y != rect->row || packet.x < rect->column ||
            packet.x >= rect->column + static_cast<std::ptrdiff_t>(rect->width))
        return false;
    scroll_to_bottom();
    return true;
}

bool AltScreen::handle_scrollbar_mouse_report(const detail::MousePacket& packet) {
    if (scrollbar_drag_) {
        if (scrollbar_drag_->lifetime.expired() || !owns(scrollbar_drag_->scroll_view)) {
            stop_scrollbar_drag();
            stop_scrollbar_hover();
            return true;
        }
        if (packet.release) {
            stop_scrollbar_drag();
            return true;
        }
        const auto* box =
                cached_frame_ ? detail::get_scroll_view_box(*cached_frame_, scrollbar_drag_->scroll_view) : nullptr;
        const auto geometry = box ? detail::get_scrollbar_geometry(*box) : std::nullopt;
        if (!geometry) {
            stop_scrollbar_drag();
            stop_scrollbar_hover();
            return true;
        }

        scroll_scrollbar_to_pointer(*scrollbar_drag_->scroll_view, *geometry, packet.y, scrollbar_drag_->grab_offset);
        return true;
    }

    if (packet.release || (packet.button & 32) != 0 || (packet.button & 3) != 0) return false;
    if (!cached_frame_) return false;
    for (auto* scroll_view : detail::scroll_views_at(*cached_frame_, packet.x, packet.y)) {
        if (scroll_view == nullptr || !owns(scroll_view)) continue;
        const auto* box = detail::get_scroll_view_box(*cached_frame_, scroll_view);
        const auto geometry = box ? detail::get_scrollbar_geometry(*box) : std::nullopt;
        if (!geometry || packet.x != geometry->column || packet.y < geometry->track_top ||
                packet.y >= geometry->track_top + static_cast<std::ptrdiff_t>(geometry->track_height)) {
            continue;
        }

        clear_text_selection();
        pressed_url_.reset();
        clear_selection_click_history();
        set_scrollbar_hover(scroll_view);
        const auto grab_offset =
                packet.y >= geometry->thumb_top &&
                                packet.y < geometry->thumb_top + static_cast<std::ptrdiff_t>(geometry->thumb_height)
                        ? packet.y - geometry->thumb_top
                        : static_cast<std::ptrdiff_t>(geometry->thumb_height / 2);
        scrollbar_drag_ = ScrollbarDrag{
                .scroll_view = scroll_view, .grab_offset = grab_offset, .lifetime = scroll_view->lifetime_token()};
        if (packet.y < geometry->thumb_top ||
                packet.y >= geometry->thumb_top + static_cast<std::ptrdiff_t>(geometry->thumb_height)) {
            scroll_scrollbar_to_pointer(*scroll_view, *geometry, packet.y, grab_offset);
        }
        return true;
    }
    return false;
}

void AltScreen::stop_scrollbar_drag() noexcept { scrollbar_drag_.reset(); }

void AltScreen::validate_scrollbar_interaction(const detail::LayoutFrame& frame) {
    if (!scrollbar_drag_) return;
    if (scrollbar_drag_->lifetime.expired() || !owns(scrollbar_drag_->scroll_view)) {
        stop_scrollbar_drag();
        stop_scrollbar_hover();
        return;
    }
    const auto* box = detail::get_scroll_view_box(frame, scrollbar_drag_->scroll_view);
    if (box == nullptr || !detail::get_scrollbar_geometry(*box)) {
        stop_scrollbar_drag();
        stop_scrollbar_hover();
    }
}

void AltScreen::update_scrollbar_hover(std::ptrdiff_t x, std::ptrdiff_t y) {
    ScrollView* next = nullptr;
    if (cached_frame_) {
        for (auto* viewport : detail::scroll_views_at(*cached_frame_, x, y)) {
            if (viewport == nullptr || !owns(viewport)) continue;
            const auto* box = detail::get_scroll_view_box(*cached_frame_, viewport);
            if (box == nullptr) continue;
            const auto geometry = detail::get_scrollbar_geometry(*box, true);
            if (!geometry) continue;
            const auto track_last = geometry->track_top + static_cast<std::ptrdiff_t>(geometry->track_height);
            if (x != geometry->column || y < geometry->track_top || y >= track_last) continue;
            next = viewport;
            break;
        }
    }
    set_scrollbar_hover(next);
}

void AltScreen::set_scrollbar_hover(ScrollView* scroll_view) {
    if (scroll_view != nullptr && !owns(scroll_view)) scroll_view = nullptr;
    if (scroll_view == scrollbar_hover_ && !scrollbar_hover_lifetime_.expired()) return;
    // `owns()` distinguishes live-tree targets from detached targets but does
    // not distinguish a detached, still-live viewport from a destroyed one.
    // The weak token lets us deactivate the former without dereferencing the
    // latter (pi keeps both JavaScript objects alive).
    if (scrollbar_hover_ != nullptr && !scrollbar_hover_lifetime_.expired())
        scrollbar_hover_->set_scrollbar_active(false);
    scrollbar_hover_ = scroll_view;
    scrollbar_hover_lifetime_ = scroll_view != nullptr ? scroll_view->lifetime_token() : std::weak_ptr<void>{};
    if (scrollbar_hover_ != nullptr) scrollbar_hover_->set_scrollbar_active(true);
}

void AltScreen::stop_scrollbar_hover() { set_scrollbar_hover(nullptr); }

void AltScreen::invalidate_composition_cache() noexcept {
    // The frame boxes and the document rows name components by pointer, and a
    // composition that changed may have destroyed one: a hit test needs the next
    // frame, never a stale name. A selection can also name a viewport being
    // removed, so its coordinates are no longer meaningful after this point.
    cached_frame_.reset();
    implicit_document_rows_.clear();
    clear_text_selection();
}

void AltScreen::clear_mouse_gesture() noexcept {
    mouse_capture_.reset();
    mouse_press_target_.reset();
    mouse_press_point_.reset();
    mouse_press_moved_ = false;
}

RawInputListenerHandle AltScreen::add_raw_input_listener(RawInputListener listener) {
    return detail::TuiHost::add_raw_input_listener(std::move(listener));
}

support::ExpectedVoid AltScreen::remove_raw_input_listener(RawInputListenerId id) {
    return detail::TuiHost::remove_raw_input_listener(id);
}

TerminalColorSchemeListenerHandle AltScreen::on_terminal_color_scheme_change(TerminalColorSchemeListener listener) {
    return detail::TuiHost::on_terminal_color_scheme_change(std::move(listener));
}

void AltScreen::set_terminal_color_scheme_notifications(bool enabled) {
    detail::TuiHost::set_terminal_color_scheme_notifications(enabled, started_, stopping_);
}

std::future<TerminalColors> AltScreen::query_terminal_colors(TerminalColorsQueryOptions options) {
    return detail::TuiHost::query_terminal_colors(std::move(options));
}

support::Expected<OverlayHandle> AltScreen::show_overlay(std::unique_ptr<Overlay> overlay) {
    if (!overlay) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation, "TUI cannot attach a null Overlay"));
    }
    auto* component = overlay.get();
    auto state = std::make_shared<detail::OverlayHandleState>();
    if (auto attached = compositor_->add_overlay(
                std::move(overlay), focused_, ++detail::TuiHost::focus_order_counter_, state);
            !attached) {
        return std::unexpected(attached.error());
    }
    auto handle = OverlayHandle{overlay_handle_context_, std::move(state), component};
    if (!component->options().non_capturing && compositor_->visible(component, terminal_.dimensions()))
        set_focus_internal(component, false);
    request_render();
    return handle;
}

support::Expected<std::reference_wrapper<Overlay>> AltScreen::add_overlay(std::unique_ptr<Overlay> overlay) {
    if (!overlay) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation, "TUI cannot attach a null Overlay"));
    }
    auto attached = compositor_->add_overlay(std::move(overlay), focused_, ++detail::TuiHost::focus_order_counter_);
    if (attached && !attached->get().options().non_capturing &&
            compositor_->visible(&attached->get(), terminal_.dimensions()))
        set_focus_internal(&attached->get(), false);
    if (attached) request_render();
    return attached;
}

support::ExpectedVoid AltScreen::remove_overlay(Overlay* overlay) {
    if (overlay == nullptr) return {};
    if (!compositor_->owns(overlay)) {
        return std::unexpected(
                support::make_error(support::ErrorCode::Validation, "Overlay is not attached to this TUI"));
    }
    remove_overlay_internal(overlay);
    return {};
}

void AltScreen::remove_overlay_internal(Overlay* overlay) {
    if (overlay == nullptr || !compositor_->owns(overlay)) return;
    const auto pre_focus = compositor_->pre_focus(overlay);
    const bool was_focused = focused_ == overlay;
    const auto restore_state = detail::TuiHost::overlay_focus_restore_;
    const bool was_blocker =
            restore_state.status == OverlayFocusRestoreStatus::Blocked && restore_state.blocked_by == overlay;
    const bool restore_another_overlay = was_blocker && restore_state.resume_to_overlay &&
                                         restore_state.overlay != overlay && compositor_->owns(restore_state.overlay);
    compositor_->retarget_pre_focus(overlay, pre_focus);
    clear_overlay_restore_for(overlay);
    if (was_focused) apply_focus(nullptr);
    static_cast<void>(compositor_->remove(overlay));

    if (restore_another_overlay) {
        detail::TuiHost::overlay_focus_restore_ = restore_state;
        detail::TuiHost::overlay_focus_restore_.blocked_by = nullptr;
        set_focus_internal(restore_state.overlay, true);
    } else if (was_blocker) {
        detail::TuiHost::overlay_focus_restore_ = restore_state;
        detail::TuiHost::overlay_focus_restore_.blocked_by = nullptr;
        auto* resume_target = restore_state.resume_to_overlay ? static_cast<Component*>(restore_state.overlay)
                                                              : restore_state.resume_target;
        if (!restore_state.resume_to_overlay || compositor_->owns(restore_state.overlay))
            set_focus_internal(resume_target, true);
        else
            detail::TuiHost::overlay_focus_restore_ = {};
    } else if (was_focused) {
        auto* next = compositor_->topmost_focusable(terminal_.dimensions());
        auto* target = next != nullptr ? static_cast<Component*>(next) : pre_focus;
        set_focus_internal(target, false);
        if (focused_ == nullptr) fallback_focus();
    }
    request_render();
}

support::ExpectedVoid AltScreen::hide_overlay(Overlay* overlay) {
    if (overlay == nullptr) return {};
    if (!compositor_->owns(overlay)) {
        return std::unexpected(
                support::make_error(support::ErrorCode::Validation, "Overlay is not attached to this TUI"));
    }
    overlay->set_visible(false);
    hide_overlay_internal(overlay, true);
    return {};
}

void AltScreen::hide_overlay_internal(Overlay* overlay, bool hidden) {
    if (!compositor_->owns(overlay) || compositor_->is_hidden(overlay) == hidden) return;
    const bool was_focused = focused_ == overlay;
    static_cast<void>(compositor_->set_hidden(overlay, hidden));
    if (hidden) {
        const bool restore_was_blocked =
                detail::TuiHost::overlay_focus_restore_.status == OverlayFocusRestoreStatus::Blocked &&
                detail::TuiHost::overlay_focus_restore_.overlay == overlay &&
                focused_ == detail::TuiHost::overlay_focus_restore_.blocked_by;
        const auto restore_state = detail::TuiHost::overlay_focus_restore_;
        clear_overlay_restore_for(overlay);
        if (was_focused) {
            auto* pre_focus = compositor_->pre_focus(overlay);
            auto* next = compositor_->topmost_focusable(terminal_.dimensions());
            set_focus_internal(next != nullptr ? static_cast<Component*>(next) : pre_focus, false);
        } else if (restore_was_blocked) {
            auto* pre_focus = compositor_->pre_focus(overlay);
            auto* next = compositor_->topmost_focusable(terminal_.dimensions());
            set_focus_internal(next != nullptr ? static_cast<Component*>(next) : pre_focus, false);
            if (!restore_state.resume_to_overlay) {
                detail::TuiHost::overlay_focus_restore_ = restore_state;
                detail::TuiHost::overlay_focus_restore_.blocked_by = focused_;
            }
        }
    } else if (!overlay->options().non_capturing && compositor_->visible(overlay, terminal_.dimensions())) {
        compositor_->set_focus_order(overlay, ++detail::TuiHost::focus_order_counter_);
        set_focus_internal(overlay, false);
    }
    invalidate();
}

support::ExpectedVoid AltScreen::restore_overlay(Overlay* overlay) {
    if (overlay == nullptr) return {};
    if (!compositor_->owns(overlay)) {
        return std::unexpected(
                support::make_error(support::ErrorCode::Validation, "Overlay is not attached to this TUI"));
    }
    overlay->set_visible(true);
    hide_overlay_internal(overlay, false);
    return {};
}

void AltScreen::focus_overlay_internal(Overlay* overlay) {
    if (!compositor_->owns(overlay) || !compositor_->visible(overlay, terminal_.dimensions()) ||
            overlay->options().non_capturing)
        return;
    compositor_->set_focus_order(overlay, ++detail::TuiHost::focus_order_counter_);
    set_focus_internal(overlay, false);
    request_render();
}

void AltScreen::unfocus_overlay_internal(Overlay* overlay, std::optional<Component*> target) {
    if (!compositor_->owns(overlay)) return;
    const bool is_focused = focused_ == overlay;
    const bool pending = detail::TuiHost::overlay_focus_restore_.status != OverlayFocusRestoreStatus::Inactive &&
                         detail::TuiHost::overlay_focus_restore_.overlay == overlay;
    if (!is_focused && !pending) return;
    if (detail::TuiHost::overlay_focus_restore_.status == OverlayFocusRestoreStatus::Blocked &&
            detail::TuiHost::overlay_focus_restore_.overlay == overlay &&
            focused_ == detail::TuiHost::overlay_focus_restore_.blocked_by) {
        if (target) {
            detail::TuiHost::overlay_focus_restore_.resume_to_overlay = false;
            detail::TuiHost::overlay_focus_restore_.resume_target = *target;
        } else {
            clear_overlay_restore_for(overlay);
        }
        request_render();
        return;
    }
    clear_overlay_restore_for(overlay);
    if (is_focused || target) {
        auto* top = compositor_->topmost_focusable(terminal_.dimensions());
        Component* fallback =
                top != nullptr && top != overlay ? static_cast<Component*>(top) : compositor_->pre_focus(overlay);
        set_focus_internal(target ? *target : fallback, true);
        if (!target || dynamic_cast<Overlay*>(*target) == nullptr) clear_overlay_restore_for(overlay);
    }
    request_render();
}

bool AltScreen::is_overlay_focused_internal(const Overlay* overlay) const {
    return compositor_->owns(overlay) && focused_ == overlay;
}

std::optional<OverlayBounds> AltScreen::overlay_bounds_internal(const Overlay* overlay) const {
    if (!compositor_->owns(overlay) || compositor_->is_hidden(overlay)) return std::nullopt;
    const auto dimensions = terminal_.dimensions();
    if (!compositor_->visible(overlay, dimensions)) return std::nullopt;
    return compositor_->bounds(overlay);
}

void AltScreen::clear_overlay_restore_for(Overlay* overlay) {
    if (detail::TuiHost::overlay_focus_restore_.status != OverlayFocusRestoreStatus::Inactive &&
            detail::TuiHost::overlay_focus_restore_.overlay == overlay)
        detail::TuiHost::overlay_focus_restore_ = {};
}

Component* AltScreen::resolve_overlay_restore() {
    if (detail::TuiHost::overlay_focus_restore_.status != OverlayFocusRestoreStatus::Blocked) return nullptr;
    if (!detail::TuiHost::overlay_focus_restore_.resume_to_overlay) {
        auto* target = detail::TuiHost::overlay_focus_restore_.resume_target;
        detail::TuiHost::overlay_focus_restore_ = {};
        return target;
    }
    auto* overlay = detail::TuiHost::overlay_focus_restore_.overlay;
    if (compositor_->owns(overlay) && compositor_->visible(overlay, terminal_.dimensions())) return overlay;
    detail::TuiHost::overlay_focus_restore_ = {};
    return nullptr;
}

void AltScreen::reconcile_overlay_focus(TerminalDimensions dimensions) {
    auto* focused_overlay = dynamic_cast<Overlay*>(focused_);
    if (focused_overlay != nullptr && compositor_->owns(focused_overlay) &&
            !compositor_->visible(focused_overlay, dimensions)) {
        auto* replacement = compositor_->topmost_focusable(dimensions);
        if (replacement != nullptr) {
            set_focus_internal(replacement, true);
            detail::TuiHost::overlay_focus_restore_ = OverlayFocusRestoreState{
                    .status = OverlayFocusRestoreStatus::Blocked,
                    .overlay = focused_overlay,
                    .blocked_by = replacement,
            };
        } else {
            Component* fallback = compositor_->pre_focus(focused_overlay);
            auto* fallback_overlay = dynamic_cast<Overlay*>(fallback);
            while (fallback_overlay != nullptr && compositor_->owns(fallback_overlay) &&
                    !compositor_->visible(fallback_overlay, dimensions)) {
                fallback = compositor_->pre_focus(fallback_overlay);
                fallback_overlay = dynamic_cast<Overlay*>(fallback);
            }
            set_focus_internal(fallback, true);
            detail::TuiHost::overlay_focus_restore_ = OverlayFocusRestoreState{
                    .status = OverlayFocusRestoreStatus::Eligible,
                    .overlay = focused_overlay,
            };
        }
    }

    auto* current_overlay = dynamic_cast<Overlay*>(focused_);
    if (current_overlay != nullptr && compositor_->owns(current_overlay)) {
        if (detail::TuiHost::overlay_focus_restore_.status == OverlayFocusRestoreStatus::Blocked &&
                detail::TuiHost::overlay_focus_restore_.blocked_by == current_overlay &&
                compositor_->owns(detail::TuiHost::overlay_focus_restore_.overlay) &&
                compositor_->visible(detail::TuiHost::overlay_focus_restore_.overlay, dimensions))
            set_focus_internal(detail::TuiHost::overlay_focus_restore_.overlay, true);
        return;
    }
    if ((detail::TuiHost::overlay_focus_restore_.status == OverlayFocusRestoreStatus::Eligible ||
                (detail::TuiHost::overlay_focus_restore_.status == OverlayFocusRestoreStatus::Blocked &&
                        detail::TuiHost::overlay_focus_restore_.resume_to_overlay &&
                        !compositor_->visible(detail::TuiHost::overlay_focus_restore_.overlay, dimensions))) &&
            compositor_->owns(detail::TuiHost::overlay_focus_restore_.overlay) &&
            compositor_->visible(detail::TuiHost::overlay_focus_restore_.overlay, dimensions)) {
        set_focus_internal(detail::TuiHost::overlay_focus_restore_.overlay, true);
    } else if (detail::TuiHost::overlay_focus_restore_.status == OverlayFocusRestoreStatus::Blocked &&
               detail::TuiHost::overlay_focus_restore_.blocked_by != focused_) {
        if (detail::TuiHost::overlay_focus_restore_.resume_to_overlay) {
            if (compositor_->owns(detail::TuiHost::overlay_focus_restore_.overlay) &&
                    compositor_->visible(detail::TuiHost::overlay_focus_restore_.overlay, dimensions))
                set_focus_internal(detail::TuiHost::overlay_focus_restore_.overlay, true);
        } else {
            auto* target = resolve_overlay_restore();
            set_focus_internal(target, true);
        }
    }
}

TuiAltScreenRenderState AltScreen::capture_render_state() const {
    TuiAltScreenRenderState state;
    state.viewport_rows = previous_lines_;
    state.width = previous_dimensions_ ? previous_dimensions_->columns : 0;
    state.height = previous_dimensions_ ? previous_dimensions_->rows : 0;
    state.cursor = cursor_;
    return state;
}

support::ExpectedVoid AltScreen::restore_render_state(const TuiAltScreenRenderState& state) {
    if (started_) {
        return std::unexpected(support::make_error(
                support::ErrorCode::Validation, "Cannot restore render state while the TUI is running"));
    }
    previous_lines_ = state.viewport_rows;
    if (state.width > 0 && state.height > 0) {
        previous_dimensions_ = TerminalDimensions{.columns = state.width, .rows = state.height};
        first_render_ = false;
        baseline_from_restore_ = true;
    } else {
        previous_dimensions_.reset();
        first_render_ = true;
        baseline_from_restore_ = false;
    }
    reset_pending_ = false;
    cursor_ = state.cursor;
    return {};
}

void AltScreen::reset_render_state() noexcept {
    previous_lines_.clear();
    previous_dimensions_.reset();
    cursor_.reset();
    cursor_visible_ = false;
    first_render_ = false;
    reset_pending_ = true;
}

} // namespace cch::tui
