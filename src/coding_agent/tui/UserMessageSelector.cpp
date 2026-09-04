#include "UserMessageSelector.hpp"

#include "Theme.hpp"

#include <cch/tui/Utils.hpp>

#include <cch/support/Error.hpp>
#include <algorithm>
#include <cctype>
#include <format>
#include <string>
#include <utility>
#include <vector>

namespace cch::coding_agent::tui {
namespace {

constexpr std::size_t kMaxVisibleMessages = 10;

[[nodiscard]] std::string border_rule(std::size_t width) {
    std::string rule;
    rule.reserve(width * 3);
    const auto count = width > 0 ? width : std::size_t{1};
    for (std::size_t index = 0; index < count; ++index) rule += "─";
    return rule;
}

[[nodiscard]] std::string normalize_message_text(std::string text) {
    for (auto& character : text) {
        if (character == '\n') {
            character = ' ';
        }
    }
    const auto not_space = [](unsigned char character) {
        return std::isspace(character) == 0;
    };
    text.erase(text.begin(), std::find_if(text.begin(), text.end(), not_space));
    text.erase(std::find_if(text.rbegin(), text.rend(), not_space).base(), text.end());
    return text;
}

} // namespace

UserMessageSelectorComponent::UserMessageSelectorComponent(
    const LiveTheme& theme,
    std::shared_ptr<const cch::tui::KeybindingRegistry> keybindings,
    std::vector<UserForkItem> messages,
    std::optional<std::string> initial_selected_id,
    UserMessageSelectSink on_select,
    UserMessageCancelSink on_cancel,
    UserMessageInvalidateSink on_invalidate)
    : theme_(theme),
      keybindings_(std::move(keybindings)),
      messages_(std::move(messages)),
      on_select_(std::move(on_select)),
      on_cancel_(std::move(on_cancel)),
      on_invalidate_(std::move(on_invalidate)) {
    // pi: start at the provided entry, else the most recent message.
    std::size_t initial_index = 0;
    if (initial_selected_id) {
        const auto found = std::find_if(
            messages_.begin(), messages_.end(),
            [&](const UserForkItem& message) {
                return message.entry_id == *initial_selected_id;
            });
        if (found != messages_.end()) {
            initial_index = static_cast<std::size_t>(found - messages_.begin());
        } else {
            initial_index = messages_.empty() ? 0 : messages_.size() - 1;
        }
    } else {
        initial_index = messages_.empty() ? 0 : messages_.size() - 1;
    }
    selected_index_ = initial_index;
}

cch::tui::InputAdmissionOutcome UserMessageSelectorComponent::handle_input(const cch::tui::InputEventVariant& input) {
    const auto* key = std::get_if<cch::tui::KeyEvent>(&input);
    if (!cch::tui::carries_press_behavior(key)) {
        return cch::tui::InputAdmissionOutcome::Unhandled;
    }
    if (keybindings_->matches(*key, "tui.select.up")) {
        if (!messages_.empty()) {
            selected_index_ = selected_index_ == 0
                ? messages_.size() - 1
                : selected_index_ - 1;
        }
        if (on_invalidate_) on_invalidate_();
        return cch::tui::InputAdmissionOutcome::Consumed;
    }
    if (keybindings_->matches(*key, "tui.select.down")) {
        if (!messages_.empty()) {
            selected_index_ = selected_index_ == messages_.size() - 1
                ? 0
                : selected_index_ + 1;
        }
        if (on_invalidate_) on_invalidate_();
        return cch::tui::InputAdmissionOutcome::Consumed;
    }
    if (keybindings_->matches(*key, "tui.select.confirm")) {
        if (!messages_.empty() && on_select_) {
            on_select_(messages_[selected_index_].entry_id);
        }
        return cch::tui::InputAdmissionOutcome::Consumed;
    }
    if (keybindings_->matches(*key, "tui.select.cancel")) {
        if (on_cancel_) {
            on_cancel_();
        }
        return cch::tui::InputAdmissionOutcome::Consumed;
    }
    return cch::tui::InputAdmissionOutcome::Consumed;
}

support::Expected<cch::tui::RenderResult> UserMessageSelectorComponent::render(
    std::size_t width) {
    std::vector<std::string> lines;
    lines.push_back("");
    lines.push_back(theme_.foreground(ThemeToken::Accent, "Fork from Message"));
    lines.push_back(theme_.foreground(
        ThemeToken::Muted,
        "Select a user message to copy the active path up to that point into a new session"));
    lines.push_back("");
    lines.push_back(theme_.foreground(ThemeToken::Border, border_rule(width)));
    lines.push_back("");

    if (messages_.empty()) {
        lines.push_back(theme_.foreground(ThemeToken::Muted, "  No user messages found"));
        lines.push_back("");
        lines.push_back(theme_.foreground(ThemeToken::Border, border_rule(width)));
        return cch::tui::RenderResult{.lines = std::move(lines)};
    }

    const auto start_index = std::max<std::size_t>(
        0,
        std::min(
            selected_index_ > kMaxVisibleMessages / 2
                ? selected_index_ - kMaxVisibleMessages / 2
                : 0,
            messages_.size() > kMaxVisibleMessages
                ? messages_.size() - kMaxVisibleMessages
                : 0));
    const auto end_index = std::min(
        start_index + kMaxVisibleMessages, messages_.size());

    for (std::size_t index = start_index; index < end_index; ++index) {
        const auto& message = messages_[index];
        const bool is_selected = index == selected_index_;
        const auto normalized = normalize_message_text(message.text);
        const auto max_width = width > 2 ? width - 2 : std::size_t{0};
        auto truncated = cch::tui::truncate_text(normalized, max_width, "…");
        if (!truncated) return std::unexpected(truncated.error());
        const auto cursor = is_selected
            ? theme_.foreground(ThemeToken::Accent, "› ")
            : "  ";
        auto message_line = cursor + (is_selected ? "\x1b[1m" + *truncated + "\x1b[22m" : *truncated);
        lines.push_back(std::move(message_line));

        const auto metadata = std::format(
            "  Message {} of {}", index + 1, messages_.size());
        lines.push_back(theme_.foreground(ThemeToken::Muted, metadata));
        lines.push_back("");
    }

    if (start_index > 0 || end_index < messages_.size()) {
        const auto scroll = std::format(
            "  ({}/{})", selected_index_ + 1, messages_.size());
        lines.push_back(theme_.foreground(
            ThemeToken::Muted,
            cch::tui::truncate_text(scroll, width, "").value_or(scroll)));
    }
    lines.push_back("");
    lines.push_back(theme_.foreground(ThemeToken::Border, border_rule(width)));
    return cch::tui::RenderResult{.lines = std::move(lines)};
}

} // namespace cch::coding_agent::tui
