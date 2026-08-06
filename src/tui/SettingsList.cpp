#include <cch/tui/SettingsList.hpp>

#include <cch/tui/Fuzzy.hpp>
#include <cch/tui/Utils.hpp>

#include "tui/InteractionUtils.hpp"
#include "tui/UnicodeWidth.hpp"

#include <algorithm>
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

// Behavioral baseline: pi 864b35c settings-list.ts.
[[nodiscard]] bool is_search_text(const KeyEvent& event) {
    if (event.ctrl || event.alt || event.key.empty()) return false;
    return event.key != "enter" && event.key != "tab" && event.key != "escape" &&
        event.key != "backspace" && event.key != "delete" && event.key != "insert" &&
        event.key != "clear" && event.key != "home" && event.key != "end" &&
        event.key != "pageUp" && event.key != "pageDown" && event.key != "up" &&
        event.key != "down" && event.key != "left" && event.key != "right" &&
        event.key != "space";
}

[[nodiscard]] std::string without_spaces(std::string text) {
    std::erase_if(text, [](char value) {
        return value == ' ' || value == '\r' || value == '\n' || value == '\t';
    });
    return text;
}

} // namespace

struct SettingsList::Impl : public std::enable_shared_from_this<SettingsList::Impl> {
    std::vector<SettingItem> items;
    std::vector<std::size_t> filtered_indices;
    std::size_t selected_index{0};
    std::size_t max_visible{5};
    bool search_enabled{false};
    std::string search;
    SettingsListTheme theme;
    std::shared_ptr<SettingsChangeSink> on_change;
    std::shared_ptr<SettingsCancelSink> on_cancel;
    SettingsSubmenuFactoryHook submenu_factory;
    std::shared_ptr<const KeybindingRegistry> keybindings;
    std::unique_ptr<Component> submenu;
    std::optional<util::Error> callback_error;
    bool focused{false};
    std::size_t render_width{80};

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
        callback_error = util::make_error(
            util::ErrorCode::Unknown,
            std::move(message),
            "the interaction callback threw an exception");
    }

    void apply_filter() {
        std::vector<std::size_t> indices(items.size());
        std::iota(indices.begin(), indices.end(), 0);
        filtered_indices = fuzzy_filter(
            std::move(indices),
            search,
            [&](std::size_t index) -> std::string { return items[index].label; });
        selected_index = 0;
    }

    void emit_change(const SettingItem& item) {
        auto sink = on_change;
        if (!sink || !*sink) return;
        try {
            (*sink)(item.id, item.current_value);
        } catch (...) {
            report_callback_failure("TUI SettingsList change callback failed");
        }
    }

    void close_submenu(std::size_t parent_selection) {
        if (submenu) {
            if (auto* focusable = dynamic_cast<Focusable*>(submenu.get())) focusable->set_focused(false);
        }
        submenu.reset();
        const auto& displayed = displayed_indices();
        selected_index = displayed.empty() ? 0 : std::min(parent_selection, displayed.size() - 1);
    }

    void activate_item() {
        auto* item = selected();
        if (item == nullptr) return;
        if (std::holds_alternative<SettingSubmenu>(item->control)) {
            if (!submenu_factory) {
                callback_error = util::make_error(
                    util::ErrorCode::Validation,
                    "TUI SettingsList submenu item has no submenu factory");
                return;
            }
            const auto item_index = static_cast<std::size_t>(item - items.data());
            const auto parent_selection = selected_index;
            auto finished = std::make_shared<bool>(false);
            std::weak_ptr<Impl> weak = shared_from_this();
            SettingsSubmenuDoneSink done = [weak, finished, item_index, parent_selection](
                                           std::optional<std::string> selected_value) {
                if (*finished) return;
                *finished = true;
                auto impl = weak.lock();
                if (!impl || item_index >= impl->items.size()) return;
                if (selected_value) {
                    impl->items[item_index].current_value = std::move(*selected_value);
                    impl->emit_change(impl->items[item_index]);
                }
                impl->close_submenu(parent_selection);
            };
            std::unique_ptr<Component> created;
            try {
                created = submenu_factory(*item, std::move(done));
            } catch (...) {
                report_callback_failure("TUI SettingsList submenu factory failed");
                return;
            }
            if (*finished) return;
            if (!created) {
                callback_error = util::make_error(
                    util::ErrorCode::Validation,
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

    [[nodiscard]] util::Expected<std::string> hint_line(std::string text, std::size_t width) {
        auto bounded = truncate_text(text, width, "");
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
    impl_->keybindings = options.keybindings ? std::move(options.keybindings) : default_tui_keybindings();
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

std::optional<SettingItem> SettingsList::selected_item() const {
    const auto* item = impl_->selected();
    return item == nullptr ? std::nullopt : std::optional<SettingItem>(*item);
}

std::string SettingsList::search_query() const {
    return impl_->search;
}

bool SettingsList::submenu_open() const {
    return static_cast<bool>(impl_->submenu);
}

util::Expected<RenderResult> SettingsList::render(std::size_t width) {
    auto impl = impl_;
    if (width == 0) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "TUI SettingsList requires a positive visible width"));
    }
    impl->render_width = width;
    if (impl->callback_error) return std::unexpected(*impl->callback_error);
    if (impl->submenu) return impl->submenu->render(width);

    std::vector<std::string> lines;
    if (impl->search_enabled) {
        auto query = truncate_text("> " + impl->search, width, "");
        if (!query) return std::unexpected(query.error());
        lines.push_back(std::move(*query));
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
        max_label_width = std::min<std::size_t>(30, max_label_width);
        for (std::size_t index = range.begin; index < range.end; ++index) {
            const auto& item = impl->items[displayed[index]];
            const auto selected = index == impl->selected_index;
            auto prefix = selected ? impl->theme.cursor : std::string("  ");
            auto bounded_prefix = truncate_text(prefix, width, "");
            if (!bounded_prefix) return std::unexpected(bounded_prefix.error());
            prefix = std::move(*bounded_prefix);
            const auto prefix_width = visible_width(prefix);
            const auto label_limit = width > prefix_width ? std::min(max_label_width, width - prefix_width) : 0;
            auto label = truncate_text(item.label, label_limit, "", true);
            if (!label) return std::unexpected(label.error());
            auto styled_label = detail::apply_selection_style(
                impl->theme.label,
                std::move(*label),
                selected,
                "SettingsList label");
            if (!styled_label) return std::unexpected(styled_label.error());
            std::string line = prefix + *styled_label;
            if (visible_width(line) + 2 < width) {
                line += "  ";
                const auto value_width = width - visible_width(line);
                auto value = truncate_text(item.current_value, value_width, "");
                if (!value) return std::unexpected(value.error());
                auto styled_value = detail::apply_selection_style(
                    impl->theme.value,
                    std::move(*value),
                    selected,
                    "SettingsList value");
                if (!styled_value) return std::unexpected(styled_value.error());
                line += *styled_value;
            }
            auto bounded = truncate_text(line, width, "");
            if (!bounded) return std::unexpected(bounded.error());
            lines.push_back(std::move(*bounded));
        }
        if (range.begin > 0 || range.end < displayed.size()) {
            auto scroll = impl->hint_line(
                std::format("  ({}/{})", impl->selected_index + 1, displayed.size()),
                width);
            if (!scroll) return std::unexpected(scroll.error());
            lines.push_back(std::move(*scroll));
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
        auto confirm_keys = impl->keybindings->key_text("tui.select.confirm");
        const KeyEvent space{.key = "space"};
        if (!impl->keybindings->matches(space, "tui.select.confirm")) {
            if (!confirm_keys.empty()) confirm_keys += '/';
            confirm_keys += "space";
        }
        const auto cancel_keys = impl->keybindings->key_text("tui.select.cancel");
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

void SettingsList::handle_input(const InputEventVariant& input) {
    auto impl = impl_;
    if (impl->submenu) {
        if (auto* handler = dynamic_cast<InputHandler*>(impl->submenu.get())) handler->handle_input(input);
        return;
    }
    if (const auto* paste = std::get_if<PasteEvent>(&input)) {
        if (!impl->search_enabled) return;
        impl->search += without_spaces(paste->text);
        impl->apply_filter();
        return;
    }
    const auto* key = std::get_if<KeyEvent>(&input);
    if (key == nullptr || key->type == KeyEventType::Release) return;
    const auto& displayed = impl->displayed_indices();
    if (impl->keybindings->matches(*key, "tui.select.up")) {
        if (displayed.empty()) return;
        impl->selected_index = impl->selected_index == 0 ? displayed.size() - 1 : impl->selected_index - 1;
        return;
    }
    if (impl->keybindings->matches(*key, "tui.select.down")) {
        if (displayed.empty()) return;
        impl->selected_index = impl->selected_index + 1 == displayed.size() ? 0 : impl->selected_index + 1;
        return;
    }
    if (impl->keybindings->matches(*key, "tui.select.confirm") || matches_key(*key, "space")) {
        impl->activate_item();
        return;
    }
    if (impl->keybindings->matches(*key, "tui.select.cancel")) {
        auto sink = impl->on_cancel;
        if (!sink || !*sink) return;
        try {
            (*sink)();
        } catch (...) {
            impl->report_callback_failure("TUI SettingsList cancel callback failed");
        }
        return;
    }
    if (!impl->search_enabled) return;
    if (impl->keybindings->matches(*key, "tui.editor.deleteCharBackward")) {
        auto graphemes = detail::split_graphemes(impl->search);
        if (!graphemes.empty()) graphemes.pop_back();
        impl->search.clear();
        for (const auto& grapheme : graphemes) impl->search += grapheme;
        impl->apply_filter();
        return;
    }
    if (is_search_text(*key)) {
        impl->search += without_spaces(key->key);
        impl->apply_filter();
    }
}

bool SettingsList::accepts_key_releases() const {
    return false;
}

void SettingsList::set_focused(bool focused) {
    auto impl = impl_;
    impl->focused = focused;
    if (impl->submenu) {
        if (auto* focusable = dynamic_cast<Focusable*>(impl->submenu.get())) focusable->set_focused(focused);
    }
}

bool SettingsList::focused() const {
    return impl_->focused;
}

std::optional<CursorPosition> SettingsList::cursor_location() const {
    if (!impl_->focused || !impl_->search_enabled || impl_->submenu) return std::nullopt;
    const auto width = std::max<std::size_t>(1, impl_->render_width);
    return CursorPosition{
        .column = std::min<std::size_t>(2 + visible_width(impl_->search), width - 1),
        .row = 0,
    };
}

} // namespace cch::tui
