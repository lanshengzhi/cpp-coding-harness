#include <cch/tui/Editor.hpp>

#include "tui/InteractionUtils.hpp"
#include "tui/UnicodeWidth.hpp"

#include <utf8proc.h>

#include <algorithm>
#include <cctype>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::tui {
namespace {

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

[[nodiscard]] bool is_word_segment(std::string_view segment) {
    const auto [codepoint, bytes] = detail::decode_utf8(segment, 0);
    if (bytes == 0) return false;
    if (codepoint < 128) return std::isalnum(static_cast<unsigned char>(codepoint)) || codepoint == '_';
    const auto category = utf8proc_category(static_cast<utf8proc_int32_t>(codepoint));
    return category == UTF8PROC_CATEGORY_LU || category == UTF8PROC_CATEGORY_LL ||
        category == UTF8PROC_CATEGORY_LT || category == UTF8PROC_CATEGORY_LM ||
        category == UTF8PROC_CATEGORY_LO || category == UTF8PROC_CATEGORY_ND ||
        category == UTF8PROC_CATEGORY_NL || category == UTF8PROC_CATEGORY_NO;
}

[[nodiscard]] bool is_printable(const KeyEvent& event) {
    if (event.ctrl || event.alt || event.key.size() == 0) return false;
    return event.key != "enter" && event.key != "tab" && event.key != "escape" &&
        event.key != "backspace" && event.key != "delete" && event.key != "insert" &&
        event.key != "clear" && event.key != "home" && event.key != "end" &&
        event.key != "pageUp" && event.key != "pageDown" && event.key != "up" &&
        event.key != "down" && event.key != "left" && event.key != "right";
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

[[nodiscard]] bool starts_with_case_insensitive(std::string_view value, std::string_view prefix) {
    if (prefix.size() > value.size()) return false;
    for (std::size_t index = 0; index < prefix.size(); ++index) {
        const auto left = static_cast<unsigned char>(value[index]);
        const auto right = static_cast<unsigned char>(prefix[index]);
        if (std::tolower(left) != std::tolower(right)) return false;
    }
    return true;
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
    AutocompleteProvider autocomplete_provider;
    std::vector<AutocompleteItem> autocomplete;
    std::string autocomplete_prefix;
    std::size_t autocomplete_selected{0};
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

    void close_autocomplete() {
        autocomplete.clear();
        autocomplete_prefix.clear();
        autocomplete_selected = 0;
    }

    void refresh_autocomplete(bool force = false) {
        if (!autocomplete_provider) return;
        std::optional<AutocompleteSuggestions> requested;
        try {
            requested = autocomplete_provider(AutocompleteRequest{
                .lines = [&]() {
                    std::vector<std::string> result;
                    result.reserve(document.size());
                    for (const auto& line : document) result.push_back(line_text(line));
                    return result;
                }(),
                .cursor = cursor,
                .force = force,
            });
        } catch (...) {
            callback_error = util::make_error(
                util::ErrorCode::Unknown,
                "Editor autocomplete provider failed",
                "the autocomplete callback threw an exception");
            close_autocomplete();
            return;
        }
        if (!requested || requested->items.empty()) {
            close_autocomplete();
            return;
        }
        autocomplete.clear();
        autocomplete_prefix = requested->prefix;
        auto filter = autocomplete_prefix;
        if (!filter.empty() && (filter.front() == '/' || filter.front() == '@' || filter.front() == '#')) {
            filter.erase(0, 1);
        }
        for (const auto& item : requested->items) {
            if (filter.empty() || starts_with_case_insensitive(item.value, filter) ||
                starts_with_case_insensitive(item.label, filter)) {
                autocomplete.push_back(item);
            }
        }
        if (autocomplete.empty()) close_autocomplete();
        else autocomplete_selected = std::min(autocomplete_selected, autocomplete.size() - 1);
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
        if (record_undo) push_undo();
        insert_segments(segment_lines(text));
        notify_change();
        if (update_autocomplete) refresh_autocomplete();
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
        refresh_autocomplete();
    }

    void move_left() {
        if (cursor.column > 0) --cursor.column;
        else if (cursor.line > 0) {
            --cursor.line;
            cursor.column = document[cursor.line].size();
        }
        last_yank_ring_index.reset();
    }

    void move_right() {
        if (cursor.column < document[cursor.line].size()) ++cursor.column;
        else if (cursor.line + 1 < document.size()) {
            ++cursor.line;
            cursor.column = 0;
        }
        last_yank_ring_index.reset();
    }

    void move_word(bool forward) {
        if (forward) {
            while (cursor.line + 1 < document.size() || cursor.column < document[cursor.line].size()) {
                if (cursor.column == document[cursor.line].size()) {
                    ++cursor.line;
                    cursor.column = 0;
                    continue;
                }
                if (is_word_segment(document[cursor.line][cursor.column].text)) break;
                ++cursor.column;
            }
            while (cursor.column < document[cursor.line].size() &&
                   is_word_segment(document[cursor.line][cursor.column].text)) ++cursor.column;
        } else {
            while (cursor.line > 0 || cursor.column > 0) {
                if (cursor.column == 0) {
                    --cursor.line;
                    cursor.column = document[cursor.line].size();
                    continue;
                }
                if (is_word_segment(document[cursor.line][cursor.column - 1].text)) break;
                --cursor.column;
            }
            while (cursor.column > 0 && is_word_segment(document[cursor.line][cursor.column - 1].text)) --cursor.column;
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
        refresh_autocomplete();
    }

    void delete_word(bool forward) {
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
        refresh_autocomplete();
    }

    void yank() {
        if (kill_ring.empty()) return;
        push_undo();
        const auto start = cursor;
        insert_segments(materialize_kill(kill_ring.front()));
        notify_change();
        last_yank_ring_index = 0;
        last_yank_range = std::pair{start, cursor};
    }

    void yank_pop() {
        if (!last_yank_ring_index || !last_yank_range || kill_ring.size() < 2) return;
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
        if (undo.empty()) return;
        auto previous = std::move(undo.back());
        undo.pop_back();
        document = std::move(previous.document);
        cursor = previous.cursor;
        pastes = std::move(previous.pastes);
        paste_counter = previous.paste_counter;
        close_autocomplete();
        notify_change();
    }

    void submit() {
        const auto result = trim_outer_whitespace(expanded());
        document.assign(1, {});
        cursor = {};
        pastes.clear();
        paste_counter = 0;
        undo.clear();
        close_autocomplete();
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
            close_autocomplete();
            return;
        }
        insert_segments(segment_lines(filtered));
        notify_change();
        close_autocomplete();
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
                const auto segment_width = detail::visible_width(line[index].text);
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

    void accept_autocomplete() {
        if (autocomplete.empty()) return;
        const auto& item = autocomplete[autocomplete_selected];
        auto prefix = autocomplete_prefix;
        auto prefix_segments = segment_lines(prefix).front();
        if (prefix_segments.size() > cursor.column) return;
        push_undo();
        auto& line = document[cursor.line];
        const auto start = cursor.column - prefix_segments.size();
        line.erase(line.begin() + static_cast<std::ptrdiff_t>(start),
                   line.begin() + static_cast<std::ptrdiff_t>(cursor.column));
        cursor.column = start;
        std::string value = item.value;
        if (!prefix.empty() && (prefix.front() == '/' || prefix.front() == '@' || prefix.front() == '#') &&
            !value.starts_with(prefix.front())) {
            value.insert(value.begin(), prefix.front());
        }
        insert_segments(segment_lines(value));
        close_autocomplete();
        notify_change();
    }
};

Editor::Editor(EditorOptions options, EditorChangeSink on_change, EditorSubmitSink on_submit)
    : impl_(std::make_unique<Impl>()) {
    if (!options.keybindings) options.keybindings = default_tui_keybindings();
    impl_->options = std::move(options);
    impl_->on_change = std::move(on_change);
    impl_->on_submit = std::move(on_submit);
}

Editor::Editor(Editor&&) noexcept = default;
Editor& Editor::operator=(Editor&&) noexcept = default;
Editor::~Editor() = default;

std::string Editor::text() const {
    return document_text(impl_->document);
}

std::string Editor::expanded_text() const {
    return impl_->expanded();
}

std::vector<std::string> Editor::lines() const {
    std::vector<std::string> result;
    result.reserve(impl_->document.size());
    for (const auto& line : impl_->document) result.push_back(line_text(line));
    return result;
}

EditorCursor Editor::cursor() const {
    return impl_->cursor;
}

void Editor::set_text(std::string text) {
    text = normalize_input(std::move(text));
    if (text == this->text()) return;
    impl_->push_undo();
    impl_->document = segment_lines(text);
    impl_->pastes.clear();
    impl_->paste_counter = 0;
    impl_->cursor = {.line = impl_->document.size() - 1, .column = impl_->document.back().size()};
    impl_->close_autocomplete();
    impl_->notify_change();
}

void Editor::insert_text_at_cursor(std::string text) {
    impl_->close_autocomplete();
    impl_->insert_text(std::move(text), true, false);
}

void Editor::set_theme(EditorTheme theme) {
    impl_->theme = std::move(theme);
    invalidate();
}

void Editor::set_autocomplete_provider(AutocompleteProvider provider) {
    impl_->autocomplete_provider = std::move(provider);
    impl_->close_autocomplete();
}

bool Editor::autocomplete_open() const {
    return !impl_->autocomplete.empty();
}

std::vector<AutocompleteItem> Editor::autocomplete_items() const {
    return impl_->autocomplete;
}

util::Expected<RenderResult> Editor::render(std::size_t width) {
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
        const auto line_width = detail::visible_width(line);
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
        if (is_printable(*event)) {
            impl_->jump_to(event->key, *impl_->jump_direction);
            impl_->jump_direction.reset();
            return;
        }
        impl_->jump_direction.reset();
    }

    if (autocomplete_open()) {
        if (matches("tui.select.cancel")) {
            impl_->close_autocomplete();
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
            impl_->accept_autocomplete();
            return;
        }
    }

    if (matches("tui.input.tab")) {
        impl_->refresh_autocomplete(true);
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
        impl_->cursor.column = 0;
        return;
    }
    if (matches("tui.editor.cursorLineEnd")) {
        impl_->cursor.column = impl_->document[impl_->cursor.line].size();
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
        impl_->move_vertical(-1);
        return;
    }
    if (matches("tui.editor.cursorDown")) {
        impl_->move_vertical(1);
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
        impl_->insert_text("\n", true);
        return;
    }
    if (matches("tui.input.submit")) {
        if (!impl_->options.disable_submit) impl_->submit();
        return;
    }
    if (is_printable(*event)) impl_->insert_text(event->key, true);
}

bool Editor::accepts_key_releases() const {
    return false;
}

void Editor::set_focused(bool focused) {
    impl_->focused = focused;
}

bool Editor::focused() const {
    return impl_->focused;
}

std::optional<CursorPosition> Editor::cursor_location() const {
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
    const auto vl_text_width = detail::visible_width(vl.text);
    // Compute the column as the cursor's position within this visual line,
    // relative to the visual line's segment range.
    const auto cursor_in_line = impl_->cursor.column - vl.start;
    const auto segs_in_line = vl.end - vl.start;
    std::size_t col = 0;
    if (segs_in_line > 0 && cursor_in_line <= segs_in_line) {
        // Sum visible widths of segments that are fully within this visual line
        const auto seg_end = vl.start + cursor_in_line;
        for (std::size_t i = vl.start; i < seg_end && i < impl_->document[vl.logical_line].size(); ++i) {
            col += detail::visible_width(impl_->document[vl.logical_line][i].text);
        }
        // Cap at the visual line's text width
        col = std::min(col, vl_text_width);
    }
    return CursorPosition{.column = col, .row = display_row};
}

void Editor::set_available_height(std::size_t rows) {
    impl_->available_height = std::max<std::size_t>(1, rows);
}

} // namespace cch::tui
