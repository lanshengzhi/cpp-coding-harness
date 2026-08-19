#include <cch/tui/Editor.hpp>

#include <cch/tui/Utils.hpp>

#include "tui/InteractionUtils.hpp"
#include "tui/TextBuffer.hpp"
#include "tui/UnicodeWidth.hpp"

#include <cch/support/Error.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::tui {
namespace {

// Behavioral baseline: pi 83114817 packages/tui/src/components/editor.ts
// (addToHistory submit-path recording and cursor-boundary history recall;
// the async autocomplete request lifecycle: trigger characters and patterns,
// debounce, force, cancellation, staleness rejection, applyCompletion
// delegation, and the select.confirm fall-through).

/// History depth cap matching pi's addToHistory.
constexpr std::size_t kMaxHistoryEntries = 100;

/// U+2500 BOX DRAWINGS LIGHT HORIZONTAL repeated to the width (the editor
/// border rule, matching pi's `borderColor("─").repeat(width)`).
[[nodiscard]] std::string horizontal_rule(std::size_t width) {
    std::string rule;
    rule.reserve(width * 3);
    for (std::size_t index = 0; index < width; ++index) rule += "─";
    return rule;
}

[[nodiscard]] std::string normalize_input(std::string text) {
    std::string normalized;
    normalized.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] == '\r') {
            normalized.push_back('\n');
            if (index + 1 < text.size() && text[index + 1] == '\n') ++index;
        } else if (text[index] == '\t') {
            normalized += "    ";
        } else {
            normalized.push_back(text[index]);
        }
    }
    return normalized;
}

[[nodiscard]] std::string trim_outer_whitespace(std::string text) {
    const auto first = std::find_if_not(text.begin(), text.end(), [](unsigned char value) {
        return std::isspace(value) != 0;
    });
    const auto last = std::find_if_not(text.rbegin(), text.rend(), [](unsigned char value) {
        return std::isspace(value) != 0;
    }).base();
    if (first >= last) return {};
    return {first, last};
}

} // namespace

struct Editor::Impl {
    EditorOptions options;
    EditorChangeSink on_change;
    EditorSubmitSink on_submit;
    EditorTheme theme;

    detail::TextBuffer buffer{detail::TextBufferOptions{.multiline = true, .enable_paste_markers = true}};

    std::unique_ptr<AutocompleteProvider> autocomplete_provider;
    std::vector<AutocompleteItem> autocomplete;
    std::string autocomplete_prefix;
    std::size_t autocomplete_selected{0};
    enum class AutocompleteState { None, Regular, Force };
    AutocompleteState autocomplete_state{AutocompleteState::None};
    std::vector<std::string> autocomplete_trigger_characters{"@", "#"};

    /// Self-reference for closures that may outlive the owning Editor
    /// (provider result sinks and debounce timers); the Editor destructor
    /// breaks the cycle. Closures must capture `self`, never `this`.
    std::shared_ptr<Impl> self;
    /// Serializes every public entry point (input, render, timers, sinks).
    std::mutex impl_mutex;
    /// Guards the cross-thread request bookkeeping and pending-result slot
    /// (the provider result sink may be invoked from any thread).
    std::mutex autocomplete_mutex;
    std::size_t autocomplete_start_token{0};
    std::size_t autocomplete_request_id{0};
    std::stop_source request_stop_source;
    struct PendingAutocomplete {
        std::size_t request_id{0};
        bool force{false};
        bool explicit_tab{false};
        std::string snapshot_text;
        EditorCursor snapshot_cursor;
        std::optional<AutocompleteSuggestions> result;
    };
    std::optional<PendingAutocomplete> pending_autocomplete;

    // Prompt history for up/down navigation. Most recent entry first, exactly
    // as pi's addToHistory unshifts; nullopt means not browsing.
    std::vector<std::string> history;
    std::optional<std::size_t> history_index;
    struct HistoryDraft {
        detail::TextBuffer buffer;
    };
    std::optional<HistoryDraft> history_draft;
    bool focused{false};
    std::size_t available_height{5};
    std::size_t layout_width{80};
    std::size_t scroll_offset{0};
    enum class JumpDirection { Forward, Backward };
    std::optional<JumpDirection> jump_direction;
    std::optional<support::Error> callback_error;

    [[nodiscard]] EditorCursor cursor() const {
        const auto cur = buffer.cursor();
        return EditorCursor{.line = cur.line, .column = cur.column};
    }

    /// Rows reserved for the optional top/bottom border (pi's editor always
    /// renders both border lines; an empty hook renders none).
    [[nodiscard]] std::size_t border_rows() const {
        return theme.border ? 2 : 0;
    }

    /// Content rows available inside the border, at least 1.
    [[nodiscard]] std::size_t content_height() const {
        const auto bordered = available_height > border_rows()
            ? available_height - border_rows()
            : 1;
        return std::max<std::size_t>(1, std::min(options.max_visible_lines, bordered));
    }

    /// pi `createScrollBorder`: `─── ↑ N more ` (or ↓) plus the fill, with
    /// ellipsis truncation on tiny widths.
    [[nodiscard]] std::string scroll_border(
        std::string_view direction,
        std::size_t hidden_line_count,
        std::size_t width) const {
        const auto indicator = std::format("─── {} {} more ", direction, hidden_line_count);
        const auto indicator_width = visible_width(indicator);
        if (indicator_width >= width) {
            // pi: `"...".slice(0, availableWidth)` + a column-sliced indicator.
            const auto ellipsis = std::string{std::string_view{"..."}.substr(0, width)};
            const auto slice_width =
                width > visible_width(ellipsis) ? width - visible_width(ellipsis) : 0;
            auto sliced = slice_by_column(indicator, 0, slice_width, true);
            return (sliced ? *sliced : std::string{}) + ellipsis;
        }
        return indicator + horizontal_rule(width - indicator_width);
    }

    void notify_change() {
        if (!on_change) return;
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
        try {
#endif
            if (auto result = on_change(buffer.text()); !result) {
                // An explicit change-sink failure is a bounded callback
                // diagnostic (ADR 0017); it never vetoes editing.
                callback_error = std::move(result.error());
            }
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
        } catch (...) {
            callback_error = support::make_error(
                support::ErrorCode::Unknown,
                "Editor change sink failed",
                "the change callback threw an exception");
        }
#endif
    }

    [[nodiscard]] std::string text() const {
        return buffer.text();
    }

    [[nodiscard]] std::string expanded() const {
        return buffer.expanded_text();
    }

    [[nodiscard]] std::size_t cursor_byte_offset() const {
        return buffer.cursor_byte_offset();
    }

    [[nodiscard]] std::string line_prefix_before_cursor() const {
        return buffer.line_prefix_before_cursor();
    }

    [[nodiscard]] std::vector<std::string> line_strings() const {
        return buffer.line_strings();
    }

    void cancel_autocomplete_request() {
        ++autocomplete_start_token;
        // A cancelled request's late result must never reopen the menu: bump
        // the request id so its delivery is rejected as stale (pi aborts the
        // controller and isAutocompleteRequestCurrent checks signal.aborted).
        {
            std::lock_guard lock(autocomplete_mutex);
            ++autocomplete_request_id;
        }
        if (options.autocomplete_debounce_timer) {
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
            try {
#endif
                options.autocomplete_debounce_timer->cancel();
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
            } catch (...) {
                // Cancellation is best-effort; a throwing timer cannot veto input.
            }
#endif
        }
        request_stop_source.request_stop();
    }

    void clear_autocomplete_ui() {
        autocomplete.clear();
        autocomplete_prefix.clear();
        autocomplete_selected = 0;
        autocomplete_state = AutocompleteState::None;
    }

    void cancel_autocomplete() {
        cancel_autocomplete_request();
        clear_autocomplete_ui();
    }

    /// Whether the text before the cursor ends in a token starting with one
    /// of the trigger characters (pi buildTriggerPattern/buildDebouncePattern;
    /// the two patterns coincide on single-line editor text).
    [[nodiscard]] bool autocomplete_pattern_matches(std::string_view text_before_cursor) const {
        if (autocomplete_trigger_characters.empty()) return false;
        std::size_t token_start = 0;
        for (std::size_t index = 0; index < text_before_cursor.size(); ++index) {
            if (text_before_cursor[index] == ' ' || text_before_cursor[index] == '\t') token_start = index + 1;
        }
        if (token_start >= text_before_cursor.size()) return false;
        const auto token = text_before_cursor.substr(token_start);
        for (const auto& trigger : autocomplete_trigger_characters) {
            if (token.starts_with(trigger)) return true;
        }
        return false;
    }

    /// Whether an attachment request deserves the 20 ms debounce (pi
    /// buildDebouncePattern): an unclosed `@"..."` quoted path may contain
    /// spaces, so the plain token test alone misses it.
    [[nodiscard]] bool autocomplete_debounce_matches(std::string_view text_before_cursor) const {
        bool in_quotes = false;
        std::optional<std::size_t> quote_start;
        for (std::size_t index = 0; index < text_before_cursor.size(); ++index) {
            if (text_before_cursor[index] != '"') continue;
            in_quotes = !in_quotes;
            quote_start = index;
        }
        if (in_quotes && *quote_start > 0 && text_before_cursor[*quote_start - 1] == '@') {
            const auto at_index = *quote_start - 1;
            if (at_index == 0 || text_before_cursor[at_index - 1] == ' ' || text_before_cursor[at_index - 1] == '\t') {
                return true;
            }
        }
        return autocomplete_pattern_matches(text_before_cursor);
    }

    [[nodiscard]] bool is_slash_menu_allowed() const {
        return buffer.cursor().line == 0;
    }

    [[nodiscard]] bool is_at_start_of_message() const {
        if (!is_slash_menu_allowed()) return false;
        const auto trimmed = trim_outer_whitespace(line_prefix_before_cursor());
        return trimmed.empty() || trimmed == "/";
    }

    [[nodiscard]] bool in_slash_command_context(std::string_view text_before_cursor) const {
        return is_slash_menu_allowed() && detail::trim_start_ascii(text_before_cursor).starts_with('/');
    }

    [[nodiscard]] bool is_trigger_character(char ch) const {
        for (const auto& trigger : autocomplete_trigger_characters) {
            if (trigger.size() == 1 && trigger[0] == ch) return true;
        }
        return false;
    }

    [[nodiscard]] static bool is_word_character(char ch) {
        return std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '.' || ch == '_' || ch == '-';
    }

    void set_autocomplete_trigger_characters(const std::vector<std::string>& characters) {
        auto next = std::vector<std::string>{"@", "#"};
        for (const auto& character : characters) {
            if (character.size() != 1 || character == "/" ||
                std::isspace(static_cast<unsigned char>(character[0])) != 0 ||
                std::find(next.begin(), next.end(), character) != next.end()) {
                continue;
            }
            next.push_back(character);
        }
        autocomplete_trigger_characters = std::move(next);
    }

    [[nodiscard]] std::chrono::milliseconds get_autocomplete_debounce_ms(bool force, bool explicit_tab) const {
        if (explicit_tab || force) return {};
        return autocomplete_debounce_matches(line_prefix_before_cursor())
            ? std::chrono::milliseconds{20}
            : std::chrono::milliseconds{};
    }

    [[nodiscard]] std::optional<std::size_t> best_autocomplete_match_index(
        const std::vector<AutocompleteItem>& items,
        const std::string& prefix) const {
        if (prefix.empty()) return std::nullopt;
        std::optional<std::size_t> first_prefix_index;
        for (std::size_t index = 0; index < items.size(); ++index) {
            if (items[index].value == prefix) return index;
            if (!first_prefix_index && items[index].value.starts_with(prefix)) first_prefix_index = index;
        }
        return first_prefix_index;
    }

    void request_autocomplete(bool force, bool explicit_tab) {
        if (!autocomplete_provider) return;
        if (force &&
            !autocomplete_provider->should_trigger_file_completion(
                line_strings(),
                buffer.cursor().line,
                cursor_byte_offset())) {
            return;
        }
        cancel_autocomplete_request();
        const auto start_token = ++autocomplete_start_token;
        const auto debounce_ms = get_autocomplete_debounce_ms(force, explicit_tab);
        if (debounce_ms > std::chrono::milliseconds{} && options.autocomplete_debounce_timer) {
            options.autocomplete_debounce_timer->start(
                debounce_ms,
                [self = self, start_token, force, explicit_tab]() -> support::ExpectedVoid {
                    self->on_debounce_fired(start_token, force, explicit_tab);
                    return {};
                });
            return;
        }
        start_autocomplete_request(start_token, force, explicit_tab);
    }

    void on_debounce_fired(std::size_t start_token, bool force, bool explicit_tab) {
        std::lock_guard lock(impl_mutex);
        start_autocomplete_request(start_token, force, explicit_tab);
    }

    void start_autocomplete_request(std::size_t start_token, bool force, bool explicit_tab) {
        if (start_token != autocomplete_start_token) return;
        if (!autocomplete_provider) return;
        const auto snapshot_text = text();
        const auto snapshot_cursor = cursor();
        const auto cursor_column = cursor_byte_offset();
        const auto request_id = [this] {
            std::lock_guard lock(autocomplete_mutex);
            return ++autocomplete_request_id;
        }();
        request_stop_source = std::stop_source{};
        AutocompleteRequest request{
            .lines = line_strings(),
            .cursor_line = buffer.cursor().line,
            .cursor_column = cursor_column,
            .force = force,
            .stop_token = request_stop_source.get_token(),
        };
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
        try {
#endif
            autocomplete_provider->get_suggestions(
                request,
                [self = self, request_id, snapshot_text, snapshot_cursor, force, explicit_tab](
                    std::optional<AutocompleteSuggestions> result) -> support::ExpectedVoid {
                    self->deliver_autocomplete_result(
                        request_id,
                        std::move(result),
                        snapshot_text,
                        snapshot_cursor,
                        force,
                        explicit_tab);
                    return {};
                });
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
        } catch (...) {
            callback_error = support::make_error(
                support::ErrorCode::Unknown,
                "Editor autocomplete provider failed",
                "the autocomplete callback threw an exception");
            cancel_autocomplete();
            return;
        }
#endif
        drain_autocomplete_result();
    }

    void deliver_autocomplete_result(
        std::size_t request_id,
        std::optional<AutocompleteSuggestions> result,
        std::string snapshot_text,
        EditorCursor snapshot_cursor,
        bool force,
        bool explicit_tab) {
        {
            std::lock_guard lock(autocomplete_mutex);
            if (request_id != autocomplete_request_id) return;  // stale response
            pending_autocomplete = PendingAutocomplete{
                .request_id = request_id,
                .force = force,
                .explicit_tab = explicit_tab,
                .snapshot_text = std::move(snapshot_text),
                .snapshot_cursor = snapshot_cursor,
                .result = std::move(result),
            };
        }
        if (options.autocomplete_render_request) {
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
            try {
#endif
                (void)options.autocomplete_render_request();
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
            } catch (...) {
                // Scheduling notifications cannot make input delivery fail
                // (same bound as Tui::invalidate's render-request sink).
            }
#endif
        }
    }

    void drain_autocomplete_result() {
        std::optional<PendingAutocomplete> pending;
        {
            std::lock_guard lock(autocomplete_mutex);
            pending = std::move(pending_autocomplete);
            pending_autocomplete.reset();
        }
        if (!pending) return;
        // pi isAutocompleteRequestCurrent: the result only applies while the
        // buffer and cursor still match the request snapshot.
        if (pending->snapshot_text != text() || pending->snapshot_cursor != cursor()) return;
        if (!pending->result || pending->result->items.empty()) {
            cancel_autocomplete();
            return;
        }
        if (pending->force && pending->explicit_tab && pending->result->items.size() == 1) {
            // Tab on a unique match completes immediately without opening the
            // menu (pi runAutocompleteRequest).
            apply_autocomplete_item(pending->result->items.front(), pending->result->prefix, true);
            return;
        }
        apply_autocomplete_suggestions(*pending->result, pending->force);
    }

    void apply_autocomplete_suggestions(const AutocompleteSuggestions& suggestions, bool force) {
        autocomplete_prefix = suggestions.prefix;
        autocomplete = suggestions.items;
        if (const auto best = best_autocomplete_match_index(autocomplete, autocomplete_prefix)) {
            autocomplete_selected = *best;
        } else {
            autocomplete_selected = std::min(autocomplete_selected, autocomplete.size() - 1);
        }
        autocomplete_state = force ? AutocompleteState::Force : AutocompleteState::Regular;
    }

    void update_autocomplete() {
        if (autocomplete_state == AutocompleteState::None) return;
        if (!autocomplete_provider) return;
        request_autocomplete(autocomplete_state == AutocompleteState::Force, false);
    }

    void try_trigger_autocomplete() {
        request_autocomplete(false, false);
    }

    /// pi insertCharacter's autocomplete tail: auto-trigger on slash at
    /// message start, trigger characters at token boundaries, and word
    /// characters inside a slash or attachment context; an open menu instead
    /// re-queries with its force mode.
    void maybe_trigger_autocomplete(char last_char) {
        if (autocomplete_state != AutocompleteState::None) {
            update_autocomplete();
            return;
        }
        const auto text_before_cursor = line_prefix_before_cursor();
        if (last_char == '/' && is_at_start_of_message()) {
            try_trigger_autocomplete();
            return;
        }
        if (is_trigger_character(last_char)) {
            const auto length = text_before_cursor.size();
            if (length == 1 ||
                (length >= 2 && (text_before_cursor[length - 2] == ' ' || text_before_cursor[length - 2] == '\t'))) {
                try_trigger_autocomplete();
            }
            return;
        }
        if (is_word_character(last_char)) {
            if (in_slash_command_context(text_before_cursor) || autocomplete_pattern_matches(text_before_cursor)) {
                try_trigger_autocomplete();
            }
        }
    }

    /// pi handleBackspace/handleForwardDelete's autocomplete tail.
    void update_or_retrigger_after_erase() {
        if (autocomplete_state != AutocompleteState::None) {
            update_autocomplete();
            return;
        }
        const auto text_before_cursor = line_prefix_before_cursor();
        if (in_slash_command_context(text_before_cursor) || autocomplete_pattern_matches(text_before_cursor)) {
            try_trigger_autocomplete();
        }
    }

    void handle_tab_completion() {
        if (!autocomplete_provider) return;
        const auto text_before_cursor = line_prefix_before_cursor();
        if (in_slash_command_context(text_before_cursor) &&
            detail::trim_start_ascii(text_before_cursor).find(' ') == std::string_view::npos) {
            request_autocomplete(false, true);
        } else {
            request_autocomplete(true, true);
        }
    }

    void apply_completion_result(const AutocompleteApplyResult& result, std::string_view prefix) {
        if (result.cursor_line >= buffer.line_count()) return;
        const auto& lines = buffer.document();
        const auto& line = lines[result.cursor_line];
        const auto cursor_bytes = buffer.cursor_byte_offset();
        if (prefix.size() > cursor_bytes) return;
        const auto start_bytes = cursor_bytes - prefix.size();
        const auto current_line_text = buffer.line_strings()[result.cursor_line];
        if (current_line_text.substr(start_bytes, prefix.size()) != prefix) return;

        // Map byte offsets to segment boundaries; bail when a boundary cuts
        // through an atomic segment (e.g. a paste marker).
        const auto segment_index_at = [&line](std::size_t bytes) -> std::optional<std::size_t> {
            std::size_t accumulated = 0;
            for (std::size_t index = 0; index < line.size(); ++index) {
                if (accumulated == bytes) return index;
                accumulated += line[index].text.size();
                if (accumulated == bytes) return index + 1;
            }
            if (accumulated == bytes) return line.size();
            return std::nullopt;
        };
        const auto start_segment = segment_index_at(start_bytes);
        const auto cursor_segment = segment_index_at(cursor_bytes);
        if (!start_segment || !cursor_segment) return;
        const auto start_segment_value = *start_segment;
        const auto cursor_segment_value = *cursor_segment;

        const auto& result_line = result.lines[result.cursor_line];
        const auto result_cursor = std::min(result.cursor_column, result_line.size());
        if (result_cursor < start_bytes) return;

        // pi applyCompletion's quote adjustment: when completing into an
        // unclosed quoted prefix the item already carries the closing quote,
        // so the typed leading quote after the cursor is dropped.
        const bool is_quoted_prefix = prefix.starts_with('"') || prefix.starts_with("@\"");
        const bool has_trailing_quote_in_item =
            result_cursor > start_bytes && result_line[result_cursor - 1] == '"';
        const bool has_leading_quote_after_cursor =
            cursor_segment_value < line.size() && line[cursor_segment_value].text == "\"";
        const auto after_begin = cursor_segment_value +
            ((is_quoted_prefix && has_trailing_quote_in_item && has_leading_quote_after_cursor) ? 1 : 0);

        const auto original_after = current_line_text.substr(cursor_bytes);
        const auto expected_after = result_line.substr(result_cursor);
        const bool quote_adjusted =
            (is_quoted_prefix && has_trailing_quote_in_item && has_leading_quote_after_cursor);
        if (result_line.substr(0, start_bytes) != current_line_text.substr(0, start_bytes)) return;
        if (expected_after != (quote_adjusted && !original_after.empty() ? original_after.substr(1)
                                                                         : original_after)) {
            return;
        }

        buffer.apply_completion_edit(
            result.cursor_line,
            start_segment_value,
            after_begin,
            result_line.substr(start_bytes, result_cursor - start_bytes),
            result_line.substr(0, result_cursor));
    }

    void apply_autocomplete_item(const AutocompleteItem& item, std::string_view prefix, bool notify) {
        if (!autocomplete_provider) return;
        buffer.push_undo();
        const auto cur = buffer.cursor();
        const auto result = autocomplete_provider->apply_completion(
            line_strings(),
            cur.line,
            cursor_byte_offset(),
            item,
            prefix);
        apply_completion_result(result, prefix);
        cancel_autocomplete();
        if (notify) notify_change();
    }

    void accept_selected_autocomplete(bool fallthrough_submit) {
        if (autocomplete.empty() || !autocomplete_provider) return;
        apply_autocomplete_item(autocomplete[autocomplete_selected], autocomplete_prefix, !fallthrough_submit);
    }

    void insert_character(std::string_view text) {
        if (text.empty()) return;
        exit_history_browsing();
        buffer.insert_character(text);
        notify_change();
        maybe_trigger_autocomplete(text.back());
    }

    void insert_text(std::string text, bool record_undo, bool update_autocomplete = true) {
        text = normalize_input(std::move(text));
        if (text.empty()) return;
        exit_history_browsing();
        buffer.insert_text(text, record_undo);
        notify_change();
        if (update_autocomplete) maybe_trigger_autocomplete(text.back());
    }

    void erase_at_cursor(bool backward) {
        exit_history_browsing();
        if (backward) {
            buffer.backspace();
        } else {
            buffer.forward_delete();
        }
        notify_change();
        update_or_retrigger_after_erase();
    }

    void move_left() {
        buffer.move_left();
        if (autocomplete_state != AutocompleteState::None) update_autocomplete();
    }

    void move_right() {
        buffer.move_right();
        if (autocomplete_state != AutocompleteState::None) update_autocomplete();
    }

    void move_word(bool forward) {
        if (forward) {
            buffer.move_word_forward();
        } else {
            buffer.move_word_backward();
        }
    }

    void kill_to_line(bool forward) {
        exit_history_browsing();
        if (forward) {
            buffer.kill_to_line_end();
        } else {
            buffer.kill_to_line_start();
        }
        notify_change();
    }

    void delete_word(bool forward) {
        exit_history_browsing();
        if (forward) {
            buffer.delete_word_forward();
        } else {
            buffer.delete_word_backward();
        }
        notify_change();
    }

    void yank() {
        exit_history_browsing();
        buffer.yank();
        notify_change();
    }

    void yank_pop() {
        exit_history_browsing();
        buffer.yank_pop();
        notify_change();
    }

    void undo_once() {
        exit_history_browsing();
        buffer.undo();
        cancel_autocomplete();
        notify_change();
    }

    void add_to_history(std::string text) {
        text = trim_outer_whitespace(std::move(text));
        if (text.empty()) return;
        if (!history.empty() && history.front() == text) return;
        history.insert(history.begin(), std::move(text));
        if (history.size() > kMaxHistoryEntries) history.pop_back();
    }

    void exit_history_browsing() {
        history_index.reset();
        history_draft.reset();
    }

    [[nodiscard]] bool editor_is_empty() const {
        return buffer.empty();
    }

    struct VisualLine {
        std::size_t logical_line{0};
        std::size_t start{0};
        std::size_t end{0};
        std::string text;
    };

    [[nodiscard]] std::size_t find_current_visual_line(const std::vector<VisualLine>& visual) const {
        const auto cur = buffer.cursor();
        for (std::size_t index = 0; index < visual.size(); ++index) {
            if (visual[index].logical_line == cur.line && cur.column >= visual[index].start &&
                cur.column <= visual[index].end) {
                return index;
            }
        }
        return visual.empty() ? 0 : visual.size() - 1;
    }

    [[nodiscard]] bool on_first_visual_line() const {
        return find_current_visual_line(visual_lines(layout_width)) == 0;
    }

    [[nodiscard]] bool on_last_visual_line() const {
        const auto visual = visual_lines(layout_width);
        return find_current_visual_line(visual) + 1 == visual.size();
    }

    void move_to_line_start() {
        buffer.move_to_line_start();
    }

    void move_to_line_end() {
        buffer.move_to_line_end();
    }

    void set_text_internal(std::string text, bool cursor_at_start) {
        buffer.set_text(std::move(text));
        if (cursor_at_start) {
            buffer.set_cursor(detail::BufferCursor{.line = 0, .column = 0});
        }
        scroll_offset = 0;
        notify_change();
    }

    void navigate_history(int direction) {
        if (history.empty()) return;

        const auto current = history_index ? static_cast<int>(*history_index) : -1;
        const auto new_index = current - direction;  // Up(-1) increases index, Down(1) decreases
        if (new_index < -1 || new_index >= static_cast<int>(history.size())) return;

        // Capture state when first entering history browsing mode.
        if (!history_index && new_index >= 0) {
            buffer.push_undo();
            history_draft = HistoryDraft{.buffer = buffer};
        }

        if (new_index == -1) {
            history_index.reset();
            auto draft = history_draft;
            history_draft.reset();
            if (draft) {
                buffer = std::move(draft->buffer);
                scroll_offset = 0;
                notify_change();
            } else {
                set_text_internal("", false);
            }
        } else {
            history_index = static_cast<std::size_t>(new_index);
            set_text_internal(history[*history_index], direction == -1);
        }
    }

    void submit() {
        const auto result = trim_outer_whitespace(expanded());
        exit_history_browsing();
        add_to_history(result);
        buffer.set_text("");
        cancel_autocomplete();
        scroll_offset = 0;
        notify_change();
        if (!on_submit) return;
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
        try {
#endif
            if (auto submitted = on_submit(result); !submitted) {
                // An explicit submit-sink failure is a bounded callback
                // diagnostic (ADR 0017); it never vetoes editing state.
                callback_error = std::move(submitted.error());
            }
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
        } catch (...) {
            callback_error = support::make_error(
                support::ErrorCode::Unknown,
                "Editor submit sink failed",
                "the submit callback threw an exception");
        }
#endif
    }

    void paste(std::string text) {
        exit_history_browsing();
        buffer.insert_paste(std::move(text));
        notify_change();
        cancel_autocomplete();
    }

    [[nodiscard]] std::vector<VisualLine> visual_lines(std::size_t width) const {
        std::vector<VisualLine> result;
        const auto& doc = buffer.document();
        for (std::size_t line_index = 0; line_index < doc.size(); ++line_index) {
            const auto& line = doc[line_index];
            if (line.empty()) {
                result.push_back({.logical_line = line_index, .start = 0, .end = 0, .text = {}});
                continue;
            }
            std::size_t start = 0;
            std::size_t used = 0;
            std::string rendered;
            for (std::size_t index = 0; index < line.size(); ++index) {
                const auto segment_width = visible_width(line[index].text);
                if (used != 0 && used + segment_width > width) {
                    result.push_back({.logical_line = line_index, .start = start, .end = index, .text = std::move(rendered)});
                    start = index;
                    used = 0;
                    rendered.clear();
                }
                if (segment_width > width) {
                    for (const auto& grapheme : detail::split_graphemes(line[index].text)) {
                        if (used != 0 && used + detail::grapheme_width(grapheme) > width) {
                            result.push_back({.logical_line = line_index, .start = start, .end = index, .text = std::move(rendered)});
                            rendered.clear();
                            used = 0;
                        }
                        rendered += grapheme;
                        used += detail::grapheme_width(grapheme);
                    }
                } else {
                    rendered += line[index].text;
                    used += segment_width;
                }
            }
            result.push_back({.logical_line = line_index, .start = start, .end = line.size(), .text = std::move(rendered)});
        }
        return result;
    }

    /// Render pi's fake cursor (editor.ts render): reverse video on the grapheme
    /// at the cursor position, or a highlighted space at end of line.
    void insert_fake_cursor(const VisualLine& visual_line, std::size_t width, std::string& line) const {
        const auto cursor_segment = buffer.cursor().column;
        const auto& doc = buffer.document();
        if (cursor_segment < visual_line.end) {
            std::size_t byte_offset = 0;
            for (std::size_t index = visual_line.start; index < cursor_segment; ++index) {
                byte_offset += doc[visual_line.logical_line][index].text.size();
            }
            const auto& segment_text = doc[visual_line.logical_line][cursor_segment].text;
            const auto graphemes = detail::split_graphemes(segment_text);
            const auto& at_cursor = graphemes.front();
            line.insert(byte_offset, "\x1b[7m");
            line.insert(byte_offset + 4 + at_cursor.size(), "\x1b[27m");
            return;
        }
        if (visible_width(line) < width) {
            line += "\x1b[7m \x1b[27m";
        }
    }

    void move_vertical(int direction) {
        const auto visual = visual_lines(layout_width);
        const auto cur = buffer.cursor();
        std::size_t current = 0;
        for (std::size_t index = 0; index < visual.size(); ++index) {
            if (visual[index].logical_line == cur.line && cur.column >= visual[index].start &&
                cur.column <= visual[index].end) {
                current = index;
                break;
            }
        }
        if (direction < 0 && current == 0) return;
        if (direction > 0 && current + 1 == visual.size()) return;
        const auto& from = visual[current];
        const auto& to = visual[static_cast<std::size_t>(static_cast<int>(current) + direction)];
        const auto target_line = to.logical_line;
        const auto target_column = std::min(to.end, to.start + (cur.column - from.start));
        buffer.set_cursor(detail::BufferCursor{.line = target_line, .column = target_column});
        if (autocomplete_state != AutocompleteState::None) update_autocomplete();
    }

    void jump_to(std::string_view target, JumpDirection direction) {
        buffer.jump_to(target, direction == JumpDirection::Forward);
    }
};

Editor::Editor(EditorOptions options, EditorChangeSink on_change, EditorSubmitSink on_submit)
    : impl_(std::make_shared<Impl>()) {
    impl_->self = impl_;
    if (!options.keybindings) options.keybindings = default_tui_keybindings();
    impl_->options = std::move(options);
    impl_->on_change = std::move(on_change);
    impl_->on_submit = std::move(on_submit);
}

Editor::Editor(Editor&& other) noexcept : impl_(std::move(other.impl_)) {
    other.impl_.reset();
}

void Editor::release_autocomplete_cycles() noexcept {
    if (!impl_) return;
    impl_->cancel_autocomplete_request();
    impl_->autocomplete_provider.reset();
    impl_->self.reset();
}

Editor& Editor::operator=(Editor&& other) noexcept {
    if (this != &other) {
        release_autocomplete_cycles();
        impl_ = std::move(other.impl_);
        other.impl_.reset();
    }
    return *this;
}

Editor::~Editor() {
    release_autocomplete_cycles();
}

std::string Editor::text() const {
    std::lock_guard lock(impl_->impl_mutex);
    return impl_->text();
}

std::string Editor::expanded_text() const {
    std::lock_guard lock(impl_->impl_mutex);
    return impl_->expanded();
}

std::vector<std::string> Editor::lines() const {
    std::lock_guard lock(impl_->impl_mutex);
    return impl_->line_strings();
}

EditorCursor Editor::cursor() const {
    std::lock_guard lock(impl_->impl_mutex);
    return impl_->cursor();
}

void Editor::set_text(std::string text) {
    std::lock_guard lock(impl_->impl_mutex);
    impl_->exit_history_browsing();
    text = normalize_input(std::move(text));
    if (text == impl_->buffer.text()) return;
    impl_->buffer.push_undo();
    impl_->buffer.set_text(std::move(text));
    impl_->cancel_autocomplete();
    impl_->notify_change();
}

void Editor::insert_text_at_cursor(std::string text) {
    std::lock_guard lock(impl_->impl_mutex);
    impl_->cancel_autocomplete();
    impl_->insert_text(std::move(text), true, false);
}

void Editor::add_to_history(std::string text) {
    std::lock_guard lock(impl_->impl_mutex);
    impl_->add_to_history(std::move(text));
}

void Editor::set_theme(EditorTheme theme) {
    std::lock_guard lock(impl_->impl_mutex);
    impl_->theme = std::move(theme);
    invalidate();
}

void Editor::set_autocomplete_provider(std::unique_ptr<AutocompleteProvider> provider) {
    std::lock_guard lock(impl_->impl_mutex);
    impl_->cancel_autocomplete();
    impl_->autocomplete_provider = std::move(provider);
    impl_->set_autocomplete_trigger_characters(
        impl_->autocomplete_provider ? impl_->autocomplete_provider->trigger_characters()
                                     : std::vector<std::string>{});
}

void Editor::set_keybindings(std::shared_ptr<const KeybindingRegistry> keybindings) {
    std::lock_guard lock(impl_->impl_mutex);
    if (keybindings) {
        impl_->options.keybindings = std::move(keybindings);
    }
    invalidate();
}

bool Editor::autocomplete_open() const {
    std::lock_guard lock(impl_->impl_mutex);
    return impl_->autocomplete_state != Impl::AutocompleteState::None;
}

std::vector<AutocompleteItem> Editor::autocomplete_items() const {
    std::lock_guard lock(impl_->impl_mutex);
    return impl_->autocomplete;
}

std::size_t Editor::autocomplete_selected_index() const {
    std::lock_guard lock(impl_->impl_mutex);
    return impl_->autocomplete_selected;
}

support::Expected<RenderResult> Editor::render(std::size_t width) {
    std::lock_guard lock(impl_->impl_mutex);
    impl_->drain_autocomplete_result();
    if (impl_->callback_error) return std::unexpected(*impl_->callback_error);
    if (width == 0) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation, "Editor requires a positive visible width"));
    }
    impl_->layout_width = width;
    for (const auto& logical_line : impl_->buffer.document()) {
        for (const auto& segment : logical_line) {
            for (const auto& grapheme : detail::split_graphemes(segment.text)) {
                if (detail::grapheme_width(grapheme) > width) {
                    return std::unexpected(support::make_error(
                        support::ErrorCode::Validation,
                        "Editor grapheme is wider than the available visible width"));
                }
            }
        }
    }
    const auto visual = impl_->visual_lines(width);
    std::size_t cursor_line = 0;
    const auto cur = impl_->buffer.cursor();
    for (std::size_t index = 0; index < visual.size(); ++index) {
        if (visual[index].logical_line == cur.line && cur.column >= visual[index].start &&
            cur.column <= visual[index].end) {
            cursor_line = index;
            break;
        }
    }
    const auto visible_count = std::max<std::size_t>(1, impl_->content_height());
    if (cursor_line < impl_->scroll_offset) impl_->scroll_offset = cursor_line;
    if (cursor_line >= impl_->scroll_offset + visible_count) impl_->scroll_offset = cursor_line + 1 - visible_count;
    std::vector<std::string> result;
    if (impl_->theme.border) {
        auto top_border = impl_->scroll_offset > 0
            ? impl_->scroll_border("↑", impl_->scroll_offset, width)
            : horizontal_rule(width);
        auto styled_border = detail::apply_text_style(
            impl_->theme.border, std::move(top_border), "Editor border");
        if (!styled_border) return std::unexpected(styled_border.error());
        result.push_back(std::move(*styled_border));
    }
    const auto end = std::min(visual.size(), impl_->scroll_offset + visible_count);
    for (std::size_t index = impl_->scroll_offset; index < end; ++index) {
        auto line = visual[index].text;
        if (index == cursor_line) {
            impl_->insert_fake_cursor(visual[index], width, line);
        }
        const auto line_width = visible_width(line);
        if (line_width < width) line.append(width - line_width, ' ');
        auto styled = detail::apply_text_style(impl_->theme.text, std::move(line), "Editor text");
        if (!styled) return std::unexpected(styled.error());
        result.push_back(std::move(*styled));
    }
    if (result.size() == (impl_->theme.border ? 1 : 0)) {
        auto styled = detail::apply_text_style(
            impl_->theme.text, std::string(width, ' '), "Editor text");
        if (!styled) return std::unexpected(styled.error());
        result.push_back(std::move(*styled));
    }
    if (impl_->theme.border) {
        const auto shown = impl_->scroll_offset + visible_count;
        const auto lines_below = visual.size() > shown ? visual.size() - shown : 0;
        auto bottom_border = lines_below > 0
            ? impl_->scroll_border("↓", lines_below, width)
            : horizontal_rule(width);
        auto styled_border = detail::apply_text_style(
            impl_->theme.border, std::move(bottom_border), "Editor border");
        if (!styled_border) return std::unexpected(styled_border.error());
        result.push_back(std::move(*styled_border));
    }
    return RenderResult{.lines = std::move(result)};
}

void Editor::invalidate() {}

void Editor::handle_input(const InputEventVariant& input) {
    std::lock_guard lock(impl_->impl_mutex);
    impl_->drain_autocomplete_result();
    if (const auto* paste = std::get_if<PasteEvent>(&input)) {
        impl_->paste(paste->text);
        return;
    }
    const auto* event = std::get_if<KeyEvent>(&input);
    if (event == nullptr || event->type == KeyEventType::Release) return;
    const auto matches = [this, event](std::string_view action_id) {
        return impl_->options.keybindings->matches(*event, action_id);
    };

    if (impl_->jump_direction) {
        if (matches("tui.editor.jumpForward") || matches("tui.editor.jumpBackward")) {
            impl_->jump_direction.reset();
            return;
        }
        if (detail::is_printable(*event)) {
            impl_->jump_to(detail::printable_text(*event), *impl_->jump_direction);
            impl_->jump_direction.reset();
            return;
        }
        impl_->jump_direction.reset();
    }

    if (impl_->autocomplete_state != Impl::AutocompleteState::None) {
        if (matches("tui.select.cancel")) {
            impl_->cancel_autocomplete();
            return;
        }
        if (matches("tui.select.up")) {
            if (impl_->autocomplete_selected > 0) --impl_->autocomplete_selected;
            return;
        }
        if (matches("tui.select.down")) {
            impl_->autocomplete_selected = std::min(
                impl_->autocomplete_selected + 1,
                impl_->autocomplete.size() - 1);
            return;
        }
        if (matches("tui.input.tab")) {
            impl_->accept_selected_autocomplete(false);
            return;
        }
        if (matches("tui.select.confirm")) {
            const bool fallthrough_submit = impl_->autocomplete_prefix.starts_with('/');
            impl_->accept_selected_autocomplete(fallthrough_submit);
            if (!fallthrough_submit) return;
        }
    }

    if (matches("tui.input.tab")) {
        impl_->handle_tab_completion();
        return;
    }
    if (matches("tui.editor.undo")) {
        impl_->undo_once();
        return;
    }
    if (matches("tui.editor.jumpForward") || matches("tui.editor.jumpBackward")) {
        impl_->jump_direction = matches("tui.editor.jumpForward")
            ? Impl::JumpDirection::Forward
            : Impl::JumpDirection::Backward;
        return;
    }
    if (matches("tui.editor.deleteCharBackward") || matches_key(*event, "shift+backspace")) {
        impl_->erase_at_cursor(true);
        return;
    }
    if (matches("tui.editor.deleteCharForward") || matches_key(*event, "shift+delete")) {
        impl_->erase_at_cursor(false);
        return;
    }
    if (matches("tui.editor.deleteWordBackward")) {
        impl_->delete_word(false);
        return;
    }
    if (matches("tui.editor.deleteWordForward")) {
        impl_->delete_word(true);
        return;
    }
    if (matches("tui.editor.deleteToLineStart")) {
        impl_->kill_to_line(false);
        return;
    }
    if (matches("tui.editor.deleteToLineEnd")) {
        impl_->kill_to_line(true);
        return;
    }
    if (matches("tui.editor.yank")) {
        impl_->yank();
        return;
    }
    if (matches("tui.editor.yankPop")) {
        impl_->yank_pop();
        return;
    }
    if (matches("tui.editor.cursorLineStart")) {
        impl_->move_to_line_start();
        return;
    }
    if (matches("tui.editor.cursorLineEnd")) {
        impl_->move_to_line_end();
        return;
    }
    if (matches("tui.editor.cursorLeft")) {
        impl_->move_left();
        return;
    }
    if (matches("tui.editor.cursorRight")) {
        impl_->move_right();
        return;
    }
    if (matches("tui.editor.cursorWordLeft")) {
        impl_->move_word(false);
        return;
    }
    if (matches("tui.editor.cursorWordRight")) {
        impl_->move_word(true);
        return;
    }
    if (matches("tui.editor.cursorUp")) {
        const auto cur = impl_->buffer.cursor();
        if (impl_->on_first_visual_line() &&
            (impl_->editor_is_empty() || impl_->history_index.has_value() || cur.column == 0)) {
            impl_->navigate_history(-1);
        } else if (impl_->on_first_visual_line()) {
            impl_->move_to_line_start();
        } else {
            impl_->move_vertical(-1);
        }
        return;
    }
    if (matches("tui.editor.cursorDown")) {
        if (impl_->history_index.has_value() && impl_->on_last_visual_line()) {
            impl_->navigate_history(1);
        } else if (impl_->on_last_visual_line()) {
            impl_->move_to_line_end();
        } else {
            impl_->move_vertical(1);
        }
        return;
    }
    if (matches("tui.editor.pageUp")) {
        for (std::size_t index = 0; index < impl_->options.max_visible_lines; ++index) impl_->move_vertical(-1);
        return;
    }
    if (matches("tui.editor.pageDown")) {
        for (std::size_t index = 0; index < impl_->options.max_visible_lines; ++index) impl_->move_vertical(1);
        return;
    }
    if (matches("tui.input.newLine")) {
        impl_->cancel_autocomplete();
        impl_->insert_text("\n", true);
        return;
    }
    if (matches("tui.input.submit")) {
        impl_->submit();
        return;
    }
    if (detail::is_printable(*event)) impl_->insert_character(detail::printable_text(*event));
}

bool Editor::accepts_key_releases() const {
    std::lock_guard lock(impl_->impl_mutex);
    return false;
}

void Editor::set_focused(bool focused) {
    std::lock_guard lock(impl_->impl_mutex);
    impl_->focused = focused;
}

bool Editor::focused() const {
    std::lock_guard lock(impl_->impl_mutex);
    return impl_->focused;
}

std::optional<CursorPosition> Editor::cursor_location() const {
    std::lock_guard lock(impl_->impl_mutex);
    if (!impl_->focused || impl_->layout_width == 0) return std::nullopt;
    const auto visual = impl_->visual_lines(impl_->layout_width);
    if (visual.empty()) return std::nullopt;

    std::size_t visual_row = 0;
    bool found = false;
    const auto cur = impl_->buffer.cursor();
    for (std::size_t index = 0; index < visual.size(); ++index) {
        if (visual[index].logical_line == cur.line &&
            cur.column >= visual[index].start &&
            cur.column <= visual[index].end) {
            visual_row = index;
            found = true;
            break;
        }
    }
    if (!found) return std::nullopt;

    const auto visible_count = std::max<std::size_t>(1, impl_->content_height());
    if (visual_row < impl_->scroll_offset) return std::nullopt;
    if (visual_row >= impl_->scroll_offset + visible_count) return std::nullopt;

    const auto display_row = impl_->border_rows() + visual_row - impl_->scroll_offset;

    const auto& vl = visual[visual_row];
    const auto vl_text_width = visible_width(vl.text);
    const auto cursor_in_line = cur.column - vl.start;
    const auto segs_in_line = vl.end - vl.start;
    std::size_t col = 0;
    if (segs_in_line > 0 && cursor_in_line <= segs_in_line) {
        const auto seg_end = vl.start + cursor_in_line;
        const auto& doc = impl_->buffer.document();
        for (std::size_t i = vl.start; i < seg_end && i < doc[vl.logical_line].size(); ++i) {
            col += visible_width(doc[vl.logical_line][i].text);
        }
        col = std::min(col, vl_text_width);
    }
    return CursorPosition{.column = col, .row = display_row};
}

void Editor::set_available_height(std::size_t rows) {
    std::lock_guard lock(impl_->impl_mutex);
    impl_->available_height = std::max<std::size_t>(1, rows);
}

} // namespace cch::tui
