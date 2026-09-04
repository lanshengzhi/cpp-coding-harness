#include <cch/tui/SelectList.hpp>

#include <cch/tui/Fuzzy.hpp>
#include <cch/tui/Input.hpp>
#include <cch/tui/Utils.hpp>

#include "tui/InteractionUtils.hpp"
#include "tui/UnicodeWidth.hpp"

#include <cch/support/Error.hpp>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <exception>
#include <format>
#include <memory>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::tui {
namespace {

// Behavioral baseline: pi 83114817 packages/tui/src/components/select-list.ts.
// The 32-column default primary width, 2-column gap, 10-column minimum
// description width, and the tui.select.up/down/confirm/cancel interactions
// are aligned with the frozen baseline; semantic page keys
// (tui.select.pageUp/pageDown) remain the recorded C++ extension (pi's
// select-list only pages via up/down at this baseline).
constexpr std::size_t kDefaultPrimaryColumnWidth = 32;
constexpr std::size_t kPrimaryColumnGap = 2;
constexpr std::size_t kMinDescriptionWidth = 10;

/// Selection dispatch order (pi select-list.ts, plus the C++ semantic page
/// keys). The first bound action in table order wins, so table order IS the
/// precedence.
constexpr std::array<std::string_view, 6> kSelectListActions = {
        "tui.select.up",
        "tui.select.down",
        "tui.select.pageUp",
        "tui.select.pageDown",
        "tui.select.confirm",
        "tui.select.cancel",
};

[[nodiscard]] bool starts_with_case_insensitive(std::string_view value, std::string_view prefix) {
    if (prefix.size() > value.size()) return false;
    for (std::size_t index = 0; index < prefix.size(); ++index) {
        const auto left = static_cast<unsigned char>(value[index]);
        const auto right = static_cast<unsigned char>(prefix[index]);
        if (std::tolower(left) != std::tolower(right)) return false;
    }
    return true;
}

[[nodiscard]] std::string single_line(std::string text) {
    std::string result;
    result.reserve(text.size());
    bool replacing_newline = false;
    for (const auto& value : text) {
        if (value == '\r' || value == '\n') {
            if (!replacing_newline) result.push_back(' ');
            replacing_newline = true;
        } else {
            result.push_back(value);
            replacing_newline = false;
        }
    }
    const auto first = result.find_first_not_of(" \t");
    if (first == std::string::npos) return {};
    const auto last = result.find_last_not_of(" \t");
    return result.substr(first, last - first + 1);
}

[[nodiscard]] std::string_view display_value(const SelectItem& item) {
    return item.label.empty() ? std::string_view(item.value) : std::string_view(item.label);
}

/// The item's searchable text for the default fuzzy ranking (#586). An item
/// with `search_text` set is searched on that text alone; otherwise its
/// non-empty label, description and value are joined by single spaces (in
/// that order) so every visible part of the row is matchable.
[[nodiscard]] std::string item_searchable_text(const SelectItem& item) {
    if (item.search_text) return *item.search_text;
    std::string text;
    const auto append = [&text](std::string_view part) {
        if (part.empty()) return;
        if (!text.empty()) text.push_back(' ');
        text.append(part);
    };
    append(item.label);
    if (item.description) append(*item.description);
    append(item.value);
    return text;
}

/// Split chrome text (title/hint) into its rendered rows: every source line
/// separated by '\n' becomes one row (empty lines preserved), dropping a
/// trailing carriage return from each line so CRLF input renders cleanly.
[[nodiscard]] std::vector<std::string> multiline_chrome_text(std::string_view text) {
    std::vector<std::string> lines;
    std::size_t begin = 0;
    while (true) {
        const auto newline = text.find('\n', begin);
        const auto line = text.substr(begin, newline == std::string_view::npos ? text.size() - begin : newline - begin);
        if (!line.empty() && line.back() == '\r') {
            lines.emplace_back(line.substr(0, line.size() - 1));
        } else {
            lines.emplace_back(line);
        }
        if (newline == std::string_view::npos) break;
        begin = newline + 1;
    }
    return lines;
}

} // namespace

struct SelectList::Impl {
    std::vector<SelectItem> items;
    std::vector<std::size_t> filtered_indices;
    std::size_t selected_index{0};
    std::size_t max_visible{5};
    SelectListTheme theme;
    SelectListLayoutOptions layout;
    std::shared_ptr<SelectItemSink> on_select;
    std::shared_ptr<SelectCancelSink> on_cancel;
    std::shared_ptr<SelectItemSink> on_selection_change;
    std::shared_ptr<const KeybindingRegistry> keybindings;
    std::optional<support::Error> callback_error;
    bool focused{false};

    // #586: embedded search and chrome framing.
    bool search_enabled{false};
    std::unique_ptr<Input> search_input;
    /// Legacy prefix-filter text (case-insensitive prefix on `value`),
    /// re-applied by `set_items` when search is disabled.
    std::string legacy_filter_prefix;
    SelectSearchFilterHook search_filter_hook;
    std::optional<std::string> title;
    std::optional<std::string> hint;
    TextStyleHook border_hook;
    std::optional<std::string> no_match_text;
    /// Row of the search line in the last successful render output; unset
    /// until the search line has actually been emitted (and reset by any
    /// later render attempt, successful or not).
    std::optional<std::size_t> search_line_row;

    [[nodiscard]] const SelectItem* selected() const {
        if (selected_index >= filtered_indices.size()) return nullptr;
        return &items[filtered_indices[selected_index]];
    }

    void report_callback_failure(std::string message) {
        callback_error = support::make_error(
            support::ErrorCode::Unknown,
            std::move(message),
            "the interaction callback threw an exception");
    }

    void notify_selection_change() {
        const auto* item = selected();
        auto sink = on_selection_change;
        if (item == nullptr || !sink || !*sink) return;
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
        try {
#endif
            if (auto observed = (*sink)(*item); !observed) {
                callback_error = std::move(observed.error());
            }
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
        } catch (...) {
            report_callback_failure("TUI SelectList selection callback failed");
        }
#endif
    }

    /// The value of the currently selected item, when one is selected.
    [[nodiscard]] std::optional<std::string> selected_value() const {
        const auto* item = selected();
        return item == nullptr ? std::nullopt : std::optional<std::string>(item->value);
    }

    /// Whether chrome framing is active: any of title/hint/border_hook set.
    [[nodiscard]] bool chrome_enabled() const {
        return static_cast<bool>(border_hook) || (title && !title->empty()) || (hint && !hint->empty());
    }

    /// One full-width `─` rule line, styled through `border_hook` when set.
    [[nodiscard]] support::Expected<std::string> border_line(std::size_t width) {
        std::string rule;
        rule.reserve(width * 3);
        for (std::size_t index = 0; index < width; ++index)
            rule += "─";
        return detail::apply_text_style(border_hook, std::move(rule), "SelectList border");
    }

    /// The rendered rows of a chrome text block (title/hint): each source
    /// line split on '\n' (empty lines preserved), truncated to the width.
    [[nodiscard]] support::Expected<std::vector<std::string>> chrome_block_rows(
            const std::optional<std::string>& content, std::size_t width) const {
        std::vector<std::string> rows;
        if (!content || content->empty()) return rows;
        for (const auto& source_line : multiline_chrome_text(*content)) {
            auto bounded = truncate_text(source_line, width, "");
            if (!bounded) return std::unexpected(bounded.error());
            rows.push_back(std::move(*bounded));
        }
        return rows;
    }

    /// The query the embedded input currently holds ("" when search is off).
    [[nodiscard]] std::string query_text() const { return search_input ? search_input->value() : std::string{}; }

    /// Recompute `filtered_indices` from the current items and query:
    /// search mode ranks through `search_filter_hook` when set (fully
    /// replacing the default) or the toolkit's fuzzy subsequence filter over
    /// each item's searchable text; an empty query keeps every item in
    /// original order. Search-disabled mode applies the legacy
    /// case-insensitive prefix filter on `value`. A throwing search hook is
    /// recorded as a bounded callback error and leaves the previous ranking
    /// in place.
    void recompute_filtered() {
        if (!search_enabled) {
            filtered_indices.clear();
            if (legacy_filter_prefix.empty()) {
                filtered_indices.reserve(items.size());
                for (std::size_t index = 0; index < items.size(); ++index) {
                    filtered_indices.push_back(index);
                }
                return;
            }
            for (std::size_t index = 0; index < items.size(); ++index) {
                if (starts_with_case_insensitive(items[index].value, legacy_filter_prefix)) {
                    filtered_indices.push_back(index);
                }
            }
            return;
        }

        const auto query = query_text();
        if (search_filter_hook) {
            std::vector<std::size_t> matched;
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
            try {
#endif
                matched = search_filter_hook(query, items);
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
            } catch (...) {
                report_callback_failure("TUI SelectList search filter hook failed");
                return;
            }
#endif
            // The hook fully replaces fuzzy ranking; defensively drop
            // out-of-range and duplicate indices while honoring its order.
            filtered_indices.clear();
            std::vector<char> seen(items.size(), 0);
            filtered_indices.reserve(matched.size());
            for (const auto index : matched) {
                if (index >= items.size() || seen[index] != 0) continue;
                seen[index] = 1;
                filtered_indices.push_back(index);
            }
            return;
        }

        if (query.empty()) {
            filtered_indices.clear();
            filtered_indices.reserve(items.size());
            for (std::size_t index = 0; index < items.size(); ++index) {
                filtered_indices.push_back(index);
            }
            return;
        }
        std::vector<std::size_t> indices(items.size());
        std::iota(indices.begin(), indices.end(), 0);
        filtered_indices = fuzzy_filter(
                std::move(indices), query, [&](std::size_t index) { return item_searchable_text(items[index]); });
    }

    /// Selection policy after a ranking change from typed input: the
    /// top-ranked match of the new query becomes the selection. When `notify`
    /// and the selected item's value actually changed, the selection-change
    /// sink fires (matching the navigation behavior).
    void apply_query_change(bool notify) {
        const auto prior = selected_value();
        recompute_filtered();
        selected_index = 0;
        if (notify && selected_value() != prior) notify_selection_change();
    }

    /// Selection policy for `set_items`: keep the previously selected item
    /// (by value) at its new filtered position when still present, else clamp
    /// the previous filtered position into the new range.
    void restore_selection(std::optional<std::string> prior_value, std::size_t prior_index) {
        if (filtered_indices.empty()) {
            selected_index = 0;
            return;
        }
        if (prior_value) {
            for (std::size_t position = 0; position < filtered_indices.size(); ++position) {
                if (items[filtered_indices[position]].value == *prior_value) {
                    selected_index = position;
                    return;
                }
            }
        }
        selected_index = std::min(prior_index, filtered_indices.size() - 1);
    }

    [[nodiscard]] std::pair<std::size_t, std::size_t> primary_bounds() const {
        const auto raw_min = layout.min_primary_column_width.value_or(
            layout.max_primary_column_width.value_or(kDefaultPrimaryColumnWidth));
        const auto raw_max = layout.max_primary_column_width.value_or(
            layout.min_primary_column_width.value_or(kDefaultPrimaryColumnWidth));
        return {
            std::max<std::size_t>(1, std::min(raw_min, raw_max)),
            std::max<std::size_t>(1, std::max(raw_min, raw_max)),
        };
    }

    [[nodiscard]] std::size_t primary_column_width() const {
        const auto [minimum, maximum] = primary_bounds();
        std::size_t widest = 0;
        for (const auto& index : filtered_indices) {
            widest = std::max(widest, visible_width(display_value(items[index])) + kPrimaryColumnGap);
        }
        return std::clamp(widest, minimum, maximum);
    }

    [[nodiscard]] support::Expected<std::string> truncate_primary(
        const SelectItem& item,
        bool is_selected,
        std::size_t max_width,
        std::size_t column_width) {
        const auto text = std::string(display_value(item));
        std::string transformed = text;
        if (layout.truncate_primary) {
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
            try {
#endif
                transformed = layout.truncate_primary(
                    text,
                    max_width,
                    column_width,
                    item,
                    is_selected);
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
            } catch (...) {
                return std::unexpected(support::make_error(
                    support::ErrorCode::Unknown,
                    "TUI SelectList truncation hook failed",
                    "the truncation callback threw an exception"));
            }
#endif
        }
        return truncate_text(transformed, max_width, "");
    }

    [[nodiscard]] support::Expected<std::string> render_item(
        const SelectItem& item,
        bool is_selected,
        std::size_t width,
        std::size_t primary_width) {
        const std::string prefix = is_selected ? "→ " : "  ";
        const auto prefix_width = visible_width(prefix);
        std::string line;

        const auto description = item.description ? single_line(*item.description) : std::string{};
        if (!description.empty() && width > 40 && width > prefix_width + 4) {
            const auto effective_primary = std::max<std::size_t>(
                1,
                std::min(primary_width, width - prefix_width - 4));
            const auto primary_limit = std::max<std::size_t>(1, effective_primary - kPrimaryColumnGap);
            auto primary = truncate_primary(item, is_selected, primary_limit, effective_primary);
            if (!primary) return std::unexpected(primary.error());
            const auto primary_visible = visible_width(*primary);
            const auto spacing_size = std::max<std::size_t>(1, effective_primary - primary_visible);
            const auto description_start = prefix_width + primary_visible + spacing_size;
            const auto remaining = width > description_start + 2 ? width - description_start - 2 : 0;
            if (remaining > kMinDescriptionWidth) {
                auto truncated_description = truncate_text(description, remaining, "");
                if (!truncated_description) return std::unexpected(truncated_description.error());
                const auto spacing = std::string(spacing_size, ' ');
                if (is_selected) {
                    line = prefix + *primary + spacing + *truncated_description;
                } else {
                    auto styled_description = detail::apply_text_style(
                        theme.description,
                        spacing + *truncated_description,
                        "SelectList description");
                    if (!styled_description) return std::unexpected(styled_description.error());
                    line = prefix + *primary + *styled_description;
                }
            }
        }

        if (line.empty()) {
            const auto available = width > prefix_width + 2 ? width - prefix_width - 2 : 0;
            auto primary = truncate_primary(item, is_selected, available, available);
            if (!primary) return std::unexpected(primary.error());
            line = prefix + *primary;
            auto bounded = truncate_text(line, width, "");
            if (!bounded) return std::unexpected(bounded.error());
            line = std::move(*bounded);
        }
        if (is_selected) {
            auto styled = detail::apply_text_style(theme.selected_text, std::move(line), "SelectList selected text");
            if (!styled) return std::unexpected(styled.error());
            line = std::move(*styled);
        }
        return line;
    }
};

SelectList::SelectList(std::vector<SelectItem> items, SelectListOptions options)
    : impl_(std::make_shared<Impl>()) {
    impl_->items = std::move(items);
    impl_->filtered_indices.reserve(impl_->items.size());
    impl_->max_visible = std::max<std::size_t>(1, options.max_visible);
    impl_->theme = std::move(options.theme);
    impl_->layout = std::move(options.layout);
    impl_->on_select = std::make_shared<SelectItemSink>(std::move(options.on_select));
    impl_->on_cancel = std::make_shared<SelectCancelSink>(std::move(options.on_cancel));
    impl_->on_selection_change = std::make_shared<SelectItemSink>(std::move(options.on_selection_change));
    impl_->keybindings = options.keybindings ? std::move(options.keybindings) : default_tui_keybindings();
    impl_->search_enabled = options.enable_search;
    impl_->title = std::move(options.title);
    impl_->hint = std::move(options.hint);
    impl_->border_hook = std::move(options.border_hook);
    impl_->search_filter_hook = std::move(options.search_filter_hook);
    impl_->no_match_text = std::move(options.no_match_text);
    if (impl_->search_enabled) {
        // The embedded input shares SelectList's effective registry so its
        // editing actions (incl. cancel-as-first-action) resolve identically.
        impl_->search_input = std::make_unique<Input>(InputOptions{
                .keybindings = impl_->keybindings,
                .placeholder = std::move(options.search_placeholder),
        });
        if (options.initial_search) {
            impl_->search_input->set_value(std::move(*options.initial_search));
            // A prefilled query reads naturally with the cursor at its end so
            // subsequent typing appends; the registry's cursorLineEnd action
            // performs the move without widening Input's public API.
            impl_->search_input->handle_input(KeyEvent{.key = "end"});
        }
    }
    // Rank the initial items against the starting query (all items in order
    // when search is off or the query is empty). Programmatic ranking never
    // fires selection-change notifications.
    impl_->recompute_filtered();
    impl_->selected_index = 0;
}

SelectList::SelectList(SelectList&&) noexcept = default;
SelectList& SelectList::operator=(SelectList&&) noexcept = default;
SelectList::~SelectList() = default;

void SelectList::set_filter(std::string filter) {
    auto impl = impl_;
    if (impl->search_enabled && impl->search_input) {
        // In search mode the legacy filter text becomes the search query;
        // ranking proceeds exactly as for typed input (top-ranked selection,
        // no selection-change notification for this programmatic change). The
        // cursor lands at the end so continued typing appends.
        impl->search_input->set_value(std::move(filter));
        impl->search_input->handle_input(KeyEvent{.key = "end"});
        impl->recompute_filtered();
        impl->selected_index = 0;
        return;
    }
    impl->legacy_filter_prefix = std::move(filter);
    impl->recompute_filtered();
    impl->selected_index = 0;
}

void SelectList::set_items(std::vector<SelectItem> items) {
    auto impl = impl_;
    const auto prior_value = impl->selected_value();
    const auto prior_index = impl->selected_index;
    impl->items = std::move(items);
    impl->recompute_filtered();
    impl->restore_selection(prior_value, prior_index);
}

void SelectList::set_selected_index(std::size_t index) {
    auto impl = impl_;
    impl->selected_index = impl->filtered_indices.empty()
                               ? 0
                               : std::min(index, impl->filtered_indices.size() - 1);
}

std::optional<SelectItem> SelectList::selected_item() const {
    const auto* item = impl_->selected();
    return item == nullptr ? std::nullopt : std::optional<SelectItem>(*item);
}

std::string SelectList::search_query() const { return impl_->query_text(); }

support::Expected<RenderResult> SelectList::render(std::size_t width) {
    auto impl = impl_;
    // Any render attempt invalidates the previous layout: the cursor is only
    // reportable against the most recent successful emission of the search
    // line (width/callback-error failures reset it to no cursor).
    impl->search_line_row.reset();
    if (width == 0) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "TUI SelectList requires a positive visible width"));
    }
    if (impl->callback_error) return std::unexpected(*impl->callback_error);

    // Chrome framing (#586), active whenever title, hint or border_hook is
    // set. The frame separates each present block from the content above it
    // with exactly one blank line:
    //
    //   top border rule (─ × width, styled via border_hook when set)
    //   blank
    //   title rows (each source '\n'-separated line, empty lines preserved)
    //   blank
    //   hint rows (same treatment)
    //   blank
    //   search input row (when enable_search; recorded for cursor_location)
    //   list rows: items or the no-match row, then the scroll indicator when
    //              the window clips
    //   bottom border rule
    //
    // Absent title/hint blocks are skipped entirely. Without chrome the body
    // starts at row 0, so a search input (when enabled) is always row 0.
    const auto chrome = impl->chrome_enabled();
    std::vector<std::string> lines;
    if (chrome) {
        auto top = impl->border_line(width);
        if (!top) return std::unexpected(top.error());
        lines.push_back(std::move(*top));
        const std::optional<std::string>* const blocks[] = {&impl->title, &impl->hint};
        for (const auto* block : blocks) {
            if (!block || !*block || (*block)->empty()) continue;
            lines.emplace_back();
            auto rows = impl->chrome_block_rows(*block, width);
            if (!rows) return std::unexpected(rows.error());
            for (auto& row : *rows)
                lines.push_back(std::move(row));
        }
        lines.emplace_back();
    }

    if (impl->search_enabled && impl->search_input) {
        auto search = impl->search_input->render(width);
        if (!search) return std::unexpected(search.error());
        // Input documents a single-line render contract; record where the
        // search row landed so cursor_location can offset the input cursor.
        if (!search->lines.empty()) {
            impl->search_line_row = lines.size();
            lines.push_back(std::move(search->lines.front()));
        }
    }

    if (impl->filtered_indices.empty()) {
        auto message = truncate_text(impl->no_match_text.value_or("  No matching commands"), width, "");
        if (!message) return std::unexpected(message.error());
        auto styled = detail::apply_text_style(impl->theme.no_match, std::move(*message), "SelectList no match");
        if (!styled) return std::unexpected(styled.error());
        lines.push_back(std::move(*styled));
    } else {
        const auto range =
                detail::centered_visible_range(impl->filtered_indices.size(), impl->selected_index, impl->max_visible);
        const auto primary_width = impl->primary_column_width();
        for (std::size_t index = range.begin; index < range.end; ++index) {
            auto line = impl->render_item(
                    impl->items[impl->filtered_indices[index]], index == impl->selected_index, width, primary_width);
            if (!line) return std::unexpected(line.error());
            lines.push_back(std::move(*line));
        }
        if (range.begin > 0 || range.end < impl->filtered_indices.size()) {
            auto text = std::format("  ({}/{})", impl->selected_index + 1, impl->filtered_indices.size());
            auto bounded = truncate_text(text, width, "");
            if (!bounded) return std::unexpected(bounded.error());
            auto styled =
                    detail::apply_text_style(impl->theme.scroll_info, std::move(*bounded), "SelectList scroll info");
            if (!styled) return std::unexpected(styled.error());
            lines.push_back(std::move(*styled));
        }
    }

    if (chrome) {
        auto bottom = impl->border_line(width);
        if (!bottom) return std::unexpected(bottom.error());
        lines.push_back(std::move(*bottom));
    }
    return RenderResult{.lines = std::move(lines)};
}

void SelectList::invalidate() {
    if (impl_->search_input) impl_->search_input->invalidate();
}

void SelectList::handle_input(const InputEventVariant& input) {
    auto impl = impl_;
    if (impl->search_enabled && impl->search_input && std::holds_alternative<PasteEvent>(input)) {
        const auto prior_value = impl->search_input->value();
        impl->search_input->handle_input(input);
        if (impl->search_input->value() != prior_value) impl->apply_query_change(true);
        return;
    }
    const auto* key = std::get_if<KeyEvent>(&input);
    if (key == nullptr || key->type == KeyEventType::Release) return;
    const auto count = impl->filtered_indices.size();
    const auto action = impl->keybindings->first_match(*key, kSelectListActions);
    if (action == "tui.select.up") {
        if (count == 0) return;
        impl->selected_index = impl->selected_index == 0 ? count - 1 : impl->selected_index - 1;
        impl->notify_selection_change();
        return;
    }
    if (action == "tui.select.down") {
        if (count == 0) return;
        impl->selected_index = impl->selected_index + 1 == count ? 0 : impl->selected_index + 1;
        impl->notify_selection_change();
        return;
    }
    if (action == "tui.select.pageUp" || action == "tui.select.pageDown") {
        if (count == 0) return;
        const auto page = impl->max_visible;
        if (action == "tui.select.pageUp") {
            impl->selected_index = impl->selected_index > page ? impl->selected_index - page : 0;
        } else {
            impl->selected_index = std::min(impl->selected_index + page, count - 1);
        }
        impl->notify_selection_change();
        return;
    }
    if (action == "tui.select.confirm") {
        const auto* item = impl->selected();
        auto sink = impl->on_select;
        if (item == nullptr || !sink || !*sink) return;
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
        try {
#endif
            if (auto selected = (*sink)(*item); !selected) {
                impl->callback_error = std::move(selected.error());
            }
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
        } catch (...) {
            impl->report_callback_failure("TUI SelectList select callback failed");
        }
#endif
        return;
    }
    if (action == "tui.select.cancel") {
        auto sink = impl->on_cancel;
        if (!sink || !*sink) return;
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
        try {
#endif
            if (auto cancelled = (*sink)(); !cancelled) {
                impl->callback_error = std::move(cancelled.error());
            }
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
        } catch (...) {
            impl->report_callback_failure("TUI SelectList cancel callback failed");
        }
#endif
        return;
    }
    if (!impl->search_enabled || !impl->search_input) return;
    // Remaining keys (editing, cursor movement, undo, printable characters)
    // are the embedded Input component's behaviors; re-rank only when the
    // edit actually changed the query (pi select-list search flow).
    const auto prior_value = impl->search_input->value();
    impl->search_input->handle_input(input);
    if (impl->search_input->value() != prior_value) impl->apply_query_change(true);
}

bool SelectList::accepts_key_releases() const {
    return false;
}

void SelectList::set_focused(bool focused) {
    auto impl = impl_;
    impl->focused = focused;
    if (impl->search_input) impl->search_input->set_focused(focused);
}

bool SelectList::focused() const {
    return impl_->focused;
}

std::optional<CursorPosition> SelectList::cursor_location() const {
    const auto impl = impl_;
    if (!impl->focused || !impl->search_enabled || !impl->search_input) return std::nullopt;
    // No cursor until a render has actually placed the search line (its row
    // is the exact count of border/title/hint/blank rows emitted above it).
    if (!impl->search_line_row) return std::nullopt;
    const auto cursor = impl->search_input->cursor_location();
    if (!cursor) return std::nullopt;
    return CursorPosition{
            .column = cursor->column,
            .row = *impl->search_line_row + cursor->row,
    };
}

} // namespace cch::tui
