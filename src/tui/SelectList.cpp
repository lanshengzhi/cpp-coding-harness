#include <cch/tui/SelectList.hpp>

#include <cch/tui/Utils.hpp>

#include "tui/InteractionUtils.hpp"
#include "tui/UnicodeWidth.hpp"

#include <cch/util/Error.hpp>
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <exception>
#include <format>
#include <memory>
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
    std::optional<util::Error> callback_error;
    bool focused{false};

    [[nodiscard]] const SelectItem* selected() const {
        if (selected_index >= filtered_indices.size()) return nullptr;
        return &items[filtered_indices[selected_index]];
    }

    void report_callback_failure(std::string message) {
        callback_error = util::make_error(
            util::ErrorCode::Unknown,
            std::move(message),
            "the interaction callback threw an exception");
    }

    void notify_selection_change() {
        const auto* item = selected();
        auto sink = on_selection_change;
        if (item == nullptr || !sink || !*sink) return;
        try {
            (*sink)(*item);
        } catch (...) {
            report_callback_failure("TUI SelectList selection callback failed");
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
        for (const auto& index : filtered_indices) {
            widest = std::max(widest, visible_width(display_value(items[index])) + kPrimaryColumnGap);
        }
        return std::clamp(widest, minimum, maximum);
    }

    [[nodiscard]] util::Expected<std::string> truncate_primary(
        const SelectItem& item,
        bool is_selected,
        std::size_t max_width,
        std::size_t column_width) {
        const auto text = std::string(display_value(item));
        std::string transformed = text;
        if (layout.truncate_primary) {
            try {
                transformed = layout.truncate_primary(
                    text,
                    max_width,
                    column_width,
                    item,
                    is_selected);
            } catch (...) {
                return std::unexpected(util::make_error(
                    util::ErrorCode::Unknown,
                    "TUI SelectList truncation hook failed",
                    "the truncation callback threw an exception"));
            }
        }
        return truncate_text(transformed, max_width, "");
    }

    [[nodiscard]] util::Expected<std::string> render_item(
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
    for (std::size_t index = 0; index < impl_->items.size(); ++index) {
        impl_->filtered_indices.push_back(index);
    }
    impl_->max_visible = std::max<std::size_t>(1, options.max_visible);
    impl_->theme = std::move(options.theme);
    impl_->layout = std::move(options.layout);
    impl_->on_select = std::make_shared<SelectItemSink>(std::move(options.on_select));
    impl_->on_cancel = std::make_shared<SelectCancelSink>(std::move(options.on_cancel));
    impl_->on_selection_change = std::make_shared<SelectItemSink>(std::move(options.on_selection_change));
    impl_->keybindings = options.keybindings ? std::move(options.keybindings) : default_tui_keybindings();
}

SelectList::SelectList(SelectList&&) noexcept = default;
SelectList& SelectList::operator=(SelectList&&) noexcept = default;
SelectList::~SelectList() = default;

void SelectList::set_filter(std::string filter) {
    auto impl = impl_;
    impl->filtered_indices.clear();
    for (std::size_t index = 0; index < impl->items.size(); ++index) {
        if (starts_with_case_insensitive(impl->items[index].value, filter)) {
            impl->filtered_indices.push_back(index);
        }
    }
    impl->selected_index = 0;
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

util::Expected<RenderResult> SelectList::render(std::size_t width) {
    auto impl = impl_;
    if (width == 0) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "TUI SelectList requires a positive visible width"));
    }
    if (impl->callback_error) return std::unexpected(*impl->callback_error);
    if (impl->filtered_indices.empty()) {
        auto message = truncate_text("  No matching commands", width, "");
        if (!message) return std::unexpected(message.error());
        auto styled = detail::apply_text_style(impl->theme.no_match, std::move(*message), "SelectList no match");
        if (!styled) return std::unexpected(styled.error());
        return RenderResult{.lines = {std::move(*styled)}};
    }

    const auto range = detail::centered_visible_range(
        impl->filtered_indices.size(),
        impl->selected_index,
        impl->max_visible);
    const auto primary_width = impl->primary_column_width();
    std::vector<std::string> lines;
    for (std::size_t index = range.begin; index < range.end; ++index) {
        auto line = impl->render_item(
            impl->items[impl->filtered_indices[index]],
            index == impl->selected_index,
            width,
            primary_width);
        if (!line) return std::unexpected(line.error());
        lines.push_back(std::move(*line));
    }
    if (range.begin > 0 || range.end < impl->filtered_indices.size()) {
        auto text = std::format("  ({}/{})", impl->selected_index + 1, impl->filtered_indices.size());
        auto bounded = truncate_text(text, width, "");
        if (!bounded) return std::unexpected(bounded.error());
        auto styled = detail::apply_text_style(impl->theme.scroll_info, std::move(*bounded), "SelectList scroll info");
        if (!styled) return std::unexpected(styled.error());
        lines.push_back(std::move(*styled));
    }
    return RenderResult{.lines = std::move(lines)};
}

void SelectList::invalidate() {}

void SelectList::handle_input(const InputEventVariant& input) {
    const auto* key = std::get_if<KeyEvent>(&input);
    if (key == nullptr || key->type == KeyEventType::Release) return;
    auto impl = impl_;
    const auto count = impl->filtered_indices.size();
    if (impl->keybindings->matches(*key, "tui.select.up")) {
        if (count == 0) return;
        impl->selected_index = impl->selected_index == 0 ? count - 1 : impl->selected_index - 1;
        impl->notify_selection_change();
        return;
    }
    if (impl->keybindings->matches(*key, "tui.select.down")) {
        if (count == 0) return;
        impl->selected_index = impl->selected_index + 1 == count ? 0 : impl->selected_index + 1;
        impl->notify_selection_change();
        return;
    }
    if (impl->keybindings->matches(*key, "tui.select.pageUp") ||
        impl->keybindings->matches(*key, "tui.select.pageDown")) {
        if (count == 0) return;
        const auto page = impl->max_visible;
        if (impl->keybindings->matches(*key, "tui.select.pageUp")) {
            impl->selected_index = impl->selected_index > page ? impl->selected_index - page : 0;
        } else {
            impl->selected_index = std::min(impl->selected_index + page, count - 1);
        }
        impl->notify_selection_change();
        return;
    }
    if (impl->keybindings->matches(*key, "tui.select.confirm")) {
        const auto* item = impl->selected();
        auto sink = impl->on_select;
        if (item == nullptr || !sink || !*sink) return;
        try {
            (*sink)(*item);
        } catch (...) {
            impl->report_callback_failure("TUI SelectList select callback failed");
        }
        return;
    }
    if (impl->keybindings->matches(*key, "tui.select.cancel")) {
        auto sink = impl->on_cancel;
        if (!sink || !*sink) return;
        try {
            (*sink)();
        } catch (...) {
            impl->report_callback_failure("TUI SelectList cancel callback failed");
        }
    }
}

bool SelectList::accepts_key_releases() const {
    return false;
}

void SelectList::set_focused(bool focused) {
    impl_->focused = focused;
}

bool SelectList::focused() const {
    return impl_->focused;
}

} // namespace cch::tui
