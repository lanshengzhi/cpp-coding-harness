#include "SlashCommandEffects.hpp"

#include "coding_agent/AgentSession.hpp"
#include "coding_agent/tui/KeybindingsManager.hpp"

#include <cch/tui/Component.hpp>
#include <cch/tui/Keybindings.hpp>
#include <cch/tui/Overlay.hpp>

#include <exception>
#include <format>
#include <optional>
#include <string>
#include <utility>

namespace cch::coding_agent::tui {
namespace {

using ActionSink = std::move_only_function<void()>;

/// The `/hotkeys` overlay content wrapper: renders the help view and cancels
/// through the host sink on `tui.select.cancel`.
class DismissibleView final
    : public cch::tui::Component,
      public cch::tui::InputHandler,
      public cch::tui::Focusable {
public:
    DismissibleView(
        std::unique_ptr<cch::tui::Component> content,
        std::shared_ptr<const cch::tui::KeybindingRegistry> keybindings,
        ActionSink on_cancel)
        : content_(std::move(content)),
          keybindings_(std::move(keybindings)),
          on_cancel_(std::move(on_cancel)) {}
    DismissibleView(DismissibleView&&) = delete;
    DismissibleView& operator=(DismissibleView&&) = delete;
    ~DismissibleView() override = default;

    DismissibleView(const DismissibleView&) = delete;
    DismissibleView& operator=(const DismissibleView&) = delete;

    [[nodiscard]] support::Expected<cch::tui::RenderResult> render(std::size_t width) override {
        if (callback_error_) return std::unexpected(*callback_error_);
        return content_->render(width);
    }
    void invalidate() override { content_->invalidate(); }
    void handle_input(const cch::tui::InputEventVariant& input) override {
        const auto* key = std::get_if<cch::tui::KeyEvent>(&input);
        if (key == nullptr || key->type == cch::tui::KeyEventType::Release ||
            !keybindings_->matches(*key, "tui.select.cancel") || !on_cancel_) {
            return;
        }
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
        try {
#endif
            on_cancel_();
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
        } catch (const std::exception& error) {
            callback_error_ = support::make_error(
                support::ErrorCode::Unknown,
                "Hotkey help cancellation failed",
                error.what());
        } catch (...) {
            callback_error_ = support::make_error(
                support::ErrorCode::Unknown,
                "Hotkey help cancellation failed");
        }
#endif
    }
    [[nodiscard]] bool accepts_key_releases() const override { return false; }
    void set_focused(bool focused) override { focused_ = focused; }
    [[nodiscard]] bool focused() const override { return focused_; }
    [[nodiscard]] std::optional<cch::tui::CursorPosition> cursor_location() const override {
        return std::nullopt;
    }

private:
    std::unique_ptr<cch::tui::Component> content_;
    std::shared_ptr<const cch::tui::KeybindingRegistry> keybindings_;
    ActionSink on_cancel_;
    std::optional<support::Error> callback_error_;
    bool focused_{false};
};

} // namespace

std::string format_session_info(const coding_agent::AgentSession& session) {
    // pi `handleSessionCommand` shape: Name (when set), File, ID, the
    // Messages breakdown, and the Tokens totals. Workspace/provider/model
    // are not pi fields and are intentionally absent (strict subset).
    const auto name = session.session_name();
    const auto path = session.session_path();
    const auto stats = session.session_stats();
    std::string info = "Session Info\n\n";
    if (name && !name->empty()) {
        info += std::format("Name: {}\n", *name);
    }
    info += std::format("File: {}\n", path ? path->string() : std::string{"In-memory"});
    info += std::format("ID: {}\n\n", session.session_id());
    info += "Messages\n";
    info += std::format("Total: {}\n", stats.total_messages);
    info += std::format("User: {}\n", stats.user_messages);
    info += std::format("Assistant: {}\n", stats.assistant_messages);
    info += std::format("Tools: {} calls, {} results\n", stats.tool_calls, stats.tool_results);
    info += "\nTokens\n";
    // pi: "Input" is the full prompt volume (input + cached + written);
    // the C++ subset renders the provider-independent split.
    const auto prompt_tokens = stats.input_tokens + stats.cache_read + stats.cache_write;
    info += std::format("Input: {}\n", prompt_tokens);
    if (prompt_tokens > 0 && (stats.cache_read > 0 || stats.cache_write > 0)) {
        info += std::format("Cached: {}\n", stats.cache_read);
        info += std::format("Uncached: {}\n", stats.input_tokens + stats.cache_write);
    }
    info += std::format("Output: {}\n", stats.output_tokens);
    info += std::format("Total: {}\n", prompt_tokens + stats.output_tokens);
    return info;
}

support::Expected<std::unique_ptr<cch::tui::Overlay>> make_hotkeys_overlay(
    std::shared_ptr<const cch::tui::KeybindingRegistry> keybindings,
    std::move_only_function<void()> on_cancel) {
    cch::tui::OverlayOptions options;
    options.position = cch::tui::OverlayPosition::TopLeft;
    options.size_constraints.max_width = 90;
    options.size_constraints.max_height = 26;
    options.z_index = 100;
    auto overlay = std::make_unique<cch::tui::Overlay>(std::move(options));
    auto content = std::make_unique<DismissibleView>(
        make_hotkey_help_view(keybindings),
        std::move(keybindings),
        std::move(on_cancel));
    if (auto attached = overlay->add_child(std::move(content)); !attached) {
        return std::unexpected(attached.error());
    }
    return overlay;
}

} // namespace cch::coding_agent::tui
