#include <cch/tui/SettingsList.hpp>

#include "tui/InteractionUtils.hpp"
#include "tui/UnicodeWidth.hpp"

#include <utf8proc.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
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

[[nodiscard]] std::string casefold_text(std::string_view text) {
    utf8proc_uint8_t* mapped = nullptr;
    const auto size = utf8proc_map(
        reinterpret_cast<const utf8proc_uint8_t*>(text.data()),
        static_cast<utf8proc_ssize_t>(text.size()),
        &mapped,
        static_cast<utf8proc_option_t>(UTF8PROC_STABLE | UTF8PROC_COMPOSE | UTF8PROC_CASEFOLD));
    if (size < 0 || mapped == nullptr) {
        std::free(mapped);
        return std::string(text);
    }
    std::string result(reinterpret_cast<const char*>(mapped), static_cast<std::size_t>(size));
    std::free(mapped);
    return result;
}

[[nodiscard]] std::optional<double> match_fuzzy_query(
    std::string_view normalized_query,
    std::string_view normalized_text) {
    if (normalized_query.empty()) return 0.0;
    if (normalized_query.size() > normalized_text.size()) return std::nullopt;
    std::size_t query_index = 0;
    std::optional<std::size_t> previous;
    double score = 0.0;
    std::size_t consecutive = 0;
    for (std::size_t index = 0;
         index < normalized_text.size() && query_index < normalized_query.size();
         ++index) {
        if (normalized_text[index] != normalized_query[query_index]) continue;
        const auto boundary = index == 0 ||
            std::string_view(" -_./:").find(normalized_text[index - 1]) != std::string_view::npos;
        if (previous && *previous + 1 == index) {
            ++consecutive;
            score -= static_cast<double>(consecutive * 5);
        } else {
            consecutive = 0;
            if (previous) score += static_cast<double>((index - *previous - 1) * 2);
        }
        if (boundary) score -= 10.0;
        score += static_cast<double>(index) * 0.1;
        previous = index;
        ++query_index;
    }
    if (query_index != normalized_query.size()) return std::nullopt;
    if (normalized_query == normalized_text) score -= 100.0;
    return score;
}

[[nodiscard]] std::optional<std::string> swapped_alpha_numeric(std::string_view query) {
    const auto split = std::find_if(query.begin(), query.end(), [](unsigned char value) {
        return std::isdigit(value) != 0;
    });
    if (split != query.begin() && split != query.end() &&
        std::all_of(query.begin(), split, [](unsigned char value) { return std::isalpha(value) != 0; }) &&
        std::all_of(split, query.end(), [](unsigned char value) { return std::isdigit(value) != 0; })) {
        return std::string(split, query.end()) + std::string(query.begin(), split);
    }
    const auto letters = std::find_if(query.begin(), query.end(), [](unsigned char value) {
        return std::isalpha(value) != 0;
    });
    if (letters != query.begin() && letters != query.end() &&
        std::all_of(query.begin(), letters, [](unsigned char value) { return std::isdigit(value) != 0; }) &&
        std::all_of(letters, query.end(), [](unsigned char value) { return std::isalpha(value) != 0; })) {
        return std::string(letters, query.end()) + std::string(query.begin(), letters);
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<double> fuzzy_score(std::string_view query, std::string_view text) {
    const auto normalized_query = casefold_text(query);
    const auto normalized_text = casefold_text(text);
    if (auto primary = match_fuzzy_query(normalized_query, normalized_text)) return primary;
    const auto swapped = swapped_alpha_numeric(normalized_query);
    if (!swapped) return std::nullopt;
    if (auto matched = match_fuzzy_query(*swapped, normalized_text)) return *matched + 5.0;
    return std::nullopt;
}

[[nodiscard]] std::vector<std::string> fuzzy_tokens(std::string_view query) {
    std::vector<std::string> tokens;
    std::size_t begin = 0;
    for (std::size_t index = 0; index <= query.size(); ++index) {
        const auto separator = index == query.size() || query[index] == '/' ||
            std::isspace(static_cast<unsigned char>(query[index])) != 0;
        if (!separator) continue;
        if (index > begin) tokens.emplace_back(query.substr(begin, index - begin));
        begin = index + 1;
    }
    return tokens;
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
        filtered_indices.clear();
        const auto tokens = fuzzy_tokens(search);
        std::vector<std::pair<double, std::size_t>> matches;
        for (std::size_t index = 0; index < items.size(); ++index) {
            double total_score = 0.0;
            bool all_match = true;
            for (const auto& token : tokens) {
                if (auto score = fuzzy_score(token, items[index].label)) {
                    total_score += *score;
                } else {
                    all_match = false;
                    break;
                }
            }
            if (all_match) matches.emplace_back(total_score, index);
        }
        std::stable_sort(matches.begin(), matches.end(), [](const auto& left, const auto& right) {
            return left.first < right.first;
        });
        for (const auto& [score, index] : matches) {
            (void)score;
            filtered_indices.push_back(index);
        }
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
        auto bounded = detail::truncate_text(text, width, "");
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

util::Expected<std::vector<std::string>> SettingsList::render(std::size_t width) {
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
        auto query = detail::truncate_text("> " + impl->search, width, "");
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
            max_label_width = std::max(max_label_width, detail::visible_width(item.label));
        }
        max_label_width = std::min<std::size_t>(30, max_label_width);
        for (std::size_t index = range.begin; index < range.end; ++index) {
            const auto& item = impl->items[displayed[index]];
            const auto selected = index == impl->selected_index;
            auto prefix = selected ? impl->theme.cursor : std::string("  ");
            auto bounded_prefix = detail::truncate_text(prefix, width, "");
            if (!bounded_prefix) return std::unexpected(bounded_prefix.error());
            prefix = std::move(*bounded_prefix);
            const auto prefix_width = detail::visible_width(prefix);
            const auto label_limit = width > prefix_width ? std::min(max_label_width, width - prefix_width) : 0;
            auto label = detail::truncate_text(item.label, label_limit, "", true);
            if (!label) return std::unexpected(label.error());
            auto styled_label = detail::apply_selection_style(
                impl->theme.label,
                std::move(*label),
                selected,
                "SettingsList label");
            if (!styled_label) return std::unexpected(styled_label.error());
            std::string line = prefix + *styled_label;
            if (detail::visible_width(line) + 2 < width) {
                line += "  ";
                const auto value_width = width - detail::visible_width(line);
                auto value = detail::truncate_text(item.current_value, value_width, "");
                if (!value) return std::unexpected(value.error());
                auto styled_value = detail::apply_selection_style(
                    impl->theme.value,
                    std::move(*value),
                    selected,
                    "SettingsList value");
                if (!styled_value) return std::unexpected(styled_value.error());
                line += *styled_value;
            }
            auto bounded = detail::truncate_text(line, width, "");
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
            auto wrapped = detail::wrap_text(*selected->description, description_width);
            if (!wrapped) return std::unexpected(wrapped.error());
            for (const auto& line : *wrapped) {
                auto styled = detail::apply_text_style(
                    impl->theme.description,
                    "  " + line,
                    "SettingsList description");
                if (!styled) return std::unexpected(styled.error());
                auto bounded = detail::truncate_text(*styled, width, "");
                if (!bounded) return std::unexpected(bounded.error());
                lines.push_back(std::move(*bounded));
            }
        }
    }

    if (!impl->items.empty() || impl->search_enabled) {
        lines.emplace_back();
        auto hint = impl->hint_line(
            impl->search_enabled
                ? "  Type to search · Enter/Space to change · Esc to cancel"
                : "  Enter/Space to change · Esc to cancel",
            width);
        if (!hint) return std::unexpected(hint.error());
        lines.push_back(std::move(*hint));
    }
    return lines;
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
    if (matches_key(*key, "up")) {
        if (displayed.empty()) return;
        impl->selected_index = impl->selected_index == 0 ? displayed.size() - 1 : impl->selected_index - 1;
        return;
    }
    if (matches_key(*key, "down")) {
        if (displayed.empty()) return;
        impl->selected_index = impl->selected_index + 1 == displayed.size() ? 0 : impl->selected_index + 1;
        return;
    }
    if (matches_key(*key, "enter") || matches_key(*key, "space")) {
        impl->activate_item();
        return;
    }
    if (matches_key(*key, "escape") || matches_key(*key, "ctrl+c")) {
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
    if (matches_key(*key, "backspace")) {
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
        .column = std::min<std::size_t>(2 + detail::visible_width(impl_->search), width - 1),
        .row = 0,
    };
}

} // namespace cch::tui
