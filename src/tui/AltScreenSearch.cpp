#include <cch/tui/AltScreenSearch.hpp>

#include "tui/UnicodeWidth.hpp"

#include <cch/tui/Utils.hpp>

#include <utf8proc.h>

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>

namespace cch::tui {
namespace {

[[nodiscard]] bool printable_ascii(std::string_view text) {
    return std::all_of(text.begin(), text.end(), [](unsigned char ch) { return ch >= 0x20 && ch <= 0x7e; });
}

[[nodiscard]] bool unicode_space(char32_t value) {
    return (value >= 0x09 && value <= 0x0d) || value == 0x20 || value == 0xa0 || value == 0x1680 ||
           (value >= 0x2000 && value <= 0x200a) || value == 0x2028 || value == 0x2029 || value == 0x202f ||
           value == 0x205f || value == 0x3000 || value == 0xfeff;
}

/// Match JavaScript `\\s` against every code point of one frozen grapheme.
/// Fuzzy tests one preceding code unit; word navigation tests a whole segment.

[[nodiscard]] std::string normalize_query(std::string_view query) {
    std::string normalized;
    bool pending_space = false;
    for (std::size_t position = 0; position < query.size();) {
        const auto [codepoint, size] = detail::decode_utf8(query, position);
        if (size == 0) break;
        if (unicode_space(codepoint)) {
            if (!normalized.empty()) pending_space = true;
        } else {
            if (pending_space) normalized.push_back(' ');
            normalized.append(query.substr(position, size));
            pending_space = false;
        }
        position += size;
    }
    return normalized;
}

/// Build row spans using pi's printable-ASCII fast path and grapheme cell
/// widths
/// (`alt-screen-search.ts:34-106`).
[[nodiscard]] detail::AltScreenSearchCorpus build_search_corpus(const std::vector<std::string>& lines) {
    detail::AltScreenSearchCorpus corpus;
    bool pending_separator = false;
    const auto append_separator = [&] {
        if (!pending_separator) return;
        corpus.text.push_back(' ');
        pending_separator = false;
    };

    for (std::size_t row = 0; row < lines.size(); ++row) {
        const auto line = strip_terminal_sequences(lines[row]);
        std::size_t column = 0;
        if (printable_ascii(line)) {
            std::size_t index = 0;
            while (index < line.size()) {
                if (line[index] == ' ') {
                    if (!corpus.text.empty()) pending_separator = true;
                    ++column;
                    ++index;
                    continue;
                }
                auto end = index + 1;
                while (end < line.size() && line[end] != ' ')
                    ++end;
                append_separator();
                const auto text_start = corpus.text.size();
                corpus.text.append(line, index, end - index);
                corpus.spans.push_back(detail::AltScreenSearchSourceSpan{
                        .text_start = text_start,
                        .text_end = corpus.text.size(),
                        .row = row,
                        .start_col = column,
                        .end_col = column + end - index,
                        .linear_columns = true,
                });
                column += end - index;
                index = end;
            }
        } else {
            for (const auto& grapheme : detail::split_graphemes(line)) {
                bool whitespace = true;
                for (std::size_t offset = 0; offset < grapheme.size();) {
                    const auto [codepoint, size] = detail::decode_utf8(grapheme, offset);
                    if (size == 0) {
                        whitespace = false;
                        break;
                    }
                    if (!unicode_space(codepoint)) whitespace = false;
                    offset += size;
                }
                const auto width = visible_width(grapheme);
                if (whitespace) {
                    if (!corpus.text.empty()) pending_separator = true;
                    column += width;
                    continue;
                }
                append_separator();
                const auto text_start = corpus.text.size();
                corpus.text += grapheme;
                corpus.spans.push_back(detail::AltScreenSearchSourceSpan{
                        .text_start = text_start,
                        .text_end = corpus.text.size(),
                        .row = row,
                        .start_col = column,
                        .end_col = column + width,
                        .linear_columns = false,
                });
                column += width;
            }
        }
        if (!corpus.text.empty()) pending_separator = true;
    }
    return corpus;
}

[[nodiscard]] std::vector<std::size_t> utf8_boundaries(std::string_view text) {
    std::vector<std::size_t> boundaries{0};
    for (std::size_t position = 0; position < text.size();) {
        const auto [codepoint, size] = detail::decode_utf8(text, position);
        (void)codepoint;
        if (size == 0) break;
        position += size;
        boundaries.push_back(position);
    }
    return boundaries;
}

/// Map non-overlapping Unicode case-insensitive matches back to row segments
/// (`alt-screen-search.ts:116-148`).
[[nodiscard]] std::vector<SearchMatch> find_corpus_matches(
        const detail::AltScreenSearchCorpus& corpus, std::string_view query) {
    std::vector<SearchMatch> matches;
    if (query.empty()) return matches;
    const auto boundaries = utf8_boundaries(corpus.text);
    std::vector<char32_t> query_codepoints;
    for (std::size_t position = 0; position < query.size();) {
        const auto [codepoint, size] = detail::decode_utf8(query, position);
        if (size == 0) break;
        query_codepoints.push_back(codepoint);
        position += size;
    }
    if (query_codepoints.empty()) return matches;

    std::size_t span_index = 0;
    std::size_t boundary_index = 0;
    while (boundary_index + query_codepoints.size() < boundaries.size()) {
        const auto start = boundaries[boundary_index];
        bool matched = true;
        for (std::size_t offset = 0; offset < query_codepoints.size(); ++offset) {
            const auto [codepoint, size] = detail::decode_utf8(corpus.text, boundaries[boundary_index + offset]);
            if (size == 0 || utf8proc_tolower(static_cast<utf8proc_int32_t>(codepoint)) !=
                                     utf8proc_tolower(static_cast<utf8proc_int32_t>(query_codepoints[offset]))) {
                matched = false;
                break;
            }
        }
        if (!matched) {
            ++boundary_index;
            continue;
        }
        const auto end = boundaries[boundary_index + query_codepoints.size()];
        while (span_index < corpus.spans.size() && corpus.spans[span_index].text_end <= start)
            ++span_index;
        SearchMatch match;
        for (std::size_t index = span_index; index < corpus.spans.size(); ++index) {
            const auto& span = corpus.spans[index];
            if (span.text_start >= end) break;
            if (span.text_end <= start) continue;
            const auto start_col = span.linear_columns
                                           ? span.start_col + std::max(start, span.text_start) - span.text_start
                                           : span.start_col;
            const auto end_col = span.linear_columns ? span.start_col + std::min(end, span.text_end) - span.text_start
                                                     : span.end_col;
            if (!match.segments.empty() && match.segments.back().row == span.row &&
                    start_col <= match.segments.back().end_col) {
                match.segments.back().end_col = std::max(match.segments.back().end_col, end_col);
            } else {
                match.segments.push_back(SearchSegment{.row = span.row, .start_col = start_col, .end_col = end_col});
            }
        }
        while (span_index < corpus.spans.size() && corpus.spans[span_index].text_end <= end)
            ++span_index;
        if (!match.segments.empty()) matches.push_back(std::move(match));
        boundary_index += query_codepoints.size();
    }
    return matches;
}

[[nodiscard]] std::string fit(std::string_view text, std::size_t width, bool pad = true) {
    auto clipped = truncate_text(text, width, "", pad);
    return clipped ? std::move(*clipped) : std::string(width, ' ');
}

[[nodiscard]] std::string repeat_utf8(std::string_view text, std::size_t count) {
    std::string result;
    result.reserve(text.size() * count);
    for (std::size_t index = 0; index < count; ++index)
        result.append(text);
    return result;
}

} // namespace

SearchResult SearchIndex::search(const std::vector<std::string>& lines, std::string_view query) {
    const bool source_changed = !initialized_ || source_lines_ != lines;
    if (source_changed) {
        source_lines_ = lines;
        corpus_ = build_search_corpus(lines);
    }
    const auto normalized = normalize_query(query);
    const bool changed = source_changed || normalized != normalized_query_;
    if (changed) {
        matches_ = find_corpus_matches(corpus_, normalized);
        normalized_query_ = normalized;
    }
    initialized_ = true;
    return SearchResult{.matches = matches_, .changed = changed};
}

std::vector<SearchMatch> find_alt_screen_search_matches(const std::vector<std::string>& lines, std::string_view query) {
    return find_corpus_matches(build_search_corpus(lines), normalize_query(query));
}

std::string alt_screen_search_match_key(const SearchMatch& match) {
    if (match.segments.empty()) return {};
    const auto& first = match.segments.front();
    const auto& last = match.segments.back();
    return std::to_string(first.row) + ":" + std::to_string(first.start_col) + ":" + std::to_string(last.row) + ":" +
           std::to_string(last.end_col);
}

AltScreenSearchComponent::AltScreenSearchComponent(std::move_only_function<void(std::string)> on_query_change,
        const KeybindingRegistry& keybindings,
        std::shared_ptr<KeybindingsContext> keybinding_context,
        SearchNavigationButtonStyle navigation_button_style)
    : input_(InputOptions{
              .keybindings = keybinding_context ? keybinding_context->snapshot() : default_tui_keybindings(),
              .keybinding_context = keybinding_context,
              .prompt = " ",
              .placeholder = "Find in transcript",
              .placeholder_style = [](std::string_view text) { return "\x1b[2m" + std::string(text) + "\x1b[22m"; },
      }),
      keybindings_(keybindings), keybinding_context_(std::move(keybinding_context)),
      on_query_change_(std::move(on_query_change)), navigation_button_style_(std::move(navigation_button_style)) {
    if (!navigation_button_style_) navigation_button_style_ = [](std::string text, bool) { return text; };
}

std::string AltScreenSearchComponent::query() const { return input_.value(); }

void AltScreenSearchComponent::set_result(std::ptrdiff_t index, std::size_t count) {
    result_index_ = index;
    result_count_ = count;
}

std::optional<int> AltScreenSearchComponent::navigation_direction_at(std::size_t row, std::size_t column) const {
    if (row != 2 || previous_button_start_ < 0) return std::nullopt;
    const auto cell = static_cast<std::ptrdiff_t>(column);
    if (cell >= previous_button_start_ && cell < previous_button_end_) return -1;
    if (cell >= next_button_start_ && cell < next_button_end_) return 1;
    return std::nullopt;
}

bool AltScreenSearchComponent::set_hovered_navigation_direction(std::optional<int> direction) {
    if (direction == hovered_navigation_direction_) return false;
    hovered_navigation_direction_ = direction;
    return true;
}

std::string AltScreenSearchComponent::key_label(std::string_view id) const {
    const auto keys = keybinding_context_ ? keybinding_context_->keys(id) : keybindings_.keys(id);
    if (keys.empty()) return "Unbound";
    std::string label;
    std::size_t start = 0;
    const auto& key = keys.front();
    while (start <= key.size()) {
        const auto end = key.find('+', start);
        auto part = key.substr(start, end == std::string::npos ? key.size() - start : end - start);
        if (!part.empty()) {
            if (!label.empty()) label.push_back('+');
            if (part == "alt")
                label += "Alt";
            else {
                part.front() = static_cast<char>(std::toupper(static_cast<unsigned char>(part.front())));
                label += part;
            }
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return label;
}

support::Expected<RenderResult> AltScreenSearchComponent::render(std::size_t width) {
    const auto safe_width = std::max<std::size_t>(1, width);
    if (safe_width == 1) return RenderResult{.lines = {"┌", "│", "└"}};
    const auto inner_width = safe_width - 2;
    const auto current_query = input_.value();
    const auto result = current_query.empty() ? std::string{}
                        : result_count_ == 0  ? std::string{"No matches"}
                                              : std::to_string(result_index_ + 1) + "/" + std::to_string(result_count_);
    const auto result_space = inner_width > 3 ? inner_width - 3 : 0;
    const auto visible_result = fit(result, result_space, false);
    const auto result_text = visible_result.empty() ? std::string{} : "\x1b[2m " + visible_result + " \x1b[22m";
    const auto input_width = inner_width > visible_width(result_text) ? inner_width - visible_width(result_text) : 0;
    auto input_result = input_.render(std::max<std::size_t>(1, input_width));
    if (!input_result) return std::unexpected(input_result.error());
    const auto input_line = input_width == 0 || input_result->lines.empty()
                                    ? std::string{}
                                    : fit(input_result->lines.front(), input_width, false);
    const auto input_padding = input_width > visible_width(input_line) ? input_width - visible_width(input_line) : 0;
    const auto content = fit(input_line + std::string(input_padding, ' ') + result_text, inner_width);

    auto previous = "↑ " + key_label("tui.altScreen.searchPrevious");
    auto next = "↓ " + key_label("tui.altScreen.searchNext");
    std::string separator{" · "};
    constexpr std::size_t gap = 1;
    const auto available = inner_width > 3 ? inner_width - 3 : 0;
    auto controls_width = visible_width(previous) + visible_width(separator) + visible_width(next);
    if (controls_width > available) {
        previous = "↑";
        next = "↓";
        separator = " ";
        controls_width = visible_width(previous) + 1 + visible_width(next);
    }
    const bool show_buttons = controls_width <= available;
    const auto rendered_buttons =
            show_buttons ? navigation_button_style_(previous, hovered_navigation_direction_ == -1) + separator +
                                   navigation_button_style_(next, hovered_navigation_direction_ == 1)
                         : std::string{};
    const auto gaps_width = show_buttons ? gap * 2 : 0;
    const auto right_rule_width = !rendered_buttons.empty() && inner_width > controls_width + gaps_width ? 1U : 0U;
    const auto left_rule_width =
            inner_width > (show_buttons ? controls_width : 0) + gaps_width + right_rule_width
                    ? inner_width - (show_buttons ? controls_width : 0) - gaps_width - right_rule_width
                    : 0U;
    const auto previous_start = 1 + left_rule_width + gap;
    previous_button_start_ = show_buttons ? static_cast<std::ptrdiff_t>(previous_start) : -1;
    previous_button_end_ =
            show_buttons ? previous_button_start_ + static_cast<std::ptrdiff_t>(visible_width(previous)) : -1;
    next_button_start_ =
            show_buttons ? previous_button_end_ + static_cast<std::ptrdiff_t>(visible_width(separator)) : -1;
    next_button_end_ = show_buttons ? next_button_start_ + static_cast<std::ptrdiff_t>(visible_width(next)) : -1;

    std::string bottom = "└" + repeat_utf8("─", left_rule_width);
    if (!rendered_buttons.empty()) bottom += " " + rendered_buttons + " ";
    bottom += repeat_utf8("─", right_rule_width) + "┘";
    return RenderResult{.lines = {"┌" + repeat_utf8("─", inner_width) + "┐", "│" + content + "│", std::move(bottom)}};
}

void AltScreenSearchComponent::invalidate() { input_.invalidate(); }

InputAdmissionOutcome AltScreenSearchComponent::handle_input(const InputEventVariant& event) {
    const auto before = input_.value();
    const auto outcome = input_.handle_input(event);
    const auto after = input_.value();
    if (before != after && on_query_change_) on_query_change_(after);
    return outcome;
}

void AltScreenSearchComponent::set_focused(bool focused) {
    focused_ = focused;
    input_.set_focused(focused);
}

bool AltScreenSearchComponent::focused() const { return focused_; }

std::optional<CursorPosition> AltScreenSearchComponent::cursor_location() const {
    const auto position = input_.cursor_location();
    if (!position) return std::nullopt;
    return CursorPosition{.column = position->column + 1, .row = position->row + 1};
}

} // namespace cch::tui
