#pragma once

#include "coding_agent/tui/PromptSlot.hpp"

#include <cch/tui/Component.hpp>
#include <cch/tui/Input.hpp>
#include <cch/tui/Keybindings.hpp>
#include <cch/util/Error.hpp>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <variant>
#include <vector>

namespace cch::coding_agent::tui {

class LiveTheme;

using OpenBrowserSink = std::move_only_function<void(std::string)>;
using LoginDialogActionSink = std::move_only_function<void()>;

/// The login dialog (pi `login-dialog.ts`): replaces the editor during a
/// login flow and renders the auth URL / device code / manual code / prompt /
/// info / waiting / progress views as the provider's Auth Interaction events
/// arrive. The dialog owns the `std::stop_source` whose token the flow hands
/// to the Auth Interaction (pi: the dialog owns the AbortController);
/// Esc/Ctrl+C requests stop and rejects any pending prompt with the stable
/// cancelled error (message "Login cancelled").
///
/// Threading: view methods may run on the login-flow executor while render
/// and input handling run on the TUI threads; a mutex serializes the content
/// model. Sinks fire outside the lock.
class LoginDialogComponent final
    : public cch::tui::Component,
      public cch::tui::InputHandler,
      public cch::tui::Focusable {
public:
    LoginDialogComponent(
        const LiveTheme& theme,
        std::shared_ptr<const cch::tui::KeybindingRegistry> keybindings,
        std::string title,
        LoginDialogActionSink on_invalidate,
        OpenBrowserSink on_open_browser,
        LoginDialogActionSink on_cancel = {});
    LoginDialogComponent(LoginDialogComponent&&) = delete;
    LoginDialogComponent& operator=(LoginDialogComponent&&) = delete;
    ~LoginDialogComponent() override = default;
    LoginDialogComponent(const LoginDialogComponent&) = delete;
    LoginDialogComponent& operator=(const LoginDialogComponent&) = delete;

    /// The dialog-owned stop token (pi `dialog.signal`): handed to the Auth
    /// Interaction so provider flows observe the user's cancellation.
    [[nodiscard]] std::stop_token stop_token() const noexcept {
        return stop_source_.get_token();
    }

    // ── Views (pi method names) ──────────────────────────────────────────

    /// Auth URL view (pi `showAuth`): hyperlink + click hint + optional
    /// warning instructions; opens the browser best-effort. Clears content.
    void show_auth(std::string url, std::optional<std::string> instructions);
    /// Device code view (pi `showDeviceCode`): verification URI + user code.
    /// Clears content.
    void show_device_code(std::string user_code, std::string verification_uri);
    /// Info view (pi `showInfo`): message + optional links + optional close
    /// hint. Appends without clearing.
    void show_info(
        std::string message,
        std::vector<std::pair<std::string, std::optional<std::string>>> links = {},
        bool show_close_hint = false);
    /// Waiting view (pi `showWaiting`): dim message + cancel hint. Appends.
    void show_waiting(std::string message);
    /// Progress view (pi `showProgress`): one dim line. Appends.
    void show_progress(std::string message);

    /// Prompt view (pi `showPrompt`): message + optional `e.g.,` placeholder
    /// + input + cancel/submit hints. Appends without clearing (preserves a
    /// previously shown URL). Resolves with the submitted text; rejects with
    /// the stable cancelled error when the dialog or this prompt is
    /// cancelled.
    [[nodiscard]] boost::asio::awaitable<util::Expected<std::string>> show_prompt(
        std::string message,
        std::optional<std::string> placeholder);
    /// Manual-code input view (pi `showManualInput`): dim prompt + input +
    /// cancel hint. Same resolution contract as show_prompt.
    [[nodiscard]] boost::asio::awaitable<util::Expected<std::string>> show_manual_input(
        std::string prompt);

    /// Reject only the pending prompt with the stable cancelled error (pi's
    /// per-prompt `manualAbort`: the Codex callback-vs-manual-input race).
    /// The dialog's stop source and cancel sink are NOT fired.
    void cancel_pending_prompt();

    [[nodiscard]] util::Expected<cch::tui::RenderResult> render(std::size_t width) override;
    void invalidate() override;
    void handle_input(const cch::tui::InputEventVariant& input) override;
    [[nodiscard]] bool accepts_key_releases() const override { return false; }
    void set_focused(bool focused) override;
    [[nodiscard]] bool focused() const override;
    [[nodiscard]] std::optional<cch::tui::CursorPosition> cursor_location() const override;

private:
    struct SpacerItem {};
    struct TextItem {
        std::string content;
    };
    struct InputSlotItem {};
    using ContentItem = std::variant<SpacerItem, TextItem, InputSlotItem>;

    [[nodiscard]] boost::asio::awaitable<util::Expected<std::string>> run_prompt(
        std::vector<ContentItem> items);
    /// Locked-section cancel: request stop, reject the pending slot, and
    /// report whether the cancel sink should fire.
    void cancel();

    const LiveTheme& theme_; // must outlive this component.
    std::shared_ptr<const cch::tui::KeybindingRegistry> keybindings_;
    std::string title_;
    LoginDialogActionSink on_invalidate_;
    OpenBrowserSink on_open_browser_;
    LoginDialogActionSink on_cancel_;
    std::stop_source stop_source_;

    mutable std::mutex mutex_;
    std::vector<ContentItem> content_;
    cch::tui::Input input_;
    bool input_visible_{false};
    /// The awaiting prompt slot (the shared `PromptSlot` in PromptSlot.hpp).
    std::shared_ptr<PromptSlot> pending_slot_;
    // Set by the input's sinks while handle_input holds the mutex; processed
    // before the lock is released so the sinks never re-enter the dialog.
    std::optional<std::string> pending_submit_;
    bool pending_escape_{false};
    std::optional<cch::tui::CursorPosition> cursor_cache_;
};

} // namespace cch::coding_agent::tui
