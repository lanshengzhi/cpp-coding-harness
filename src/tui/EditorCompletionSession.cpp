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
#include <type_traits>
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

#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
[[nodiscard]] support::Error provider_failure(std::string detail) {
    return support::make_error(support::ErrorCode::Unknown, "Editor autocomplete provider failed", std::move(detail));
}
#endif

} // namespace

struct EditorCompletionSession::Impl {
    enum class MenuState { None, Regular, Force };

    struct PendingDue {
        std::size_t generation{0};
        EditorCompletionIntent intent{};
        bool ready{false};
    };

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
        explicit Control(EditorCompletionWakeSink wake_sink, EditorRenderRequestSink render_sink)
            : on_wake(std::move(wake_sink)), render_request(std::move(render_sink)) {}

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
        std::optional<PendingDue> due{};
        std::optional<PendingDelivery> pending{};
        std::shared_ptr<AutocompleteProvider> provider{};
        std::vector<AutocompleteItem> items{};
        std::string prefix{};
        std::size_t selected{0};
        MenuState menu_state{MenuState::None};

        // These sinks are installed once and remain immutable while Control
        // is live. Every callback invokes them without holding mutex.
        EditorCompletionWakeSink on_wake;
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

        void clear_menu_locked() {
            items.clear();
            prefix.clear();
            selected = 0;
            menu_state = MenuState::None;
        }

        [[nodiscard]] EditorCompletionEffect effect_locked() const {
            return EditorCompletionEffect{
                    .menu =
                            EditorCompletionMenuPresentation{
                                    .open = menu_state != MenuState::None,
                                    .forced = menu_state == MenuState::Force,
                                    .items = items,
                                    .prefix = prefix,
                                    .selected_index = selected,
                            },
                    .application = std::nullopt,
                    .submit = false,
            };
        }
    };

    struct RequestAdmission {
        std::size_t generation{0};
        std::size_t request_id{0};
        EditorCompletionIntent intent{};
        bool start_immediately{false};
    };

    static void cancel_current_lifecycle(
            const std::shared_ptr<Control>& control, const std::shared_ptr<AutocompleteDebounceTimer>& timer) noexcept;
    [[nodiscard]] static std::optional<RequestAdmission> admit_request(const std::shared_ptr<Control>& control,
            const std::shared_ptr<AutocompleteDebounceTimer>& timer,
            EditorCompletionRefresh refresh);
    [[nodiscard]] static support::Expected<EditorCompletionEffect> drain_pending(
            const std::shared_ptr<Control>& control,
            const std::shared_ptr<AutocompleteDebounceTimer>& timer,
            const EditorCompletionView& view);
    [[nodiscard]] static support::Expected<EditorCompletionEffect> start_request(
            const std::shared_ptr<Control>& control,
            const std::shared_ptr<AutocompleteDebounceTimer>& timer,
            const RequestAdmission& admission,
            const EditorCompletionView& view);
    [[nodiscard]] static support::Expected<EditorCompletionEffect> handle_refresh(
            const std::shared_ptr<Control>& control,
            const std::shared_ptr<AutocompleteDebounceTimer>& timer,
            EditorCompletionRefresh refresh,
            const EditorCompletionView& view);
    [[nodiscard]] static support::Expected<EditorCompletionEffect> handle_wake(const std::shared_ptr<Control>& control,
            const std::shared_ptr<AutocompleteDebounceTimer>& timer,
            const EditorCompletionView& view);
    [[nodiscard]] static support::Expected<EditorCompletionEffect> handle_menu(const std::shared_ptr<Control>& control,
            const std::shared_ptr<AutocompleteDebounceTimer>& timer,
            EditorCompletionMenuInteraction interaction,
            const EditorCompletionView& view);

    Impl(std::unique_ptr<AutocompleteDebounceTimer> timer,
            EditorCompletionWakeSink wake_sink,
            EditorRenderRequestSink render_sink)
        : debounce_timer(std::shared_ptr<AutocompleteDebounceTimer>(std::move(timer))),
          control(std::make_shared<Control>(std::move(wake_sink), std::move(render_sink))) {}

    std::shared_ptr<AutocompleteDebounceTimer> debounce_timer;
    std::shared_ptr<Control> control;
};

EditorCompletionSession::EditorCompletionSession(std::unique_ptr<AutocompleteDebounceTimer> debounce_timer,
        EditorCompletionWakeSink on_wake,
        EditorRenderRequestSink render_request)
    : impl_(std::make_unique<Impl>(std::move(debounce_timer), std::move(on_wake), std::move(render_request))) {}

EditorCompletionSession::~EditorCompletionSession() { close(); }

EditorCompletionSession::EditorCompletionSession(EditorCompletionSession&&) noexcept = default;

EditorCompletionSession& EditorCompletionSession::operator=(EditorCompletionSession&& other) noexcept {
    if (this != &other) {
        close();
        impl_ = std::move(other.impl_);
    }
    return *this;
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
            control->due.reset();
            retired_provider = std::move(control->provider);
            control->pending.reset();
            control->clear_menu_locked();
        }
    }
    retired_provider.reset();
    request_stop(previous_stop_source);
    cancel_timer(impl_->debounce_timer);
}

void EditorCompletionSession::Impl::cancel_current_lifecycle(
        const std::shared_ptr<Control>& control, const std::shared_ptr<AutocompleteDebounceTimer>& timer) noexcept {
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
        control->due.reset();
        control->pending.reset();
        control->clear_menu_locked();
    }
    request_stop(previous_stop_source);
    cancel_timer(timer);
}

EditorCompletionProviderSetup EditorCompletionSession::set_provider(std::unique_ptr<AutocompleteProvider> provider) {
    EditorCompletionProviderSetup setup;
    if (!impl_ || !impl_->control) return setup;

    const auto control = impl_->control;
    Impl::cancel_current_lifecycle(control, impl_->debounce_timer);

    if (!provider) {
        std::shared_ptr<AutocompleteProvider> retired_provider;
        {
            std::lock_guard lock(control->mutex);
            retired_provider = std::move(control->provider);
        }
        retired_provider.reset();
        return setup;
    }

    auto shared_provider = std::shared_ptr<AutocompleteProvider>(std::move(provider));
    setup.trigger_characters = shared_provider->trigger_characters();
    std::shared_ptr<AutocompleteProvider> retired_provider;
    {
        std::lock_guard lock(control->mutex);
        if (control->open) {
            retired_provider = std::move(control->provider);
            control->provider = std::move(shared_provider);
        }
    }
    retired_provider.reset();
    return setup;
}

[[nodiscard]] std::optional<EditorCompletionSession::Impl::RequestAdmission>
EditorCompletionSession::Impl::admit_request(const std::shared_ptr<Control>& control,
        const std::shared_ptr<AutocompleteDebounceTimer>& timer,
        EditorCompletionRefresh refresh) {
    std::stop_source previous_stop_source;
    std::optional<EditorCompletionSession::Impl::RequestAdmission> admission;
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

        admission = EditorCompletionSession::Impl::RequestAdmission{
                .generation = control->generation,
                .request_id = control->request_id,
                .intent = refresh.intent,
                .start_immediately = refresh.debounce <= std::chrono::milliseconds{} || !timer,
        };
        if (!admission->start_immediately) {
            control->due = EditorCompletionSession::Impl::PendingDue{
                    .generation = admission->generation,
                    .intent = refresh.intent,
                    .ready = false,
            };
        }
    }
    request_stop(previous_stop_source);
    cancel_timer(timer);
    return admission;
}

support::Expected<EditorCompletionEffect> EditorCompletionSession::Impl::drain_pending(
        const std::shared_ptr<Control>& control,
        const std::shared_ptr<AutocompleteDebounceTimer>& timer,
        const EditorCompletionView& view) {
    EditorCompletionSession::Impl::PendingDelivery pending;
    {
        std::lock_guard lock(control->mutex);
        if (!control->open || !control->pending) return control->effect_locked();
        pending = std::move(*control->pending);
        control->pending.reset();
    }

    // The result has already been accepted once into the pending slot. A
    // changed text/cursor snapshot is a benign stale drop and retains the
    // current menu projection.
    if (pending.snapshot_text != view.text || pending.snapshot_cursor != view.cursor) {
        std::lock_guard lock(control->mutex);
        return control->effect_locked();
    }
    if (!pending.result || pending.result->items.empty()) {
        cancel_current_lifecycle(control, timer);
        std::lock_guard lock(control->mutex);
        return control->effect_locked();
    }

    if (pending.intent.force && pending.intent.explicit_tab && pending.result->items.size() == 1) {
        std::shared_ptr<AutocompleteProvider> provider;
        {
            std::lock_guard lock(control->mutex);
            if (!control->open || control->request_id != pending.request_id) return control->effect_locked();
            provider = control->provider;
        }
        if (!provider) {
            cancel_current_lifecycle(control, timer);
            std::lock_guard lock(control->mutex);
            return control->effect_locked();
        }

        AutocompleteApplyResult result;
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
        try {
#endif
            result = provider->apply_completion(view.lines,
                    view.cursor_line,
                    view.cursor_column,
                    pending.result->items.front(),
                    pending.result->prefix);
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
        } catch (...) {
            cancel_current_lifecycle(control, timer);
            return std::unexpected(provider_failure("the autocomplete application threw an exception"));
        }
#endif
        cancel_current_lifecycle(control, timer);
        return EditorCompletionEffect{
                .menu = {},
                .application =
                        EditorCompletionApplication{
                                .result = std::move(result),
                                .prefix = pending.result->prefix,
                                .notify = true,
                        },
                .submit = false,
        };
    }

    {
        std::lock_guard lock(control->mutex);
        if (!control->open || control->request_id != pending.request_id) return control->effect_locked();
        control->prefix = pending.result->prefix;
        control->items = pending.result->items;
        if (const auto best = best_match_index(control->items, control->prefix)) {
            control->selected = *best;
        } else {
            control->selected = std::min(control->selected, control->items.size() - 1);
        }
        control->menu_state = pending.intent.force ? EditorCompletionSession::Impl::MenuState::Force
                                                   : EditorCompletionSession::Impl::MenuState::Regular;
        return control->effect_locked();
    }
}

support::Expected<EditorCompletionEffect> EditorCompletionSession::Impl::start_request(
        const std::shared_ptr<Control>& control,
        const std::shared_ptr<AutocompleteDebounceTimer>& timer,
        const EditorCompletionSession::Impl::RequestAdmission& admission,
        const EditorCompletionView& view) {
    std::shared_ptr<AutocompleteProvider> provider;
    std::size_t request_id = 0;
    std::stop_token stop_token;
    {
        std::lock_guard lock(control->mutex);
        if (!control->open || control->generation != admission.generation ||
                (control->started_generation && *control->started_generation == admission.generation)) {
            return control->effect_locked();
        }
        control->started_generation = admission.generation;
        control->due.reset();
        provider = control->provider;
        request_id = control->request_id;
        stop_token = control->request_stop_source.get_token();
    }
    if (!provider) {
        std::lock_guard lock(control->mutex);
        return control->effect_locked();
    }

    const auto snapshot_text = view.text;
    const auto snapshot_cursor = view.cursor;
    const auto intent = admission.intent;
    AutocompleteRequest request{
            .lines = view.lines,
            .cursor_line = view.cursor_line,
            .cursor_column = view.cursor_column,
            .force = intent.force,
            .stop_token = stop_token,
    };
    const auto weak_control = std::weak_ptr<EditorCompletionSession::Impl::Control>{control};
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
                                state->pending = EditorCompletionSession::Impl::PendingDelivery{
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
        cancel_current_lifecycle(control, timer);
        return std::unexpected(provider_failure("the autocomplete provider threw an exception"));
    }
#endif
    return drain_pending(control, timer, view);
}

support::Expected<EditorCompletionEffect> EditorCompletionSession::Impl::handle_refresh(
        const std::shared_ptr<Control>& control,
        const std::shared_ptr<AutocompleteDebounceTimer>& timer,
        EditorCompletionRefresh refresh,
        const EditorCompletionView& view) {
    std::shared_ptr<AutocompleteProvider> provider;
    {
        std::lock_guard lock(control->mutex);
        if (!control->open || !control->provider) return control->effect_locked();
        provider = control->provider;
    }

    if (refresh.intent.force) {
        bool allowed = false;
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
        try {
#endif
            allowed = provider->should_trigger_file_completion(view.lines, view.cursor_line, view.cursor_column);
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
        } catch (...) {
            return std::unexpected(provider_failure("the file-completion gate threw an exception"));
        }
#endif
        if (!allowed) {
            std::lock_guard lock(control->mutex);
            return control->effect_locked();
        }
    }

    const auto admission = admit_request(control, timer, refresh);
    if (!admission) {
        std::lock_guard lock(control->mutex);
        return control->effect_locked();
    }
    if (admission->start_immediately) {
        return start_request(control, timer, *admission, view);
    }

    const auto weak_control = std::weak_ptr<EditorCompletionSession::Impl::Control>{control};
    const auto generation = admission->generation;
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    try {
#endif
        timer->start(refresh.debounce, [weak_control, generation]() -> support::ExpectedVoid {
            if (const auto state = weak_control.lock()) {
                bool current = false;
                {
                    std::lock_guard lock(state->mutex);
                    if (state->open && state->due && state->due->generation == generation) {
                        state->due->ready = true;
                        current = true;
                    }
                }
                if (current && state->on_wake) {
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
                    try {
#endif
                        (void)state->on_wake();
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
                    } catch (...) {
                        // A stale or unavailable serialized entry is a benign drop.
                    }
#endif
                }
            }
            return {};
        });
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    } catch (...) {
        cancel_current_lifecycle(control, timer);
        return std::unexpected(provider_failure("the autocomplete debounce timer failed"));
    }
#endif
    std::lock_guard lock(control->mutex);
    return control->effect_locked();
}

support::Expected<EditorCompletionEffect> EditorCompletionSession::Impl::handle_wake(
        const std::shared_ptr<Control>& control,
        const std::shared_ptr<AutocompleteDebounceTimer>& timer,
        const EditorCompletionView& view) {
    std::optional<EditorCompletionSession::Impl::RequestAdmission> admission;
    {
        std::lock_guard lock(control->mutex);
        if (!control->open) return control->effect_locked();
        if (control->due && control->due->ready) {
            admission = EditorCompletionSession::Impl::RequestAdmission{
                    .generation = control->due->generation,
                    .request_id = control->request_id,
                    .intent = control->due->intent,
                    .start_immediately = true,
            };
            control->due.reset();
        }
    }
    if (admission) return start_request(control, timer, *admission, view);
    return drain_pending(control, timer, view);
}

support::Expected<EditorCompletionEffect> EditorCompletionSession::Impl::handle_menu(
        const std::shared_ptr<Control>& control,
        const std::shared_ptr<AutocompleteDebounceTimer>& timer,
        EditorCompletionMenuInteraction interaction,
        const EditorCompletionView& view) {
    if (interaction.action == EditorCompletionMenuAction::MoveUp ||
            interaction.action == EditorCompletionMenuAction::MoveDown) {
        std::lock_guard lock(control->mutex);
        if (control->open && !control->items.empty()) {
            if (interaction.action == EditorCompletionMenuAction::MoveDown) {
                control->selected = std::min(control->selected + 1, control->items.size() - 1);
            } else if (control->selected > 0) {
                --control->selected;
            }
        }
        return control->effect_locked();
    }

    std::shared_ptr<AutocompleteProvider> provider;
    AutocompleteItem item;
    std::string prefix;
    bool submit = false;
    {
        std::lock_guard lock(control->mutex);
        if (!control->open || !control->provider || control->items.empty()) return control->effect_locked();
        provider = control->provider;
        item = control->items[control->selected];
        prefix = control->prefix;
        submit = interaction.action == EditorCompletionMenuAction::Confirm && prefix.starts_with("/");
    }

    AutocompleteApplyResult result;
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    try {
#endif
        result = provider->apply_completion(view.lines, view.cursor_line, view.cursor_column, item, prefix);
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    } catch (...) {
        cancel_current_lifecycle(control, timer);
        return std::unexpected(provider_failure("the autocomplete application threw an exception"));
    }
#endif
    cancel_current_lifecycle(control, timer);
    return EditorCompletionEffect{
            .menu = {},
            .application =
                    EditorCompletionApplication{
                            .result = std::move(result),
                            .prefix = std::move(prefix),
                            .notify = !submit,
                    },
            .submit = submit,
    };
}

support::Expected<EditorCompletionEffect> EditorCompletionSession::handle(
        EditorCompletionInteraction interaction, EditorCompletionView view) {
    if (!impl_ || !impl_->control) return EditorCompletionEffect{};
    const auto control = impl_->control;
    return std::visit(
            [this, &control, &view](auto&& value) -> support::Expected<EditorCompletionEffect> {
                using Value = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<Value, EditorCompletionRefresh>) {
                    return Impl::handle_refresh(control, impl_->debounce_timer, std::move(value), view);
                } else if constexpr (std::is_same_v<Value, EditorCompletionWake>) {
                    return Impl::handle_wake(control, impl_->debounce_timer, view);
                } else if constexpr (std::is_same_v<Value, EditorCompletionCancel>) {
                    Impl::cancel_current_lifecycle(control, impl_->debounce_timer);
                    std::lock_guard lock(control->mutex);
                    return control->effect_locked();
                } else {
                    return Impl::handle_menu(control, impl_->debounce_timer, std::move(value), view);
                }
            },
            std::move(interaction));
}

} // namespace cch::tui::detail
