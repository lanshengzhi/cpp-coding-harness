#include <cch/tui/Editor.hpp>

#include <cch/tui/Utils.hpp>

#include "tui/InteractionUtils.hpp"
#include "tui/UnicodeWidth.hpp"
#include "tui/WordNavigation.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <map>
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

struct Segment {
    std::string text;
    std::optional<std::size_t> paste_id;
};

using Line = std::vector<Segment>;
using Document = std::vector<Line>;

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

[[nodiscard]] std::vector<Line> segment_lines(std::string_view text) {
    std::vector<Line> result(1);
    std::size_t start = 0;
    while (start <= text.size()) {
        const auto newline = text.find('\n', start);
        const auto end = newline == std::string_view::npos ? text.size() : newline;
        auto graphemes = detail::split_graphemes(text.substr(start, end - start));
        for (auto& grapheme : graphemes) result.back().push_back({.text = std::move(grapheme), .paste_id = std::nullopt});
        if (newline == std::string_view::npos) break;
        result.emplace_back();
        start = newline + 1;
    }
    return result;
}

[[nodiscard]] std::string line_text(const Line& line) {
    std::string text;
    for (const auto& segment : line) text += segment.text;
    return text;
}

[[nodiscard]] std::string document_text(const Document& document) {
    std::string text;
    for (std::size_t index = 0; index < document.size(); ++index) {
        if (index != 0) text += '\n';
        text += line_text(document[index]);
    }
    return text;
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
    struct Snapshot {
        Document document;
        EditorCursor cursor;
        std::map<std::size_t, std::string> pastes;
        std::size_t paste_counter{0};
    };

    EditorOptions options;
    EditorChangeSink on_change;
    EditorSubmitSink on_submit;
    EditorTheme theme;
    Document document{1};
    EditorCursor cursor;
    std::map<std::size_t, std::string> pastes;
    std::size_t paste_counter{0};
    std::vector<Snapshot> undo;
    struct KillEntry {
        Document document;
        std::map<std::size_t, std::string> pastes;
    };
    std::vector<KillEntry> kill_ring;
    std::optional<std::size_t> last_yank_ring_index;
    std::optional<std::pair<EditorCursor, EditorCursor>> last_yank_range;
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
        Document document;
        EditorCursor cursor;
    };
    std::optional<HistoryDraft> history_draft;
    bool focused{false};
    std::size_t available_height{5};
    std::size_t layout_width{80};
    std::size_t scroll_offset{0};
    enum class JumpDirection { Forward, Backward };
    std::optional<JumpDirection> jump_direction;
    std::optional<util::Error> callback_error;

    void clamp_cursor() {
        if (document.empty()) document.emplace_back();
        cursor.line = std::min(cursor.line, document.size() - 1);
        cursor.column = std::min(cursor.column, document[cursor.line].size());
    }

    [[nodiscard]] Snapshot snapshot() const {
        return {.document = document, .cursor = cursor, .pastes = pastes, .paste_counter = paste_counter};
    }

    void push_undo() {
        undo.push_back(snapshot());
        last_yank_ring_index.reset();
        last_yank_range.reset();
    }

    void notify_change() {
        if (!on_change) return;
        try {
            on_change(document_text(document));
        } catch (...) {
            callback_error = util::make_error(
                util::ErrorCode::Unknown,
                "Editor change sink failed",
                "the change callback threw an exception");
        }
    }

    [[nodiscard]] std::string text() const {
        return document_text(document);
    }

    [[nodiscard]] std::string expanded() const {
        std::string result;
        for (std::size_t line_index = 0; line_index < document.size(); ++line_index) {
            if (line_index != 0) result += '\n';
            for (const auto& segment : document[line_index]) {
                if (segment.paste_id) {
                    const auto found = pastes.find(*segment.paste_id);
                    result += found == pastes.end() ? segment.text : found->second;
                } else {
                    result += segment.text;
                }
            }
        }
        return result;
    }

    [[nodiscard]] std::size_t byte_offset_of(const Line& line, std::size_t grapheme_column) const {
        std::size_t bytes = 0;
        for (std::size_t index = 0; index < std::min(grapheme_column, line.size()); ++index) {
            bytes += line[index].text.size();
        }
        return bytes;
    }

    [[nodiscard]] std::size_t cursor_byte_offset() const {
        return byte_offset_of(document[cursor.line], cursor.column);
    }

    [[nodiscard]] std::string line_prefix_before_cursor() const {
        return line_text(document[cursor.line]).substr(0, cursor_byte_offset());
    }

    [[nodiscard]] std::vector<std::string> line_strings() const {
        std::vector<std::string> result;
        result.reserve(document.size());
        for (const auto& line : document) result.push_back(line_text(line));
        return result;
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
            try {
                options.autocomplete_debounce_timer->cancel();
            } catch (...) {
                // Cancellation is best-effort; a throwing timer cannot veto input.
            }
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
    /// buildDebouncePattern): an unclosed `@\"...` quoted path may contain
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
        return cursor.line == 0;
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
                cursor.line,
                cursor_byte_offset())) {
            return;
        }
        cancel_autocomplete_request();
        const auto start_token = ++autocomplete_start_token;
        const auto debounce_ms = get_autocomplete_debounce_ms(force, explicit_tab);
        if (debounce_ms > std::chrono::milliseconds{} && options.autocomplete_debounce_timer) {
            options.autocomplete_debounce_timer->start(
                debounce_ms,
                [self = self, start_token, force, explicit_tab] {
                    self->on_debounce_fired(start_token, force, explicit_tab);
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
        const auto snapshot_cursor = cursor;
        const auto cursor_column = cursor_byte_offset();
        const auto request_id = [this] {
            std::lock_guard lock(autocomplete_mutex);
            return ++autocomplete_request_id;
        }();
        request_stop_source = std::stop_source{};
        AutocompleteRequest request{
            .lines = line_strings(),
            .cursor_line = cursor.line,
            .cursor_column = cursor_column,
            .force = force,
            .stop_token = request_stop_source.get_token(),
        };
        try {
            autocomplete_provider->get_suggestions(
                request,
                [self = self, request_id, snapshot_text, snapshot_cursor, force, explicit_tab](
                    std::optional<AutocompleteSuggestions> result) {
                    self->deliver_autocomplete_result(
                        request_id,
                        std::move(result),
                        snapshot_text,
                        snapshot_cursor,
                        force,
                        explicit_tab);
                });
        } catch (...) {
            callback_error = util::make_error(
                util::ErrorCode::Unknown,
                "Editor autocomplete provider failed",
                "the autocomplete callback threw an exception");
            cancel_autocomplete();
            return;
        }
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
            try {
                options.autocomplete_render_request();
            } catch (...) {
                // Scheduling notifications cannot make input delivery fail
                // (same bound as Tui::invalidate's render-request sink).
            }
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
        if (pending->snapshot_text != text() || pending->snapshot_cursor != cursor) return;
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
        if (result.cursor_line >= document.size()) return;
        auto& line = document[result.cursor_line];
        const auto cursor_bytes = cursor_byte_offset();
        if (prefix.size() > cursor_bytes) return;
        const auto start_bytes = cursor_bytes - prefix.size();
        const auto current_line_text = line_text(line);
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

        // The provider's surgery only touches the region between the prefix
        // start and the result cursor; the regions outside it must be
        // byte-identical to the original line (pi's applyCompletion shape).
        const auto original_after = current_line_text.substr(cursor_bytes);
        const auto expected_after = result_line.substr(result_cursor);
        const bool quote_adjusted =
            (is_quoted_prefix && has_trailing_quote_in_item && has_leading_quote_after_cursor);
        if (result_line.substr(0, start_bytes) != current_line_text.substr(0, start_bytes)) return;
        if (expected_after != (quote_adjusted && !original_after.empty() ? original_after.substr(1)
                                                                         : original_after)) {
            return;
        }

        // Rebuild the line from the result text for the edited region while
        // preserving the untouched segments (paste-marker associations).
        Line rebuilt;
        rebuilt.reserve(start_segment_value + (line.size() - after_begin) + 1);
        rebuilt.insert(
            rebuilt.end(),
            line.begin(),
            line.begin() + static_cast<std::ptrdiff_t>(start_segment_value));
        auto middle_segments = segment_lines(result_line.substr(start_bytes, result_cursor - start_bytes)).front();
        rebuilt.insert(
            rebuilt.end(),
            std::make_move_iterator(middle_segments.begin()),
            std::make_move_iterator(middle_segments.end()));
        rebuilt.insert(
            rebuilt.end(),
            line.begin() + static_cast<std::ptrdiff_t>(after_begin),
            line.end());
        line = std::move(rebuilt);

        const auto result_prefix = std::string_view{result_line}.substr(0, result_cursor);
        cursor.column = segment_lines(result_prefix).front().size();
    }

    void apply_autocomplete_item(const AutocompleteItem& item, std::string_view prefix, bool notify) {
        if (!autocomplete_provider) return;
        push_undo();
        const auto result = autocomplete_provider->apply_completion(
            line_strings(),
            cursor.line,
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

    void insert_segments(std::vector<Line> inserted) {
        clamp_cursor();
        auto& current = document[cursor.line];
        Line after(current.begin() + static_cast<std::ptrdiff_t>(cursor.column), current.end());
        current.erase(current.begin() + static_cast<std::ptrdiff_t>(cursor.column), current.end());
        if (inserted.size() == 1) {
            current.insert(current.end(), inserted.front().begin(), inserted.front().end());
            current.insert(current.end(), after.begin(), after.end());
            cursor.column = current.size() - after.size();
            return;
        }
        current.insert(current.end(), inserted.front().begin(), inserted.front().end());
        const auto insertion = document.begin() + static_cast<std::ptrdiff_t>(cursor.line + 1);
        const auto first_inserted = document.insert(insertion, inserted.begin() + 1, inserted.end());
        const auto last_inserted = first_inserted + static_cast<std::ptrdiff_t>(inserted.size() - 2);
        last_inserted->insert(last_inserted->end(), after.begin(), after.end());
        cursor.line += inserted.size() - 1;
        cursor.column = last_inserted->size() - after.size();
    }

    void insert_text(std::string text, bool record_undo, bool update_autocomplete = true) {
        text = normalize_input(std::move(text));
        if (text.empty()) return;
        exit_history_browsing();
        if (record_undo) push_undo();
        insert_segments(segment_lines(text));
        notify_change();
        if (update_autocomplete) maybe_trigger_autocomplete(text.back());
    }

    [[nodiscard]] static std::string marker_for(std::size_t id, std::string_view text) {
        const auto lines = static_cast<std::size_t>(std::count(text.begin(), text.end(), '\n')) + 1;
        if (lines > 10) {
            return "[paste #" + std::to_string(id) + " +" + std::to_string(lines) + " lines]";
        }
        return "[paste #" + std::to_string(id) + " " + std::to_string(text.size()) + " chars]";
    }

    void forget_paste(std::size_t id) {
        pastes.erase(id);
        for (auto& line : document) {
            for (auto& segment : line) {
                if (segment.paste_id && *segment.paste_id > id) {
                    --*segment.paste_id;
                    const auto marker = segment.text;
                    const auto number_start = marker.find('#') + 1;
                    const auto number_end = marker.find(' ', number_start);
                    segment.text = marker.substr(0, number_start) + std::to_string(*segment.paste_id) +
                        marker.substr(number_end);
                }
            }
        }
        std::map<std::size_t, std::string> renumbered;
        for (auto& [paste_id, text] : pastes) {
            renumbered.emplace(paste_id > id ? paste_id - 1 : paste_id, std::move(text));
        }
        pastes = std::move(renumbered);
        if (paste_counter > 0) --paste_counter;
    }

    void erase_at_cursor(bool backward) {
        exit_history_browsing();
        clamp_cursor();
        if (backward) {
            if (cursor.column > 0) {
                push_undo();
                auto& line = document[cursor.line];
                const auto index = --cursor.column;
                if (line[index].paste_id) forget_paste(*line[index].paste_id);
                line.erase(line.begin() + static_cast<std::ptrdiff_t>(index));
            } else if (cursor.line > 0) {
                push_undo();
                const auto prior_size = document[cursor.line - 1].size();
                document[cursor.line - 1].insert(
                    document[cursor.line - 1].end(), document[cursor.line].begin(), document[cursor.line].end());
                document.erase(document.begin() + static_cast<std::ptrdiff_t>(cursor.line));
                --cursor.line;
                cursor.column = prior_size;
            } else {
                return;
            }
        } else {
            auto& line = document[cursor.line];
            if (cursor.column < line.size()) {
                push_undo();
                if (line[cursor.column].paste_id) forget_paste(*line[cursor.column].paste_id);
                line.erase(line.begin() + static_cast<std::ptrdiff_t>(cursor.column));
            } else if (cursor.line + 1 < document.size()) {
                push_undo();
                line.insert(line.end(), document[cursor.line + 1].begin(), document[cursor.line + 1].end());
                document.erase(document.begin() + static_cast<std::ptrdiff_t>(cursor.line + 1));
            } else {
                return;
            }
        }
        notify_change();
        update_or_retrigger_after_erase();
    }

    void move_left() {
        if (cursor.column > 0) --cursor.column;
        else if (cursor.line > 0) {
            --cursor.line;
            cursor.column = document[cursor.line].size();
        }
        last_yank_ring_index.reset();
        if (autocomplete_state != AutocompleteState::None) update_autocomplete();
    }

    void move_right() {
        if (cursor.column < document[cursor.line].size()) ++cursor.column;
        else if (cursor.line + 1 < document.size()) {
            ++cursor.line;
            cursor.column = 0;
        }
        last_yank_ring_index.reset();
        if (autocomplete_state != AutocompleteState::None) update_autocomplete();
    }

    void move_word(bool forward) {
        if (forward) {
            while (cursor.line + 1 < document.size() || cursor.column < document[cursor.line].size()) {
                if (cursor.column == document[cursor.line].size()) {
                    ++cursor.line;
                    cursor.column = 0;
                    continue;
                }
                if (detail::is_word_segment(document[cursor.line][cursor.column].text)) break;
                ++cursor.column;
            }
            while (cursor.column < document[cursor.line].size() &&
                   detail::is_word_segment(document[cursor.line][cursor.column].text)) ++cursor.column;
        } else {
            while (cursor.line > 0 || cursor.column > 0) {
                if (cursor.column == 0) {
                    --cursor.line;
                    cursor.column = document[cursor.line].size();
                    continue;
                }
                if (detail::is_word_segment(document[cursor.line][cursor.column - 1].text)) break;
                --cursor.column;
            }
            while (cursor.column > 0 && detail::is_word_segment(document[cursor.line][cursor.column - 1].text)) --cursor.column;
        }
        last_yank_ring_index.reset();
    }

    [[nodiscard]] Line remove_until(std::size_t target, bool forward) {
        auto& line = document[cursor.line];
        const auto start = forward ? cursor.column : target;
        const auto end = forward ? target : cursor.column;
        Line removed(
            line.begin() + static_cast<std::ptrdiff_t>(start),
            line.begin() + static_cast<std::ptrdiff_t>(end));
        line.erase(
            line.begin() + static_cast<std::ptrdiff_t>(start),
            line.begin() + static_cast<std::ptrdiff_t>(end));
        cursor.column = start;
        return removed;
    }

    void erase_range(EditorCursor start, EditorCursor end) {
        if (start.line == end.line) {
            auto& line = document[start.line];
            line.erase(
                line.begin() + static_cast<std::ptrdiff_t>(start.column),
                line.begin() + static_cast<std::ptrdiff_t>(end.column));
        } else {
            Line tail(
                document[end.line].begin() + static_cast<std::ptrdiff_t>(end.column),
                document[end.line].end());
            auto& first = document[start.line];
            first.erase(first.begin() + static_cast<std::ptrdiff_t>(start.column), first.end());
            first.insert(first.end(), tail.begin(), tail.end());
            document.erase(
                document.begin() + static_cast<std::ptrdiff_t>(start.line + 1),
                document.begin() + static_cast<std::ptrdiff_t>(end.line + 1));
        }
        cursor = start;
    }

    [[nodiscard]] KillEntry kill_entry(Document killed) const {
        KillEntry entry{.document = std::move(killed), .pastes = {}};
        for (const auto& line : entry.document) {
            for (const auto& segment : line) {
                if (segment.paste_id) entry.pastes.emplace(*segment.paste_id, pastes.at(*segment.paste_id));
            }
        }
        return entry;
    }

    [[nodiscard]] Document materialize_kill(const KillEntry& entry) {
        auto result = entry.document;
        std::map<std::size_t, std::size_t> remapped_ids;
        for (auto& line : result) {
            for (auto& segment : line) {
                if (!segment.paste_id) continue;
                const auto source_id = *segment.paste_id;
                const auto existing = remapped_ids.find(source_id);
                if (existing != remapped_ids.end()) {
                    segment.paste_id = existing->second;
                    segment.text = marker_for(existing->second, entry.pastes.at(source_id));
                    continue;
                }
                const auto target_id = ++paste_counter;
                pastes[target_id] = entry.pastes.at(source_id);
                remapped_ids.emplace(source_id, target_id);
                segment.paste_id = target_id;
                segment.text = marker_for(target_id, entry.pastes.at(source_id));
            }
        }
        return result;
    }

    void kill_to_line(bool forward) {
        exit_history_browsing();
        const auto target = forward ? document[cursor.line].size() : 0U;
        if (target != cursor.column) {
            push_undo();
            kill_ring.insert(kill_ring.begin(), kill_entry(Document{remove_until(target, forward)}));
        } else if ((!forward && cursor.line > 0) || (forward && cursor.line + 1 < document.size())) {
            push_undo();
            kill_ring.insert(kill_ring.begin(), kill_entry(Document{Line{}, Line{}}));
            if (forward) {
                document[cursor.line].insert(
                    document[cursor.line].end(),
                    document[cursor.line + 1].begin(),
                    document[cursor.line + 1].end());
                document.erase(document.begin() + static_cast<std::ptrdiff_t>(cursor.line + 1));
            } else {
                const auto prior_size = document[cursor.line - 1].size();
                document[cursor.line - 1].insert(
                    document[cursor.line - 1].end(),
                    document[cursor.line].begin(),
                    document[cursor.line].end());
                document.erase(document.begin() + static_cast<std::ptrdiff_t>(cursor.line));
                --cursor.line;
                cursor.column = prior_size;
            }
        } else {
            return;
        }
        notify_change();
    }

    void delete_word(bool forward) {
        exit_history_browsing();
        const auto original = cursor;
        move_word(forward);
        const auto target = cursor;
        if (original == target || original.line != target.line) {
            cursor = original;
            return;
        }
        const auto start = forward ? original.column : target.column;
        const auto end = forward ? target.column : original.column;
        cursor = {.line = original.line, .column = start};
        push_undo();
        const auto removed = remove_until(end, true);
        kill_ring.insert(kill_ring.begin(), kill_entry(Document{std::move(removed)}));
        notify_change();
    }

    void yank() {
        if (kill_ring.empty()) return;
        exit_history_browsing();
        push_undo();
        const auto start = cursor;
        insert_segments(materialize_kill(kill_ring.front()));
        notify_change();
        last_yank_ring_index = 0;
        last_yank_range = std::pair{start, cursor};
    }

    void yank_pop() {
        if (!last_yank_ring_index || !last_yank_range || kill_ring.size() < 2) return;
        exit_history_browsing();
        const auto previous = *last_yank_range;
        const auto prior_ring_index = *last_yank_ring_index;
        if (previous.first.line != previous.second.line) return;
        push_undo();
        cursor = previous.first;
        erase_range(previous.first, previous.second);
        const auto next = (prior_ring_index + 1) % kill_ring.size();
        insert_segments(materialize_kill(kill_ring[next]));
        notify_change();
        last_yank_ring_index = next;
        last_yank_range = std::pair{previous.first, cursor};
    }

    void undo_once() {
        exit_history_browsing();
        if (undo.empty()) return;
        auto previous = std::move(undo.back());
        undo.pop_back();
        document = std::move(previous.document);
        cursor = previous.cursor;
        pastes = std::move(previous.pastes);
        paste_counter = previous.paste_counter;
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
        return document.size() == 1 && document.front().empty();
    }

    struct VisualLine;

    [[nodiscard]] std::size_t find_current_visual_line(const std::vector<VisualLine>& visual) const {
        for (std::size_t index = 0; index < visual.size(); ++index) {
            if (visual[index].logical_line == cursor.line && cursor.column >= visual[index].start &&
                cursor.column <= visual[index].end) {
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
        last_yank_ring_index.reset();
        last_yank_range.reset();
        cursor.column = 0;
    }

    void move_to_line_end() {
        last_yank_ring_index.reset();
        last_yank_range.reset();
        cursor.column = document[cursor.line].size();
    }

    void set_text_internal(std::string text, bool cursor_at_start) {
        document = segment_lines(std::move(text));
        cursor.line = cursor_at_start ? 0 : document.size() - 1;
        cursor.column = cursor_at_start ? 0 : document[cursor.line].size();
        scroll_offset = 0;
        notify_change();
    }

    void navigate_history(int direction) {
        last_yank_ring_index.reset();
        last_yank_range.reset();
        if (history.empty()) return;

        const auto current = history_index ? static_cast<int>(*history_index) : -1;
        const auto new_index = current - direction;  // Up(-1) increases index, Down(1) decreases
        if (new_index < -1 || new_index >= static_cast<int>(history.size())) return;

        // Capture state when first entering history browsing mode.
        if (!history_index && new_index >= 0) {
            push_undo();
            history_draft = HistoryDraft{.document = document, .cursor = cursor};
        }

        if (new_index == -1) {
            history_index.reset();
            auto draft = history_draft;
            history_draft.reset();
            if (draft) {
                document = std::move(draft->document);
                cursor = draft->cursor;
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
        document.assign(1, {});
        cursor = {};
        pastes.clear();
        paste_counter = 0;
        undo.clear();
        cancel_autocomplete();
        scroll_offset = 0;
        notify_change();
        if (!on_submit) return;
        try {
            on_submit(result);
        } catch (...) {
            callback_error = util::make_error(
                util::ErrorCode::Unknown,
                "Editor submit sink failed",
                "the submit callback threw an exception");
        }
    }

    void paste(std::string text) {
        exit_history_browsing();
        std::string filtered;
        text = normalize_input(std::move(text));
        for (std::size_t index = 0; index < text.size();) {
            const auto [codepoint, bytes] = detail::decode_utf8(text, index);
            if (bytes == 0) break;
            if (codepoint == '\n' || (codepoint >= 0x20 && codepoint != 0x7f)) {
                filtered.append(text.substr(index, bytes));
            }
            index += bytes;
        }
        if (filtered.empty()) return;
        push_undo();
        const auto line_count = static_cast<std::size_t>(std::count(filtered.begin(), filtered.end(), '\n')) + 1;
        if (line_count > 10 || filtered.size() > 1000) {
            const auto id = ++paste_counter;
            pastes.emplace(id, filtered);
            const auto marker = marker_for(id, filtered);
            insert_segments({{{.text = marker, .paste_id = id}}});
            notify_change();
            cancel_autocomplete();
            return;
        }
        insert_segments(segment_lines(filtered));
        notify_change();
        cancel_autocomplete();
    }

    struct VisualLine {
        std::size_t logical_line{0};
        std::size_t start{0};
        std::size_t end{0};
        std::string text;
    };

    [[nodiscard]] std::vector<VisualLine> visual_lines(std::size_t width) const {
        std::vector<VisualLine> result;
        for (std::size_t line_index = 0; line_index < document.size(); ++line_index) {
            const auto& line = document[line_index];
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

    void move_vertical(int direction) {
        const auto visual = visual_lines(layout_width);
        std::size_t current = 0;
        for (std::size_t index = 0; index < visual.size(); ++index) {
            if (visual[index].logical_line == cursor.line && cursor.column >= visual[index].start &&
                cursor.column <= visual[index].end) {
                current = index;
                break;
            }
        }
        if (direction < 0 && current == 0) return;
        if (direction > 0 && current + 1 == visual.size()) return;
        const auto& from = visual[current];
        const auto& to = visual[static_cast<std::size_t>(static_cast<int>(current) + direction)];
        cursor.line = to.logical_line;
        cursor.column = std::min(to.end, to.start + (cursor.column - from.start));
        last_yank_ring_index.reset();
        if (autocomplete_state != AutocompleteState::None) update_autocomplete();
    }

    void jump_to(std::string_view target, JumpDirection direction) {
        if (direction == JumpDirection::Forward) {
            for (std::size_t line_index = cursor.line; line_index < document.size(); ++line_index) {
                const auto start = line_index == cursor.line ? cursor.column + 1 : 0U;
                for (std::size_t index = start; index < document[line_index].size(); ++index) {
                    if (document[line_index][index].text == target) {
                        cursor = {.line = line_index, .column = index};
                        return;
                    }
                }
            }
        } else {
            for (std::size_t line_index = cursor.line + 1; line_index-- > 0;) {
                const auto start = line_index == cursor.line ? cursor.column : document[line_index].size();
                for (std::size_t index = start; index-- > 0;) {
                    if (document[line_index][index].text == target) {
                        cursor = {.line = line_index, .column = index};
                        return;
                    }
                }
                if (line_index == 0) break;
            }
        }
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
Editor& Editor::operator=(Editor&& other) noexcept {
    if (this != &other) {
        if (impl_) impl_->self.reset();
        impl_ = std::move(other.impl_);
        other.impl_.reset();
    }
    return *this;
}
Editor::~Editor() {
    if (impl_) impl_->self.reset();
}

std::string Editor::text() const {
    std::lock_guard lock(impl_->impl_mutex);
    return document_text(impl_->document);
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
    return impl_->cursor;
}

void Editor::set_text(std::string text) {
    std::lock_guard lock(impl_->impl_mutex);
    impl_->exit_history_browsing();
    text = normalize_input(std::move(text));
    if (text == document_text(impl_->document)) return;
    impl_->push_undo();
    impl_->document = segment_lines(text);
    impl_->pastes.clear();
    impl_->paste_counter = 0;
    impl_->cursor = {.line = impl_->document.size() - 1, .column = impl_->document.back().size()};
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

util::Expected<RenderResult> Editor::render(std::size_t width) {
    std::lock_guard lock(impl_->impl_mutex);
    impl_->drain_autocomplete_result();
    if (impl_->callback_error) return std::unexpected(*impl_->callback_error);
    if (width == 0) {
        return std::unexpected(util::make_error(util::ErrorCode::Validation, "Editor requires a positive visible width"));
    }
    impl_->layout_width = width;
    for (const auto& logical_line : impl_->document) {
        for (const auto& segment : logical_line) {
            for (const auto& grapheme : detail::split_graphemes(segment.text)) {
                if (detail::grapheme_width(grapheme) > width) {
                    return std::unexpected(util::make_error(
                        util::ErrorCode::Validation,
                        "Editor grapheme is wider than the available visible width"));
                }
            }
        }
    }
    const auto visual = impl_->visual_lines(width);
    std::size_t cursor_line = 0;
    for (std::size_t index = 0; index < visual.size(); ++index) {
        if (visual[index].logical_line == impl_->cursor.line && impl_->cursor.column >= visual[index].start &&
            impl_->cursor.column <= visual[index].end) {
            cursor_line = index;
            break;
        }
    }
    const auto visible_count = std::max<std::size_t>(
        1,
        std::min(impl_->options.max_visible_lines, impl_->available_height));
    if (cursor_line < impl_->scroll_offset) impl_->scroll_offset = cursor_line;
    if (cursor_line >= impl_->scroll_offset + visible_count) impl_->scroll_offset = cursor_line + 1 - visible_count;
    std::vector<std::string> result;
    const auto end = std::min(visual.size(), impl_->scroll_offset + visible_count);
    for (std::size_t index = impl_->scroll_offset; index < end; ++index) {
        auto line = visual[index].text;
        const auto line_width = visible_width(line);
        if (line_width < width) line.append(width - line_width, ' ');
        result.push_back(std::move(line));
    }
    if (result.empty()) result.emplace_back(width, ' ');
    for (auto& line : result) {
        auto styled = detail::apply_text_style(impl_->theme.text, std::move(line), "Editor text");
        if (!styled) return std::unexpected(styled.error());
        line = std::move(*styled);
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
            // Slash-command confirm cancels the menu and falls through to submit.
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
        if (impl_->on_first_visual_line() &&
            (impl_->editor_is_empty() || impl_->history_index.has_value() || impl_->cursor.column == 0)) {
            impl_->navigate_history(-1);
        } else if (impl_->on_first_visual_line()) {
            // Already at top - jump to start of line
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
            // Already at bottom - jump to end of line
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
    if (detail::is_printable(*event)) impl_->insert_text(std::string(detail::printable_text(*event)), true);
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
    // Compute visual row/column for the cursor position
    const auto visual = impl_->visual_lines(impl_->layout_width);
    if (visual.empty()) return std::nullopt;

    std::size_t visual_row = 0;
    bool found = false;
    for (std::size_t index = 0; index < visual.size(); ++index) {
        if (visual[index].logical_line == impl_->cursor.line &&
            impl_->cursor.column >= visual[index].start &&
            impl_->cursor.column <= visual[index].end) {
            visual_row = index;
            found = true;
            break;
        }
    }
    if (!found) return std::nullopt;

    // Account for scroll offset
    const auto visible_count = std::max<std::size_t>(
        1,
        std::min(impl_->options.max_visible_lines, impl_->available_height));
    if (visual_row < impl_->scroll_offset) return std::nullopt;
    if (visual_row >= impl_->scroll_offset + visible_count) return std::nullopt;

    const auto display_row = visual_row - impl_->scroll_offset;

    // The column within the visual line is based on the actual rendered text
    // of the visual line, mapped from the cursor's logical column position.
    // Since visual lines may split single wide segments, we use the visual
    // line's text width from its start to the cursor's proportional position.
    const auto& vl = visual[visual_row];
    const auto vl_text_width = visible_width(vl.text);
    // Compute the column as the cursor's position within this visual line,
    // relative to the visual line's segment range.
    const auto cursor_in_line = impl_->cursor.column - vl.start;
    const auto segs_in_line = vl.end - vl.start;
    std::size_t col = 0;
    if (segs_in_line > 0 && cursor_in_line <= segs_in_line) {
        // Sum visible widths of segments that are fully within this visual line
        const auto seg_end = vl.start + cursor_in_line;
        for (std::size_t i = vl.start; i < seg_end && i < impl_->document[vl.logical_line].size(); ++i) {
            col += visible_width(impl_->document[vl.logical_line][i].text);
        }
        // Cap at the visual line's text width
        col = std::min(col, vl_text_width);
    }
    return CursorPosition{.column = col, .row = display_row};
}

void Editor::set_available_height(std::size_t rows) {
    std::lock_guard lock(impl_->impl_mutex);
    impl_->available_height = std::max<std::size_t>(1, rows);
}

} // namespace cch::tui
