#include "coding_agent/tui/StatusIndicator.hpp"

#include "coding_agent/tui/KeybindingHints.hpp"

#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace cch::coding_agent::tui {

StatusIndicator::StatusIndicator(
    Kind kind,
    const LiveTheme& theme,
    cch::tui::RenderRequestSink request_render,
    std::string message)
    : kind_(kind),
      theme_(theme),
      loader_(std::make_unique<cch::tui::Loader>(cch::tui::LoaderOptions{
          .request_render = std::move(request_render),
          .spinner_style = kind == Kind::Retry
              ? theme.foreground_hook(ThemeToken::Warning)
              : theme.foreground_hook(ThemeToken::Accent),
          .message_style = theme.foreground_hook(ThemeToken::Muted),
          .message = std::move(message),
      })) {
    loader_->start();
}

void StatusIndicator::set_message(std::string message) {
    loader_->set_message(std::move(message));
}

util::Expected<cch::tui::RenderResult> StatusIndicator::render(std::size_t width) {
    return loader_->render(width);
}

void StatusIndicator::invalidate() {
    loader_->invalidate();
}

std::string working_status_message(std::string message) {
    // pi WorkingStatusIndicator: the plain "Working..." message (the
    // interrupt hint appears in the chat status line, not the indicator).
    return message;
}

std::string retry_status_message(
    const cch::tui::KeybindingRegistry& keybindings,
    int attempt,
    int max_attempts,
    int seconds) {
    // pi keyText: the raw key text ("escape"), not the capitalized display
    // form.
    const auto cancel_key = keybindings.key_text("app.interrupt");
    return std::format(
        "Retrying ({}/{}) in {}s... ({} to cancel)",
        attempt,
        max_attempts,
        seconds,
        cancel_key.empty() ? "Unbound" : cancel_key);
}

std::string compaction_status_message(
    const cch::tui::KeybindingRegistry& keybindings,
    std::string_view reason) {
    // pi keyText: the raw key text ("escape"), not the capitalized display
    // form.
    const auto cancel_key = keybindings.key_text("app.interrupt");
    const auto hint = std::format(
        "({} to cancel)",
        cancel_key.empty() ? "Unbound" : cancel_key);
    if (reason == "manual") {
        return "Compacting context... " + hint;
    }
    return reason == "overflow"
        ? "Context overflow detected, Auto-compacting... " + hint
        : "Auto-compacting... " + hint;
}

} // namespace cch::coding_agent::tui
