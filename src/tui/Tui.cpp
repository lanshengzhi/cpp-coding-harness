#include <cch/tui/Tui.hpp>

#include "tui/InputDecoder.hpp"
#include "tui/OverlayCompositor.hpp"
#include "tui/RenderPipeline.hpp"
#include "tui/RenderUtils.hpp"

#include <cch/support/Error.hpp>
#include <algorithm>
#include <cstddef>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace cch::tui {
namespace {

constexpr std::size_t kInputDecodeChunkBytes = 4096;

[[nodiscard]] std::string describe_error(const support::Error& error) {
    if (error.detail.empty()) return error.message;
    return std::format("{} [{}]", error.message, error.detail);
}

[[nodiscard]] support::Error startup_rollback_error(
    const support::Error& startup,
    const support::Error& rollback) {
    return support::make_error(
        startup.code,
        "TUI startup failed and terminal restoration was incomplete",
        std::format(
            "startup: {}; restoration: {}",
            describe_error(startup),
            describe_error(rollback)));
}

/// The downstream consumer of the replies that survive the raw-input listener
/// stage (pi tui.ts onLateReply): a cell-size reply refines the terminal's
/// reported cell pixels here, after every listener has had its say. Consuming
/// or rewriting the bytes in a listener is what suppresses this update.
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

Tui::Tui(Terminal& terminal)
    : terminal_(terminal), stream_decoder_(std::make_unique<detail::TerminalStreamDecoder>()),
      compositor_(std::make_unique<detail::OverlayCompositor>()),
      render_pipeline_(std::make_unique<detail::RenderPipeline>(terminal)),
      raw_listeners_(std::make_shared<RawInputListenerChain>()) {}

Tui::~Tui() { (void)stop(); }

support::Expected<std::reference_wrapper<Component>> Tui::add_child(std::unique_ptr<Component> component) {
    return detail::attach_child(children_, std::move(component), "");
}

support::Expected<std::reference_wrapper<Overlay>> Tui::add_overlay(std::unique_ptr<Overlay> overlay) {
    return compositor_->add_overlay(std::move(overlay));
}

RawInputListenerHandle Tui::add_raw_input_listener(RawInputListener listener) {
    return raw_listeners_->add_owned(std::move(listener));
}

support::ExpectedVoid Tui::remove_raw_input_listener(RawInputListenerId id) {
    if (!raw_listeners_->remove(id)) {
        return std::unexpected(support::make_error(
                support::ErrorCode::Validation, "Raw-input listener is not registered with this TUI"));
    }
    return {};
}

support::ExpectedVoid Tui::remove_overlay(Overlay* overlay) {
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

support::ExpectedVoid Tui::hide_overlay(Overlay* overlay) {
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

support::ExpectedVoid Tui::restore_overlay(Overlay* overlay) {
    if (overlay == nullptr) return {};
    if (!compositor_->owns(overlay)) {
        return std::unexpected(
                support::make_error(support::ErrorCode::Validation, "Overlay is not attached to this TUI"));
    }

    overlay->set_visible(true);
    invalidate();
    return {};
}

support::ExpectedVoid Tui::start() {
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
    return {};
}

support::ExpectedVoid Tui::stop() {
    if (!started_) return {};

    started_ = false;
    const auto stop_cursor = resolve_cursor_location();
    if (auto* focusable = dynamic_cast<Focusable*>(focused_)) {
        focusable->set_focused(false);
    }
    focused_ = nullptr;
    compositor_->clear_focus_history();

    const auto exit_result = render_pipeline_->stop(stop_cursor);
    const auto cursor_result = terminal_.set_cursor_visible(true);
    stream_decoder_->reset();
    pending_render_ = false;
    const auto stop_result = terminal_.stop();

    if (!exit_result) return std::unexpected(exit_result.error());
    if (!cursor_result) return std::unexpected(cursor_result.error());
    if (!stop_result) return std::unexpected(stop_result.error());
    return {};
}

support::ExpectedVoid Tui::clear_screen() {
    if (!started_) {
        return std::unexpected(
                support::make_error(support::ErrorCode::Validation, "TUI must be started before clearing the screen"));
    }
    if (auto cleared = render_pipeline_->clear_screen(); !cleared) {
        return std::unexpected(cleared.error());
    }
    return {};
}

support::ExpectedVoid Tui::render() {
    if (!started_) {
        return std::unexpected(
                support::make_error(support::ErrorCode::Validation, "TUI must be started before rendering"));
    }

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
    if (result) {
        pending_render_ = false;
    }
    return result;
}

support::Expected<RenderResult> Tui::render_children(TerminalDimensions dimensions, bool prepare_rows) {
    RenderResult output;
    for (const auto& child : children_) {
        if (auto* viewport_aware = dynamic_cast<ViewportAware*>(child.get())) {
            viewport_aware->set_available_height(dimensions.rows);
        }
        auto rendered = child->render(dimensions.columns);
        if (!rendered) return std::unexpected(rendered.error());
        const auto row_offset = output.lines.size();
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
    return output;
}

support::ExpectedVoid Tui::set_focus(Component* component) {
    if (component != nullptr && !owns(component)) {
        // Check if component is inside an overlay
        auto* overlay = dynamic_cast<Overlay*>(component);
        if (overlay == nullptr || !compositor_->owns(overlay)) {
            return std::unexpected(support::make_error(
                support::ErrorCode::Validation,
                "TUI focus target is not attached to this root"));
        }
    }
    if (component != nullptr && dynamic_cast<Focusable*>(component) == nullptr) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "TUI focus target does not participate in focus"));
    }

    if (auto* overlay = dynamic_cast<Overlay*>(component); overlay != nullptr && component != focused_) {
        compositor_->remember_focus(overlay, focused_);
    }
    apply_focus(component);
    return {};
}

void Tui::set_render_request_sink(TuiRenderRequestSink sink) { render_request_sink_ = std::move(sink); }

void Tui::invalidate() {
    const bool request_render = started_ && !pending_render_;
    pending_render_ = true;
    for (const auto& child : children_) {
        child->invalidate();
    }
    compositor_->invalidate_all();
    if (request_render && render_request_sink_) {
        (void)render_request_sink_();
    }
}

bool Tui::owns(const Component* component) const {
    for (const auto& child : children_) {
        if (child.get() == component) {
            return true;
        }
    }
    return false;
}

void Tui::handle_input(std::string input) {
    if (!started_) return;
    if (input.empty()) {
        // ProcessTerminal applies out-of-band responses before delivering the
        // same bytes here; late response fragments are dropped by its flush.
        for (const auto& event : stream_decoder_->flush().events) dispatch_input(event);
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

void Tui::dispatch_input(const InputEventVariant& event) {
    // Try overlays first (in reverse z-order, topmost first)
    if (compositor_->dispatch_input(event, render_pipeline_->committed_dimensions()) ==
            InputAdmissionOutcome::Consumed) {
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
}

void Tui::handle_resize(TerminalDimensions) {
    if (!started_) return;
    invalidate();
}

void Tui::apply_focus(Component* component) {
    if (auto* previous = dynamic_cast<Focusable*>(focused_)) previous->set_focused(false);
    focused_ = component;
    if (auto* next = dynamic_cast<Focusable*>(focused_)) next->set_focused(true);
}

bool Tui::focus_target_available(Component* component) const {
    if (component == nullptr) return false;
    if (owns(component)) return dynamic_cast<Focusable*>(component) != nullptr;
    return compositor_->focus_target_available(component, render_pipeline_->committed_dimensions());
}

void Tui::fallback_focus() {
    if (auto* target = find_focusable_target()) {
        auto* as_component = dynamic_cast<Component*>(target);
        if (as_component != focused_) apply_focus(as_component);
    }
}

Focusable* Tui::find_focusable_target() {
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

std::optional<CursorPosition> Tui::resolve_cursor_location() const {
    if (focused_ == nullptr) return std::nullopt;
    auto* focusable = dynamic_cast<Focusable*>(focused_);
    if (focusable == nullptr) return std::nullopt;
    if (!focusable->focused()) return std::nullopt;
    return focusable->cursor_location();
}

namespace detail::testing {

std::size_t frame_prepare_call_count(const Tui& tui) noexcept {
    return tui.render_pipeline_->frame_prepare_call_count();
}

} // namespace detail::testing

} // namespace cch::tui
