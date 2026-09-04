#include "LoginDialog.hpp"

#include "DynamicBorder.hpp"
#include "KeybindingHints.hpp"
#include "Theme.hpp"

#include <cch/tui/Text.hpp>

#include <cch/support/Error.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <utility>

namespace cch::coding_agent::tui {
namespace {

/// pi's stable login-cancellation error (the recorded C++ enrichment: the
/// kind travels on the error so the frontend suppresses on kind, not string).
[[nodiscard]] support::Error login_cancelled_error() {
    return support::make_error(support::ErrorCode::Cancelled, "Login cancelled");
}

/// pi's OSC 8 hyperlink wrapper.
[[nodiscard]] std::string hyperlink(std::string_view url, std::string_view text) {
    return "\x1b]8;;" + std::string{url} + "\x07" + std::string{text} + "\x1b]8;;\x07";
}

[[nodiscard]] std::string click_hint() {
    return "Ctrl+click to open";
}

} // namespace

LoginDialogComponent::LoginDialogComponent(
    const LiveTheme& theme,
    std::shared_ptr<const cch::tui::KeybindingRegistry> keybindings,
    std::string title,
    LoginDialogActionSink on_invalidate,
    OpenBrowserSink on_open_browser,
    LoginDialogActionSink on_cancel)
    : theme_(theme),
      keybindings_(std::move(keybindings)),
      title_(std::move(title)),
      on_invalidate_(std::move(on_invalidate)),
      on_open_browser_(std::move(on_open_browser)),
      on_cancel_(std::move(on_cancel)),
      input_(
          cch::tui::InputOptions{.keybindings = keybindings_},
          [this](std::string value) -> support::ExpectedVoid {
              pending_submit_ = std::move(value);
              return {};
          },
          [this]() -> support::ExpectedVoid {
              pending_escape_ = true;
              return {};
          }) {}

void LoginDialogComponent::show_auth(
    std::string url,
    std::optional<std::string> instructions) {
    {
        std::lock_guard lock(mutex_);
        content_.clear();
        input_visible_ = false;
        content_.emplace_back(SpacerItem{});
        content_.emplace_back(TextItem{
            theme_.foreground(ThemeToken::Accent, hyperlink(url, url))});
        content_.emplace_back(TextItem{
            theme_.foreground(ThemeToken::Dim, hyperlink(url, click_hint()))});
        if (instructions) {
            content_.emplace_back(SpacerItem{});
            content_.emplace_back(TextItem{
                theme_.foreground(ThemeToken::Warning, *instructions)});
        }
    }
    // pi: best-effort browser open with the presented URL, never via a shell.
    if (on_open_browser_) on_open_browser_(std::move(url));
    if (on_invalidate_) on_invalidate_();
}

void LoginDialogComponent::show_device_code(
    std::string user_code,
    std::string verification_uri) {
    {
        std::lock_guard lock(mutex_);
        content_.clear();
        input_visible_ = false;
        content_.emplace_back(SpacerItem{});
        content_.emplace_back(TextItem{
            theme_.foreground(ThemeToken::Accent, hyperlink(verification_uri, verification_uri))});
        content_.emplace_back(TextItem{
            theme_.foreground(ThemeToken::Dim, hyperlink(verification_uri, click_hint()))});
        content_.emplace_back(SpacerItem{});
        content_.emplace_back(TextItem{
            theme_.foreground(ThemeToken::Warning, "Enter code: " + user_code)});
    }
    if (on_invalidate_) on_invalidate_();
}

void LoginDialogComponent::show_info(
    std::string message,
    std::vector<std::pair<std::string, std::optional<std::string>>> links,
    bool show_close_hint) {
    {
        std::lock_guard lock(mutex_);
        content_.emplace_back(SpacerItem{});
        content_.emplace_back(TextItem{theme_.foreground(ThemeToken::Text, message)});
        for (const auto& [url, label] : links) {
            const std::string text = label ? *label + ": " + url : url;
            content_.emplace_back(TextItem{
                theme_.foreground(ThemeToken::Accent, hyperlink(url, text))});
        }
        if (show_close_hint) {
            content_.emplace_back(SpacerItem{});
            content_.emplace_back(TextItem{
                "(" + key_hint(theme_, *keybindings_, "tui.select.cancel", "to close") + ")"});
        }
    }
    if (on_invalidate_) on_invalidate_();
}

void LoginDialogComponent::show_waiting(std::string message) {
    {
        std::lock_guard lock(mutex_);
        content_.emplace_back(SpacerItem{});
        content_.emplace_back(TextItem{theme_.foreground(ThemeToken::Dim, message)});
        content_.emplace_back(TextItem{
            "(" + key_hint(theme_, *keybindings_, "tui.select.cancel", "to cancel") + ")"});
    }
    if (on_invalidate_) on_invalidate_();
}

void LoginDialogComponent::show_progress(std::string message) {
    {
        std::lock_guard lock(mutex_);
        content_.emplace_back(TextItem{theme_.foreground(ThemeToken::Dim, message)});
    }
    if (on_invalidate_) on_invalidate_();
}

boost::asio::awaitable<support::Expected<std::string>> LoginDialogComponent::show_prompt(
    std::string message,
    std::optional<std::string> placeholder) {
    // pi `showPrompt`: appends (preserving a previously shown URL), then
    // clears the input value.
    std::vector<ContentItem> items;
    items.emplace_back(SpacerItem{});
    items.emplace_back(TextItem{theme_.foreground(ThemeToken::Text, message)});
    if (placeholder) {
        items.emplace_back(TextItem{
            theme_.foreground(ThemeToken::Dim, "e.g., " + *placeholder)});
    }
    items.emplace_back(InputSlotItem{});
    items.emplace_back(TextItem{
        "(" + key_hint(theme_, *keybindings_, "tui.select.cancel", "to cancel,") + " " +
        key_hint(theme_, *keybindings_, "tui.select.confirm", "to submit") + ")"});
    co_return co_await run_prompt(std::move(items));
}

boost::asio::awaitable<support::Expected<std::string>> LoginDialogComponent::show_manual_input(
    std::string prompt) {
    // pi `showManualInput`: clear the input, append the dim prompt, input,
    // and the cancel hint.
    std::vector<ContentItem> items;
    items.emplace_back(SpacerItem{});
    items.emplace_back(TextItem{theme_.foreground(ThemeToken::Dim, prompt)});
    items.emplace_back(InputSlotItem{});
    items.emplace_back(TextItem{
        "(" + key_hint(theme_, *keybindings_, "tui.select.cancel", "to cancel") + ")"});
    co_return co_await run_prompt(std::move(items));
}

boost::asio::awaitable<support::Expected<std::string>> LoginDialogComponent::run_prompt(
    std::vector<ContentItem> items) {
    const auto executor = co_await boost::asio::this_coro::executor;
    auto slot = std::make_shared<PromptSlot>(executor);
    {
        std::lock_guard lock(mutex_);
        for (auto& item : items) content_.push_back(std::move(item));
        input_.set_value("");
        input_visible_ = true;
        pending_slot_ = slot;
    }
    if (on_invalidate_) on_invalidate_();

    boost::system::error_code error;
    auto result = co_await slot->channel.async_receive(
        boost::asio::redirect_error(boost::asio::use_awaitable, error));
    if (error) {
        co_return std::unexpected(support::make_error(
            support::ErrorCode::Unknown,
            "login prompt channel failed",
            error.message()));
    }
    co_return std::move(result);
}

void LoginDialogComponent::cancel_pending_prompt() {
    std::shared_ptr<PromptSlot> slot;
    {
        std::lock_guard lock(mutex_);
        slot = std::move(pending_slot_);
        pending_slot_.reset();
    }
    if (slot) slot->resolve(std::unexpected(login_cancelled_error()));
}

void LoginDialogComponent::cancel() {
    std::shared_ptr<PromptSlot> slot;
    {
        std::lock_guard lock(mutex_);
        slot = std::move(pending_slot_);
        pending_slot_.reset();
    }
    // pi cancel(): abort the flow (the provider observes the stop token),
    // reject the pending prompt, and report completion to the host.
    (void)stop_source_.request_stop();
    if (slot) slot->resolve(std::unexpected(login_cancelled_error()));
    if (on_cancel_) on_cancel_();
    if (on_invalidate_) on_invalidate_();
}

support::Expected<cch::tui::RenderResult> LoginDialogComponent::render(std::size_t width) {
    std::lock_guard lock(mutex_);
    cch::tui::RenderResult result;
    const auto append = [&result, width](cch::tui::Component& component) -> support::ExpectedVoid {
        auto rendered = component.render(width);
        if (!rendered) return std::unexpected(rendered.error());
        for (auto& line : rendered->lines) result.lines.push_back(std::move(line));
        return {};
    };
    const auto append_text = [&append](const std::string& content) -> support::ExpectedVoid {
        cch::tui::Text line(content, 1, 0);
        return append(line);
    };

    // pi's composition: border / bold accent title / content / border.
    DynamicBorder top_border(theme_.foreground_hook(ThemeToken::Border));
    if (auto appended = append(top_border); !appended) return std::unexpected(appended.error());
    if (auto appended = append_text(
            theme_.foreground(ThemeToken::Accent, "\x1b[1m" + title_ + "\x1b[22m"));
        !appended) {
        return std::unexpected(appended.error());
    }

    cursor_cache_.reset();
    for (const auto& item : content_) {
        if (std::holds_alternative<SpacerItem>(item)) {
            if (auto appended = append_text(""); !appended) {
                return std::unexpected(appended.error());
            }
            continue;
        }
        if (const auto* text = std::get_if<TextItem>(&item)) {
            if (auto appended = append_text(text->content); !appended) {
                return std::unexpected(appended.error());
            }
            continue;
        }
        // The input slot: render the persistent Input and cache its cursor
        // row for the focus lifecycle.
        if (input_visible_) {
            auto cursor = input_.cursor_location();
            auto rendered = input_.render(width);
            if (!rendered) return std::unexpected(rendered.error());
            if (cursor) {
                cursor_cache_ = cch::tui::CursorPosition{
                    .column = cursor->column,
                    .row = result.lines.size() + cursor->row,
                };
            }
            for (auto& line : rendered->lines) result.lines.push_back(std::move(line));
        }
    }

    DynamicBorder bottom_border(theme_.foreground_hook(ThemeToken::Border));
    if (auto appended = append(bottom_border); !appended) return std::unexpected(appended.error());
    return result;
}

void LoginDialogComponent::invalidate() {
    std::lock_guard lock(mutex_);
    input_.invalidate();
}

cch::tui::InputAdmissionOutcome LoginDialogComponent::handle_input(const cch::tui::InputEventVariant& input) {
    const auto* key = std::get_if<cch::tui::KeyEvent>(&input);
    if (key == nullptr || key->type == cch::tui::KeyEventType::Release)
        return cch::tui::InputAdmissionOutcome::Unhandled;
    if (keybindings_->matches(*key, "tui.select.cancel")) {
        cancel();
        return cch::tui::InputAdmissionOutcome::Consumed;
    }

    std::shared_ptr<PromptSlot> slot;
    std::string submitted;
    bool escape = false;
    {
        std::lock_guard lock(mutex_);
        if (input_visible_) {
            // The Input's sinks only record into members here; they run
            // synchronously under this lock and never re-enter the dialog.
            static_cast<void>(input_.handle_input(input));
        }
        if (pending_escape_) {
            pending_escape_ = false;
            escape = true;
        }
        if (pending_submit_ && pending_slot_) {
            submitted = std::move(*pending_submit_);
            slot = std::move(pending_slot_);
            pending_slot_.reset();
            // pi replaceInputWithSubmittedText: the input freezes into the
            // echoed submission line.
            for (auto& item : content_) {
                if (std::holds_alternative<InputSlotItem>(item)) {
                    item = TextItem{"> " + submitted};
                }
            }
            input_visible_ = false;
        }
        pending_submit_.reset();
    }
    if (escape) {
        cancel();
        return cch::tui::InputAdmissionOutcome::Consumed;
    }
    if (slot) {
        slot->resolve(std::move(submitted));
        if (on_invalidate_) on_invalidate_();
    }
    return cch::tui::InputAdmissionOutcome::Consumed;
}

void LoginDialogComponent::set_focused(bool focused) {
    std::lock_guard lock(mutex_);
    input_.set_focused(focused);
}

bool LoginDialogComponent::focused() const {
    std::lock_guard lock(mutex_);
    return input_.focused();
}

std::optional<cch::tui::CursorPosition> LoginDialogComponent::cursor_location() const {
    std::lock_guard lock(mutex_);
    return cursor_cache_;
}

} // namespace cch::coding_agent::tui
