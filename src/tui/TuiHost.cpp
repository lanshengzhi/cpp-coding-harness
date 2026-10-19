#include <cch/tui/TuiHost.hpp>

#include "InputDecoder.hpp"

#include <format>
#include <string_view>
#include <thread>
#include <utility>

// Shared interaction core for TUI roots (#997): the state and pipeline lifted
// out of the regular root so an alternate-screen root gets the exact same
// listener ordering, decoder demux, focus bookkeeping and color-query
// behavior. Rendering and lifecycle stay on the concrete roots.

namespace cch::tui::detail {
namespace {

constexpr std::size_t kInputDecodeChunkBytes = 4096;

} // namespace

TuiHost::TuiHost(Terminal& terminal)
    : terminal_(terminal), raw_listeners_(std::make_shared<RawInputListenerChain>()),
      stream_decoder_(std::make_unique<TerminalStreamDecoder>()) {}

TuiHost::~TuiHost() { cancel_color_queries(); }

void TuiHost::finish_child_operation() noexcept {
    --child_operation_depth_;
    if (child_operation_depth_ == 0) retired_children_.clear();
}

RawInputListenerHandle TuiHost::add_raw_input_listener(RawInputListener listener) {
    return raw_listeners_->add_owned(std::move(listener));
}

support::ExpectedVoid TuiHost::remove_raw_input_listener(RawInputListenerId id) {
    if (!raw_listeners_->remove(id)) {
        return std::unexpected(support::make_error(
                support::ErrorCode::Validation, "Raw-input listener is not registered with this TUI"));
    }
    return {};
}

void TuiHost::set_debug_hook(TuiDebugHook hook) { debug_hook_ = std::move(hook); }

bool TuiHost::invoke_debug_hook(const InputEventVariant& event) {
    const auto* key = std::get_if<KeyEvent>(&event);
    if (debug_hook_ == nullptr || key == nullptr || !matches_key(*key, kTuiDebugKey)) return false;
    debug_hook_();
    return true;
}

void TuiHost::handle_input(std::string input, const std::function<void(const InputEventVariant&)>& dispatch) {
    stream_decoder_->set_kitty_protocol_active(terminal_.capabilities().keyboard_protocol == KeyboardProtocol::Kitty);
    // Frozen global debug-key stage (pi tui.ts:1074-1078): it runs after the
    // raw-input listener chain, and a matched chord consumes the event instead
    // of forwarding it to focus reconciliation or the focused component.
    const auto handle_event = [&](const InputEventVariant& event) {
        if (invoke_debug_hook(event)) return;
        dispatch(event);
    };
    if (input.empty()) {
        auto flushed = stream_decoder_->flush();
        apply_terminal_responses(flushed.responses);
        apply_cell_size_responses(flushed.responses);
        for (auto& frame : flushed.mouse_frames) {
            (void)raw_listeners_->dispatch(frame);
        }
        for (const auto& event : flushed.events)
            handle_event(event);
        return;
    }

    // Frozen raw-input listener stage (pi tui.ts handleInput inputListeners):
    // listeners observe input in registration order and may consume it or
    // replace it for the rest of the chain. The decoder runs first so
    // response-only bytes (color, scheme, keyboard negotiation) never reach
    // the chain; the cell-size reply is the one response deliberately kept in
    // the forwarded bytes and applied after the chain.
    for (std::size_t offset = 0; offset < input.size(); offset += kInputDecodeChunkBytes) {
        const auto chunk = std::string_view(input).substr(offset, kInputDecodeChunkBytes);
        auto decoded = stream_decoder_->feed(chunk);
        apply_terminal_responses(decoded.responses);

        // Complete mouse reports are input for the byte-level stage even though
        // they decode to no typed event, so they run through the same listener
        // chain as the key bytes (pi StdinBuffer hands the frame to the data
        // handler, which reaches the input listeners). A report keeps its own
        // fragment: a listener that consumes it stops the chain for that report,
        // and a listener rewrite is ignored because a report is not text.
        for (auto& frame : decoded.mouse_frames) {
            (void)raw_listeners_->dispatch(frame);
        }

        if (decoded.forwarded_input.empty()) continue;

        const auto dispatched = raw_listeners_->dispatch(decoded.forwarded_input);
        if (dispatched.consumed || dispatched.input.empty()) continue;

        if (dispatched.input == decoded.forwarded_input) {
            apply_cell_size_responses(decoded.responses);
            for (const auto& event : decoded.events)
                handle_event(event);
        } else {
            for (std::size_t roffset = 0; roffset < dispatched.input.size(); roffset += kInputDecodeChunkBytes) {
                const auto rchunk = std::string_view(dispatched.input).substr(roffset, kInputDecodeChunkBytes);
                auto rdecoded = stream_decoder_->feed(rchunk);
                apply_terminal_responses(rdecoded.responses);
                apply_cell_size_responses(rdecoded.responses);
                for (const auto& event : rdecoded.events)
                    handle_event(event);
            }
        }
    }
}

void TuiHost::reset_decoder() noexcept { stream_decoder_->reset(); }

void TuiHost::apply_focus(Component* component, const std::function<bool(Component*)>& available) {
    ChildOperation operation(*this);
    auto* previous = focused_;
    focused_ = component;
    if (auto* focusable = dynamic_cast<Focusable*>(previous)) focusable->set_focused(false);
    if (focused_ != component || !available(component)) return;
    if (auto* focusable = dynamic_cast<Focusable*>(component)) focusable->set_focused(true);
}

TerminalColorSchemeListenerHandle TuiHost::on_terminal_color_scheme_change(TerminalColorSchemeListener listener) {
    const auto state = scheme_listeners_;
    TerminalColorSchemeListenerId id{0};
    {
        std::lock_guard lock(state->mutex);
        id = state->next_id++;
        state->entries.emplace_back(id, std::move(listener));
    }
    return TerminalColorSchemeListenerHandle([weak = std::weak_ptr{state}, id] {
        if (const auto listeners = weak.lock()) {
            std::lock_guard lock(listeners->mutex);
            std::erase_if(listeners->entries, [id](const auto& pair) { return pair.first == id; });
        }
    });
}

void TuiHost::set_terminal_color_scheme_notifications(bool enabled, bool started, bool stopping) {
    if (color_scheme_notifications_enabled_ == enabled) return;
    color_scheme_notifications_enabled_ = enabled;
    if (started && !stopping) {
        (void)terminal_.write(enabled ? "\x1b[?2031h" : "\x1b[?2031l");
    }
}

void TuiHost::dispatch_color_scheme_report(TerminalColorScheme scheme) {
    std::vector<TerminalColorSchemeListener> callbacks;
    const auto state = scheme_listeners_;
    {
        std::lock_guard lock(state->mutex);
        callbacks.reserve(state->entries.size());
        for (const auto& [_, listener] : state->entries) {
            callbacks.push_back(listener);
        }
    }
    for (const auto& cb : callbacks) {
        if (cb) cb(scheme);
    }
}

TerminalColors TuiHost::QueryState::build_terminal_colors(const QueryState& query) {
    TerminalColors result;
    result.foreground = query.foreground;
    result.background = query.background;

    bool all_palette_entries_replied = true;
    for (const auto& entry : query.palette) {
        if (!entry.has_value()) {
            all_palette_entries_replied = false;
            break;
        }
    }
    if (all_palette_entries_replied) {
        std::vector<RgbColor> palette;
        palette.reserve(query.palette.size());
        for (const auto& entry : query.palette) {
            palette.push_back(*entry);
        }
        result.palette = std::move(palette);
    }
    return result;
}

std::future<TerminalColors> TuiHost::query_terminal_colors(TerminalColorsQueryOptions options) {
    auto query = std::make_shared<QueryState>();
    query->id = next_query_id_++;
    query->on_complete = std::move(options.on_complete);
    query->on_late_reply = std::move(options.on_late_reply);
    query->deadline = std::chrono::steady_clock::now() + options.timeout;
    auto future = query->promise.get_future();

    {
        std::lock_guard lock(color_queries_mutex_);
        pending_color_queries_.push_back(query);
    }

    // The timer runs as a detached thread holding the query state shared_ptr:
    // the query outlives the caller, and the timer must not be joined from
    // inside itself when it is the last owner. The thread holds no root
    // reference. Ordinary stop cancels under the query mutex; an explicit
    // renderer handoff may keep completion at this same deadline, so such
    // callbacks must own lifetime-safe state independent of the retired root.
    // The static timeout path re-checks query flags under the same mutex.
    std::thread([query] {
        std::unique_lock lock(query->mutex);
        query->cv.wait_until(lock, query->deadline, [&] { return query->completed || query->cancelled; });
        if (query->completed || query->cancelled) return;
        lock.unlock();
        handle_color_query_timeout(query);
    }).detach();

    (void)terminal_.write(build_terminal_color_query());
    return future;
}

void TuiHost::cancel_color_queries(bool preserve_completion) {
    std::deque<std::shared_ptr<QueryState>> queries_to_cancel;
    {
        std::lock_guard lock(color_queries_mutex_);
        queries_to_cancel = std::move(pending_color_queries_);
        pending_color_queries_.clear();
    }
    for (const auto& q : queries_to_cancel) {
        std::lock_guard lock(q->mutex);
        q->on_late_reply = nullptr;
        if (preserve_completion && !q->promise_resolved && !q->completed && !q->cancelled) {
            // The existing timer owns only QueryState, never the stopped root.
            // Its original deadline delivers the partial result once; the query
            // is already absent from input routing and cannot accept late input.
            continue;
        }
        q->cancelled = true;
        q->on_complete = nullptr;
        if (!q->promise_resolved) {
            q->promise_resolved = true;
            q->promise.set_value(TerminalColors{});
        }
        q->cv.notify_all();
    }
}

void TuiHost::handle_color_query_timeout(const std::shared_ptr<QueryState>& query) {
    TerminalColors partial;
    TerminalColorsCallback on_complete_cb;
    {
        std::unique_lock lock(query->mutex);
        if (query->completed || query->cancelled || query->promise_resolved) return;
        query->promise_resolved = true;
        partial = QueryState::build_terminal_colors(*query);
        on_complete_cb = std::move(query->on_complete);
        query->promise.set_value(partial);
    }
    if (on_complete_cb) {
        on_complete_cb(partial);
    }
}

bool TuiHost::consume_terminal_color_response(const OscColorResponse& response) {
    std::shared_ptr<QueryState> query;
    {
        std::lock_guard lock(color_queries_mutex_);
        if (pending_color_queries_.empty()) return false;
        query = pending_color_queries_.front();
    }

    TerminalColors current_result;
    bool should_complete = false;
    TerminalColorsCallback on_complete_cb;
    TerminalColorsCallback on_late_reply_cb;

    {
        std::lock_guard lock(query->mutex);
        if (query->completed || query->cancelled) return true;

        const auto key = response.target.key();
        if (query->replied.contains(key)) {
            return true;
        }
        query->replied.insert(key);

        if (response.target.kind == OscColorTargetKind::Foreground) {
            query->foreground = response.rgb;
        } else if (response.target.kind == OscColorTargetKind::Background) {
            query->background = response.rgb;
        } else if (response.target.palette_index < 16) {
            query->palette[response.target.palette_index] = response.rgb;
        }

        if (query->replied.size() == 18) {
            should_complete = true;
            query->completed = true;
            query->cv.notify_all();
        }

        current_result = QueryState::build_terminal_colors(*query);

        if (should_complete) {
            if (!query->promise_resolved) {
                query->promise_resolved = true;
                query->promise.set_value(current_result);
                on_complete_cb = std::move(query->on_complete);
            } else if (query->on_late_reply) {
                on_late_reply_cb = std::move(query->on_late_reply);
            }
        } else if (query->promise_resolved && query->on_late_reply) {
            on_late_reply_cb = std::move(query->on_late_reply);
        }
    }

    if (should_complete) {
        std::lock_guard lock(color_queries_mutex_);
        if (!pending_color_queries_.empty() && pending_color_queries_.front() == query) {
            pending_color_queries_.pop_front();
        }
    }

    if (on_complete_cb) {
        on_complete_cb(current_result);
    }
    if (on_late_reply_cb) {
        on_late_reply_cb(current_result);
        if (!should_complete) {
            std::lock_guard lock(query->mutex);
            if (!query->cancelled && !query->completed && !query->on_late_reply) {
                query->on_late_reply = std::move(on_late_reply_cb);
            }
        }
    }

    return true;
}

bool TuiHost::consume_terminal_device_attributes() {
    std::shared_ptr<QueryState> query;
    {
        std::lock_guard lock(color_queries_mutex_);
        if (pending_color_queries_.empty()) return false;
        query = pending_color_queries_.front();
        pending_color_queries_.pop_front();
    }

    TerminalColors result;
    TerminalColorsCallback on_complete_cb;
    TerminalColorsCallback on_late_reply_cb;

    {
        std::lock_guard lock(query->mutex);
        if (query->completed || query->cancelled) return true;
        query->completed = true;
        query->cv.notify_all();
        result = QueryState::build_terminal_colors(*query);

        if (!query->promise_resolved) {
            query->promise_resolved = true;
            query->promise.set_value(result);
            on_complete_cb = std::move(query->on_complete);
        } else if (query->on_late_reply) {
            on_late_reply_cb = std::move(query->on_late_reply);
        }
    }

    if (on_complete_cb) {
        on_complete_cb(result);
    }
    if (on_late_reply_cb) {
        on_late_reply_cb(result);
    }

    return true;
}

void TuiHost::apply_terminal_responses(const std::vector<TerminalResponseVariant>& responses) {
    for (const auto& response : responses) {
        if (const auto* scheme = std::get_if<ColorSchemeResponse>(&response)) {
            if (scheme->kind == ColorSchemeResponseKind::ColorScheme) {
                dispatch_color_scheme_report(scheme->appearance == TerminalAppearance::Light
                                                     ? TerminalColorScheme::Light
                                                     : TerminalColorScheme::Dark);
            } else if (scheme->color_response) {
                consume_terminal_color_response(*scheme->color_response);
            }
            continue;
        }
        if (const auto* keyboard = std::get_if<KeyboardProtocolResponse>(&response)) {
            if (keyboard->kind == KeyboardProtocolResponseKind::DeviceAttributes) {
                consume_terminal_device_attributes();
            }
            continue;
        }
    }
}

void TuiHost::apply_cell_size_responses(const std::vector<TerminalResponseVariant>& responses) {
    for (const auto& response : responses) {
        if (const auto* cell_size = std::get_if<CellSizeResponse>(&response)) {
            (void)terminal_.apply_cell_pixel_dimensions(CellPixelDimensions{
                    .width = cell_size->width_px,
                    .height = cell_size->height_px,
            });
        }
    }
}

} // namespace cch::tui::detail
