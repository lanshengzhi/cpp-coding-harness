#include <cch/tui/Tui.hpp>

#include "tui/InputDecoder.hpp"
#include "tui/OverlayCompositor.hpp"
#include "tui/RenderPipeline.hpp"
#include "tui/RenderUtils.hpp"
#include "tui/TuiTestHooks.hpp"

#include <cch/support/Error.hpp>

#include <algorithm>
#include <format>
#include <limits>
#include <utility>

namespace cch::tui {
namespace {

constexpr std::size_t kInputDecodeChunkBytes = 4096;

[[nodiscard]] std::string describe_error(const support::Error& error) {
    if (error.detail.empty()) return error.message;
    return std::format("{} [{}]", error.message, error.detail);
}

[[nodiscard]] support::Error startup_rollback_error(const support::Error& startup, const support::Error& rollback) {
    return support::make_error(startup.code,
            "TUI startup failed and terminal restoration was incomplete",
            std::format("startup: {}; restoration: {}", describe_error(startup), describe_error(rollback)));
}

/// The downstream consumer of the replies that survive the raw-input listener
/// stage. Decoded responses are applied in order to the terminal instance.
void apply_late_replies(Terminal& terminal, const std::vector<detail::TerminalResponseVariant>& responses) {
    for (const auto& response : responses) {
        const auto* cell_size = std::get_if<detail::CellSizeResponse>(&response);
        if (cell_size == nullptr) continue;
        (void)terminal.apply_cell_pixel_dimensions(CellPixelDimensions{
                .width = cell_size->width_px,
                .height = cell_size->height_px,
        });
    }
}

} // namespace

struct MainScreen::RenderScheduleState {
    MainScreen* owner{nullptr};
};

MainScreen::MainScreen(Terminal& terminal)
    : terminal_(terminal), render_schedule_state_(std::make_shared<RenderScheduleState>()),
      stream_decoder_(std::make_unique<detail::TerminalStreamDecoder>()),
      compositor_(std::make_unique<detail::OverlayCompositor>()),
      render_pipeline_(std::make_unique<detail::RenderPipeline>(terminal)),
      raw_listeners_(std::make_shared<RawInputListenerChain>()) {
    render_schedule_state_->owner = this;
}

MainScreen::~MainScreen() {
    (void)stop();
    render_schedule_state_->owner = nullptr;
}

support::Expected<std::reference_wrapper<Component>> MainScreen::add_child(std::unique_ptr<Component> component) {
    auto attached = detail::attach_child(children_, std::move(component), "");
    if (attached) {
        rendered_child_rows_.clear();
        rendered_children_valid_ = false;
        request_render();
        return *attached;
    }
    return std::unexpected(attached.error());
}

support::ExpectedVoid MainScreen::remove_child(Component* component) {
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
    compositor_->forget_component(component);
    retired_children_.push_back(std::move(*found));
    children_.erase(found);
    rendered_child_rows_.clear();
    rendered_children_valid_ = false;
    if (was_focused) {
        apply_focus(nullptr);
        if (focused_ == nullptr) fallback_focus();
    }
    request_render();
    return {};
}

void MainScreen::clear_children() {
    ChildOperation operation(*this);
    const bool was_focused = focused_ != nullptr && owns(focused_);
    for (const auto& child : children_)
        compositor_->forget_component(child.get());
    for (auto& child : children_)
        retired_children_.push_back(std::move(child));
    children_.clear();
    rendered_child_rows_.clear();
    rendered_children_valid_ = false;
    if (was_focused) apply_focus(nullptr);
    request_render();
}

void MainScreen::finish_child_operation() noexcept {
    --child_operation_depth_;
    if (child_operation_depth_ == 0) retired_children_.clear();
}

support::Expected<std::reference_wrapper<Overlay>> MainScreen::add_overlay(std::unique_ptr<Overlay> overlay) {
    auto attached = compositor_->add_overlay(std::move(overlay));
    if (attached) request_render();
    return attached;
}

RawInputListenerHandle MainScreen::add_raw_input_listener(RawInputListener listener) {
    return raw_listeners_->add_owned(std::move(listener));
}

support::ExpectedVoid MainScreen::remove_raw_input_listener(RawInputListenerId id) {
    if (!raw_listeners_->remove(id)) {
        return std::unexpected(support::make_error(
                support::ErrorCode::Validation, "Raw-input listener is not registered with this TUI"));
    }
    return {};
}

support::ExpectedVoid MainScreen::remove_overlay(Overlay* overlay) {
    if (overlay == nullptr) return {};
    if (!compositor_->owns(overlay)) {
        return std::unexpected(
                support::make_error(support::ErrorCode::Validation, "Overlay is not attached to this TUI"));
    }

    const bool was_focused = focused_ == static_cast<Component*>(overlay);
    auto* return_focus = compositor_->return_focus(overlay);
    if (was_focused) apply_focus(nullptr);
    compositor_->forget_focus(overlay, return_focus);

    if (auto removed = compositor_->remove(overlay); !removed) {
        return std::unexpected(removed.error());
    }

    if (was_focused) {
        if (focus_target_available(return_focus))
            apply_focus(return_focus);
        else
            fallback_focus();
    }
    return {};
}

support::ExpectedVoid MainScreen::hide_overlay(Overlay* overlay) {
    if (overlay == nullptr) return {};
    if (!compositor_->owns(overlay)) {
        return std::unexpected(
                support::make_error(support::ErrorCode::Validation, "Overlay is not attached to this TUI"));
    }

    auto* return_focus = compositor_->return_focus(overlay);
    const auto was_focused = focused_ == static_cast<Component*>(overlay);
    overlay->set_visible(false);

    if (was_focused) {
        apply_focus(nullptr);
        if (focus_target_available(return_focus))
            apply_focus(return_focus);
        else
            fallback_focus();
    }

    invalidate();
    return {};
}

support::ExpectedVoid MainScreen::restore_overlay(Overlay* overlay) {
    if (overlay == nullptr) return {};
    if (!compositor_->owns(overlay)) {
        return std::unexpected(
                support::make_error(support::ErrorCode::Validation, "Overlay is not attached to this TUI"));
    }

    overlay->set_visible(true);
    invalidate();
    return {};
}

support::ExpectedVoid MainScreen::start() {
    if (started_) {
        return {};
    }

    // The latch is armed before the terminal starts: ProcessTerminal
    // forwards startup-preserved input synchronously inside start(), and
    // dropping it on the not-yet-started guard would lose keystrokes typed
    // during the capability probes (#610).
    started_ = true;
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

    if (auto result = terminal_.set_cursor_visible(false); !result) {
        started_ = false;
        if (auto stopped = terminal_.stop(); !stopped) {
            return std::unexpected(startup_rollback_error(result.error(), stopped.error()));
        }
        return std::unexpected(result.error());
    }

    render_pipeline_->start(terminal_.dimensions());
    pending_render_ = false;
    if (scheduler_ != nullptr) last_render_at_.reset();
    request_render();
    return {};
}

support::ExpectedVoid MainScreen::stop() {
    if (!started_ || stopping_) return {};

    cancel_scheduled_render();
    ++render_generation_;
    render_scheduled_ = false;
    ChildOperation operation(*this);
    stopping_ = true;
    started_ = false;
    const auto stop_cursor = resolve_cursor_location();
    apply_focus(nullptr);
    compositor_->clear_focus_history();

    const auto exit_result = render_pipeline_->stop(stop_cursor);
    const auto cursor_result = terminal_.set_cursor_visible(true);
    stream_decoder_->reset();
    pending_render_ = false;
    last_render_at_.reset();
    const auto stop_result = terminal_.stop();
    stopping_ = false;

    if (!exit_result) return std::unexpected(exit_result.error());
    if (!cursor_result) return std::unexpected(cursor_result.error());
    if (!stop_result) return std::unexpected(stop_result.error());
    return {};
}

support::ExpectedVoid MainScreen::clear_screen() {
    if (!started_) {
        return std::unexpected(
                support::make_error(support::ErrorCode::Validation, "TUI must be started before clearing the screen"));
    }
    if (auto cleared = render_pipeline_->clear_screen(); !cleared) {
        return std::unexpected(cleared.error());
    }
    return {};
}

support::ExpectedVoid MainScreen::render() { return render_now(); }

support::ExpectedVoid MainScreen::render_now() {
    if (!started_) {
        return std::unexpected(
                support::make_error(support::ErrorCode::Validation, "TUI must be started before rendering"));
    }

    cancel_scheduled_render();
    ++render_generation_;
    render_scheduled_ = false;
    if (scheduler_ != nullptr) last_render_at_ = scheduler_->now();

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
    if (result) {
        pending_render_ = false;
    }
    if (render_complete_sink_) render_complete_sink_(result);
    if (!result && result.error().code == support::ErrorCode::Busy) request_render();
    return result;
}

support::ExpectedVoid MainScreen::render_frame() {
    ChildOperation operation(*this);
    const auto dimensions = terminal_.dimensions();
    if (dimensions.columns == 0 || dimensions.rows == 0) {
        return std::unexpected(
                support::make_error(support::ErrorCode::Validation, "TUI requires positive terminal dimensions"));
    }

    const auto capabilities = terminal_.capabilities();
    // An overlay splices into prepared composed rows, so a visible overlay keeps
    // the pre-#711 order: prepare -> composite -> pad -> reset. Without one,
    // compositing leaves the composed rows untouched and preparation can follow
    // the differential state, so only changed rows are prepared (#711).
    const bool compose_overlays = compositor_->has_visible_overlays(dimensions);
    render_pipeline_->begin_frame();
    auto rendered = render_children(dimensions, compose_overlays);
    if (!rendered) return std::unexpected(rendered.error());
    // Main-screen images are buffer-absolute and follow content into the
    // terminal's scrollback (fork-B image-follows-content): they are not
    // bounded by the viewport, only by the composed buffer itself.
    auto materialized = detail::OverlayCompositor::materialize_images(
            std::move(*rendered), capabilities, dimensions.columns, std::numeric_limits<std::size_t>::max());
    if (auto overlay_result = compositor_->composite(dimensions, capabilities, materialized); !overlay_result) {
        return std::unexpected(overlay_result.error());
    }
    auto result = render_pipeline_->render(
            std::move(materialized), dimensions, capabilities, compose_overlays, resolve_cursor_location());
    return result;
}

support::Expected<RenderResult> MainScreen::render_children(TerminalDimensions dimensions, bool prepare_rows) {
    RenderResult output;
    rendered_child_rows_.clear();
    rendered_children_valid_ = false;
    std::vector<std::size_t> rendered_child_rows;
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
        if (!rendered) {
            rendered_child_rows_.clear();
            rendered_children_valid_ = false;
            return std::unexpected(rendered.error());
        }
        if (!owns(child)) continue;
        const auto row_offset = output.lines.size();
        rendered_child_rows.push_back(rendered->lines.size());
        for (auto& line : rendered->lines) {
            if (!prepare_rows) {
                // Frame-level preparation follows the differential state in
                // `render`; only an overlay frame prepares here, before
                // compositing splices into these rows (#711).
                output.lines.push_back(std::move(line));
                continue;
            }
            render_pipeline_->note_prepared_line();
            auto prepared = detail::prepare_rendered_line(line, dimensions.columns);
            if (!prepared) return std::unexpected(prepared.error());
            output.lines.push_back(std::move(prepared->text));
        }
        for (auto& image : rendered->images) {
            image.region.row += row_offset;
            output.images.push_back(std::move(image));
        }
        if (rendered->viewport_height.has_value()) {
            output.viewport_height = rendered->viewport_height;
        }
        for (auto& line : rendered->dock_lines) {
            if (!prepare_rows) {
                output.dock_lines.push_back(std::move(line));
                continue;
            }
            render_pipeline_->note_prepared_line();
            auto prepared = detail::prepare_rendered_line(line, dimensions.columns);
            if (!prepared) return std::unexpected(prepared.error());
            output.dock_lines.push_back(std::move(prepared->text));
        }
    }
    rendered_child_rows_ = std::move(rendered_child_rows);
    rendered_children_valid_ = true;
    return output;
}

support::ExpectedVoid MainScreen::set_focus(Component* component) {
    ChildOperation operation(*this);
    if (component != nullptr && stopping_) {
        return std::unexpected(
                support::make_error(support::ErrorCode::Validation, "TUI cannot focus components while stopping"));
    }
    if (component != nullptr && !owns(component)) {
        // Check if component is inside an overlay
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

    if (auto* overlay = dynamic_cast<Overlay*>(component); overlay != nullptr && component != focused_) {
        compositor_->remember_focus(overlay, focused_);
    }
    apply_focus(component);
    return {};
}

void MainScreen::request_render(bool force) {
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

void MainScreen::request_immediate_render() {
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

void MainScreen::invalidate() {
    ChildOperation operation(*this);
    pending_render_ = true;
    std::vector<Component*> children;
    children.reserve(children_.size());
    for (const auto& child : children_)
        children.push_back(child.get());
    rendered_child_rows_.clear();
    rendered_children_valid_ = false;
    for (auto* child : children) {
        if (owns(child)) child->invalidate();
    }
    compositor_->invalidate_all();
    request_render();
}

bool MainScreen::owns(const Component* component) const {
    if (component == nullptr) return false;
    for (const auto& child : children_) {
        if (child && child->contains(component)) {
            return true;
        }
    }
    return false;
}

void MainScreen::set_scheduler(TuiScheduler* scheduler) {
    if (scheduler_ == scheduler) return;
    cancel_scheduled_render();
    ++render_generation_;
    render_scheduled_ = false;
    scheduler_ = scheduler;
    if (started_ && pending_render_) schedule_render();
}

void MainScreen::set_render_prepare_sink(TuiRenderPrepareSink sink) { render_prepare_sink_ = std::move(sink); }

void MainScreen::set_render_complete_sink(TuiRenderCompleteSink sink) { render_complete_sink_ = std::move(sink); }

void MainScreen::schedule_render() {
    if (!started_ || stopping_ || scheduler_ == nullptr || render_scheduled_) return;
    auto delay = std::chrono::steady_clock::duration::zero();
    if (last_render_at_) {
        const auto elapsed = scheduler_->now() - *last_render_at_;
        const auto interval = std::chrono::milliseconds{16};
        if (elapsed < interval) delay = interval - elapsed;
    }
    const auto generation = ++render_generation_;
    render_scheduled_ = true;
    const std::weak_ptr<RenderScheduleState> weak_state = render_schedule_state_;
    scheduler_->schedule(delay, [weak_state, generation] {
        if (const auto state = weak_state.lock(); state && state->owner != nullptr) {
            state->owner->execute_scheduled_render(generation);
        }
    });
}

void MainScreen::cancel_scheduled_render() noexcept {
    if (scheduler_ != nullptr) scheduler_->cancel();
}

void MainScreen::execute_scheduled_render(std::size_t generation) {
    if (!started_ || stopping_ || !render_scheduled_ || generation != render_generation_) return;
    (void)render_now();
}

void MainScreen::handle_input(std::string input) {
    if (!started_) return;
    stream_decoder_->set_kitty_protocol_active(terminal_.capabilities().keyboard_protocol == KeyboardProtocol::Kitty);
    if (input.empty()) {
        // ProcessTerminal applies out-of-band responses before delivering the
        // same bytes here; late response fragments are dropped by its flush.
        for (const auto& event : stream_decoder_->flush().events)
            dispatch_input(event);
        return;
    }

    // Frozen raw-input listener stage (pi tui.ts handleInput inputListeners):
    // listeners observe the raw bytes in registration order and may consume
    // them or replace them for the rest of the chain. Consumption - and a
    // surviving empty text - stop typed dispatch without reaching the
    // downstream reply consumer either.
    const auto dispatched = raw_listeners_->dispatch(input);
    if (dispatched.consumed || dispatched.input.empty()) return;

    for (std::size_t offset = 0; offset < dispatched.input.size(); offset += kInputDecodeChunkBytes) {
        const auto chunk = std::string_view(dispatched.input).substr(offset, kInputDecodeChunkBytes);
        auto decoded = stream_decoder_->feed(chunk);
        apply_late_replies(terminal_, decoded.responses);
        for (const auto& event : decoded.events)
            dispatch_input(event);
    }
}

void MainScreen::dispatch_input(const InputEventVariant& event) {
    ChildOperation operation(*this);
    // Try overlays first (in reverse z-order, topmost first)
    if (compositor_->dispatch_input(event, render_pipeline_->committed_dimensions()) ==
            InputAdmissionOutcome::Consumed) {
        request_immediate_render();
        return;
    }

    // Fallback to focused base component
    auto* input_handler = dynamic_cast<InputHandler*>(focused_);
    if (input_handler == nullptr) {
        // Try first focusable for initial dispatch
        fallback_focus();
        input_handler = dynamic_cast<InputHandler*>(focused_);
        if (input_handler == nullptr) return;
    }
    static_cast<void>(input_handler->handle_input(event));
    request_immediate_render();
}

void MainScreen::handle_resize(TerminalDimensions) {
    if (!started_) return;
    invalidate();
}

void MainScreen::apply_focus(Component* component) {
    ChildOperation operation(*this);
    auto* previous = focused_;
    focused_ = component;
    if (auto* focusable = dynamic_cast<Focusable*>(previous)) focusable->set_focused(false);
    if (focused_ != component || !focus_target_available(component)) return;
    if (auto* focusable = dynamic_cast<Focusable*>(component)) focusable->set_focused(true);
}

bool MainScreen::focus_target_available(Component* component) const {
    if (component == nullptr) return false;
    if (owns(component)) return dynamic_cast<Focusable*>(component) != nullptr;
    return compositor_->focus_target_available(component, render_pipeline_->committed_dimensions());
}

void MainScreen::fallback_focus() {
    ChildOperation operation(*this);
    if (auto* target = find_focusable_target()) {
        auto* as_component = dynamic_cast<Component*>(target);
        if (as_component != focused_) apply_focus(as_component);
    }
}

Focusable* MainScreen::find_focusable_target() {
    // Look in visible overlays first (topmost first)
    if (auto* overlay = compositor_->topmost_focusable(render_pipeline_->committed_dimensions())) {
        return overlay;
    }

    // Fallback to first focusable base child
    for (const auto& child : children_) {
        auto* focusable = dynamic_cast<Focusable*>(child.get());
        if (focusable != nullptr) return focusable;
    }
    return nullptr;
}

std::optional<CursorPosition> MainScreen::resolve_cursor_location() const {
    if (rendered_children_valid_ && rendered_child_rows_.size() == children_.size()) {
        std::size_t row_offset = 0;
        for (std::size_t index = 0; index < children_.size(); ++index) {
            const auto& child = children_[index];
            if (const auto loc = child->cursor_location()) {
                return CursorPosition{.column = loc->column, .row = row_offset + loc->row};
            }
            row_offset += rendered_child_rows_[index];
        }
    }

    if (focused_ == nullptr) return std::nullopt;
    auto* focusable = dynamic_cast<Focusable*>(focused_);
    if (focusable == nullptr || !focusable->focused()) return std::nullopt;
    return focusable->cursor_location();
}

namespace detail::testing {

std::size_t frame_prepare_call_count(const MainScreen& tui) noexcept {
    return tui.render_pipeline_->frame_prepare_call_count();
}

} // namespace detail::testing

} // namespace cch::tui
