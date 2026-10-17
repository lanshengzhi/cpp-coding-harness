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

} // namespace

struct MainScreen::RenderScheduleState {
    MainScreen* owner{nullptr};
};

OverlayHandle::OverlayHandle(std::weak_ptr<detail::OverlayHandleContext> context,
        std::shared_ptr<detail::OverlayHandleState> state,
        Overlay* overlay) noexcept
    : context_(std::move(context)), state_(std::move(state)), overlay_(overlay) {}

void OverlayHandle::hide() const {
    if (const auto context = context_.lock(); context && context->owner != nullptr)
        context->owner->remove_overlay_internal(overlay_);
}

void OverlayHandle::set_hidden(bool hidden) const {
    if (const auto context = context_.lock(); context && context->owner != nullptr)
        context->owner->hide_overlay_internal(overlay_, hidden);
}

bool OverlayHandle::is_hidden() const { return state_ == nullptr || state_->hidden; }

void OverlayHandle::focus() const {
    if (const auto context = context_.lock(); context && context->owner != nullptr)
        context->owner->focus_overlay_internal(overlay_);
}

void OverlayHandle::unfocus(std::optional<Component*> target) const {
    if (const auto context = context_.lock(); context && context->owner != nullptr)
        context->owner->unfocus_overlay_internal(overlay_, target);
}

bool OverlayHandle::is_focused() const {
    const auto context = context_.lock();
    return context && context->owner != nullptr && context->owner->is_overlay_focused_internal(overlay_);
}

std::optional<OverlayBounds> OverlayHandle::get_bounds() const {
    const auto context = context_.lock();
    if (!context || context->owner == nullptr || is_hidden()) return std::nullopt;
    return context->owner->overlay_bounds_internal(overlay_);
}

MainScreen::MainScreen(Terminal& terminal, bool show_hardware_cursor)
    : detail::TuiHost(terminal), render_schedule_state_(std::make_shared<RenderScheduleState>()),
      overlay_handle_context_(std::make_shared<detail::OverlayHandleContext>()),
      compositor_(std::make_unique<detail::OverlayCompositor>()),
      render_pipeline_(std::make_unique<detail::RenderPipeline>(terminal, show_hardware_cursor)) {
    render_schedule_state_->owner = this;
    overlay_handle_context_->owner = this;
}

bool MainScreen::show_hardware_cursor() const noexcept { return render_pipeline_->show_hardware_cursor(); }

void MainScreen::set_show_hardware_cursor(bool enabled) {
    if (render_pipeline_->show_hardware_cursor() == enabled) return;
    render_pipeline_->set_show_hardware_cursor(enabled);
    if (!enabled && started_ && !stopping_) {
        (void)terminal_.set_cursor_visible(false);
    }
    request_render();
}

MainScreen::~MainScreen() {
    (void)stop();
    render_schedule_state_->owner = nullptr;
    overlay_handle_context_->owner = nullptr;
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
    const bool was_blocker = overlay_focus_restore_.status == OverlayFocusRestoreStatus::Blocked &&
                             overlay_focus_restore_.blocked_by == component;
    const auto restore_state = overlay_focus_restore_;
    compositor_->forget_component(component);
    if (was_blocker) {
        overlay_focus_restore_ = {};
        if (restore_state.resume_to_overlay && compositor_->owns(restore_state.overlay)) {
            set_focus_internal(restore_state.overlay, false);
        } else if (!restore_state.resume_to_overlay && restore_state.resume_target != component &&
                   focus_target_available(restore_state.resume_target)) {
            set_focus_internal(restore_state.resume_target, false);
        } else if (focused_ == component) {
            apply_focus(nullptr);
        }
    } else if (overlay_focus_restore_.status == OverlayFocusRestoreStatus::Blocked &&
               !overlay_focus_restore_.resume_to_overlay && overlay_focus_restore_.resume_target == component) {
        overlay_focus_restore_.resume_target = nullptr;
    }
    retired_children_.push_back(std::move(*found));
    children_.erase(found);
    rendered_child_rows_.clear();
    rendered_children_valid_ = false;
    if (was_focused && !was_blocker) {
        set_focus_internal(nullptr, false);
        if (focused_ == nullptr) fallback_focus();
    }
    request_render();
    return {};
}

void MainScreen::clear_children() { static_cast<void>(take_children()); }

std::vector<std::unique_ptr<Component>> MainScreen::take_children() {
    ChildOperation operation(*this);
    const bool was_focused = focused_ != nullptr && owns(focused_);
    const bool was_blocker = overlay_focus_restore_.status == OverlayFocusRestoreStatus::Blocked &&
                             overlay_focus_restore_.blocked_by != nullptr && owns(overlay_focus_restore_.blocked_by);
    const auto restore_state = overlay_focus_restore_;
    const bool removed_restore_target =
            restore_state.status == OverlayFocusRestoreStatus::Blocked && !restore_state.resume_to_overlay &&
            std::any_of(children_.begin(), children_.end(), [&restore_state](const auto& child) {
                return child.get() == restore_state.resume_target;
            });
    for (const auto& child : children_)
        compositor_->forget_component(child.get());
    if (was_blocker) {
        overlay_focus_restore_ = {};
        if (restore_state.resume_to_overlay && compositor_->owns(restore_state.overlay)) {
            set_focus_internal(restore_state.overlay, false);
        } else if (!restore_state.resume_to_overlay && !removed_restore_target &&
                   focus_target_available(restore_state.resume_target)) {
            set_focus_internal(restore_state.resume_target, false);
        } else if (focused_ != nullptr && owns(focused_)) {
            apply_focus(nullptr);
        }
    } else if (removed_restore_target) {
        overlay_focus_restore_.resume_target = nullptr;
    }
    auto taken = std::move(children_);
    children_.clear();
    rendered_child_rows_.clear();
    rendered_children_valid_ = false;
    if (was_focused && !was_blocker) set_focus_internal(nullptr, false);
    request_render();
    return taken;
}

bool MainScreen::has_overlay_entries() const noexcept { return compositor_->has_overlay_entries(); }

void MainScreen::clear_overlays() {
    while (compositor_->has_overlay_entries()) {
        auto* overlay = compositor_->topmost_overlay();
        if (overlay == nullptr) break;
        remove_overlay_internal(overlay);
    }
}

support::Expected<OverlayHandle> MainScreen::show_overlay(std::unique_ptr<Overlay> overlay) {
    if (!overlay) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation, "TUI cannot attach a null Overlay"));
    }
    auto* component = overlay.get();
    auto state = std::make_shared<detail::OverlayHandleState>();
    if (auto attached = compositor_->add_overlay(std::move(overlay), focused_, ++focus_order_counter_, state);
            !attached) {
        return std::unexpected(attached.error());
    }
    auto handle = OverlayHandle{overlay_handle_context_, std::move(state), component};
    if (!component->options().non_capturing && compositor_->visible(component, terminal_.dimensions()))
        set_focus_internal(component, false);
    request_render();
    return handle;
}

support::Expected<std::reference_wrapper<Overlay>> MainScreen::add_overlay(std::unique_ptr<Overlay> overlay) {
    if (!overlay) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation, "TUI cannot attach a null Overlay"));
    }
    auto attached = compositor_->add_overlay(std::move(overlay), focused_, ++focus_order_counter_);
    if (attached && !attached->get().options().non_capturing &&
            compositor_->visible(&attached->get(), terminal_.dimensions()))
        set_focus_internal(&attached->get(), false);
    if (attached) request_render();
    return attached;
}

RawInputListenerHandle MainScreen::add_raw_input_listener(RawInputListener listener) {
    return TuiHost::add_raw_input_listener(std::move(listener));
}

support::ExpectedVoid MainScreen::remove_raw_input_listener(RawInputListenerId id) {
    return TuiHost::remove_raw_input_listener(id);
}

support::ExpectedVoid MainScreen::remove_overlay(Overlay* overlay) {
    if (overlay == nullptr) return {};
    if (!compositor_->owns(overlay)) {
        return std::unexpected(
                support::make_error(support::ErrorCode::Validation, "Overlay is not attached to this TUI"));
    }
    remove_overlay_internal(overlay);
    return {};
}

void MainScreen::remove_overlay_internal(Overlay* overlay) {
    if (overlay == nullptr || !compositor_->owns(overlay)) return;
    const auto pre_focus = compositor_->pre_focus(overlay);
    const bool was_focused = focused_ == overlay;
    const auto restore_state = overlay_focus_restore_;
    const bool was_blocker =
            restore_state.status == OverlayFocusRestoreStatus::Blocked && restore_state.blocked_by == overlay;
    const bool restore_another_overlay = was_blocker && restore_state.resume_to_overlay &&
                                         restore_state.overlay != overlay && compositor_->owns(restore_state.overlay);
    compositor_->retarget_pre_focus(overlay, pre_focus);
    clear_overlay_restore_for(overlay);
    if (was_focused) apply_focus(nullptr);
    static_cast<void>(compositor_->remove(overlay));

    if (restore_another_overlay) {
        overlay_focus_restore_ = restore_state;
        overlay_focus_restore_.blocked_by = nullptr;
        set_focus_internal(restore_state.overlay, true);
    } else if (was_blocker) {
        overlay_focus_restore_ = restore_state;
        overlay_focus_restore_.blocked_by = nullptr;
        auto* resume_target = restore_state.resume_to_overlay ? static_cast<Component*>(restore_state.overlay)
                                                              : restore_state.resume_target;
        if (!restore_state.resume_to_overlay || compositor_->owns(restore_state.overlay))
            set_focus_internal(resume_target, true);
        else
            overlay_focus_restore_ = {};
    } else if (was_focused) {
        auto* next = compositor_->topmost_focusable(terminal_.dimensions());
        auto* target = next != nullptr ? static_cast<Component*>(next) : pre_focus;
        set_focus_internal(target, false);
        if (focused_ == nullptr) fallback_focus();
    }
    request_render();
}

support::ExpectedVoid MainScreen::hide_overlay(Overlay* overlay) {
    if (overlay == nullptr) return {};
    if (!compositor_->owns(overlay)) {
        return std::unexpected(
                support::make_error(support::ErrorCode::Validation, "Overlay is not attached to this TUI"));
    }
    overlay->set_visible(false);
    hide_overlay_internal(overlay, true);
    return {};
}

void MainScreen::hide_overlay_internal(Overlay* overlay, bool hidden) {
    if (!compositor_->owns(overlay) || compositor_->is_hidden(overlay) == hidden) return;
    const bool was_focused = focused_ == overlay;
    static_cast<void>(compositor_->set_hidden(overlay, hidden));
    if (hidden) {
        const bool restore_was_blocked = overlay_focus_restore_.status == OverlayFocusRestoreStatus::Blocked &&
                                         overlay_focus_restore_.overlay == overlay &&
                                         focused_ == overlay_focus_restore_.blocked_by;
        const auto restore_state = overlay_focus_restore_;
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
                overlay_focus_restore_ = restore_state;
                overlay_focus_restore_.blocked_by = focused_;
            }
        }
    } else if (!overlay->options().non_capturing && compositor_->visible(overlay, terminal_.dimensions())) {
        compositor_->set_focus_order(overlay, ++focus_order_counter_);
        set_focus_internal(overlay, false);
    }
    invalidate();
}

support::ExpectedVoid MainScreen::restore_overlay(Overlay* overlay) {
    if (overlay == nullptr) return {};
    if (!compositor_->owns(overlay)) {
        return std::unexpected(
                support::make_error(support::ErrorCode::Validation, "Overlay is not attached to this TUI"));
    }
    overlay->set_visible(true);
    hide_overlay_internal(overlay, false);
    return {};
}

void MainScreen::focus_overlay_internal(Overlay* overlay) {
    if (!compositor_->owns(overlay) || !compositor_->visible(overlay, terminal_.dimensions()) ||
            overlay->options().non_capturing)
        return;
    compositor_->set_focus_order(overlay, ++focus_order_counter_);
    set_focus_internal(overlay, false);
    request_render();
}

bool MainScreen::is_overlay_focused_internal(const Overlay* overlay) const {
    return compositor_->owns(overlay) && focused_ == overlay;
}

std::optional<OverlayBounds> MainScreen::overlay_bounds_internal(const Overlay* overlay) const {
    if (!compositor_->owns(overlay)) return std::nullopt;
    const auto dimensions = terminal_.dimensions();
    if (!compositor_->visible(overlay, dimensions)) return std::nullopt;
    return compositor_->bounds(overlay);
}

void MainScreen::unfocus_overlay_internal(Overlay* overlay, std::optional<Component*> target) {
    if (!compositor_->owns(overlay)) return;
    const bool is_focused = focused_ == overlay;
    const bool pending = overlay_focus_restore_.status != OverlayFocusRestoreStatus::Inactive &&
                         overlay_focus_restore_.overlay == overlay;
    if (!is_focused && !pending) return;

    if (overlay_focus_restore_.status == OverlayFocusRestoreStatus::Blocked &&
            overlay_focus_restore_.overlay == overlay && focused_ == overlay_focus_restore_.blocked_by) {
        if (target) {
            overlay_focus_restore_.resume_to_overlay = false;
            overlay_focus_restore_.resume_target = *target;
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

support::ExpectedVoid MainScreen::start() {
    if (started_) {
        return {};
    }

    // The latch is armed before the terminal starts: ProcessTerminal
    // forwards startup-preserved input synchronously inside start(), and
    // dropping it on the not-yet-started guard would lose keystrokes typed
    // during the capability probes (#610). A focus target is established
    // first so those initial keystrokes reach a component; dispatch itself
    // stays strictly focused-only afterwards (#982).
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
        render_pipeline_->reset_render_state();
        return std::unexpected(result.error());
    }

    if (auto result = terminal_.set_cursor_visible(false); !result) {
        started_ = false;
        render_pipeline_->reset_render_state();
        if (auto stopped = terminal_.stop(); !stopped) {
            return std::unexpected(startup_rollback_error(result.error(), stopped.error()));
        }
        return std::unexpected(result.error());
    }

    render_pipeline_->start(terminal_.dimensions());
    pending_render_ = false;
    if (color_scheme_notifications_enabled_) {
        (void)terminal_.write("\x1b[?2031h");
    }
    if (scheduler_ != nullptr) last_render_at_.reset();
    request_render();
    return {};
}

support::ExpectedVoid MainScreen::stop(TuiStopOptions options) {
    if (!started_ || stopping_) return {};

    cancel_scheduled_render();
    ++render_generation_;
    render_scheduled_ = false;
    ChildOperation operation(*this);
    stopping_ = true;
    started_ = false;
    const auto stop_cursor = resolve_cursor_location();
    apply_focus(nullptr);
    overlay_focus_restore_ = {};

    if (color_scheme_notifications_enabled_) {
        (void)terminal_.write("\x1b[?2031l");
    }

    cancel_color_queries(options.preserve_pending_color_query_completion);

    const auto exit_result = render_pipeline_->stop(stop_cursor, options);
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

TuiMainScreenRenderState MainScreen::capture_render_state() const { return render_pipeline_->capture_render_state(); }

support::ExpectedVoid MainScreen::restore_render_state(TuiMainScreenRenderState state) {
    if (started_) {
        return std::unexpected(support::make_error(
                support::ErrorCode::Validation, "Cannot restore render state while the TUI is running"));
    }
    return render_pipeline_->restore_render_state(std::move(state));
}

void MainScreen::reset_render_state() noexcept { render_pipeline_->reset_render_state(); }

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
    reconcile_overlay_focus(dimensions);
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
    const bool has_overlay_entries = compositor_->has_overlay_entries();
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
    const auto cursor_location = materialized.cursor ? materialized.cursor : resolve_cursor_location();
    materialized.cursor.reset();
    auto result = render_pipeline_->render(std::move(materialized),
            dimensions,
            capabilities,
            compose_overlays,
            has_overlay_entries,
            clear_on_shrink_,
            cursor_location);
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
    if (auto* overlay = dynamic_cast<Overlay*>(component); overlay != nullptr && !focus_target_available(component)) {
        return std::unexpected(
                support::make_error(support::ErrorCode::Validation, "TUI overlay focus target is not visible"));
    }

    // Focusing the already-focused target is a no-op: re-applying focus
    // would emit spurious unfocus/focus transitions (upstream parity).
    if (component == focused_) return {};

    set_focus_internal(component, false);
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

std::size_t MainScreen::full_redraws() const noexcept {
    return render_pipeline_ != nullptr ? render_pipeline_->full_redraws() : 0;
}

TuiWriteAccounting MainScreen::write_accounting() const noexcept {
    return render_pipeline_ != nullptr ? render_pipeline_->write_accounting() : TuiWriteAccounting{};
}

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
    TuiHost::handle_input(std::move(input), [this](const InputEventVariant& event) { dispatch_input(event); });
}

void MainScreen::dispatch_input(const InputEventVariant& event) {
    ChildOperation operation(*this);
    reconcile_overlay_focus(terminal_.dimensions());
    auto* input_handler = dynamic_cast<InputHandler*>(focused_);
    if (input_handler == nullptr) return;
    static_cast<void>(input_handler->handle_input(event));
    request_immediate_render();
}

void MainScreen::handle_resize(TerminalDimensions dimensions) {
    if (!started_) return;
    reconcile_overlay_focus(dimensions);
    invalidate();
}

void MainScreen::apply_focus(Component* component) {
    TuiHost::apply_focus(component, [this](Component* candidate) { return focus_target_available(candidate); });
}

void MainScreen::set_focus_internal(Component* component, bool preserve_restore) {
    const auto previous = focused_;
    const auto restore_before = overlay_focus_restore_;
    const auto is_overlay = [this](Component* candidate) {
        return dynamic_cast<Overlay*>(candidate) != nullptr && compositor_->owns(dynamic_cast<Overlay*>(candidate));
    };
    auto* previous_overlay = is_overlay(previous) ? dynamic_cast<Overlay*>(previous) : nullptr;
    auto* next_overlay = is_overlay(component) ? dynamic_cast<Overlay*>(component) : nullptr;

    if (component != nullptr && next_overlay == nullptr) {
        if (overlay_focus_restore_.status == OverlayFocusRestoreStatus::Blocked &&
                overlay_focus_restore_.blocked_by == previous) {
            if (!overlay_focus_restore_.resume_to_overlay) {
                component = overlay_focus_restore_.resume_target;
                overlay_focus_restore_ = {};
            } else if (previous == nullptr || !owns(previous)) {
                component = resolve_overlay_restore();
            } else {
                overlay_focus_restore_.blocked_by = component;
            }
        } else if (!preserve_restore && previous_overlay != nullptr &&
                   overlay_focus_restore_.status == OverlayFocusRestoreStatus::Eligible &&
                   overlay_focus_restore_.overlay == previous_overlay) {
            overlay_focus_restore_ = OverlayFocusRestoreState{
                    .status = OverlayFocusRestoreStatus::Blocked,
                    .overlay = previous_overlay,
                    .blocked_by = component,
            };
        }
    } else if (component == nullptr) {
        if (overlay_focus_restore_.status == OverlayFocusRestoreStatus::Blocked &&
                overlay_focus_restore_.blocked_by == previous) {
            component = resolve_overlay_restore();
        } else if (overlay_focus_restore_.status == OverlayFocusRestoreStatus::Blocked &&
                   !overlay_focus_restore_.resume_to_overlay && previous == overlay_focus_restore_.resume_target) {
            overlay_focus_restore_ = {};
        } else if (!preserve_restore) {
            overlay_focus_restore_ = {};
        }
    }

    apply_focus(component);
    if (auto* focused_overlay = dynamic_cast<Overlay*>(focused_);
            focused_overlay != nullptr && compositor_->visible(focused_overlay, terminal_.dimensions())) {
        overlay_focus_restore_ = OverlayFocusRestoreState{
                .status = OverlayFocusRestoreStatus::Eligible,
                .overlay = focused_overlay,
        };
    } else if (preserve_restore && restore_before.status != OverlayFocusRestoreStatus::Inactive &&
               overlay_focus_restore_.status == OverlayFocusRestoreStatus::Inactive) {
        overlay_focus_restore_ = restore_before;
    }
}

void MainScreen::clear_overlay_restore_for(Overlay* overlay) {
    if (overlay_focus_restore_.status != OverlayFocusRestoreStatus::Inactive &&
            overlay_focus_restore_.overlay == overlay)
        overlay_focus_restore_ = {};
}

Component* MainScreen::resolve_overlay_restore() {
    if (overlay_focus_restore_.status != OverlayFocusRestoreStatus::Blocked) return nullptr;
    if (!overlay_focus_restore_.resume_to_overlay) {
        auto* target = overlay_focus_restore_.resume_target;
        overlay_focus_restore_ = {};
        return target;
    }
    auto* overlay = overlay_focus_restore_.overlay;
    if (compositor_->owns(overlay) && compositor_->visible(overlay, terminal_.dimensions())) return overlay;
    overlay_focus_restore_ = {};
    return nullptr;
}

void MainScreen::reconcile_overlay_focus(TerminalDimensions dimensions) {
    auto* focused_overlay = dynamic_cast<Overlay*>(focused_);
    if (focused_overlay != nullptr && compositor_->owns(focused_overlay) &&
            !compositor_->visible(focused_overlay, dimensions)) {
        auto* replacement = compositor_->topmost_focusable(dimensions);
        if (replacement != nullptr) {
            set_focus_internal(replacement, true);
            overlay_focus_restore_ = OverlayFocusRestoreState{
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
            overlay_focus_restore_ = OverlayFocusRestoreState{
                    .status = OverlayFocusRestoreStatus::Eligible,
                    .overlay = focused_overlay,
            };
        }
    }

    if (auto* focused_overlay = dynamic_cast<Overlay*>(focused_);
            focused_overlay != nullptr && compositor_->owns(focused_overlay)) {
        if (overlay_focus_restore_.status == OverlayFocusRestoreStatus::Blocked &&
                overlay_focus_restore_.blocked_by == focused_overlay &&
                compositor_->owns(overlay_focus_restore_.overlay) &&
                compositor_->visible(overlay_focus_restore_.overlay, dimensions)) {
            set_focus_internal(overlay_focus_restore_.overlay, true);
        }
        return;
    }
    if ((overlay_focus_restore_.status == OverlayFocusRestoreStatus::Eligible ||
                (overlay_focus_restore_.status == OverlayFocusRestoreStatus::Blocked &&
                        overlay_focus_restore_.resume_to_overlay &&
                        !compositor_->visible(overlay_focus_restore_.overlay, dimensions))) &&
            compositor_->owns(overlay_focus_restore_.overlay) &&
            compositor_->visible(overlay_focus_restore_.overlay, dimensions)) {
        set_focus_internal(overlay_focus_restore_.overlay, true);
    } else if (overlay_focus_restore_.status == OverlayFocusRestoreStatus::Blocked &&
               overlay_focus_restore_.blocked_by != focused_) {
        if (overlay_focus_restore_.resume_to_overlay) {
            if (compositor_->owns(overlay_focus_restore_.overlay) &&
                    compositor_->visible(overlay_focus_restore_.overlay, dimensions))
                set_focus_internal(overlay_focus_restore_.overlay, true);
        } else {
            auto* target = resolve_overlay_restore();
            set_focus_internal(target, true);
        }
    }
}

bool MainScreen::focus_target_available(Component* component) const {
    if (component == nullptr) return false;
    if (owns(component)) return dynamic_cast<Focusable*>(component) != nullptr;
    return compositor_->focus_target_available(component, terminal_.dimensions());
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
    if (auto* overlay = compositor_->topmost_focusable(terminal_.dimensions())) {
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

TerminalColorSchemeListenerHandle MainScreen::on_terminal_color_scheme_change(TerminalColorSchemeListener listener) {
    return TuiHost::on_terminal_color_scheme_change(std::move(listener));
}

void MainScreen::set_terminal_color_scheme_notifications(bool enabled) {
    TuiHost::set_terminal_color_scheme_notifications(enabled, started_, stopping_);
}

std::future<TerminalColors> MainScreen::query_terminal_colors(TerminalColorsQueryOptions options) {
    return TuiHost::query_terminal_colors(std::move(options));
}

namespace detail::testing {

std::size_t frame_prepare_call_count(const MainScreen& tui) noexcept {
    return tui.render_pipeline_->frame_prepare_call_count();
}

} // namespace detail::testing

} // namespace cch::tui
