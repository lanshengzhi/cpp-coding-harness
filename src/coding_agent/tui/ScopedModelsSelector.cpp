#include "ScopedModelsSelector.hpp"

#include "DynamicBorder.hpp"
#include "KeybindingHints.hpp"
#include "ModelSearch.hpp"
#include "Theme.hpp"

#include <cch/ai/Model.hpp>
#include <cch/tui/Fuzzy.hpp>
#include <cch/tui/Text.hpp>
#include <cch/tui/Utils.hpp>

#include <cch/support/Error.hpp>
#include <algorithm>
#include <array>
#include <cstddef>
#include <iterator>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::coding_agent::tui {
namespace {

constexpr std::size_t kMaxVisible = 8;

/// Dispatch order (pi `scoped-models-selector.ts`): navigation, reorder, then
/// toggle. The first bound action in table order wins, so the reorder flags
/// retain their original up-before-down precedence. Navigation and toggle are
/// delegated to the SelectList, which keeps its own up-before-confirm order.
constexpr std::array<std::string_view, 5> kScopedModelsSelectorActions = {
        "tui.select.up",
        "tui.select.down",
        "app.models.reorderUp",
        "app.models.reorderDown",
        "tui.select.confirm",
};

/// The `provider/id` keys in catalog order (pi `allIds`).
[[nodiscard]] std::vector<std::pair<std::string, ai::Model>> index_models(std::vector<ai::Model>& all_models) {
    std::vector<std::pair<std::string, ai::Model>> result;
    result.reserve(all_models.size());
    for (auto& model : all_models) {
        result.emplace_back(model.provider + "/" + model.id, std::move(model));
    }
    return result;
}

[[nodiscard]] std::vector<std::string> model_keys(const std::vector<std::pair<std::string, ai::Model>>& models_by_id) {
    std::vector<std::string> keys;
    keys.reserve(models_by_id.size());
    for (const auto& [full_id, model] : models_by_id) {
        static_cast<void>(model);
        keys.push_back(full_id);
    }
    return keys;
}

/// One `SelectItem` per scoped-model row in pi `getSortedIds` order: enabled
/// ids first (in order), then the rest. The label carries the visible row
/// (id, `[provider]`/`[unavailable]`, and the ✓/✗ status marker except in the
/// all-enabled state), `value` is the `provider/id` key, and `search_text`
/// is the historical fuzzy search text.
[[nodiscard]] std::vector<cch::tui::SelectItem> build_items(const LiveTheme& theme,
        const std::vector<std::pair<std::string, ai::Model>>& models_by_id,
        const std::vector<std::string>& all_ids,
        const std::optional<std::vector<std::string>>& enabled_ids) {
    std::vector<cch::tui::SelectItem> items;
    items.reserve(all_ids.size() + (enabled_ids ? enabled_ids->size() : 0));
    const auto is_enabled = [&](std::string_view id) {
        if (!enabled_ids) return true;
        return std::find(enabled_ids->begin(), enabled_ids->end(), id) != enabled_ids->end();
    };
    const auto append_item = [&](const std::string& id) {
        const auto found = std::find_if(
                models_by_id.begin(), models_by_id.end(), [&](const auto& entry) { return entry.first == id; });
        const ai::Model* model = found == models_by_id.end() ? nullptr : &found->second;
        // The visible id is the bare model id for known models (pi's row
        // layout); unavailable entries fall back to the stored full id.
        std::string label = model != nullptr ? model->id : id;
        if (model != nullptr) {
            label += theme.foreground(ThemeToken::Muted, " [" + model->provider + "]");
            // pi: the all-enabled state renders no status marker.
            if (enabled_ids) {
                label += is_enabled(id) ? theme.foreground(ThemeToken::Success, " ✓")
                                        : theme.foreground(ThemeToken::Dim, " ✗");
            }
        } else {
            label += theme.foreground(ThemeToken::Muted, " [unavailable]");
            label += theme.foreground(ThemeToken::Dim, " ✗");
        }
        std::string search_text;
        if (model != nullptr) {
            search_text = get_model_search_text(ModelSearchItem{
                    .id = model->id,
                    .provider = model->provider,
                    .name = model->name.empty() ? std::nullopt : std::optional<std::string>{model->name},
            });
        } else {
            search_text = id;
        }
        items.push_back(cch::tui::SelectItem{
                .value = id,
                .label = std::move(label),
                .description = std::nullopt,
                .search_text = std::move(search_text),
        });
    };
    if (enabled_ids) {
        for (const auto& id : *enabled_ids)
            append_item(id);
    }
    for (const auto& id : all_ids) {
        if (!enabled_ids || std::find(enabled_ids->begin(), enabled_ids->end(), id) == enabled_ids->end()) {
            append_item(id);
        }
    }
    return items;
}

} // namespace

ScopedModelsSelectorComponent::ScopedModelsSelectorComponent(const LiveTheme& theme,
        std::shared_ptr<const cch::tui::KeybindingRegistry> keybindings,
        std::vector<ai::Model> all_models,
        std::optional<std::vector<std::string>> enabled_ids,
        ScopedModelsChangeSink on_change,
        ScopedModelsPersistSink on_persist,
        ScopedModelsCancelSink on_cancel)
    : theme_(theme), keybindings_(std::move(keybindings)), on_change_(std::move(on_change)),
      on_persist_(std::move(on_persist)), on_cancel_(std::move(on_cancel)), models_by_id_(index_models(all_models)),
      all_ids_(model_keys(models_by_id_)), enabled_ids_(std::move(enabled_ids)), filtered_ids_{},
      select_list_(build_items(theme_, models_by_id_, all_ids_, enabled_ids_),
              cch::tui::SelectListOptions{
                      .max_visible = kMaxVisible,
                      .theme = theme_.select_list_theme(),
                      .on_select = [this](const cch::tui::SelectItem& item) -> support::ExpectedVoid {
                          // pi `tui.select.confirm` toggles the selected model.
                          toggle(item.value);
                          dirty_ = true;
                          sync_items();
                          notify_change();
                          return {};
                      },
                      .on_cancel = [this]() -> support::ExpectedVoid {
                          if (on_cancel_) on_cancel_();
                          return {};
                      },
                      .keybindings = keybindings_,
                      .enable_search = true,
                      .search_filter_hook =
                              [this](std::string_view query, const std::vector<cch::tui::SelectItem>& items) {
                                  // The default fuzzy ranking (identical semantics to the
                                  // SelectList's built-in path), also recording the
                                  // filtered ids in display order for the filtered
                                  // enable-all/clear-all actions.
                                  std::vector<std::size_t> indices(items.size());
                                  std::iota(indices.begin(), indices.end(), 0);
                                  if (!query.empty()) {
                                      indices = cch::tui::fuzzy_filter(
                                              std::move(indices), query, [&items](std::size_t index) {
                                                  return cch::tui::select_item_search_text(items[index]);
                                              });
                                  }
                                  filtered_ids_.clear();
                                  filtered_ids_.reserve(indices.size());
                                  for (const auto index : indices) {
                                      filtered_ids_.push_back(items[index].value);
                                  }
                                  return indices;
                              },
                      // The SelectList's generic no-match row carries the
                      // selector's pi wording.
                      .no_match_text = "  No matching models",
              }) {}

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
    std::iter_swap(enabled_ids_->begin() + index, enabled_ids_->begin() + new_index);
}

void ScopedModelsSelectorComponent::sync_items() {
    select_list_.set_items(build_items(theme_, models_by_id_, all_ids_, enabled_ids_));
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

support::Expected<cch::tui::RenderResult> ScopedModelsSelectorComponent::render(std::size_t width) {
    cch::tui::RenderResult result;
    const auto append = [&result, width](cch::tui::Component& component) -> support::ExpectedVoid {
        auto rendered = component.render(width);
        if (!rendered) return std::unexpected(rendered.error());
        for (auto& line : rendered->lines) result.lines.push_back(std::move(line));
        return {};
    };

    // pi's composition: border / title / muted session-only line / list
    // (search input, rows, scroll info — all from the SelectList) / selected
    // model name / footer hint / border.
    DynamicBorder top_border(theme_.foreground_hook(ThemeToken::Border));
    if (auto appended = append(top_border); !appended) return std::unexpected(appended.error());
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
    // Everything above this point is chrome the component owns: the SelectList
    // cursor must be offset by exactly these rows (its search input is its own
    // row 0). The old code reported the search cursor at row 0, floating the
    // IME cursor above the dialog.
    rows_before_list_ = result.lines.size();

    auto rendered_list = select_list_.render(width);
    if (!rendered_list) return std::unexpected(rendered_list.error());
    for (auto& line : rendered_list->lines)
        result.lines.push_back(std::move(line));

    const auto selected = select_list_.selected_item();
    if (selected) {
        const auto found = std::find_if(models_by_id_.begin(), models_by_id_.end(), [&](const auto& entry) {
            return entry.first == selected->value;
        });
        const std::string name_line =
                found == models_by_id_.end() ? "  Model unavailable" : "  Model Name: " + found->second.name;
        result.lines.emplace_back();
        auto bounded = cch::tui::truncate_text(theme_.foreground(ThemeToken::Muted, name_line), width, "");
        if (!bounded) return std::unexpected(bounded.error());
        result.lines.push_back(std::move(*bounded));
    }

    {
        cch::tui::Text footer(footer_text(), 1, 0);
        if (auto appended = append(footer); !appended) return std::unexpected(appended.error());
    }
    DynamicBorder bottom_border(theme_.foreground_hook(ThemeToken::Border));
    if (auto appended = append(bottom_border); !appended) return std::unexpected(appended.error());
    return result;
}

void ScopedModelsSelectorComponent::invalidate() { select_list_.invalidate(); }

void ScopedModelsSelectorComponent::handle_input(const cch::tui::InputEventVariant& input) {
    // Paste events belong to the embedded search input; releases are dropped
    // by the SelectList.
    const auto* key = std::get_if<cch::tui::KeyEvent>(&input);
    if (key == nullptr || key->type == cch::tui::KeyEventType::Release) {
        select_list_.handle_input(input);
        return;
    }

    // The table preserves pi's order: navigation and toggle live inside the
    // SelectList (its own dispatch keeps up-before-confirm precedence and
    // wraps around); reorder sits between them as before.
    const auto action = keybindings_->first_match(*key, kScopedModelsSelectorActions);
    if (action == "tui.select.up" || action == "tui.select.down" || action == "tui.select.confirm") {
        select_list_.handle_input(input);
        return;
    }

    // Reorder enabled models (pi `app.models.reorderUp`/`reorderDown`).
    const bool reorder_up = action == "app.models.reorderUp";
    const bool reorder_down = action == "app.models.reorderDown";
    if (reorder_up || reorder_down) {
        if (!enabled_ids_) return;
        const auto selected = select_list_.selected_item();
        if (!selected) return;
        const auto found = std::find(enabled_ids_->begin(), enabled_ids_->end(), selected->value);
        if (found == enabled_ids_->end()) return;
        const auto current_index = static_cast<std::ptrdiff_t>(found - enabled_ids_->begin());
        const auto new_index = current_index + (reorder_up ? -1 : 1);
        if (new_index < 0 || new_index >= static_cast<std::ptrdiff_t>(enabled_ids_->size())) {
            return;
        }
        move(selected->value, reorder_up ? -1 : 1);
        dirty_ = true;
        sync_items();
        notify_change();
        return;
    }

    // Enable all (filtered when a search is active, otherwise all).
    if (keybindings_->matches(*key, "app.models.enableAll")) {
        std::optional<std::vector<std::string>> target_ids;
        if (!select_list_.search_query().empty()) {
            target_ids = filtered_ids_;
        }
        enable_all(std::move(target_ids));
        dirty_ = true;
        sync_items();
        notify_change();
        return;
    }

    // Clear all (filtered when a search is active, otherwise all).
    if (keybindings_->matches(*key, "app.models.clearAll")) {
        std::optional<std::vector<std::string>> target_ids;
        if (!select_list_.search_query().empty()) {
            target_ids = filtered_ids_;
        }
        clear_all(std::move(target_ids));
        dirty_ = true;
        sync_items();
        notify_change();
        return;
    }

    // Toggle the provider of the current item (pi `app.models.toggleProvider`).
    if (keybindings_->matches(*key, "app.models.toggleProvider")) {
        const auto selected = select_list_.selected_item();
        if (!selected) return;
        const auto found = std::find_if(models_by_id_.begin(), models_by_id_.end(), [&](const auto& entry) {
            return entry.first == selected->value;
        });
        if (found == models_by_id_.end()) return;
        toggle_provider(found->second.provider);
        dirty_ = true;
        sync_items();
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

    // Ctrl+C clears the search, or cancels when it is empty (pi). Intercepted
    // before the SelectList, whose cancel action would always cancel.
    if (matches_key(*key, "ctrl+c")) {
        if (!select_list_.search_query().empty()) {
            select_list_.set_filter("");
        } else if (on_cancel_) {
            on_cancel_();
        }
        return;
    }

    // Escape cancels (pi); the SelectList dispatches its cancel action. All
    // remaining keys (search editing, printable characters) also go to the
    // embedded search input through the SelectList.
    select_list_.handle_input(input);
}

void ScopedModelsSelectorComponent::set_focused(bool focused) { select_list_.set_focused(focused); }

bool ScopedModelsSelectorComponent::focused() const { return select_list_.focused(); }

std::optional<cch::tui::CursorPosition> ScopedModelsSelectorComponent::cursor_location() const {
    auto cursor = select_list_.cursor_location();
    if (!cursor) return std::nullopt;
    cursor->row += rows_before_list_;
    return cursor;
}

} // namespace cch::coding_agent::tui
