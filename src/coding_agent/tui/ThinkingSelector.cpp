#include "ThinkingSelector.hpp"

#include "DynamicBorder.hpp"
#include "KeybindingHints.hpp"
#include "Theme.hpp"

#include <cch/tui/Text.hpp>

#include <cch/support/Error.hpp>

#include <string_view>

namespace cch::coding_agent::tui {
namespace {

// pi `thinking-selector.ts` `LEVEL_DESCRIPTIONS`, verbatim (identical values
// to settings-selector's `THINKING_DESCRIPTIONS`; pi keeps two tables).
[[nodiscard]] std::string_view thinking_level_description(std::string_view level) {
    if (level == "off") return "No reasoning";
    if (level == "minimal") return "Very brief reasoning (~1k tokens)";
    if (level == "low") return "Light reasoning (~2k tokens)";
    if (level == "medium") return "Moderate reasoning (~8k tokens)";
    if (level == "high") return "Deep reasoning (~16k tokens)";
    if (level == "xhigh") return "Extra-high reasoning (~32k tokens)";
    if (level == "max") return "Maximum reasoning";
    return {};
}

} // namespace

ThinkingSelectorComponent::ThinkingSelectorComponent(const LiveTheme& theme,
        std::shared_ptr<const cch::tui::KeybindingRegistry> keybindings,
        std::string current_level,
        std::vector<std::string> available_levels,
        ThinkingSelectorSelectSink on_select,
        ThinkingSelectorCancelSink on_cancel,
        ThinkingSelectorSelectAsDefaultSink on_select_as_default,
        std::optional<std::string> default_level)
    : theme_(theme), keybindings_(std::move(keybindings)), current_level_(std::move(current_level)),
      available_levels_(std::move(available_levels)), on_select_(std::move(on_select)),
      on_cancel_(std::move(on_cancel)), on_select_as_default_(std::move(on_select_as_default)),
      default_level_(std::move(default_level)),
      // The list/search presentation lives in the shared SelectList; sinks
      // only re-enter this component (the SelectList outlives neither).
      select_list_(build_items(),
              cch::tui::SelectListOptions{
                      .max_visible = 10,
                      .theme = theme_.select_list_theme(),
                      .on_select = [this](const cch::tui::SelectItem& item) -> support::ExpectedVoid {
                          if (on_select_) on_select_(item.value);
                          return {};
                      },
                      .on_cancel = [this]() -> support::ExpectedVoid {
                          if (on_cancel_) on_cancel_();
                          return {};
                      },
                      .keybindings = keybindings_,
                      .enable_search = true,
                      .no_match_text = "  No matching levels",
              }) {
    // pi: the selector opens with the current level highlighted.
    for (std::size_t index = 0; index < available_levels_.size(); ++index) {
        if (available_levels_[index] == current_level_) {
            select_list_.set_selected_index(index);
            break;
        }
    }
}

std::vector<cch::tui::SelectItem> ThinkingSelectorComponent::build_items() const {
    std::vector<cch::tui::SelectItem> items;
    items.reserve(available_levels_.size());
    for (const auto& level : available_levels_) {
        const bool current = level == current_level_;
        const bool default_level = default_level_.has_value() && *default_level_ == level;
        auto description = std::string{thinking_level_description(level)};
        if (default_level) description += " · default";
        items.push_back(cch::tui::SelectItem{
                .value = level,
                .label = std::string{current ? "✓ " : "  "} + level,
                .description = description,
        });
    }
    return items;
}

support::Expected<cch::tui::RenderResult> ThinkingSelectorComponent::render(std::size_t width) {
    cch::tui::RenderResult result;
    const auto append = [&result, width](cch::tui::Component& component) -> support::ExpectedVoid {
        auto rendered = component.render(width);
        if (!rendered) return std::unexpected(rendered.error());
        for (auto& line : rendered->lines)
            result.lines.push_back(std::move(line));
        return {};
    };

    // pi's constructor composition: border / spacer / title / spacer / cycle
    // hint / spacer / list chrome / spacer / save hint / border. The list
    // block (search input, rows, scroll info) renders from the shared
    // SelectList.
    DynamicBorder top_border(theme_.foreground_hook(ThemeToken::Border));
    if (auto appended = append(top_border); !appended) return std::unexpected(appended.error());
    {
        cch::tui::Text spacer("", 1, 0);
        if (auto appended = append(spacer); !appended) return std::unexpected(appended.error());
    }
    {
        cch::tui::Text title("Thinking Level", 0, 0);
        if (auto appended = append(title); !appended) return std::unexpected(appended.error());
    }
    {
        cch::tui::Text spacer("", 1, 0);
        if (auto appended = append(spacer); !appended) return std::unexpected(appended.error());
    }
    {
        cch::tui::Text cycle_hint(format_key_text(keybindings_->key_text("app.thinking.cycle"), true) +
                                          " cycles thinking levels in-session",
                0,
                0);
        if (auto appended = append(cycle_hint); !appended) return std::unexpected(appended.error());
    }
    {
        cch::tui::Text spacer("", 1, 0);
        if (auto appended = append(spacer); !appended) return std::unexpected(appended.error());
    }

    cursor_row_offset_ = result.lines.size();
    {
        auto rendered = select_list_.render(width);
        if (!rendered) return std::unexpected(rendered.error());
        for (auto& line : rendered->lines)
            result.lines.push_back(std::move(line));
    }
    {
        cch::tui::Text spacer("", 1, 0);
        if (auto appended = append(spacer); !appended) return std::unexpected(appended.error());
    }
    // pi's dim hint line: the save-as-default affordance renders only when
    // the sink exists (`  Enter to select · Ctrl+S to set as default ·
    // Escape/Ctrl+C to cancel` through pi's `keyDisplayText`).
    if (on_select_as_default_) {
        cch::tui::Text save_hint(
                theme_.foreground(ThemeToken::Muted,
                        "  " + format_key_text(keybindings_->key_text("tui.select.confirm"), true) + " to select · " +
                                format_key_text(keybindings_->key_text("app.thinking.save"), true) +
                                " to set as default · " +
                                format_key_text(keybindings_->key_text("tui.select.cancel"), true) + " to cancel"),
                0,
                0);
        if (auto appended = append(save_hint); !appended) return std::unexpected(appended.error());
    }
    DynamicBorder bottom_border(theme_.foreground_hook(ThemeToken::Border));
    if (auto appended = append(bottom_border); !appended) return std::unexpected(appended.error());
    return result;
}

void ThinkingSelectorComponent::invalidate() {}

cch::tui::InputAdmissionOutcome ThinkingSelectorComponent::handle_input(const cch::tui::InputEventVariant& input) {
    const auto* key = std::get_if<cch::tui::KeyEvent>(&input);
    if (key != nullptr && key->type == cch::tui::KeyEventType::Release) {
        return cch::tui::InputAdmissionOutcome::Unhandled;
    }
    // pi `handleInput`'s `app.thinking.save` branch: the save-as-default key
    // pre-empts SelectList so it never lands in the search input.
    if (key != nullptr && on_select_as_default_ && keybindings_->matches(*key, "app.thinking.save")) {
        if (const auto selection = select_list_.selected_item()) {
            on_select_as_default_(selection->value);
        }
        return cch::tui::InputAdmissionOutcome::Consumed;
    }
    // Navigation, confirm/cancel, and search editing belong to the
    // SelectList.
    static_cast<void>(select_list_.handle_input(input));
    return cch::tui::InputAdmissionOutcome::Consumed;
}

void ThinkingSelectorComponent::set_focused(bool focused) {
    focused_ = focused;
    select_list_.set_focused(focused);
}

bool ThinkingSelectorComponent::focused() const { return focused_; }

std::optional<cch::tui::CursorPosition> ThinkingSelectorComponent::cursor_location() const {
    const auto cursor = select_list_.cursor_location();
    if (!cursor) return std::nullopt;
    return cch::tui::CursorPosition{
            .column = cursor->column,
            .row = cursor->row + cursor_row_offset_,
    };
}

} // namespace cch::coding_agent::tui
