#pragma once

#include <cch/tui/Component.hpp>
#include <cch/tui/Keybindings.hpp>
#include <cch/tui/Style.hpp>

#include <cch/support/Error.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cch::tui {

struct SelectItem {
    std::string value{};
    std::string label{};
    std::optional<std::string> description{std::nullopt};
    /// Optional hidden search text. When set, it is the item's sole
    /// searchable text; otherwise the item is searched as its non-empty
    /// components of label, description and value joined by single spaces.
    std::optional<std::string> search_text{std::nullopt};

    bool operator==(const SelectItem&) const = default;
};

/// The item's searchable text for fuzzy ranking: with `search_text` set the
/// item is searched on that text alone; otherwise its non-empty label,
/// description and value are joined by single spaces (in that order) so every
/// visible part of the row is matchable. SelectList's default fuzzy ranking
/// and selectors replicating that ranking through a `search_filter_hook`
/// share this derivation.
[[nodiscard]] std::string select_item_search_text(const SelectItem& item);

using SelectItemSink = std::move_only_function<support::ExpectedVoid(const SelectItem&)>;
using SelectCancelSink = std::move_only_function<support::ExpectedVoid()>;
using SelectPrimaryTruncateHook = std::move_only_function<std::string(
    std::string,
    std::size_t,
    std::size_t,
    const SelectItem&,
    bool)>;

/// Ranking hook that fully replaces SelectList's internal fuzzy matching when
/// set. Invoked with the current query on every ranking recomputation
/// (typing, query restore, `set_items`, and construction with
/// `enable_search`), including for an empty query — which must return every
/// item index `0..n-1` in order. Returned indices must be in display order;
/// out-of-range or duplicated indices are dropped.
using SelectSearchFilterHook =
        std::move_only_function<std::vector<std::size_t>(std::string_view query, const std::vector<SelectItem>& items)>;

struct SelectListTheme {
    TextStyleHook selected_text{};
    TextStyleHook description{};
    TextStyleHook scroll_info{};
    TextStyleHook no_match{};
};

struct SelectListLayoutOptions {
    std::optional<std::size_t> min_primary_column_width{std::nullopt};
    std::optional<std::size_t> max_primary_column_width{std::nullopt};
    SelectPrimaryTruncateHook truncate_primary{};
};

struct SelectListOptions {
    std::size_t max_visible{5};
    SelectListTheme theme{};
    SelectListLayoutOptions layout{};
    SelectItemSink on_select{};
    SelectCancelSink on_cancel{};
    SelectItemSink on_selection_change{};
    std::shared_ptr<const KeybindingRegistry> keybindings{};
    /// Whether up/down navigation wraps across the first and last items.
    /// Disable to clamp at list boundaries.
    bool wrap_navigation{true};
    /// Enable raw unmodified `k`/`j` as up/down navigation when search is
    /// disabled. Ignored with `enable_search` so printable input remains
    /// available to the search query.
    bool enable_raw_jk_navigation{false};

    // #586 embedded search and chrome framing.
    /// Embed a single-line search input. When true, printable keys flow into
    /// the input and the displayed items are re-ranked (fuzzy subsequence by
    /// default, or through `search_filter_hook`) after every edit; the
    /// selection moves to the top-ranked match of each new query.
    bool enable_search{false};
    /// Placeholder shown inside the embedded search input while its value is
    /// empty (rendered by the shared Input component).
    std::optional<std::string> search_placeholder{std::nullopt};
    /// Query the embedded search input starts with; ranked immediately at
    /// construction. Ignored unless `enable_search` is set.
    std::optional<std::string> initial_search{std::nullopt};
    /// Header text rendered above the list. May contain '\n': each source
    /// line renders on its own row (empty lines preserved) truncated to the
    /// component width. Presence enables chrome framing (see `render`).
    std::optional<std::string> title{std::nullopt};
    /// Footer-of-header text rendered below the title with the same
    /// multiline treatment. Presence enables chrome framing.
    std::optional<std::string> hint{std::nullopt};
    /// Style applied to the top and bottom border rule lines when chrome
    /// framing is on. Presence alone also enables chrome framing.
    TextStyleHook border_hook{};
    /// Optional full replacement for the default fuzzy ranking (see
    /// `SelectSearchFilterHook`).
    SelectSearchFilterHook search_filter_hook{};
    /// The row shown in place of the list when the filter matches nothing.
    /// Absent, the toolkit default `"  No matching commands"` renders (styled
    /// through `theme.no_match`); selectors with their own empty-state
    /// wording (pi `noMatchingItems` variants) replace the whole text.
    std::optional<std::string> no_match_text{std::nullopt};
};

/// A filterable, width-bounded selection list controlled by semantic keys.
///
/// Render layout without chrome (no title/hint/border_hook): the search input
/// line (when `enable_search`, row 0) directly followed by the item rows or
/// the no-match line, then the scroll indicator when the window clips.
///
/// Render layout with chrome framing: top border rule, blank line, title
/// lines, blank line, hint lines, blank line, then the search input line
/// (when `enable_search`) or directly the list body, and finally the bottom
/// border rule. Absent title/hint blocks are skipped; each present block and
/// the body is separated from the content above it by exactly one blank
/// line. Border rules are `─` repeated to the width, styled through
/// `border_hook` when set.
class SelectList final : public Component, public InputHandler, public Focusable {
public:
    explicit SelectList(std::vector<SelectItem> items, SelectListOptions options = {});
    SelectList(SelectList&&) noexcept;
    SelectList& operator=(SelectList&&) noexcept;
    ~SelectList() override;

    SelectList(const SelectList&) = delete;
    SelectList& operator=(const SelectList&) = delete;

    /// Apply the legacy external filter: a case-insensitive prefix match on
    /// `value`, resetting the selection to the first match. With
    /// `enable_search`, the filter text instead becomes the search query and
    /// ranking proceeds as for typed input (top-ranked selection, no
    /// selection-change notification).
    void set_filter(std::string filter);
    /// Replace the item set. The current query (or legacy filter) is
    /// re-applied and the selection is preserved by item `value` when it is
    /// still present in the filtered set, otherwise clamped to the previous
    /// filtered position.
    void set_items(std::vector<SelectItem> items);
    void set_selected_index(std::size_t index);
    [[nodiscard]] std::optional<SelectItem> selected_item() const;
    /// The current search query; empty when search is disabled (or cleared).
    [[nodiscard]] std::string search_query() const;

    [[nodiscard]] support::Expected<RenderResult> render(std::size_t width) override;
    void invalidate() override;
    void handle_input(const InputEventVariant& input) override;
    [[nodiscard]] bool accepts_key_releases() const override;
    void set_focused(bool focused) override;
    [[nodiscard]] bool focused() const override;
    /// The search input's cursor in this component's own line coordinates:
    /// the exact row of the rendered search line (borders, title, hints and
    /// blank spacers above it) and the Input's column. Reported only when
    /// focused, search is enabled, and the component has rendered the search
    /// line at least once; `std::nullopt` otherwise (search disabled never
    /// reports a cursor).
    [[nodiscard]] std::optional<CursorPosition> cursor_location() const override;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace cch::tui
