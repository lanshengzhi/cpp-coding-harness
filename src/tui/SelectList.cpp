#include <cch/tui/SelectList.hpp>

#include <cch/tui/Utils.hpp>

#include "tui/CompletionContext.hpp"
#include "tui/FuzzyInternal.hpp"
#include "tui/InteractionUtils.hpp"
#include "tui/UnicodeWidth.hpp"

#include <cch/support/Error.hpp>
#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <memory>
#include <ranges>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::tui {
namespace {

// Behavioral baseline: pi v1.0.4 at 7c10bd4337495ee613f2224843ecdf349b80d1df,
// packages/tui/src/components/select-list.ts.
constexpr std::size_t kDefaultPrimaryColumnWidth = 32;
constexpr std::size_t kPrimaryColumnGap = 2;
constexpr std::size_t kMinDescriptionWidth = 10;

constexpr std::array<std::string_view, 4> kSelectListActions = {
        "tui.select.up",
        "tui.select.down",
        "tui.select.confirm",
        "tui.select.cancel",
};

[[nodiscard]] std::string single_line(std::string text) {
    std::string result;
    result.reserve(text.size());
    for (std::size_t index = 0; index < text.size();) {
        if (text[index] == '\r' || text[index] == '\n') {
            result.push_back(' ');
            do {
                ++index;
            } while (index < text.size() && (text[index] == '\r' || text[index] == '\n'));
            continue;
        }
        const auto length = detail::decode_utf8(text, index).second;
        result.append(text, index, length);
        index += length;
    }
    return std::string(detail::trim_completion_whitespace(result));
}

[[nodiscard]] std::string_view display_value(const SelectItem& item) {
    return item.label.empty() ? std::string_view(item.value) : std::string_view(item.label);
}

} // namespace

std::string select_item_search_text(const SelectItem& item) {
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

struct SelectList::Impl {
    std::vector<SelectItem> source_items;
    std::vector<SelectItem> items;
    std::string filter;
    std::size_t selected_index{0};
    std::size_t max_visible{5};
    /// Row selected by the last left press, used by the renderer's synthesized
    /// click so a press-drag-release never activates the release row (pi
    /// `mousePressedIndex`).
    std::optional<std::size_t> mouse_pressed_index;
    SelectListTheme theme;
    SelectListLayoutOptions layout;
    std::shared_ptr<SelectItemSink> on_select;
    std::shared_ptr<SelectCancelSink> on_cancel;
    std::shared_ptr<SelectItemSink> on_selection_change;
    std::shared_ptr<const KeybindingRegistry> keybindings;
    std::shared_ptr<KeybindingsContext> keybinding_context;
    std::optional<support::Error> callback_error;
    bool focused{false};

    [[nodiscard]] const SelectItem* selected() const {
        if (selected_index >= items.size()) return nullptr;
        return &items[selected_index];
    }

    void notify_selection_change() {
        const auto* item = selected();
        auto sink = on_selection_change;
        if (item == nullptr || !sink || !*sink) return;
        const auto item_snapshot = *item;
        if (auto observed = (*sink)(item_snapshot); !observed) {
            callback_error = std::move(observed.error());
        }
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
        for (const auto& item : items) {
            widest = std::max(widest, visible_width(display_value(item)) + kPrimaryColumnGap);
        }
        return std::clamp(widest, minimum, maximum);
    }

    [[nodiscard]] support::Expected<std::string> truncate_primary(
            const SelectItem& item, bool is_selected, std::size_t max_width, std::size_t column_width) {
        const auto text = std::string(display_value(item));
        std::string transformed = text;
        if (layout.truncate_primary) {
            transformed = layout.truncate_primary(text, max_width, column_width, item, is_selected);
        }
        return truncate_text(transformed, max_width, "");
    }

    [[nodiscard]] support::Expected<std::string> render_item(
            const SelectItem& item, bool is_selected, std::size_t width, std::size_t primary_width) {
        const std::string prefix = is_selected ? "→ " : "  ";
        constexpr std::size_t prefix_width = 2;
        std::string line;

        const auto description = item.description ? single_line(*item.description) : std::string{};
        if (!description.empty() && width > 40) {
            const auto effective_primary = std::max<std::size_t>(
                    1, std::min(primary_width, width > prefix_width + 4 ? width - prefix_width - 4 : 1));
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
                            theme.description, spacing + *truncated_description, "SelectList description");
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
    impl_->source_items = items;
    impl_->items = std::move(items);
    impl_->max_visible = std::max<std::size_t>(1, options.max_visible);
    impl_->theme = std::move(options.theme);
    impl_->layout = std::move(options.layout);
    impl_->on_select = std::make_shared<SelectItemSink>(std::move(options.on_select));
    impl_->on_cancel = std::make_shared<SelectCancelSink>(std::move(options.on_cancel));
    impl_->on_selection_change = std::make_shared<SelectItemSink>(std::move(options.on_selection_change));
    impl_->keybinding_context = std::move(options.keybinding_context);
    impl_->keybindings = impl_->keybinding_context
                                 ? impl_->keybinding_context->snapshot()
                                 : (options.keybindings ? std::move(options.keybindings) : default_tui_keybindings());
}

SelectList::SelectList(SelectList&&) noexcept = default;
SelectList& SelectList::operator=(SelectList&&) noexcept = default;
SelectList::~SelectList() = default;

void SelectList::set_items(std::vector<SelectItem> items) {
    auto impl = impl_;
    const auto prior_index = impl->selected_index;
    const auto prior_value =
            impl->selected() == nullptr ? std::nullopt : std::optional<std::string>{impl->selected()->value};
    impl->source_items = std::move(items);
    auto replacement = impl->source_items;
    const auto prefix = detail::lowercase_text(impl->filter);
    if (!prefix.empty()) {
        std::erase_if(replacement,
                [&prefix](const SelectItem& item) { return !detail::lowercase_text(item.value).starts_with(prefix); });
    }
    impl->items = std::move(replacement);
    impl->selected_index = 0;
    if (impl->items.empty()) return;
    if (prior_value) {
        const auto found = std::ranges::find(impl->items, *prior_value, &SelectItem::value);
        if (found != impl->items.end()) {
            impl->selected_index = static_cast<std::size_t>(found - impl->items.begin());
            return;
        }
    }
    impl->selected_index = std::min(prior_index, impl->items.size() - 1);
}

void SelectList::set_filter(std::string filter) {
    auto impl = impl_;
    const auto prefix = detail::lowercase_text(filter);
    auto replacement = impl->source_items;
    if (!prefix.empty()) {
        std::erase_if(replacement,
                [&prefix](const SelectItem& item) { return !detail::lowercase_text(item.value).starts_with(prefix); });
    }
    impl->filter = std::move(filter);
    impl->items = std::move(replacement);
    impl->selected_index = 0;
}

std::vector<SelectItem> SelectList::filtered_items() const { return impl_->items; }

void SelectList::set_selected_index(std::size_t index) {
    auto impl = impl_;
    impl->selected_index = impl->items.empty() ? 0 : std::min(index, impl->items.size() - 1);
}

std::optional<SelectItem> SelectList::selected_item() const {
    const auto* item = impl_->selected();
    return item == nullptr ? std::nullopt : std::optional<SelectItem>(*item);
}

support::Expected<RenderResult> SelectList::render(std::size_t width) {
    auto impl = impl_;
    if (width == 0) {
        return std::unexpected(support::make_error(
                support::ErrorCode::Validation, "TUI SelectList requires a positive visible width"));
    }
    if (impl->callback_error) return std::unexpected(*impl->callback_error);

    std::vector<std::string> lines;
    if (impl->items.empty()) {
        auto styled = detail::apply_text_style(impl->theme.no_match, "  No matching commands", "SelectList no match");
        if (!styled) return std::unexpected(styled.error());
        auto bounded = truncate_text(std::move(*styled), width, "");
        if (!bounded) return std::unexpected(bounded.error());
        lines.push_back(std::move(*bounded));
        return RenderResult{.lines = std::move(lines)};
    }

    const auto start_index = std::max<std::size_t>(0,
            std::min(impl->selected_index > impl->max_visible / 2 ? impl->selected_index - impl->max_visible / 2 : 0,
                    impl->items.size() > impl->max_visible ? impl->items.size() - impl->max_visible : 0));
    const auto end_index = std::min(start_index + impl->max_visible, impl->items.size());
    const auto primary_width = impl->primary_column_width();
    for (std::size_t index = start_index; index < end_index; ++index) {
        auto line = impl->render_item(impl->items[index], index == impl->selected_index, width, primary_width);
        if (!line) return std::unexpected(line.error());
        lines.push_back(std::move(*line));
    }
    if (start_index > 0 || end_index < impl->items.size()) {
        auto text = std::format("  ({}/{})", impl->selected_index + 1, impl->items.size());
        auto bounded = truncate_text(std::move(text), width > 2 ? width - 2 : 0, "");
        if (!bounded) return std::unexpected(bounded.error());
        auto styled = detail::apply_text_style(impl->theme.scroll_info, std::move(*bounded), "SelectList scroll info");
        if (!styled) return std::unexpected(styled.error());
        lines.push_back(std::move(*styled));
    }
    return RenderResult{.lines = std::move(lines)};
}

void SelectList::invalidate() {}

InputAdmissionOutcome SelectList::handle_input(const InputEventVariant& input) {
    auto impl = impl_;
    const auto* key = std::get_if<KeyEvent>(&input);
    if (!carries_press_behavior(key)) return InputAdmissionOutcome::Unhandled;
    const auto count = impl->items.size();
    const auto keybindings = impl->keybinding_context ? impl->keybinding_context->snapshot() : impl->keybindings;
    const auto action = keybindings->first_match(*key, kSelectListActions);
    if (action == "tui.select.up") {
        if (count > 0) {
            impl->selected_index = impl->selected_index == 0 ? count - 1 : impl->selected_index - 1;
            impl->notify_selection_change();
        }
        return InputAdmissionOutcome::Consumed;
    }
    if (action == "tui.select.down") {
        if (count > 0) {
            impl->selected_index = impl->selected_index + 1 == count ? 0 : impl->selected_index + 1;
            impl->notify_selection_change();
        }
        return InputAdmissionOutcome::Consumed;
    }
    if (action == "tui.select.confirm") {
        const auto* item = impl->selected();
        auto sink = impl->on_select;
        if (item == nullptr || !sink || !*sink) return InputAdmissionOutcome::Consumed;
        const auto item_snapshot = *item;
        if (auto selected = (*sink)(item_snapshot); !selected) {
            impl->callback_error = std::move(selected.error());
        }
        return InputAdmissionOutcome::Consumed;
    }
    if (action == "tui.select.cancel") {
        auto sink = impl->on_cancel;
        if (!sink || !*sink) return InputAdmissionOutcome::Consumed;
        if (auto cancelled = (*sink)(); !cancelled) {
            impl->callback_error = std::move(cancelled.error());
        }
        return InputAdmissionOutcome::Consumed;
    }
    return InputAdmissionOutcome::Unhandled;
}

void SelectList::set_focused(bool focused) { impl_->focused = focused; }

std::optional<MouseEventResult> SelectList::handle_mouse(const MouseEvent& event) {
    // pi select-list.ts:109-143. A click is the renderer-synthesized gesture
    // for a press and release on the same cell; the press already moved the
    // selection, so the click only activates what the press selected.
    auto impl = impl_;
    if (impl->items.empty()) return std::nullopt;
    if (event.type == MouseEventType::Wheel && event.wheel_delta && *event.wheel_delta != 0) {
        const auto delta = *event.wheel_delta < 0 ? std::ptrdiff_t{-1} : std::ptrdiff_t{1};
        const auto previous = impl->selected_index;
        impl->selected_index = static_cast<std::size_t>(
                std::clamp<std::ptrdiff_t>(static_cast<std::ptrdiff_t>(impl->selected_index) + delta,
                        0,
                        static_cast<std::ptrdiff_t>(impl->items.size() - 1)));
        const bool changed = impl->selected_index != previous;
        if (changed) impl->notify_selection_change();
        return MouseEventResult{.handled = true, .render = changed};
    }
    // Hover must not change selection: the visible range is centered on it.
    if (event.button != MouseButton::Left ||
            (event.type != MouseEventType::Press && event.type != MouseEventType::Click)) {
        return std::nullopt;
    }
    const auto range = detail::centered_visible_range(impl->items.size(), impl->selected_index, impl->max_visible);
    const auto item_index = static_cast<std::ptrdiff_t>(range.begin) + event.y;
    if (item_index < static_cast<std::ptrdiff_t>(range.begin) || item_index >= static_cast<std::ptrdiff_t>(range.end)) {
        return std::nullopt;
    }
    const auto index = static_cast<std::size_t>(item_index);
    if (event.type == MouseEventType::Press) {
        impl->mouse_pressed_index = index;
        if (impl->selected_index != index) {
            impl->selected_index = index;
            impl->notify_selection_change();
        }
        return MouseEventResult{.handled = true, .focus = true};
    }
    const auto clicked_index = impl->mouse_pressed_index.value_or(index);
    impl->mouse_pressed_index.reset();
    const bool changed = impl->selected_index != clicked_index;
    impl->selected_index = clicked_index;
    if (changed) impl->notify_selection_change();
    const auto* selected = impl->selected();
    auto sink = impl->on_select;
    if (selected != nullptr && sink && *sink) {
        const auto item_snapshot = *selected;
        if (auto activated = (*sink)(item_snapshot); !activated) {
            impl->callback_error = std::move(activated.error());
        }
    }
    return MouseEventResult{.handled = true};
}

bool SelectList::focused() const { return impl_->focused; }

std::optional<CursorPosition> SelectList::cursor_location() const { return std::nullopt; }

} // namespace cch::tui
