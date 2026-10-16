#include <cch/tui/SettingsList.hpp>

#include <cch/tui/Fuzzy.hpp>
#include <cch/tui/Input.hpp>
#include <cch/tui/Utils.hpp>

#include "tui/InteractionUtils.hpp"
#include "tui/UnicodeWidth.hpp"

#include <cch/support/Error.hpp>
#include <algorithm>
#include <array>
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

// Behavioral baseline: pi 83114817 packages/tui/src/components/settings-list.ts.
// Search input flows through the single-line Input component (editing
// behaviors and paste cleaning apply) and ranking through the public Fuzzy
// module; space confirms only while the search is empty.
namespace {

/// Selection dispatch order (pi settings-list.ts). The first bound action in
/// table order wins; the raw space fallback stays OR-ed at confirm's position.
constexpr std::array<std::string_view, 4> kSettingsListActions = {
        "tui.select.up",
        "tui.select.down",
        "tui.select.confirm",
        "tui.select.cancel",
};

/// The prefix pi gives every row the cursor is not on.
[[nodiscard]] const std::string& unselected_prefix() {
    static const std::string prefix = "  ";
    return prefix;
}

} // namespace

struct SettingsList::Impl : public std::enable_shared_from_this<SettingsList::Impl> {
    std::vector<SettingItem> items;
    std::vector<std::size_t> filtered_indices;
    std::size_t selected_index{0};
    std::size_t max_visible{5};
    bool search_enabled{false};
    std::unique_ptr<Input> search_input;
    SettingsListTheme theme;
    std::shared_ptr<SettingsChangeSink> on_change;
    std::shared_ptr<SettingsCancelSink> on_cancel;
    SettingsSubmenuFactoryHook submenu_factory;
    std::shared_ptr<const KeybindingRegistry> keybindings;
    std::shared_ptr<KeybindingsContext> keybinding_context;
    std::unique_ptr<Component> submenu;
    std::optional<std::size_t> submenu_item_index;
    /// Row selected by the last left press, used by the renderer's synthesized
    /// click (pi `mousePressedIndex`).
    std::optional<std::size_t> mouse_pressed_index;
    std::optional<std::string> navigate_after_close;
    std::optional<support::Error> callback_error;
    bool focused{false};

    [[nodiscard]] const std::vector<std::size_t>& displayed_indices() const {
        return search_enabled ? filtered_indices : all_indices();
    }

    [[nodiscard]] const std::vector<std::size_t>& all_indices() const {
        return all_indices_cache;
    }

    std::vector<std::size_t> all_indices_cache;

    [[nodiscard]] SettingItem* selected() {
        const auto& displayed = displayed_indices();
        if (selected_index >= displayed.size()) return nullptr;
        return &items[displayed[selected_index]];
    }

    [[nodiscard]] const SettingItem* selected() const {
        const auto& displayed = displayed_indices();
        if (selected_index >= displayed.size()) return nullptr;
        return &items[displayed[selected_index]];
    }

    void report_callback_failure(std::string message) {
        callback_error = support::make_error(
            support::ErrorCode::Unknown,
            std::move(message),
            "the interaction callback threw an exception");
    }

    /// The current search text; empty when search is disabled (the only state
    /// in which `search_input` is null).
    [[nodiscard]] std::string search_text() const {
        return search_input ? search_input->value() : std::string{};
    }

    void apply_filter() {
        std::vector<std::size_t> indices(items.size());
        std::iota(indices.begin(), indices.end(), 0);
        filtered_indices = fuzzy_filter(
            std::move(indices),
            search_text(),
            [&](std::size_t index) -> std::string { return items[index].label; });
        selected_index = 0;
    }

    void emit_change(const SettingItem& item) {
        auto sink = on_change;
        if (!sink || !*sink) return;
        if (auto changed = (*sink)(item.id, item.current_value); !changed) {
            callback_error = std::move(changed.error());
        }
    }

    void close_submenu() {
        if (submenu) {
            if (auto* focusable = dynamic_cast<Focusable*>(submenu.get())) focusable->set_focused(false);
        }
        submenu.reset();
        if (navigate_after_close) {
            auto id = std::move(*navigate_after_close);
            navigate_after_close.reset();
            submenu_item_index.reset();
            select_item(id);
            activate_item();
            return;
        }
        if (submenu_item_index) {
            const auto parent_selection = *submenu_item_index;
            submenu_item_index.reset();
            const auto& displayed = displayed_indices();
            selected_index = displayed.empty() ? 0 : std::min(parent_selection, displayed.size() - 1);
        }
    }

    void select_item(std::string_view id) {
        const auto& displayed = displayed_indices();
        for (std::size_t index = 0; index < displayed.size(); ++index) {
            if (items[displayed[index]].id == id) {
                selected_index = index;
                return;
            }
        }
    }

    void activate_item() {
        auto* item = selected();
        if (item == nullptr) return;
        if (std::holds_alternative<SettingSubmenu>(item->control)) {
            if (!submenu_factory) {
                callback_error = support::make_error(
                    support::ErrorCode::Validation,
                    "TUI SettingsList submenu item has no submenu factory");
                return;
            }
            const auto item_index = static_cast<std::size_t>(item - items.data());
            const auto parent_selection = selected_index;
            submenu_item_index = parent_selection;
            auto finished = std::make_shared<bool>(false);
            std::weak_ptr<Impl> weak = shared_from_this();
            SettingsSubmenuDoneSink done =
                    [weak, finished, item_index](std::optional<std::string> selected_value,
                            std::optional<SettingsSubmenuOptions> submenu_options) -> support::ExpectedVoid {
                if (*finished) return {};
                *finished = true;
                auto impl = weak.lock();
                if (!impl || item_index >= impl->items.size()) return {};
                if (selected_value) {
                    impl->items[item_index].current_value = std::move(*selected_value);
                    impl->emit_change(impl->items[item_index]);
                }
                if (submenu_options && submenu_options->navigate_to && !submenu_options->navigate_to->empty()) {
                    impl->navigate_after_close = std::move(submenu_options->navigate_to);
                } else {
                    impl->navigate_after_close.reset();
                }
                impl->close_submenu();
                return {};
            };
            std::unique_ptr<Component> created;
            created = submenu_factory(*item, std::move(done));
            if (*finished) return;
            if (!created) {
                callback_error = support::make_error(
                    support::ErrorCode::Validation,
                    "TUI SettingsList submenu factory returned null");
                return;
            }
            submenu = std::move(created);
            if (focused) {
                if (auto* focusable = dynamic_cast<Focusable*>(submenu.get())) focusable->set_focused(true);
            }
            return;
        }
        auto* values = std::get_if<SettingValues>(&item->control);
        if (values == nullptr || values->values.empty()) return;
        const auto found = std::find(values->values.begin(), values->values.end(), item->current_value);
        const auto next = found == values->values.end()
                              ? values->values.begin()
                              : std::next(found) == values->values.end() ? values->values.begin() : std::next(found);
        item->current_value = *next;
        emit_change(*item);
    }

    [[nodiscard]] support::Expected<std::string> hint_line(std::string text, std::size_t width) {
        auto bounded = truncate_text(text, width);
        if (!bounded) return std::unexpected(bounded.error());
        return detail::apply_text_style(theme.hint, std::move(*bounded), "SettingsList hint");
    }
};

SettingsList::SettingsList(std::vector<SettingItem> items, SettingsListOptions options)
    : impl_(std::make_shared<Impl>()) {
    impl_->items = std::move(items);
    impl_->all_indices_cache.reserve(impl_->items.size());
    for (std::size_t index = 0; index < impl_->items.size(); ++index) {
        impl_->all_indices_cache.push_back(index);
    }
    impl_->filtered_indices = impl_->all_indices_cache;
    impl_->max_visible = std::max<std::size_t>(1, options.max_visible);
    impl_->search_enabled = options.enable_search;
    impl_->theme = std::move(options.theme);
    impl_->on_change = std::make_shared<SettingsChangeSink>(std::move(options.on_change));
    impl_->on_cancel = std::make_shared<SettingsCancelSink>(std::move(options.on_cancel));
    impl_->submenu_factory = std::move(options.submenu_factory);
    impl_->keybinding_context = std::move(options.keybinding_context);
    impl_->keybindings = impl_->keybinding_context
                                 ? impl_->keybinding_context->snapshot()
                                 : (options.keybindings ? std::move(options.keybindings) : default_tui_keybindings());
    if (impl_->search_enabled) {
        impl_->search_input = std::make_unique<Input>(InputOptions{
                .keybindings = impl_->keybindings,
                .keybinding_context = impl_->keybinding_context,
        });
    }
}

SettingsList::SettingsList(SettingsList&&) noexcept = default;
SettingsList& SettingsList::operator=(SettingsList&&) noexcept = default;
SettingsList::~SettingsList() = default;

void SettingsList::update_value(std::string id, std::string new_value) {
    auto impl = impl_;
    const auto found = std::find_if(impl->items.begin(), impl->items.end(), [&](const auto& item) {
        return item.id == id;
    });
    if (found != impl->items.end()) found->current_value = std::move(new_value);
}

void SettingsList::select_item(std::string_view id) { impl_->select_item(id); }

std::optional<SettingItem> SettingsList::selected_item() const {
    const auto* item = impl_->selected();
    return item == nullptr ? std::nullopt : std::optional<SettingItem>(*item);
}

std::string SettingsList::search_query() const {
    return impl_->search_text();
}

bool SettingsList::submenu_open() const {
    return static_cast<bool>(impl_->submenu);
}

support::Expected<RenderResult> SettingsList::render(std::size_t width) {
    auto impl = impl_;
    if (width == 0) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "TUI SettingsList requires a positive visible width"));
    }
    if (impl->callback_error) return std::unexpected(*impl->callback_error);
    if (impl->submenu) return impl->submenu->render(width);

    std::vector<std::string> lines;
    if (impl->search_enabled) {
        auto search_line = impl->search_input->render(width);
        if (!search_line) return std::unexpected(search_line.error());
        // The Input component renders exactly one line (its documented
        // single-line contract).
        lines.push_back(std::move(search_line->lines[0]));
        lines.emplace_back();
    }

    const auto& displayed = impl->displayed_indices();
    if (impl->items.empty()) {
        auto message = impl->hint_line("  No settings available", width);
        if (!message) return std::unexpected(message.error());
        lines.push_back(std::move(*message));
    } else if (displayed.empty()) {
        auto message = impl->hint_line("  No matching settings", width);
        if (!message) return std::unexpected(message.error());
        lines.push_back(std::move(*message));
    } else {
        const auto range = detail::centered_visible_range(displayed.size(), impl->selected_index, impl->max_visible);
        std::size_t max_label_width = 0;
        for (const auto& item : impl->items) {
            max_label_width = std::max(max_label_width, visible_width(item.label));
        }
        // pi `settings-list.ts`: `Math.min(36, max(label widths))`. The cap
        // is the value column's budget, so it is pi's number, not a local one.
        max_label_width = std::min<std::size_t>(36, max_label_width);
        for (std::size_t index = range.begin; index < range.end; ++index) {
            const auto& item = impl->items[displayed[index]];
            const auto selected = index == impl->selected_index;
            // pi `settings-list.ts`: the selected row's prefix is the theme
            // cursor and every other row's is two spaces. Neither is bounded
            // on its own; the row's final truncation is the only bound pi
            // applies.
            const std::string& prefix = selected ? impl->theme.cursor : unselected_prefix();
            // pi pads the label to the value column instead of truncating
            // it, so a label wider than the column keeps its text.
            const auto label_width = visible_width(item.label);
            const std::string label = label_width < max_label_width
                                              ? item.label + std::string(max_label_width - label_width, ' ')
                                              : item.label;
            auto styled_label = detail::apply_selection_style(impl->theme.label, label, selected, "SettingsList label");
            if (!styled_label) return std::unexpected(styled_label.error());
            // pi's value column starts after the prefix, the padded label,
            // and the two-column separator, and the value is truncated to
            // what remains two columns short of the row's right edge.
            const auto value_column = visible_width(prefix) + max_label_width + 2;
            const auto value_width = width > value_column + 2 ? width - value_column - 2 : 0;
            auto value = truncate_text(item.current_value, value_width, "");
            if (!value) return std::unexpected(value.error());
            auto styled_value =
                    detail::apply_selection_style(impl->theme.value, std::move(*value), selected, "SettingsList value");
            if (!styled_value) return std::unexpected(styled_value.error());
            auto bounded = truncate_text(prefix + *styled_label + "  " + *styled_value, width);
            if (!bounded) return std::unexpected(bounded.error());
            lines.push_back(std::move(*bounded));
        }
        if (range.begin > 0 || range.end < displayed.size()) {
            // pi truncates the scroll indicator to `width - 2` with the theme
            // hint's already-truncated text and the same empty ellipsis.
            auto scroll_text = truncate_text(std::format("  ({}/{})", impl->selected_index + 1, displayed.size()),
                    width > 2 ? width - 2 : 1,
                    "");
            if (!scroll_text) return std::unexpected(scroll_text.error());
            auto styled_scroll =
                    detail::apply_text_style(impl->theme.hint, std::move(*scroll_text), "SettingsList scroll");
            if (!styled_scroll) return std::unexpected(styled_scroll.error());
            lines.push_back(std::move(*styled_scroll));
        }
        const auto* selected = impl->selected();
        if (selected != nullptr && selected->description) {
            lines.emplace_back();
            const auto description_width = width > 4 ? width - 4 : 1;
            auto wrapped = wrap_text(*selected->description, description_width);
            if (!wrapped) return std::unexpected(wrapped.error());
            for (const auto& line : *wrapped) {
                auto styled = detail::apply_text_style(
                    impl->theme.description,
                    "  " + line,
                    "SettingsList description");
                if (!styled) return std::unexpected(styled.error());
                auto bounded = truncate_text(*styled, width, "");
                if (!bounded) return std::unexpected(bounded.error());
                lines.push_back(std::move(*bounded));
            }
        }
    }

    if (!impl->items.empty() || impl->search_enabled) {
        lines.emplace_back();
        const auto keybindings = impl->keybinding_context ? impl->keybinding_context->snapshot() : impl->keybindings;
        auto confirm_keys = keybindings->key_text("tui.select.confirm");
        const KeyEvent space{.key = "space"};
        if (!keybindings->matches(space, "tui.select.confirm")) {
            if (!confirm_keys.empty()) confirm_keys += '/';
            confirm_keys += "space";
        }
        const auto cancel_keys = keybindings->key_text("tui.select.cancel");
        const auto actions = std::format(
            "{} to change · {} to cancel",
            confirm_keys,
            cancel_keys.empty() ? "Unbound" : cancel_keys);
        auto hint = impl->hint_line(
            impl->search_enabled ? "  Type to search · " + actions : "  " + actions,
            width);
        if (!hint) return std::unexpected(hint.error());
        lines.push_back(std::move(*hint));
    }
    return RenderResult{.lines = std::move(lines)};
}

void SettingsList::invalidate() {
    if (impl_->submenu) impl_->submenu->invalidate();
}

InputAdmissionOutcome SettingsList::handle_input(const InputEventVariant& input) {
    auto impl = impl_;
    if (impl->submenu) {
        if (auto* handler = dynamic_cast<InputHandler*>(impl->submenu.get())) {
            return handler->handle_input(input);
        }
        return InputAdmissionOutcome::Unhandled;
    }
    if (std::holds_alternative<PasteEvent>(input)) {
        if (!impl->search_enabled || !impl->search_input) return InputAdmissionOutcome::Unhandled;
        const auto outcome = impl->search_input->handle_input(input);
        impl->apply_filter();
        return outcome;
    }
    const auto* key = std::get_if<KeyEvent>(&input);
    if (!carries_press_behavior(key)) return InputAdmissionOutcome::Unhandled;
    const auto& displayed = impl->displayed_indices();
    const auto keybindings = impl->keybinding_context ? impl->keybinding_context->snapshot() : impl->keybindings;
    const auto action = keybindings->first_match(*key, kSettingsListActions);
    if (action == "tui.select.up") {
        if (displayed.empty()) return InputAdmissionOutcome::Consumed;
        impl->selected_index = impl->selected_index == 0 ? displayed.size() - 1 : impl->selected_index - 1;
        return InputAdmissionOutcome::Consumed;
    }
    if (action == "tui.select.down") {
        if (displayed.empty()) return InputAdmissionOutcome::Consumed;
        impl->selected_index = impl->selected_index + 1 == displayed.size() ? 0 : impl->selected_index + 1;
        return InputAdmissionOutcome::Consumed;
    }
    const auto search_empty = !impl->search_input || impl->search_input->value().empty();
    if (action == "tui.select.confirm" || (matches_key(*key, "space") && (!impl->search_enabled || search_empty))) {
        impl->activate_item();
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
    if (!impl->search_enabled || !impl->search_input) return InputAdmissionOutcome::Unhandled;
    // Remaining keys are the shared Input component's editing behaviors; the
    // filter re-runs on the component's current value (pi settings-list.ts).
    const auto outcome = impl->search_input->handle_input(input);
    impl->apply_filter();
    return outcome;
}

std::optional<MouseDispatchResult> SettingsList::handle_mouse_forwarded(const MouseEvent& event) {
    // pi settings-list.ts:180-184: while a submenu is open it owns every mouse
    // event. The child's resolved result is forwarded unchanged so the child
    // keeps the drag/release target, and the dispatcher then keeps this list as
    // the focus target because it routes keys to that child itself.
    auto impl = impl_;
    if (!impl->submenu) return std::nullopt;
    auto forwarded = dispatch_mouse_event(*impl->submenu, event);
    if (!forwarded) return std::nullopt;
    forwarded->focus = true;
    return forwarded;
}

std::optional<MouseEventResult> SettingsList::handle_mouse(const MouseEvent& event) {
    // pi settings-list.ts:179-220. The search row is answered with a plain
    // result spread with `focus: true`, so the settings list stays the target
    // even though the search Input positions its own cursor.
    auto impl = impl_;
    // An open submenu already answered (or deliberately ignored) the event in
    // `handle_mouse_forwarded`; the settings rows must not fall through to it.
    if (impl->submenu) return std::nullopt;
    if (impl->search_enabled && impl->search_input) {
        if (event.y == 0) {
            auto result = impl->search_input->handle_mouse(event);
            if (!result) return std::nullopt;
            result->focus = true;
            return result;
        }
        if (event.y == 1) return std::nullopt;
    }
    const auto& displayed = impl->displayed_indices();
    if (displayed.empty()) return std::nullopt;
    if (event.type == MouseEventType::Wheel && event.wheel_delta && *event.wheel_delta != 0) {
        const auto delta = *event.wheel_delta < 0 ? std::ptrdiff_t{-1} : std::ptrdiff_t{1};
        const auto previous = impl->selected_index;
        impl->selected_index = static_cast<std::size_t>(std::clamp<std::ptrdiff_t>(
                static_cast<std::ptrdiff_t>(impl->selected_index) + delta,
                0,
                static_cast<std::ptrdiff_t>(displayed.size() - 1)));
        return MouseEventResult{.handled = true, .render = impl->selected_index != previous};
    }
    // Hover must not change selection: the visible range is centered on it.
    if (event.button != MouseButton::Left ||
            (event.type != MouseEventType::Press && event.type != MouseEventType::Click)) {
        return std::nullopt;
    }
    // With search enabled the search row and its blank separator row sit above
    // the first item row (pi `rowOffset`).
    const auto row_offset = impl->search_enabled ? std::ptrdiff_t{2} : std::ptrdiff_t{0};
    const auto range = detail::centered_visible_range(displayed.size(), impl->selected_index, impl->max_visible);
    const auto item_index = static_cast<std::ptrdiff_t>(range.begin) + event.y - row_offset;
    if (item_index < static_cast<std::ptrdiff_t>(range.begin) ||
            item_index >= static_cast<std::ptrdiff_t>(range.end)) {
        return std::nullopt;
    }
    const auto index = static_cast<std::size_t>(item_index);
    if (event.type == MouseEventType::Press) {
        impl->mouse_pressed_index = index;
        impl->selected_index = index;
        return MouseEventResult{.handled = true, .focus = true};
    }
    impl->selected_index = impl->mouse_pressed_index.value_or(index);
    impl->mouse_pressed_index.reset();
    impl->activate_item();
    return MouseEventResult{.handled = true};
}

void SettingsList::set_focused(bool focused) {
    auto impl = impl_;
    impl->focused = focused;
    if (impl->search_input) impl->search_input->set_focused(focused);
    if (impl->submenu) {
        if (auto* focusable = dynamic_cast<Focusable*>(impl->submenu.get())) focusable->set_focused(focused);
    }
}

bool SettingsList::focused() const {
    return impl_->focused;
}

std::optional<CursorPosition> SettingsList::cursor_location() const {
    if (!impl_->focused || !impl_->search_enabled || impl_->submenu) return std::nullopt;
    return impl_->search_input ? impl_->search_input->cursor_location() : std::nullopt;
}

} // namespace cch::tui
