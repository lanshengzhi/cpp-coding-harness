#include "EditorCompletionSession.hpp"

#include <cch/support/Error.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace cch::tui::detail {
namespace {

[[nodiscard]] std::optional<std::size_t> best_match_index(
        const std::vector<AutocompleteItem>& items, std::string_view prefix) {
    if (prefix.empty()) return std::nullopt;
    std::optional<std::size_t> first_prefix_index;
    for (std::size_t index = 0; index < items.size(); ++index) {
        if (items[index].value == prefix) return index;
        if (!first_prefix_index && items[index].value.starts_with(prefix)) {
            first_prefix_index = index;
        }
    }
    return first_prefix_index;
}

void cancel_timer(const std::shared_ptr<AutocompleteDebounceTimer>& timer) noexcept {
    if (!timer) return;
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    try {
#endif
        timer->cancel();
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    } catch (...) {
        // Timer cancellation is best effort; it cannot veto a serialized edit.
    }
#endif
}

void request_stop(std::stop_source& source) noexcept {
    if (source.stop_possible()) source.request_stop();
}

struct RenderNotificationFrame {
    const void* control{nullptr};
    RenderNotificationFrame* previous{nullptr};
};

thread_local RenderNotificationFrame* current_render_notification = nullptr;

} // namespace

struct EditorCompletionSession::Impl {
    enum class MenuState { None, Regular, Force };

    struct PendingDelivery {
        std::size_t request_id{0};
        EditorCompletionIntent intent{};
        std::string snapshot_text{};
        EditorCursor snapshot_cursor{};
        std::optional<AutocompleteSuggestions> result{};
    };

    /// Callback-visible state is deliberately separate from the session PImpl.
    /// Provider and timer ownership stays in Impl, so a late callback cannot
    /// make either capability destruct on its own worker thread (#473).
    struct Control {
        explicit Control(EditorCompletionDueSink due_sink, EditorRenderRequestSink render_sink)
            : on_due(std::move(due_sink)), render_request(std::move(render_sink)) {}

        std::mutex mutex;
        std::mutex callback_mutex;
        std::condition_variable callback_quiesced;
        std::size_t active_render_notifications{0};
        bool closing{false};
        bool render_request_enabled{true};
        bool render_request_failed{false};
        bool open{true};
        std::size_t generation{0};
        std::size_t request_id{0};
        std::optional<std::size_t> started_generation{};
        std::optional<std::size_t> delivered_request_id{};
        std::stop_source request_stop_source{};
        std::optional<PendingDelivery> pending{};
        std::shared_ptr<AutocompleteProvider> provider{};
        std::vector<AutocompleteItem> items{};
        std::string prefix{};
        std::size_t selected{0};
        MenuState menu_state{MenuState::None};

        // These sinks are installed once and remain immutable while Control
        // is live. Every callback invokes them without holding mutex.
        EditorCompletionDueSink on_due;
        EditorRenderRequestSink render_request;

        [[nodiscard]] bool begin_render_notification() {
            std::lock_guard lock(callback_mutex);
            if (closing || !render_request_enabled) return false;
            ++active_render_notifications;
            return true;
        }

        void end_render_notification() noexcept {
            std::lock_guard lock(callback_mutex);
            if (active_render_notifications > 0) --active_render_notifications;
            if (active_render_notifications == 0) callback_quiesced.notify_all();
        }

        void disable_render_notification() noexcept {
            std::lock_guard lock(callback_mutex);
            render_request_enabled = false;
            render_request_failed = true;
        }

        void close_render_notifications() noexcept {
            std::unique_lock lock(callback_mutex);
            closing = true;
            for (auto* frame = current_render_notification; frame != nullptr; frame = frame->previous) {
                if (frame->control == this) return;
            }
            callback_quiesced.wait(lock, [this] { return active_render_notifications == 0; });
        }
    };

    Impl(std::unique_ptr<AutocompleteDebounceTimer> timer,
            EditorCompletionDueSink due_sink,
            EditorRenderRequestSink render_sink)
        : debounce_timer(std::shared_ptr<AutocompleteDebounceTimer>(std::move(timer))),
          control(std::make_shared<Control>(std::move(due_sink), std::move(render_sink))) {}

    std::shared_ptr<AutocompleteDebounceTimer> debounce_timer;
    std::shared_ptr<Control> control;
};

EditorCompletionSession::EditorCompletionSession(std::unique_ptr<AutocompleteDebounceTimer> debounce_timer,
        EditorCompletionDueSink on_due,
        EditorRenderRequestSink render_request)
    : impl_(std::make_unique<Impl>(std::move(debounce_timer), std::move(on_due), std::move(render_request))) {}

EditorCompletionSession::~EditorCompletionSession() { close(); }

EditorCompletionSession::EditorCompletionSession(EditorCompletionSession&&) noexcept = default;

EditorCompletionSession& EditorCompletionSession::operator=(EditorCompletionSession&& other) noexcept {
    if (this != &other) {
        close();
        impl_ = std::move(other.impl_);
    }
    return *this;
}

std::vector<std::string> EditorCompletionSession::set_provider(std::unique_ptr<AutocompleteProvider> provider) {
    if (!impl_ || !impl_->control) return {};
    cancel();
    const auto control = impl_->control;
    if (!provider) {
        std::shared_ptr<AutocompleteProvider> retired_provider;
        {
            std::lock_guard lock(control->mutex);
            retired_provider = std::move(control->provider);
        }
        retired_provider.reset();
        return {};
    }

    auto shared_provider = std::shared_ptr<AutocompleteProvider>(std::move(provider));
    auto trigger_characters = shared_provider->trigger_characters();
    std::shared_ptr<AutocompleteProvider> retired_provider;
    {
        std::lock_guard lock(control->mutex);
        if (control->open) {
            retired_provider = std::move(control->provider);
            control->provider = std::move(shared_provider);
        }
    }
    retired_provider.reset();
    return trigger_characters;
}

void EditorCompletionSession::cancel() noexcept {
    if (!impl_ || !impl_->control) return;
    const auto control = impl_->control;
    std::stop_source previous_stop_source;
    {
        std::lock_guard lock(control->mutex);
        if (!control->open) return;
        previous_stop_source = control->request_stop_source;
        control->request_stop_source = std::stop_source{};
        ++control->generation;
        ++control->request_id;
        control->started_generation.reset();
        control->delivered_request_id.reset();
        control->pending.reset();
        control->items.clear();
        control->prefix.clear();
        control->selected = 0;
        control->menu_state = Impl::MenuState::None;
    }
    request_stop(previous_stop_source);
    cancel_timer(impl_->debounce_timer);
}

void EditorCompletionSession::close() noexcept {
    if (!impl_ || !impl_->control) return;
    const auto control = impl_->control;
    control->close_render_notifications();
    std::shared_ptr<AutocompleteProvider> retired_provider;
    std::stop_source previous_stop_source;
    {
        std::lock_guard lock(control->mutex);
        if (control->open) {
            control->open = false;
            ++control->generation;
            ++control->request_id;
            previous_stop_source = control->request_stop_source;
            control->request_stop_source = std::stop_source{};
            control->started_generation.reset();
            control->delivered_request_id.reset();
            retired_provider = std::move(control->provider);
            control->pending.reset();
            control->items.clear();
            control->prefix.clear();
            control->selected = 0;
            control->menu_state = Impl::MenuState::None;
        }
    }
    retired_provider.reset();
    request_stop(previous_stop_source);
    cancel_timer(impl_->debounce_timer);
}

bool EditorCompletionSession::has_provider() const {
    if (!impl_ || !impl_->control) return false;
    std::lock_guard lock(impl_->control->mutex);
    return impl_->control->open && impl_->control->provider != nullptr;
}

bool EditorCompletionSession::should_trigger_file_completion(const EditorCompletionView& view) const {
    if (!impl_ || !impl_->control) return false;
    std::shared_ptr<AutocompleteProvider> provider;
    {
        std::lock_guard lock(impl_->control->mutex);
        if (!impl_->control->open) return false;
        provider = impl_->control->provider;
    }
    if (!provider) return false;
    // Do not hold the control mutex across provider code.
    return provider->should_trigger_file_completion(view.lines, view.cursor_line, view.cursor_column);
}

std::optional<EditorCompletionDue> EditorCompletionSession::request(
        EditorCompletionIntent intent, std::chrono::milliseconds debounce) {
    if (!impl_ || !impl_->control) return std::nullopt;
    const auto control = impl_->control;
    const auto timer = impl_->debounce_timer;
    std::stop_source previous_stop_source;
    EditorCompletionDue due;
    {
        std::lock_guard lock(control->mutex);
        if (!control->open || !control->provider) return std::nullopt;
        previous_stop_source = control->request_stop_source;
        control->request_stop_source = std::stop_source{};
        ++control->generation;
        ++control->request_id;
        control->started_generation.reset();
        control->delivered_request_id.reset();
        control->pending.reset();
        due = EditorCompletionDue{
                .generation = control->generation,
                .intent = intent,
        };
    }
    request_stop(previous_stop_source);
    cancel_timer(timer);

    if (debounce <= std::chrono::milliseconds{} || !timer) return due;

    const auto weak_control = std::weak_ptr<Impl::Control>{control};
    timer->start(debounce, [weak_control, due]() -> support::ExpectedVoid {
        if (const auto state = weak_control.lock()) {
            bool current = false;
            {
                std::lock_guard lock(state->mutex);
                current = state->open && state->generation == due.generation;
            }
            if (current && state->on_due) {
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
                try {
#endif
                    (void)state->on_due(due);
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
                } catch (...) {
                    // A stale or unavailable serialized entry is a benign drop.
                }
#endif
            }
        }
        return {};
    });
    return std::nullopt;
}

support::ExpectedVoid EditorCompletionSession::start(EditorCompletionStart start) {
    if (!impl_ || !impl_->control) return {};
    const auto control = impl_->control;
    std::shared_ptr<AutocompleteProvider> provider;
    std::size_t request_id = 0;
    std::stop_token stop_token;
    {
        std::lock_guard lock(control->mutex);
        if (!control->open || control->generation != start.due.generation ||
                (control->started_generation && *control->started_generation == start.due.generation)) {
            return {};
        }
        control->started_generation = start.due.generation;
        provider = control->provider;
        request_id = control->request_id;
        stop_token = control->request_stop_source.get_token();
    }
    if (!provider) return {};

    const auto snapshot_text = start.view.text;
    const auto snapshot_cursor = start.view.cursor;
    const auto intent = start.due.intent;
    AutocompleteRequest request{
            .lines = start.view.lines,
            .cursor_line = start.view.cursor_line,
            .cursor_column = start.view.cursor_column,
            .force = intent.force,
            .stop_token = stop_token,
    };
    const auto weak_control = std::weak_ptr<Impl::Control>{control};
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    try {
#endif
        provider->get_suggestions(request,
                [weak_control, request_id, snapshot_text, snapshot_cursor, intent](
                        std::optional<AutocompleteSuggestions> result) -> support::ExpectedVoid {
                    if (const auto state = weak_control.lock()) {
                        bool accepted = false;
                        {
                            std::lock_guard lock(state->mutex);
                            if (state->open && state->request_id == request_id &&
                                    (!state->delivered_request_id || *state->delivered_request_id != request_id)) {
                                state->delivered_request_id = request_id;
                                state->pending = Impl::PendingDelivery{
                                        .request_id = request_id,
                                        .intent = intent,
                                        .snapshot_text = snapshot_text,
                                        .snapshot_cursor = snapshot_cursor,
                                        .result = std::move(result),
                                };
                                accepted = true;
                            }
                        }
                        if (accepted && state->render_request && state->begin_render_notification()) {
                            RenderNotificationFrame frame{
                                    .control = state.get(),
                                    .previous = current_render_notification,
                            };
                            current_render_notification = &frame;
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
                            try {
#endif
                                if (auto requested = state->render_request(); !requested) {
                                    state->disable_render_notification();
                                }
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
                            } catch (...) {
                                // Rendering is a best-effort weak observer; deactivate it after failure.
                                state->disable_render_notification();
                            }
#endif
                            current_render_notification = frame.previous;
                            state->end_render_notification();
                        }
                    }
                    return {};
                });
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    } catch (...) {
        cancel();
        return std::unexpected(support::make_error(support::ErrorCode::Unknown,
                "Editor autocomplete provider failed",
                "the autocomplete provider threw an exception"));
    }
#endif
    return {};
}

std::optional<EditorCompletionApplication> EditorCompletionSession::drain(const EditorCompletionView& view) {
    if (!impl_ || !impl_->control) return std::nullopt;
    const auto control = impl_->control;
    Impl::PendingDelivery pending;
    {
        std::lock_guard lock(control->mutex);
        if (!control->open || !control->pending) return std::nullopt;
        pending = std::move(*control->pending);
        control->pending.reset();
    }

    // The result has already been accepted once into the pending slot. A
    // changed text/cursor snapshot is a benign stale drop and retains any
    // existing menu until the next serialized request resolves.
    if (pending.snapshot_text != view.text || pending.snapshot_cursor != view.cursor) {
        return std::nullopt;
    }
    if (!pending.result || pending.result->items.empty()) {
        cancel();
        return std::nullopt;
    }

    if (pending.intent.force && pending.intent.explicit_tab && pending.result->items.size() == 1) {
        std::shared_ptr<AutocompleteProvider> provider;
        {
            std::lock_guard lock(control->mutex);
            if (!control->open || control->request_id != pending.request_id) return std::nullopt;
            provider = control->provider;
        }
        if (!provider) {
            cancel();
            return std::nullopt;
        }
        const auto result = provider->apply_completion(view.lines,
                view.cursor_line,
                view.cursor_column,
                pending.result->items.front(),
                pending.result->prefix);
        cancel();
        return EditorCompletionApplication{
                .result = result,
                .prefix = pending.result->prefix,
                .notify = true,
        };
    }

    {
        std::lock_guard lock(control->mutex);
        if (!control->open || control->request_id != pending.request_id) return std::nullopt;
        control->prefix = pending.result->prefix;
        control->items = pending.result->items;
        if (const auto best = best_match_index(control->items, control->prefix)) {
            control->selected = *best;
        } else {
            control->selected = std::min(control->selected, control->items.size() - 1);
        }
        control->menu_state = pending.intent.force ? Impl::MenuState::Force : Impl::MenuState::Regular;
    }
    return std::nullopt;
}

bool EditorCompletionSession::open() const {
    if (!impl_ || !impl_->control) return false;
    std::lock_guard lock(impl_->control->mutex);
    return impl_->control->menu_state != Impl::MenuState::None;
}

bool EditorCompletionSession::forced() const {
    if (!impl_ || !impl_->control) return false;
    std::lock_guard lock(impl_->control->mutex);
    return impl_->control->menu_state == Impl::MenuState::Force;
}

std::vector<AutocompleteItem> EditorCompletionSession::items() const {
    if (!impl_ || !impl_->control) return {};
    std::lock_guard lock(impl_->control->mutex);
    return impl_->control->items;
}

std::size_t EditorCompletionSession::selected_index() const {
    if (!impl_ || !impl_->control) return 0;
    std::lock_guard lock(impl_->control->mutex);
    return impl_->control->selected;
}

bool EditorCompletionSession::prefix_starts_with(std::string_view prefix) const {
    if (!impl_ || !impl_->control) return false;
    std::lock_guard lock(impl_->control->mutex);
    return impl_->control->prefix.starts_with(prefix);
}

void EditorCompletionSession::move_selection(bool down) {
    if (!impl_ || !impl_->control) return;
    std::lock_guard lock(impl_->control->mutex);
    if (impl_->control->items.empty()) return;
    if (down) {
        impl_->control->selected = std::min(impl_->control->selected + 1, impl_->control->items.size() - 1);
    } else if (impl_->control->selected > 0) {
        --impl_->control->selected;
    }
}

std::optional<EditorCompletionApplication> EditorCompletionSession::accept(
        const EditorCompletionView& view, bool fallthrough_submit) {
    if (!impl_ || !impl_->control) return std::nullopt;
    const auto control = impl_->control;
    std::shared_ptr<AutocompleteProvider> provider;
    AutocompleteItem item;
    std::string prefix;
    {
        std::lock_guard lock(control->mutex);
        if (!control->open || !control->provider || control->items.empty()) return std::nullopt;
        provider = control->provider;
        item = control->items[control->selected];
        prefix = control->prefix;
    }

    const auto result = provider->apply_completion(view.lines, view.cursor_line, view.cursor_column, item, prefix);
    cancel();
    return EditorCompletionApplication{
            .result = result,
            .prefix = std::move(prefix),
            .notify = !fallthrough_submit,
    };
}

} // namespace cch::tui::detail
