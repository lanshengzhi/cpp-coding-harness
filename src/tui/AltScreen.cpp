#include <cch/tui/AltScreen.hpp>

#include "tui/RenderUtils.hpp"
#include "tui/UnicodeWidth.hpp"

#include <cch/support/Error.hpp>

#include <algorithm>
#include <chrono>
#include <format>
#include <utility>

namespace cch::tui {
namespace {

[[nodiscard]] support::Error alt_overlay_unavailable() {
    return support::make_error(
            support::ErrorCode::Validation, "Alternate-screen overlays are not available on this root yet (#998)");
}

} // namespace

AltScreen::AltScreen(Terminal& terminal)
    : detail::TuiHost(terminal), render_schedule_state_(std::make_shared<RenderScheduleState>()) {
    render_schedule_state_->owner = this;
}

AltScreen::~AltScreen() {
    (void)stop();
    render_schedule_state_->owner = nullptr;
}

bool AltScreen::owns(const Component* component) const {
    if (component == nullptr) return false;
    for (const auto& child : children_) {
        if (child && child->contains(component)) return true;
    }
    return false;
}

support::Expected<std::reference_wrapper<Component>> AltScreen::add_child(std::unique_ptr<Component> component) {
    auto attached = detail::attach_child(children_, std::move(component), "");
    if (!attached) return std::unexpected(attached.error());
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
    retired_children_.push_back(std::move(*found));
    children_.erase(found);
    if (was_focused) {
        apply_focus(nullptr);
        fallback_focus();
    }
    request_render();
    return {};
}

void AltScreen::clear_children() {
    ChildOperation operation(*this);
    const bool was_focused = focused_ != nullptr && owns(focused_);
    for (auto& child : children_)
        retired_children_.push_back(std::move(child));
    children_.clear();
    if (was_focused) apply_focus(nullptr);
    request_render();
}

bool AltScreen::focus_target_available(Component* component) const {
    return owns(component) && dynamic_cast<Focusable*>(component) != nullptr;
}

void AltScreen::apply_focus(Component* component) {
    detail::TuiHost::apply_focus(component, [this](Component* candidate) { return focus_target_available(candidate); });
}

support::ExpectedVoid AltScreen::set_focus(Component* component) {
    ChildOperation operation(*this);
    if (component != nullptr && stopping_) {
        return std::unexpected(
                support::make_error(support::ErrorCode::Validation, "TUI cannot focus components while stopping"));
    }
    if (component != nullptr && !owns(component)) {
        return std::unexpected(
                support::make_error(support::ErrorCode::Validation, "TUI focus target is not attached to this root"));
    }
    if (component != nullptr && dynamic_cast<Focusable*>(component) == nullptr) {
        return std::unexpected(
                support::make_error(support::ErrorCode::Validation, "TUI focus target does not participate in focus"));
    }
    if (component == focused_) return {};
    apply_focus(component);
    return {};
}

void AltScreen::fallback_focus() {
    ChildOperation operation(*this);
    for (const auto& child : children_) {
        auto* focusable = dynamic_cast<Focusable*>(child.get());
        if (focusable == nullptr) continue;
        auto* as_component = dynamic_cast<Component*>(focusable);
        if (as_component != focused_) apply_focus(as_component);
        return;
    }
}

support::ExpectedVoid AltScreen::start() {
    if (started_) return {};

    started_ = true;
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

    pending_render_ = false;
    if (color_scheme_notifications_enabled_) {
        (void)terminal_.write("\x1b[?2031h");
    }
    request_render();
    return {};
}

support::ExpectedVoid AltScreen::stop(TuiStopOptions options) {
    if (!started_ || stopping_) return {};
    (void)options; // The alternate buffer is always exited; the main buffer returns.

    cancel_scheduled_render();
    ++render_generation_;
    render_scheduled_ = false;
    ChildOperation operation(*this);
    stopping_ = true;
    started_ = false;
    apply_focus(nullptr);
    overlay_focus_restore_ = {};

    if (color_scheme_notifications_enabled_) {
        (void)terminal_.write("\x1b[?2031l");
    }

    cancel_color_queries();

    // Restore the mode stack in reverse order of entry.
    const auto cursor_result = terminal_.set_cursor_visible(true);
    const auto autowrap_result = terminal_.set_autowrap(true);
    const auto exit_result = terminal_.set_alternate_screen(false);
    reset_decoder();
    pending_render_ = false;
    const auto stop_result = terminal_.stop();
    stopping_ = false;

    if (!cursor_result) return std::unexpected(cursor_result.error());
    if (!autowrap_result) return std::unexpected(autowrap_result.error());
    if (!exit_result) return std::unexpected(exit_result.error());
    if (!stop_result) return std::unexpected(stop_result.error());
    return {};
}

support::ExpectedVoid AltScreen::render() { return render_now(); }

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
    if (dimensions.columns == 0 || dimensions.rows == 0) {
        return std::unexpected(
                support::make_error(support::ErrorCode::Validation, "TUI requires positive terminal dimensions"));
    }

    std::vector<std::string> lines;
    std::optional<CursorPosition> cursor;
    if (auto rendered = render_children(dimensions, lines, cursor); !rendered) {
        return std::unexpected(rendered.error());
    }
    return render_viewport(lines, cursor, dimensions);
}

support::ExpectedVoid AltScreen::render_children(
        TerminalDimensions dimensions, std::vector<std::string>& lines, std::optional<CursorPosition>& cursor) {
    std::size_t row_offset = 0;
    std::vector<Component*> children;
    children.reserve(children_.size());
    for (const auto& child : children_)
        children.push_back(child.get());
    for (auto* child : children) {
        if (!owns(child)) continue;
        if (auto* viewport_aware = dynamic_cast<ViewportAware*>(child)) {
            viewport_aware->set_available_height(dimensions.rows);
        }
        if (!owns(child)) continue;
        auto rendered = child->render(dimensions.columns);
        if (!rendered) return std::unexpected(rendered.error());
        if (!owns(child)) continue;
        const auto child_row_offset = row_offset;
        for (auto& line : rendered->lines) {
            auto prepared = detail::prepare_rendered_line(line, dimensions.columns);
            if (!prepared) return std::unexpected(prepared.error());
            lines.push_back(std::move(prepared->text));
        }
        if (!cursor) {
            if (const auto location = child->cursor_location()) {
                cursor = CursorPosition{.column = location->column, .row = child_row_offset + location->row};
            }
        }
        row_offset += rendered->lines.size();
    }

    if (!cursor) {
        auto* focusable = dynamic_cast<Focusable*>(focused_);
        if (focusable != nullptr && focusable->focused()) cursor = focusable->cursor_location();
    }
    return {};
}

support::ExpectedVoid AltScreen::render_viewport(
        const std::vector<std::string>& lines, std::optional<CursorPosition> cursor, TerminalDimensions dimensions) {
    const auto visible_rows = dimensions.rows;
    const bool dimensions_changed = !previous_dimensions_ || previous_dimensions_->columns != dimensions.columns ||
                                    previous_dimensions_->rows != dimensions.rows;
    const bool full_repaint = first_render_ || reset_pending_ || dimensions_changed;
    const bool clearing_repaint = reset_pending_ || (clear_on_shrink_ && previous_dimensions_ &&
                                                            (dimensions.columns < previous_dimensions_->columns ||
                                                                    dimensions.rows < previous_dimensions_->rows));

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

    for (std::size_t row = 0; row < visible_rows; ++row) {
        const std::string empty;
        const auto& desired = row < lines.size() ? lines[row] : empty;
        if (!full_repaint && row < previous_lines_.size() && previous_lines_[row] == desired) continue;
        // Absolute row placement with an explicit line erase: the viewport is
        // not a flow, so a shorter row must not leave the previous tail behind.
        auto output = std::format("\x1b[{};1H\x1b[2K{}", row + 1, desired);
        if (auto written = terminal_.write(output); !written) {
            (void)terminal_.end_synchronized_update();
            return std::unexpected(written.error());
        }
    }

    const bool show_cursor = resolved.has_value();
    if (show_cursor != cursor_visible_) {
        if (auto visible = terminal_.set_cursor_visible(show_cursor); !visible) {
            (void)terminal_.end_synchronized_update();
            return std::unexpected(visible.error());
        }
        cursor_visible_ = show_cursor;
    }
    if (resolved) {
        if (auto moved = terminal_.set_cursor(*resolved); !moved) {
            (void)terminal_.end_synchronized_update();
            return std::unexpected(moved.error());
        }
    }

    if (auto ended = terminal_.end_synchronized_update(); !ended) return std::unexpected(ended.error());

    // Commit the frame only after it fully reached the terminal, so a failed
    // frame repaints instead of being skipped by the differential.
    previous_lines_.assign(visible_rows, std::string{});
    for (std::size_t row = 0; row < visible_rows && row < lines.size(); ++row)
        previous_lines_[row] = lines[row];
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

void AltScreen::invalidate() { request_render(); }

void AltScreen::set_clear_on_shrink(bool enabled) noexcept { clear_on_shrink_ = enabled; }

bool AltScreen::clear_on_shrink() const noexcept { return clear_on_shrink_; }

void AltScreen::set_scheduler(TuiScheduler* scheduler) {
    if (scheduler_ == scheduler) return;
    cancel_scheduled_render();
    ++render_generation_;
    render_scheduled_ = false;
    scheduler_ = scheduler;
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

void AltScreen::handle_input(std::string input) {
    if (!started_) return;
    detail::TuiHost::handle_input(std::move(input), [this](const InputEventVariant& event) { dispatch_input(event); });
}

void AltScreen::dispatch_input(const InputEventVariant& event) {
    ChildOperation operation(*this);
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
    (void)overlay;
    return std::unexpected(alt_overlay_unavailable());
}

support::Expected<std::reference_wrapper<Overlay>> AltScreen::add_overlay(std::unique_ptr<Overlay> overlay) {
    (void)overlay;
    return std::unexpected(alt_overlay_unavailable());
}

support::ExpectedVoid AltScreen::remove_overlay(Overlay* overlay) {
    (void)overlay;
    return std::unexpected(alt_overlay_unavailable());
}

support::ExpectedVoid AltScreen::hide_overlay(Overlay* overlay) {
    (void)overlay;
    return std::unexpected(alt_overlay_unavailable());
}

support::ExpectedVoid AltScreen::restore_overlay(Overlay* overlay) {
    (void)overlay;
    return std::unexpected(alt_overlay_unavailable());
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
