#include "StringListSelector.hpp"

#include "DynamicBorder.hpp"
#include "KeybindingHints.hpp"
#include "Theme.hpp"

#include <cch/tui/Text.hpp>

#include <cch/support/Error.hpp>
#include <exception>
#include <utility>

namespace cch::coding_agent::tui {

StringListSelector::StringListSelector(
    const LiveTheme& theme,
    std::shared_ptr<const cch::tui::KeybindingRegistry> keybindings,
    std::string title,
    std::vector<std::string> options,
    StringListSelectSink on_select,
    StringListCancelSink on_cancel,
    StringListSelectorOptions selector_options)
    : theme_(theme),
      keybindings_(std::move(keybindings)),
      title_(std::move(title)),
      options_(std::move(options)),
      on_select_(std::move(on_select)),
      on_cancel_(std::move(on_cancel)),
      selector_options_(std::move(selector_options)) {}

support::Expected<cch::tui::RenderResult> StringListSelector::render(std::size_t width) {
    cch::tui::RenderResult result;
    const auto append = [&result, width](cch::tui::Component& component) -> support::ExpectedVoid {
        auto rendered = component.render(width);
        if (!rendered) return std::unexpected(rendered.error());
        for (auto& line : rendered->lines) result.lines.push_back(std::move(line));
        return {};
    };

    // pi's composition: border / spacer / bold accent title / spacer / list /
    // spacer / hints / spacer / border.
    DynamicBorder top_border(theme_.foreground_hook(ThemeToken::Border));
    if (auto appended = append(top_border); !appended) return std::unexpected(appended.error());
    {
        cch::tui::Text spacer("", 1, 0);
        if (auto appended = append(spacer); !appended) return std::unexpected(appended.error());
    }
    {
        cch::tui::Text title(
            theme_.foreground(ThemeToken::Accent, "\x1b[1m" + title_ + "\x1b[22m"), 1, 0);
        if (auto appended = append(title); !appended) return std::unexpected(appended.error());
    }
    {
        cch::tui::Text spacer("", 1, 0);
        if (auto appended = append(spacer); !appended) return std::unexpected(appended.error());
    }
    for (std::size_t index = 0; index < options_.size(); ++index) {
        const bool selected = index == selected_index_;
        const std::string line = selected
            ? theme_.foreground(ThemeToken::Accent, "→ ") +
                theme_.foreground(ThemeToken::Accent, options_[index])
            : "  " + theme_.foreground(ThemeToken::Text, options_[index]);
        cch::tui::Text item(line, 1, 0);
        if (auto appended = append(item); !appended) return std::unexpected(appended.error());
    }
    {
        cch::tui::Text spacer("", 1, 0);
        if (auto appended = append(spacer); !appended) return std::unexpected(appended.error());
    }
    {
        cch::tui::Text hints(
            raw_key_hint(theme_, "↑↓", "navigate") + "  " +
                key_hint(theme_, *keybindings_, "tui.select.confirm", "select") + "  " +
                key_hint(theme_, *keybindings_, "tui.select.cancel", "cancel"),
            1, 0);
        if (auto appended = append(hints); !appended) return std::unexpected(appended.error());
    }
    {
        cch::tui::Text spacer("", 1, 0);
        if (auto appended = append(spacer); !appended) return std::unexpected(appended.error());
    }
    DynamicBorder bottom_border(theme_.foreground_hook(ThemeToken::Border));
    if (auto appended = append(bottom_border); !appended) return std::unexpected(appended.error());
    return result;
}

void StringListSelector::handle_input(const cch::tui::InputEventVariant& input) {
    const auto* key = std::get_if<cch::tui::KeyEvent>(&input);
    if (key == nullptr || key->type == cch::tui::KeyEventType::Release) return;

    // pi's order: tools-expand, up (or "k"), down (or "j"), confirm, cancel.
    if (keybindings_->matches(*key, "app.tools.expand")) {
        if (selector_options_.on_toggle_tools_expanded) {
            selector_options_.on_toggle_tools_expanded();
        }
        return;
    }
    const bool plain = !key->ctrl && !key->alt && !key->shift;
    if (keybindings_->matches(*key, "tui.select.up") || (plain && key->key == "k")) {
        selected_index_ = selected_index_ == 0 ? 0 : selected_index_ - 1;
        return;
    }
    if (keybindings_->matches(*key, "tui.select.down") || (plain && key->key == "j")) {
        selected_index_ = std::min(options_.empty() ? 0 : options_.size() - 1, selected_index_ + 1);
        return;
    }
    if (keybindings_->matches(*key, "tui.select.confirm")) {
        if (selected_index_ < options_.size() && on_select_) {
            on_select_(options_[selected_index_]);
        }
        return;
    }
    if (keybindings_->matches(*key, "tui.select.cancel")) {
        if (on_cancel_) on_cancel_();
        return;
    }
}

} // namespace cch::coding_agent::tui
