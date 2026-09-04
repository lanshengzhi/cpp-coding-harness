#include <cch/tui/Editor.hpp>

#include <cch/tui/TruncatedText.hpp>
#include <cch/tui/Utils.hpp>
#include "tui/EditorCompletionSession.hpp"
#include "tui/InteractionUtils.hpp"
#include "tui/TextBuffer.hpp"
#include "tui/UnicodeWidth.hpp"

#include <cch/support/Error.hpp>

#include <algorithm>
#include <array>
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

/// Dispatch tables: the first bound action in table order wins, so table
/// order IS pi's dispatch precedence (pi editor-component.ts). The open
/// completion menu's table is consulted ahead of the main table.
constexpr std::array<std::string_view, 5> kCompletionMenuActions = {
        "tui.select.cancel",
        "tui.select.up",
        "tui.select.down",
        "tui.input.tab",
        "tui.select.confirm",
};

/// Main editing dispatch order (pi editor-component.ts). The raw
/// shift+backspace / shift+delete guards stay OR-ed at their chain positions.
constexpr std::array<std::string_view, 24> kEditorActions = {
        "tui.input.tab",
        "tui.editor.undo",
        "tui.editor.jumpForward",
        "tui.editor.jumpBackward",
        "tui.editor.deleteCharBackward",
        "tui.editor.deleteCharForward",
        "tui.editor.deleteWordBackward",
        "tui.editor.deleteWordForward",
        "tui.editor.deleteToLineStart",
        "tui.editor.deleteToLineEnd",
        "tui.editor.yank",
        "tui.editor.yankPop",
        "tui.editor.cursorLineStart",
        "tui.editor.cursorLineEnd",
        "tui.editor.cursorLeft",
        "tui.editor.cursorRight",
        "tui.editor.cursorWordLeft",
        "tui.editor.cursorWordRight",
        "tui.editor.cursorUp",
        "tui.editor.cursorDown",
        "tui.editor.pageUp",
        "tui.editor.pageDown",
        "tui.input.newLine",
        "tui.input.submit",
};

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

    std::unique_ptr<detail::EditorCompletionSession> completion_session;
    detail::EditorCompletionMenuPresentation autocomplete_menu{};
    std::vector<std::string> autocomplete_trigger_characters{"@", "#"};

    /// Serializes every public entry point (input, render, and timer entry).
    /// Completion callbacks re-enter only through the weak serialized sinks
    /// installed by Editor's constructor.
    std::mutex impl_mutex;
    /// Admits one complete public operation while completion work temporarily
    /// releases impl_mutex for provider/timer calls. Recursive admission is
    /// limited to lifecycle teardown re-entered by a render notification.
    std::recursive_mutex operation_gate;

    void enter_operation() noexcept { operation_gate.lock(); }

    void leave_operation() noexcept { operation_gate.unlock(); }

    struct SerializedOperation {
        std::shared_ptr<Impl> keep_alive;
        Impl& impl; // kept alive by keep_alive for the operation's full scope.

        explicit SerializedOperation(std::shared_ptr<Impl> owner) : keep_alive(std::move(owner)), impl(*keep_alive) {
            impl.enter_operation();
        }
        ~SerializedOperation() { impl.leave_operation(); }

        SerializedOperation(const SerializedOperation&) = delete;
        SerializedOperation& operator=(const SerializedOperation&) = delete;
    };

    [[nodiscard]] SerializedOperation serialized_operation(std::shared_ptr<Impl> owner) {
        return SerializedOperation{std::move(owner)};
    }

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

    [[nodiscard]] detail::EditorCompletionView completion_view() const {
        const auto current_cursor = buffer.cursor();
        return detail::EditorCompletionView{
                .text = text(),
                .cursor = cursor(),
                .lines = line_strings(),
                .cursor_line = current_cursor.line,
                .cursor_column = cursor_byte_offset(),
        };
    }

    template <typename Callback>
    decltype(auto) outside_impl_lock(std::unique_lock<std::mutex>& lock, Callback&& callback) {
        struct RelockOnExit {
            std::unique_lock<std::mutex>& lock; // must outlive this guard's scope.
            ~RelockOnExit() {
                if (!lock.owns_lock()) lock.lock();
            }
        } relock{lock};
        lock.unlock();
        return callback();
    }

    [[nodiscard]] bool completion_view_matches(const detail::EditorCompletionView& expected) const {
        const auto current = completion_view();
        return current.text == expected.text && current.cursor == expected.cursor && current.lines == expected.lines &&
               current.cursor_line == expected.cursor_line && current.cursor_column == expected.cursor_column;
    }

    void update_completion_effect(detail::EditorCompletionEffect effect,
            const detail::EditorCompletionView& request_view,
            std::unique_lock<std::mutex>& lock) {
        autocomplete_menu = std::move(effect.menu);
        if (!effect.application || !completion_view_matches(request_view)) return;
        const auto submit_after = effect.submit;
        apply_completion_application(std::move(*effect.application));
        if (submit_after) submit(lock);
    }

    void handle_completion_interaction(detail::EditorCompletionInteraction interaction,
            const detail::EditorCompletionView& request_view,
            std::unique_lock<std::mutex>& lock) {
        auto result = outside_impl_lock(lock, [this, interaction = std::move(interaction), request_view]() mutable {
            return completion_session->handle(std::move(interaction), request_view);
        });
        if (!result) {
            callback_error = std::move(result.error());
            return;
        }
        update_completion_effect(std::move(*result), request_view, lock);
    }

    std::shared_ptr<EditorRenderRequestSink> render_request_sink;

    void request_render(std::unique_lock<std::mutex>& lock) {
        if (!render_request_sink || !*render_request_sink) return;
        const bool sink_threw = outside_impl_lock(lock, [sink = render_request_sink] {
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
            try {
#endif
                if (*sink) static_cast<void>((*sink)());
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
            } catch (...) {
                return true;
            }
#endif
            return false;
        });
        if (sink_threw) {
            // A throwing render-request sink is a bounded callback diagnostic
            // (ADR 0017); the observer is deactivated after failure.
            callback_error = support::make_error(support::ErrorCode::Unknown,
                    "Editor render request sink failed",
                    "the render request callback threw an exception");
            *render_request_sink = nullptr;
        }
    }

    void cancel_autocomplete(std::unique_lock<std::mutex>& lock, bool mutation_schedules_frame = false) {
        const bool was_open = autocomplete_menu.open;
        const auto request_view = completion_view();
        handle_completion_interaction(
                detail::EditorCompletionInteraction{detail::EditorCompletionCancel{}}, request_view, lock);
        // A text mutation in the same handling already schedules the frame
        // through the change notification; only presentation-only cancels
        // request a render.
        if (was_open && !mutation_schedules_frame) {
            request_render(lock);
        }
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

    void request_autocomplete(bool force, bool explicit_tab, std::unique_lock<std::mutex>& lock) {
        const auto request_view = completion_view();
        const auto debounce_ms = get_autocomplete_debounce_ms(force, explicit_tab);
        handle_completion_interaction(
                detail::EditorCompletionInteraction{detail::EditorCompletionRefresh{
                        .intent = detail::EditorCompletionIntent{.force = force, .explicit_tab = explicit_tab},
                        .debounce = debounce_ms,
                }},
                request_view,
                lock);
    }

    void wake_autocomplete(std::unique_lock<std::mutex>& lock) {
        const auto request_view = completion_view();
        handle_completion_interaction(
                detail::EditorCompletionInteraction{detail::EditorCompletionWake{}}, request_view, lock);
    }

    void on_completion_wake(std::shared_ptr<Impl> owner) {
        auto operation = serialized_operation(std::move(owner));
        auto& impl = operation.impl;
        std::unique_lock lock(impl.impl_mutex);
        impl.wake_autocomplete(lock);
    }

    void update_autocomplete(std::unique_lock<std::mutex>& lock) {
        if (!autocomplete_menu.open) return;
        request_autocomplete(autocomplete_menu.forced, false, lock);
    }

    void try_trigger_autocomplete(std::unique_lock<std::mutex>& lock) { request_autocomplete(false, false, lock); }

    /// pi insertCharacter's autocomplete tail: auto-trigger on slash at
    /// message start, trigger characters at token boundaries, and word
    /// characters inside a slash or attachment context; an open menu instead
    /// re-queries with its force mode.
    void maybe_trigger_autocomplete(char last_char, std::unique_lock<std::mutex>& lock) {
        if (autocomplete_menu.open) {
            update_autocomplete(lock);
            return;
        }
        const auto text_before_cursor = line_prefix_before_cursor();
        if (last_char == '/' && is_at_start_of_message()) {
            try_trigger_autocomplete(lock);
            return;
        }
        if (is_trigger_character(last_char)) {
            const auto length = text_before_cursor.size();
            if (length == 1 ||
                (length >= 2 && (text_before_cursor[length - 2] == ' ' || text_before_cursor[length - 2] == '\t'))) {
                try_trigger_autocomplete(lock);
            }
            return;
        }
        if (is_word_character(last_char)) {
            if (in_slash_command_context(text_before_cursor) || autocomplete_pattern_matches(text_before_cursor)) {
                try_trigger_autocomplete(lock);
            }
        }
    }

    /// pi handleBackspace/handleForwardDelete's autocomplete tail.
    void update_or_retrigger_after_erase(std::unique_lock<std::mutex>& lock) {
        if (autocomplete_menu.open) {
            update_autocomplete(lock);
            return;
        }
        const auto text_before_cursor = line_prefix_before_cursor();
        if (in_slash_command_context(text_before_cursor) || autocomplete_pattern_matches(text_before_cursor)) {
            try_trigger_autocomplete(lock);
        }
    }

    void handle_tab_completion(std::unique_lock<std::mutex>& lock) {
        const bool was_open = autocomplete_menu.open;
        const auto text_before_cursor = line_prefix_before_cursor();
        if (in_slash_command_context(text_before_cursor) &&
            detail::trim_start_ascii(text_before_cursor).find(' ') == std::string_view::npos) {
            request_autocomplete(false, true, lock);
        } else {
            request_autocomplete(true, true, lock);
        }
        if (!was_open && autocomplete_menu.open) {
            request_render(lock);
        }
    }

    void handle_completion_menu_action(detail::EditorCompletionMenuAction action, std::unique_lock<std::mutex>& lock) {
        if (!autocomplete_menu.open) return;
        const auto previous_selected = autocomplete_menu.selected_index;
        const auto request_view = completion_view();
        handle_completion_interaction(
                detail::EditorCompletionInteraction{detail::EditorCompletionMenuInteraction{.action = action}},
                request_view,
                lock);
        if (action == detail::EditorCompletionMenuAction::MoveUp ||
                action == detail::EditorCompletionMenuAction::MoveDown) {
            if (previous_selected != autocomplete_menu.selected_index) {
                request_render(lock);
            }
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

    void apply_completion_application(detail::EditorCompletionApplication application) {
        buffer.push_undo();
        apply_completion_result(application.result, application.prefix);
        if (application.notify) notify_change();
    }

    void insert_character(std::string_view text, std::unique_lock<std::mutex>& lock) {
        if (text.empty()) return;
        exit_history_browsing();
        buffer.insert_character(text);
        notify_change();
        maybe_trigger_autocomplete(text.back(), lock);
    }

    void insert_text(
            std::string text, bool record_undo, std::unique_lock<std::mutex>& lock, bool update_autocomplete = true) {
        text = normalize_input(std::move(text));
        if (text.empty()) return;
        exit_history_browsing();
        buffer.insert_text(text, record_undo);
        notify_change();
        if (update_autocomplete) maybe_trigger_autocomplete(text.back(), lock);
    }

    void erase_at_cursor(bool backward, std::unique_lock<std::mutex>& lock) {
        exit_history_browsing();
        if (backward) {
            buffer.backspace();
        } else {
            buffer.forward_delete();
        }
        notify_change();
        update_or_retrigger_after_erase(lock);
    }

    void move_left(std::unique_lock<std::mutex>& lock) {
        buffer.move_left();
        if (autocomplete_menu.open) update_autocomplete(lock);
    }

    void move_right(std::unique_lock<std::mutex>& lock) {
        buffer.move_right();
        if (autocomplete_menu.open) update_autocomplete(lock);
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

    void undo_once(std::unique_lock<std::mutex>& lock) {
        exit_history_browsing();
        buffer.undo();
        cancel_autocomplete(lock, true);
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

    void submit(std::unique_lock<std::mutex>& lock) {
        const auto result = trim_outer_whitespace(expanded());
        exit_history_browsing();
        add_to_history(result);
        buffer.set_text("");
        cancel_autocomplete(lock, true);
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

    void paste(std::string text, std::unique_lock<std::mutex>& lock) {
        exit_history_browsing();
        buffer.insert_paste(std::move(text));
        notify_change();
        cancel_autocomplete(lock, true);
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

    void move_vertical(int direction, std::unique_lock<std::mutex>& lock) {
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
        if (autocomplete_menu.open) update_autocomplete(lock);
    }

    void jump_to(std::string_view target, JumpDirection direction) {
        buffer.jump_to(target, direction == JumpDirection::Forward);
    }
};

Editor::Editor(EditorOptions options, EditorChangeSink on_change, EditorSubmitSink on_submit)
    : impl_(std::make_shared<Impl>()) {
    if (!options.keybindings) options.keybindings = default_tui_keybindings();
    impl_->options = std::move(options);
    impl_->on_change = std::move(on_change);
    impl_->on_submit = std::move(on_submit);
    impl_->render_request_sink = std::make_shared<EditorRenderRequestSink>(std::move(impl_->options.render_request));
    const auto weak_impl = std::weak_ptr<Impl>{impl_};
    impl_->completion_session = std::make_unique<detail::EditorCompletionSession>(
            std::move(impl_->options.autocomplete_debounce_timer),
            [weak_impl]() -> support::ExpectedVoid {
                if (const auto impl = weak_impl.lock()) impl->on_completion_wake(impl);
                return {};
            },
            [sink = impl_->render_request_sink]() -> support::ExpectedVoid {
                if (*sink) return (*sink)();
                return {};
            });
}

Editor::Editor(Editor&& other) noexcept : impl_(std::move(other.impl_)) {
    other.impl_.reset();
}

void Editor::release_autocomplete_cycles() noexcept {
    auto owner = impl_;
    if (!owner) return;
    auto operation = owner->serialized_operation(std::move(owner));
    auto& impl = operation.impl;
    std::unique_lock lock(impl.impl_mutex);
    if (impl.render_request_sink) *impl.render_request_sink = nullptr;
    impl.outside_impl_lock(lock, [session = impl.completion_session.get()] { session->close(); });
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
    auto operation = impl_->serialized_operation(impl_);
    auto& impl = operation.impl;
    std::lock_guard lock(impl.impl_mutex);
    return impl.text();
}

std::string Editor::expanded_text() const {
    auto operation = impl_->serialized_operation(impl_);
    auto& impl = operation.impl;
    std::lock_guard lock(impl.impl_mutex);
    return impl.expanded();
}

std::vector<std::string> Editor::lines() const {
    auto operation = impl_->serialized_operation(impl_);
    auto& impl = operation.impl;
    std::lock_guard lock(impl.impl_mutex);
    return impl.line_strings();
}

EditorCursor Editor::cursor() const {
    auto operation = impl_->serialized_operation(impl_);
    auto& impl = operation.impl;
    std::lock_guard lock(impl.impl_mutex);
    return impl.cursor();
}

void Editor::set_text(std::string text) {
    auto operation = impl_->serialized_operation(impl_);
    auto& impl = operation.impl;
    std::unique_lock lock(impl.impl_mutex);
    impl.exit_history_browsing();
    text = normalize_input(std::move(text));
    if (text == impl.buffer.text()) return;
    impl.buffer.push_undo();
    impl.buffer.set_text(std::move(text));
    impl.cancel_autocomplete(lock, true);
    impl.notify_change();
}

void Editor::insert_text_at_cursor(std::string text) {
    auto operation = impl_->serialized_operation(impl_);
    auto& impl = operation.impl;
    std::unique_lock lock(impl.impl_mutex);
    impl.cancel_autocomplete(lock, !text.empty());
    impl.insert_text(std::move(text), true, lock, false);
}

void Editor::add_to_history(std::string text) {
    auto operation = impl_->serialized_operation(impl_);
    auto& impl = operation.impl;
    std::lock_guard lock(impl.impl_mutex);
    impl.add_to_history(std::move(text));
}

void Editor::set_theme(EditorTheme theme) {
    auto operation = impl_->serialized_operation(impl_);
    auto& impl = operation.impl;
    std::lock_guard lock(impl.impl_mutex);
    impl.theme = std::move(theme);
    invalidate();
}

void Editor::set_autocomplete_provider(std::unique_ptr<AutocompleteProvider> provider) {
    auto operation = impl_->serialized_operation(impl_);
    auto& impl = operation.impl;
    std::unique_lock lock(impl.impl_mutex);
    detail::EditorCompletionProviderSetup setup;
    impl.outside_impl_lock(
            lock, [session = impl.completion_session.get(), &setup, provider = std::move(provider)]() mutable {
                setup = session->set_provider(std::move(provider));
            });
    impl.set_autocomplete_trigger_characters(setup.trigger_characters);
    const bool was_open = impl.autocomplete_menu.open;
    impl.autocomplete_menu = std::move(setup.menu);
    if (was_open || impl.autocomplete_menu.open) {
        impl.request_render(lock);
    }
}

void Editor::set_keybindings(std::shared_ptr<const KeybindingRegistry> keybindings) {
    auto operation = impl_->serialized_operation(impl_);
    auto& impl = operation.impl;
    std::lock_guard lock(impl.impl_mutex);
    if (keybindings) {
        impl.options.keybindings = std::move(keybindings);
    }
    invalidate();
}

support::Expected<RenderResult> Editor::render(std::size_t width) {
    auto operation = impl_->serialized_operation(impl_);
    auto& impl = operation.impl;
    std::unique_lock lock(impl.impl_mutex);
    impl.wake_autocomplete(lock);
    if (impl.callback_error) return std::unexpected(*impl.callback_error);
    if (width == 0) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation, "Editor requires a positive visible width"));
    }
    impl.layout_width = width;
    for (const auto& logical_line : impl.buffer.document()) {
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
    const auto visual = impl.visual_lines(width);
    std::size_t cursor_line = 0;
    const auto cur = impl.buffer.cursor();
    for (std::size_t index = 0; index < visual.size(); ++index) {
        if (visual[index].logical_line == cur.line && cur.column >= visual[index].start &&
            cur.column <= visual[index].end) {
            cursor_line = index;
            break;
        }
    }
    const auto visible_count = std::max<std::size_t>(1, impl.content_height());
    if (cursor_line < impl.scroll_offset) impl.scroll_offset = cursor_line;
    if (cursor_line >= impl.scroll_offset + visible_count) impl.scroll_offset = cursor_line + 1 - visible_count;
    std::vector<std::string> result;
    if (impl.theme.border) {
        auto top_border =
                impl.scroll_offset > 0 ? impl.scroll_border("↑", impl.scroll_offset, width) : horizontal_rule(width);
        auto styled_border = detail::apply_text_style(impl.theme.border, std::move(top_border), "Editor border");
        if (!styled_border) return std::unexpected(styled_border.error());
        result.push_back(std::move(*styled_border));
    }
    const auto end = std::min(visual.size(), impl.scroll_offset + visible_count);
    for (std::size_t index = impl.scroll_offset; index < end; ++index) {
        auto line = visual[index].text;
        if (index == cursor_line) {
            impl.insert_fake_cursor(visual[index], width, line);
        }
        const auto line_width = visible_width(line);
        if (line_width < width) line.append(width - line_width, ' ');
        auto styled = detail::apply_text_style(impl.theme.text, std::move(line), "Editor text");
        if (!styled) return std::unexpected(styled.error());
        result.push_back(std::move(*styled));
    }
    if (result.size() == (impl.theme.border ? 1 : 0)) {
        auto styled = detail::apply_text_style(impl.theme.text, std::string(width, ' '), "Editor text");
        if (!styled) return std::unexpected(styled.error());
        result.push_back(std::move(*styled));
    }
    if (impl.theme.border) {
        const auto shown = impl.scroll_offset + visible_count;
        const auto lines_below = visual.size() > shown ? visual.size() - shown : 0;
        auto bottom_border = lines_below > 0 ? impl.scroll_border("↓", lines_below, width) : horizontal_rule(width);
        auto styled_border = detail::apply_text_style(impl.theme.border, std::move(bottom_border), "Editor border");
        if (!styled_border) return std::unexpected(styled_border.error());
        result.push_back(std::move(*styled_border));
    }
    if (impl.autocomplete_menu.open && !impl.autocomplete_menu.items.empty()) {
        constexpr std::size_t kMaxAutocompleteRows = 5;
        const auto text_lines_count = result.size();
        const auto remainder_height =
                impl.available_height > text_lines_count ? impl.available_height - text_lines_count : 0;
        const auto autocomplete_capacity = std::min(kMaxAutocompleteRows, remainder_height);
        if (autocomplete_capacity > 0) {
            const auto selected = impl.autocomplete_menu.selected_index;
            const auto first_autocomplete = selected < autocomplete_capacity || autocomplete_capacity == 0
                                                    ? 0
                                                    : selected - autocomplete_capacity + 1;
            const auto autocomplete_count = std::min(autocomplete_capacity,
                    impl.autocomplete_menu.items.size() -
                            std::min(first_autocomplete, impl.autocomplete_menu.items.size()));
            for (std::size_t offset = 0; offset < autocomplete_count; ++offset) {
                const auto index = first_autocomplete + offset;
                std::string text = index == selected ? "> /" : "  /";
                text += impl.autocomplete_menu.items[index].label;
                if (!impl.autocomplete_menu.items[index].description.empty()) {
                    text += " — " + impl.autocomplete_menu.items[index].description;
                }
                TruncatedText item{std::move(text)};
                if (auto rendered = item.render(width); !rendered) {
                    return std::unexpected(rendered.error());
                } else if (!rendered->lines.empty()) {
                    result.push_back(std::move(rendered->lines.front()));
                }
            }
        }
    }
    return RenderResult{.lines = std::move(result)};
}

void Editor::invalidate() {}

InputAdmissionOutcome Editor::handle_input(const InputEventVariant& input) {
    auto operation = this->impl_->serialized_operation(this->impl_);
    auto& impl = operation.impl;
    std::unique_lock lock(impl.impl_mutex);
    impl.wake_autocomplete(lock);
    if (const auto* paste = std::get_if<PasteEvent>(&input)) {
        impl.paste(paste->text, lock);
        return InputAdmissionOutcome::Consumed;
    }
    const auto* event = std::get_if<KeyEvent>(&input);
    if (!carries_press_behavior(event)) {
        return InputAdmissionOutcome::Unhandled;
    }
    const auto matches = [&impl, event](std::string_view action_id) {
        return impl.options.keybindings->matches(*event, action_id);
    };

    if (impl.jump_direction) {
        if (matches("tui.editor.jumpForward") || matches("tui.editor.jumpBackward")) {
            impl.jump_direction.reset();
            return InputAdmissionOutcome::Consumed;
        }
        if (detail::is_printable(*event)) {
            impl.jump_to(detail::printable_text(*event), *impl.jump_direction);
            impl.jump_direction.reset();
            return InputAdmissionOutcome::Consumed;
        }
        impl.jump_direction.reset();
    }

    // The open completion menu's table dispatches ahead of the main table
    // (pi's autocomplete branch); a menu miss falls through to editing.
    if (impl.autocomplete_menu.open) {
        if (const auto menu_action = impl.options.keybindings->first_match(*event, kCompletionMenuActions)) {
            if (*menu_action == "tui.select.cancel") {
                impl.cancel_autocomplete(lock);
                return InputAdmissionOutcome::Consumed;
            }
            if (*menu_action == "tui.select.up") {
                impl.handle_completion_menu_action(detail::EditorCompletionMenuAction::MoveUp, lock);
                return InputAdmissionOutcome::Consumed;
            }
            if (*menu_action == "tui.select.down") {
                impl.handle_completion_menu_action(detail::EditorCompletionMenuAction::MoveDown, lock);
                return InputAdmissionOutcome::Consumed;
            }
            if (*menu_action == "tui.input.tab") {
                impl.handle_completion_menu_action(detail::EditorCompletionMenuAction::Accept, lock);
                return InputAdmissionOutcome::Consumed;
            }
            impl.handle_completion_menu_action(detail::EditorCompletionMenuAction::Confirm, lock);
            return InputAdmissionOutcome::Consumed;
        }
    }

    // Cancellation claims its key only while the menu is open. With the menu
    // closed the event stays unhandled so an interrupt/cancellation overlap
    // keeps its application-first precedence (#594).
    if (matches("tui.select.cancel")) {
        return InputAdmissionOutcome::Unhandled;
    }

    // Main dispatch in kEditorActions order. The raw shift+backspace /
    // shift+delete guards stay OR-ed at their chain positions (pi parity):
    // they fire exactly when no earlier-listed action claimed the event.
    const auto action = impl.options.keybindings->first_match(*event, kEditorActions);
    if (action == "tui.input.tab") {
        impl.handle_tab_completion(lock);
        return InputAdmissionOutcome::Consumed;
    }
    if (action == "tui.editor.undo") {
        impl.undo_once(lock);
        return InputAdmissionOutcome::Consumed;
    }
    if (action == "tui.editor.jumpForward" || action == "tui.editor.jumpBackward") {
        impl.jump_direction =
                action == "tui.editor.jumpForward" ? Impl::JumpDirection::Forward : Impl::JumpDirection::Backward;
        return InputAdmissionOutcome::Consumed;
    }
    if (action == "tui.editor.deleteCharBackward" || matches_key(*event, "shift+backspace")) {
        impl.erase_at_cursor(true, lock);
        return InputAdmissionOutcome::Consumed;
    }
    if (action == "tui.editor.deleteCharForward" || matches_key(*event, "shift+delete")) {
        impl.erase_at_cursor(false, lock);
        return InputAdmissionOutcome::Consumed;
    }
    if (action == "tui.editor.deleteWordBackward") {
        impl.delete_word(false);
        return InputAdmissionOutcome::Consumed;
    }
    if (action == "tui.editor.deleteWordForward") {
        impl.delete_word(true);
        return InputAdmissionOutcome::Consumed;
    }
    if (action == "tui.editor.deleteToLineStart") {
        impl.kill_to_line(false);
        return InputAdmissionOutcome::Consumed;
    }
    if (action == "tui.editor.deleteToLineEnd") {
        impl.kill_to_line(true);
        return InputAdmissionOutcome::Consumed;
    }
    if (action == "tui.editor.yank") {
        impl.yank();
        return InputAdmissionOutcome::Consumed;
    }
    if (action == "tui.editor.yankPop") {
        impl.yank_pop();
        return InputAdmissionOutcome::Consumed;
    }
    if (action == "tui.editor.cursorLineStart") {
        impl.move_to_line_start();
        return InputAdmissionOutcome::Consumed;
    }
    if (action == "tui.editor.cursorLineEnd") {
        impl.move_to_line_end();
        return InputAdmissionOutcome::Consumed;
    }
    if (action == "tui.editor.cursorLeft") {
        impl.move_left(lock);
        return InputAdmissionOutcome::Consumed;
    }
    if (action == "tui.editor.cursorRight") {
        impl.move_right(lock);
        return InputAdmissionOutcome::Consumed;
    }
    if (action == "tui.editor.cursorWordLeft") {
        impl.move_word(false);
        return InputAdmissionOutcome::Consumed;
    }
    if (action == "tui.editor.cursorWordRight") {
        impl.move_word(true);
        return InputAdmissionOutcome::Consumed;
    }
    if (action == "tui.editor.cursorUp") {
        const auto cur = impl.buffer.cursor();
        if (impl.on_first_visual_line() &&
                (impl.editor_is_empty() || impl.history_index.has_value() || cur.column == 0)) {
            impl.navigate_history(-1);
        } else if (impl.on_first_visual_line()) {
            impl.move_to_line_start();
        } else {
            impl.move_vertical(-1, lock);
        }
        return InputAdmissionOutcome::Consumed;
    }
    if (action == "tui.editor.cursorDown") {
        if (impl.history_index.has_value() && impl.on_last_visual_line()) {
            impl.navigate_history(1);
        } else if (impl.on_last_visual_line()) {
            impl.move_to_line_end();
        } else {
            impl.move_vertical(1, lock);
        }
        return InputAdmissionOutcome::Consumed;
    }
    if (action == "tui.editor.pageUp") {
        for (std::size_t index = 0; index < impl.options.max_visible_lines; ++index) {
            impl.move_vertical(-1, lock);
        }
        return InputAdmissionOutcome::Consumed;
    }
    if (action == "tui.editor.pageDown") {
        for (std::size_t index = 0; index < impl.options.max_visible_lines; ++index) {
            impl.move_vertical(1, lock);
        }
        return InputAdmissionOutcome::Consumed;
    }
    if (action == "tui.input.newLine") {
        impl.cancel_autocomplete(lock, true);
        impl.insert_text("\n", true, lock);
        return InputAdmissionOutcome::Consumed;
    }
    if (action == "tui.input.submit") {
        impl.submit(lock);
        return InputAdmissionOutcome::Consumed;
    }
    if (detail::is_printable(*event)) {
        impl.insert_character(detail::printable_text(*event), lock);
        return InputAdmissionOutcome::Consumed;
    }
    return InputAdmissionOutcome::Unhandled;
}

void Editor::set_focused(bool focused) {
    auto operation = impl_->serialized_operation(impl_);
    auto& impl = operation.impl;
    std::lock_guard lock(impl.impl_mutex);
    impl.focused = focused;
}

bool Editor::focused() const {
    auto operation = impl_->serialized_operation(impl_);
    auto& impl = operation.impl;
    std::lock_guard lock(impl.impl_mutex);
    return impl.focused;
}

std::optional<CursorPosition> Editor::cursor_location() const {
    auto operation = impl_->serialized_operation(impl_);
    auto& impl = operation.impl;
    std::lock_guard lock(impl.impl_mutex);
    if (!impl.focused || impl.layout_width == 0) return std::nullopt;
    const auto visual = impl.visual_lines(impl.layout_width);
    if (visual.empty()) return std::nullopt;

    std::size_t visual_row = 0;
    bool found = false;
    const auto cur = impl.buffer.cursor();
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

    const auto visible_count = std::max<std::size_t>(1, impl.content_height());
    if (visual_row < impl.scroll_offset) return std::nullopt;
    if (visual_row >= impl.scroll_offset + visible_count) return std::nullopt;

    const auto display_row = impl.border_rows() + visual_row - impl.scroll_offset;

    const auto& vl = visual[visual_row];
    const auto vl_text_width = visible_width(vl.text);
    const auto cursor_in_line = cur.column - vl.start;
    const auto segs_in_line = vl.end - vl.start;
    std::size_t col = 0;
    if (segs_in_line > 0 && cursor_in_line <= segs_in_line) {
        const auto seg_end = vl.start + cursor_in_line;
        const auto& doc = impl.buffer.document();
        for (std::size_t i = vl.start; i < seg_end && i < doc[vl.logical_line].size(); ++i) {
            col += visible_width(doc[vl.logical_line][i].text);
        }
        col = std::min(col, vl_text_width);
    }
    return CursorPosition{.column = col, .row = display_row};
}

void Editor::set_available_height(std::size_t rows) {
    auto operation = impl_->serialized_operation(impl_);
    auto& impl = operation.impl;
    std::lock_guard lock(impl.impl_mutex);
    impl.available_height = rows;
}

} // namespace cch::tui
