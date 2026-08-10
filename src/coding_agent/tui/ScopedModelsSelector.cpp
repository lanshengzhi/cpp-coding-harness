#include "ScopedModelsSelector.hpp"

#include "DynamicBorder.hpp"
#include "KeybindingHints.hpp"
#include "ModelSearch.hpp"
#include "Theme.hpp"

#include <cch/ai/Model.hpp>
#include <cch/tui/Fuzzy.hpp>
#include <cch/tui/Text.hpp>
#include <cch/tui/Utils.hpp>

#include <algorithm>
#include <exception>
#include <iterator>
#include <utility>

namespace cch::coding_agent::tui {
namespace {

constexpr std::size_t kMaxVisible = 8;

} // namespace

ScopedModelsSelectorComponent::ScopedModelsSelectorComponent(
    const LiveTheme& theme,
    std::shared_ptr<const cch::tui::KeybindingRegistry> keybindings,
    std::vector<ai::Model> all_models,
    std::optional<std::vector<std::string>> enabled_ids,
    ScopedModelsChangeSink on_change,
    ScopedModelsPersistSink on_persist,
    ScopedModelsCancelSink on_cancel)
    : theme_(theme),
      keybindings_(std::move(keybindings)),
      on_change_(std::move(on_change)),
      on_persist_(std::move(on_persist)),
      on_cancel_(std::move(on_cancel)),
      search_input_(cch::tui::InputOptions{.keybindings = keybindings_}) {
    for (auto& model : all_models) {
        const auto full_id = model.provider + "/" + model.id;
        models_by_id_.emplace_back(full_id, std::move(model));
        all_ids_.push_back(std::move(full_id));
    }
    if (enabled_ids) enabled_ids_ = std::move(enabled_ids);
    refresh();
}

bool ScopedModelsSelectorComponent::is_enabled(std::string_view id) const {
    if (!enabled_ids_) return true;
    return std::find(enabled_ids_->begin(), enabled_ids_->end(), id) != enabled_ids_->end();
}

void ScopedModelsSelectorComponent::toggle(std::string id) {
    // pi `toggle`: a first toggle on the null (all-enabled) state starts an
    // explicit list with only this id.
    if (!enabled_ids_) {
        enabled_ids_ = std::vector<std::string>{std::move(id)};
        return;
    }
    const auto found = std::find(enabled_ids_->begin(), enabled_ids_->end(), id);
    if (found == enabled_ids_->end()) {
        enabled_ids_->push_back(std::move(id));
    } else {
        enabled_ids_->erase(found);
    }
}

void ScopedModelsSelectorComponent::enable_all(
    std::optional<std::vector<std::string>> target_ids) {
    // pi `enableAll`: already-all-enabled stays null; otherwise add the
    // targets and normalize back to null when every model is enabled.
    if (!enabled_ids_) return;
    const auto& targets = target_ids ? *target_ids : all_ids_;
    for (const auto& id : targets) {
        if (!is_enabled(id)) enabled_ids_->push_back(id);
    }
    if (enabled_ids_->size() == all_ids_.size() &&
        std::all_of(all_ids_.begin(), all_ids_.end(), [this](const std::string& id) {
            return is_enabled(id);
        })) {
        enabled_ids_.reset();
    }
}

void ScopedModelsSelectorComponent::clear_all(
    std::optional<std::vector<std::string>> target_ids) {
    // pi `clearAll`: the null (all-enabled) state becomes an empty list (or
    // the complement of the targets); otherwise drop the targets.
    if (!enabled_ids_) {
        std::vector<std::string> result;
        if (target_ids) {
            for (const auto& id : all_ids_) {
                if (std::find(target_ids->begin(), target_ids->end(), id) == target_ids->end()) {
                    result.push_back(id);
                }
            }
        }
        enabled_ids_ = std::move(result);
        return;
    }
    if (!target_ids) {
        enabled_ids_->clear();
        return;
    }
    std::erase_if(*enabled_ids_, [&](const std::string& id) {
        return std::find(target_ids->begin(), target_ids->end(), id) != target_ids->end();
    });
}

void ScopedModelsSelectorComponent::toggle_provider(std::string_view provider) {
    std::vector<std::string> provider_ids;
    for (const auto& [full_id, model] : models_by_id_) {
        if (model.provider == provider) provider_ids.push_back(full_id);
    }
    if (provider_ids.empty()) return;
    const bool all_enabled = std::all_of(
        provider_ids.begin(),
        provider_ids.end(),
        [this](const std::string& id) { return is_enabled(id); });
    if (all_enabled) {
        clear_all(provider_ids);
    } else {
        enable_all(provider_ids);
    }
}

void ScopedModelsSelectorComponent::move(std::string_view id, int delta) {
    // pi `move`: a no-op on the null state and on out-of-bounds moves.
    if (!enabled_ids_) return;
    const auto found = std::find(enabled_ids_->begin(), enabled_ids_->end(), id);
    if (found == enabled_ids_->end()) return;
    const auto index = static_cast<std::ptrdiff_t>(found - enabled_ids_->begin());
    const auto new_index = index + delta;
    if (new_index < 0 || new_index >= static_cast<std::ptrdiff_t>(enabled_ids_->size())) return;
    std::iter_swap(
        enabled_ids_->begin() + index,
        enabled_ids_->begin() + new_index);
}

std::vector<ScopedModelsSelectorComponent::ModelItem>
ScopedModelsSelectorComponent::build_items() const {
    // pi `getSortedIds`: enabled ids first (in order), then the rest.
    std::vector<ModelItem> items;
    items.reserve(all_ids_.size());
    const auto append_item = [&](const std::string& id) {
        const auto found = std::find_if(
            models_by_id_.begin(),
            models_by_id_.end(),
            [&](const auto& entry) { return entry.first == id; });
        items.push_back(ModelItem{
            .full_id = id,
            .model = found == models_by_id_.end()
                ? std::nullopt
                : std::optional<ai::Model>{found->second},
            .enabled = is_enabled(id),
        });
    };
    if (enabled_ids_) {
        for (const auto& id : *enabled_ids_) append_item(id);
    }
    for (const auto& id : all_ids_) {
        if (!enabled_ids_ ||
            std::find(enabled_ids_->begin(), enabled_ids_->end(), id) == enabled_ids_->end()) {
            append_item(id);
        }
    }
    return items;
}

void ScopedModelsSelectorComponent::refresh() {
    const auto query = search_input_.value();
    auto items = build_items();
    if (!query.empty()) {
        items = cch::tui::fuzzy_filter(
            std::move(items),
            query,
            [](const ModelItem& item) {
                if (item.model) {
                    return get_model_search_text(ModelSearchItem{
                        .id = item.model->id,
                        .provider = item.model->provider,
                        .name = item.model->name.empty() ? std::nullopt
                                                         : std::optional<std::string>{item.model->name},
                    });
                }
                return item.full_id;
            });
    }
    filtered_items_ = std::move(items);
    selected_index_ = std::min(selected_index_, filtered_items_.empty() ? 0 : filtered_items_.size() - 1);
}

void ScopedModelsSelectorComponent::notify_change() {
    if (on_change_) {
        on_change_(enabled_ids_ ? std::optional<std::vector<std::string>>{*enabled_ids_}
                                : std::nullopt);
    }
}

std::string ScopedModelsSelectorComponent::footer_text() const {
    // pi's footer: key hints joined with " · ", then the count text; the
    // "(unsaved)" marker renders in the warning color while dirty.
    const auto key = [this](std::string_view action) {
        return format_key_text(keybindings_->key_text(action));
    };
    // One pass over the explicit list separates known (enabled) from
    // unavailable entries; the all-enabled state counts every model.
    std::size_t enabled_count = 0;
    std::size_t unavailable_count = 0;
    if (enabled_ids_) {
        for (const auto& id : *enabled_ids_) {
            const bool known = std::find_if(
                                   models_by_id_.begin(),
                                   models_by_id_.end(),
                                   [&](const auto& entry) { return entry.first == id; }) !=
                models_by_id_.end();
            (known ? enabled_count : unavailable_count) += 1;
        }
    } else {
        enabled_count = all_ids_.size();
    }
    const auto count_text = !enabled_ids_
        ? "all enabled"
        : std::to_string(enabled_count) + "/" + std::to_string(all_ids_.size()) + " enabled" +
            (unavailable_count > 0
                 ? " · " + std::to_string(unavailable_count) + " unavailable"
                 : "");
    std::string text = "  " + key("tui.select.confirm") + " toggle · " +
        key("app.models.enableAll") + " all · " +
        key("app.models.clearAll") + " clear · " +
        key("app.models.toggleProvider") + " provider · " +
        key("app.models.reorderUp") + "/" + key("app.models.reorderDown") + " reorder · " +
        key("app.models.save") + " save · " + count_text;
    return dirty_
        ? theme_.foreground(ThemeToken::Dim, std::move(text)) +
            theme_.foreground(ThemeToken::Warning, " (unsaved)")
        : theme_.foreground(ThemeToken::Dim, std::move(text));
}

util::ExpectedVoid ScopedModelsSelectorComponent::update_list(std::vector<std::string>& out_lines, std::size_t width) const {
    // Every emitted line is bounded to the render width (the SessionSelector
    // pattern): the render path asserts each line's visible width <= the
    // bound, so a single untruncated line would abort the TUI. ANSI styling
    // is preserved because truncate_text operates over terminal tokens; a
    // truncation failure is propagated rather than falling back to the
    // (potentially over-wide) line.
    std::vector<std::string> lines;
    const auto emit = [&lines, width](std::string line) -> util::ExpectedVoid {
        auto truncated = cch::tui::truncate_text(line, width, "");
        if (!truncated) return std::unexpected(truncated.error());
        lines.push_back(std::move(*truncated));
        return {};
    };

    if (filtered_items_.empty()) {
        if (auto pushed = emit(theme_.foreground(ThemeToken::Muted, "  No matching models"));
            !pushed) return std::unexpected(pushed.error());
        out_lines.insert(
            out_lines.end(),
            std::make_move_iterator(lines.begin()),
            std::make_move_iterator(lines.end()));
        return {};
    }

    const std::size_t count = filtered_items_.size();
    const std::size_t start = count <= kMaxVisible
        ? 0
        : std::min(selected_index_ > kMaxVisible / 2 ? selected_index_ - kMaxVisible / 2 : 0, count - kMaxVisible);
    const std::size_t end = std::min(start + kMaxVisible, count);
    const bool all_enabled = !enabled_ids_;

    for (std::size_t index = start; index < end; ++index) {
        const auto& item = filtered_items_[index];
        const bool selected = index == selected_index_;
        std::string line;
        if (selected) {
            line = theme_.foreground(ThemeToken::Accent, "→ ");
        } else {
            line = "  ";
        }
        const std::string id = item.model ? item.model->id : item.full_id;
        line += selected ? theme_.foreground(ThemeToken::Accent, id) : id;
        line += theme_.foreground(
            ThemeToken::Muted,
            item.model ? " [" + item.model->provider + "]" : " [unavailable]");
        if (item.model) {
            if (all_enabled) {
                // pi: the all-enabled state renders no status marker.
            } else if (item.enabled) {
                line += theme_.foreground(ThemeToken::Success, " ✓");
            } else {
                line += theme_.foreground(ThemeToken::Dim, " ✗");
            }
        } else {
            line += theme_.foreground(ThemeToken::Dim, " ✗");
        }
        if (auto pushed = emit(std::move(line)); !pushed) return std::unexpected(pushed.error());
    }

    if (start > 0 || end < count) {
        if (auto pushed = emit(theme_.foreground(
                ThemeToken::Muted,
                "  (" + std::to_string(selected_index_ + 1) + "/" + std::to_string(count) + ")"));
            !pushed) return std::unexpected(pushed.error());
    }

    if (!filtered_items_.empty()) {
        const auto& selected = filtered_items_[selected_index_];
        if (auto pushed = emit(""); !pushed) return std::unexpected(pushed.error());
        if (auto pushed = emit(theme_.foreground(
                ThemeToken::Muted,
                selected.model
                    ? "  Model Name: " + selected.model->name
                    : "  Model unavailable"));
            !pushed) return std::unexpected(pushed.error());
    }
    out_lines.insert(
        out_lines.end(),
        std::make_move_iterator(lines.begin()),
        std::make_move_iterator(lines.end()));
    return {};
}

util::Expected<cch::tui::RenderResult> ScopedModelsSelectorComponent::render(std::size_t width) {
    cch::tui::RenderResult result;
    const auto append = [&result, width](cch::tui::Component& component) -> util::ExpectedVoid {
        auto rendered = component.render(width);
        if (!rendered) return std::unexpected(rendered.error());
        for (auto& line : rendered->lines) result.lines.push_back(std::move(line));
        return {};
    };

    // pi's composition: border / spacer / bold accent title / muted
    // session-only line / spacer / search input / spacer / list / spacer /
    // footer hint / border.
    DynamicBorder top_border(theme_.foreground_hook(ThemeToken::Border));
    if (auto appended = append(top_border); !appended) return std::unexpected(appended.error());
    {
        cch::tui::Text spacer("", 1, 0);
        if (auto appended = append(spacer); !appended) return std::unexpected(appended.error());
    }
    {
        cch::tui::Text title(
            theme_.foreground(ThemeToken::Accent, "\x1b[1mModel Configuration\x1b[22m"), 1, 0);
        if (auto appended = append(title); !appended) return std::unexpected(appended.error());
    }
    {
        cch::tui::Text session_only(
            theme_.foreground(
                ThemeToken::Muted,
                "Session-only. " + format_key_text(keybindings_->key_text("app.models.save")) +
                    " to save to settings."),
            1, 0);
        if (auto appended = append(session_only); !appended) return std::unexpected(appended.error());
    }
    {
        cch::tui::Text spacer("", 1, 0);
        if (auto appended = append(spacer); !appended) return std::unexpected(appended.error());
    }
    if (auto appended = append(search_input_); !appended) return std::unexpected(appended.error());
    {
        cch::tui::Text spacer("", 1, 0);
        if (auto appended = append(spacer); !appended) return std::unexpected(appended.error());
    }
    {
        std::vector<std::string> lines;
        if (auto updated = update_list(lines, width); !updated) return std::unexpected(updated.error());
        for (auto& line : lines) result.lines.push_back(std::move(line));
    }
    {
        cch::tui::Text spacer("", 1, 0);
        if (auto appended = append(spacer); !appended) return std::unexpected(appended.error());
    }
    {
        cch::tui::Text footer(footer_text(), 1, 0);
        if (auto appended = append(footer); !appended) return std::unexpected(appended.error());
    }
    DynamicBorder bottom_border(theme_.foreground_hook(ThemeToken::Border));
    if (auto appended = append(bottom_border); !appended) return std::unexpected(appended.error());
    return result;
}

void ScopedModelsSelectorComponent::handle_input(const cch::tui::InputEventVariant& input) {
    const auto* key = std::get_if<cch::tui::KeyEvent>(&input);
    if (key == nullptr || key->type == cch::tui::KeyEventType::Release) return;

    if (keybindings_->matches(*key, "tui.select.up")) {
        if (filtered_items_.empty()) return;
        selected_index_ = selected_index_ == 0 ? filtered_items_.size() - 1 : selected_index_ - 1;
        return;
    }
    if (keybindings_->matches(*key, "tui.select.down")) {
        if (filtered_items_.empty()) return;
        selected_index_ = selected_index_ == filtered_items_.size() - 1 ? 0 : selected_index_ + 1;
        return;
    }

    // Reorder enabled models (pi `app.models.reorderUp`/`reorderDown`).
    const bool reorder_up = keybindings_->matches(*key, "app.models.reorderUp");
    const bool reorder_down = keybindings_->matches(*key, "app.models.reorderDown");
    if (reorder_up || reorder_down) {
        if (!enabled_ids_) return;
        if (selected_index_ >= filtered_items_.size()) return;
        const auto& item = filtered_items_[selected_index_];
        if (!item.enabled) return;
        const int delta = reorder_up ? -1 : 1;
        const auto found = std::find(enabled_ids_->begin(), enabled_ids_->end(), item.full_id);
        const auto current_index = static_cast<std::ptrdiff_t>(found - enabled_ids_->begin());
        const auto new_index = current_index + delta;
        if (new_index >= 0 && new_index < static_cast<std::ptrdiff_t>(enabled_ids_->size())) {
            move(item.full_id, delta);
            dirty_ = true;
            selected_index_ = static_cast<std::size_t>(
                static_cast<std::ptrdiff_t>(selected_index_) + delta);
            refresh();
            notify_change();
        }
        return;
    }

    // Toggle on Enter (pi `tui.select.confirm`).
    if (keybindings_->matches(*key, "tui.select.confirm")) {
        if (selected_index_ >= filtered_items_.size()) return;
        toggle(filtered_items_[selected_index_].full_id);
        dirty_ = true;
        refresh();
        notify_change();
        return;
    }

    // Enable all (filtered when a search is active, otherwise all).
    if (keybindings_->matches(*key, "app.models.enableAll")) {
        std::optional<std::vector<std::string>> target_ids;
        if (!search_input_.value().empty()) {
            target_ids = std::vector<std::string>{};
            for (const auto& item : filtered_items_) target_ids->push_back(item.full_id);
        }
        enable_all(std::move(target_ids));
        dirty_ = true;
        refresh();
        notify_change();
        return;
    }

    // Clear all (filtered when a search is active, otherwise all).
    if (keybindings_->matches(*key, "app.models.clearAll")) {
        std::optional<std::vector<std::string>> target_ids;
        if (!search_input_.value().empty()) {
            target_ids = std::vector<std::string>{};
            for (const auto& item : filtered_items_) target_ids->push_back(item.full_id);
        }
        clear_all(std::move(target_ids));
        dirty_ = true;
        refresh();
        notify_change();
        return;
    }

    // Toggle the provider of the current item (pi `app.models.toggleProvider`).
    if (keybindings_->matches(*key, "app.models.toggleProvider")) {
        if (selected_index_ >= filtered_items_.size()) return;
        const auto& item = filtered_items_[selected_index_];
        if (!item.model) return;
        toggle_provider(item.model->provider);
        dirty_ = true;
        refresh();
        notify_change();
        return;
    }

    // Save/persist to settings (pi `app.models.save`).
    if (keybindings_->matches(*key, "app.models.save")) {
        if (on_persist_) {
            on_persist_(enabled_ids_
                ? std::optional<std::vector<std::string>>{*enabled_ids_}
                : std::nullopt);
        }
        dirty_ = false;
        return;
    }

    // Ctrl+C clears the search, or cancels when it is empty (pi).
    if (matches_key(*key, "ctrl+c")) {
        if (!search_input_.value().empty()) {
            search_input_.set_value("");
            refresh();
        } else if (on_cancel_) {
            on_cancel_();
        }
        return;
    }

    // Escape cancels (pi).
    if (matches_key(*key, "escape")) {
        if (on_cancel_) on_cancel_();
        return;
    }

    search_input_.handle_input(input);
    refresh();
}

void ScopedModelsSelectorComponent::set_focused(bool focused) {
    focused_ = focused;
    search_input_.set_focused(focused);
}

bool ScopedModelsSelectorComponent::focused() const {
    return focused_;
}

std::optional<cch::tui::CursorPosition> ScopedModelsSelectorComponent::cursor_location() const {
    return search_input_.cursor_location();
}

} // namespace cch::coding_agent::tui
