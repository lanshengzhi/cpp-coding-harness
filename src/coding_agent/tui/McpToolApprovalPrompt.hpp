#pragma once

#include <cch/coding_agent/McpToolApproval.hpp>
#include <cch/support/Error.hpp>
#include <cch/tui/Component.hpp>
#include <cch/tui/Keybindings.hpp>
#include <cch/tui/SelectList.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace cch::coding_agent::tui {

class LiveTheme;

using McpToolApprovalAllowSink = std::move_only_function<void()>;
using McpToolApprovalDenySink = std::move_only_function<void()>;
using McpToolApprovalCancelSink = std::move_only_function<void()>;

/// The Native TUI's call-approval prompt for an `approval: "ask"` Upstream MCP
/// Server (issue #843).
///
/// It renders the question in full: the Qualified Tool Name the model called,
/// the Server Id and Upstream tool name it targets, and the call's arguments as
/// the JSON that would be sent. Consent is per call, so the component offers
/// exactly two answers and no "always allow" — a decision the user makes about
/// this call, not about the server.
///
/// The rows, the confirm/cancel keys, and the dismissal are the shared
/// `cch::tui::SelectList`; the component owns the chrome around them because
/// the call details are what make the question answerable.
///
/// Threading: constructed and driven on the TUI thread, like every other
/// prompt-slot component.
class McpToolApprovalPromptComponent final : public cch::tui::Component,
                                             public cch::tui::InputHandler,
                                             public cch::tui::Focusable {
public:
    /// `on_cancel` fires for a dismissal (Escape, or the host resolving the
    /// prompt on close): a dismissal is not a decision, and the caller records
    /// it as one.
    McpToolApprovalPromptComponent(const LiveTheme& theme,
            std::shared_ptr<const cch::tui::KeybindingRegistry> keybindings,
            coding_agent::McpToolApprovalRequest request,
            McpToolApprovalAllowSink on_allow,
            McpToolApprovalDenySink on_deny,
            McpToolApprovalCancelSink on_cancel);
    McpToolApprovalPromptComponent(McpToolApprovalPromptComponent&&) = delete;
    McpToolApprovalPromptComponent& operator=(McpToolApprovalPromptComponent&&) = delete;
    ~McpToolApprovalPromptComponent() override = default;
    McpToolApprovalPromptComponent(const McpToolApprovalPromptComponent&) = delete;
    McpToolApprovalPromptComponent& operator=(const McpToolApprovalPromptComponent&) = delete;

    [[nodiscard]] support::Expected<cch::tui::RenderResult> render(std::size_t width) override;
    void invalidate() override;
    cch::tui::InputAdmissionOutcome handle_input(const cch::tui::InputEventVariant& input) override;
    void set_focused(bool focused) override;
    [[nodiscard]] bool focused() const override;
    /// The SelectList's cursor translated into this component's own line
    /// coordinates; reported only when focused, rendered, and the list itself
    /// reports a cursor.
    [[nodiscard]] std::optional<cch::tui::CursorPosition> cursor_location() const override;

    /// The value the highlighted row answers: `allow` or `deny`. A call with
    /// no rows highlighted has no answer.
    [[nodiscard]] static std::string_view allow_value() noexcept { return "allow"; }
    [[nodiscard]] static std::string_view deny_value() noexcept { return "deny"; }

private:
    const LiveTheme& theme_; // must outlive this component.
    std::shared_ptr<const cch::tui::KeybindingRegistry> keybindings_;
    coding_agent::McpToolApprovalRequest request_;
    McpToolApprovalAllowSink on_allow_;
    McpToolApprovalDenySink on_deny_;
    McpToolApprovalCancelSink on_cancel_;
    cch::tui::SelectList select_list_;
    /// Rows this component emitted above the SelectList in the last successful
    /// render; `cursor_location` adds it to the list's row.
    std::size_t cursor_row_offset_{0};
    bool focused_{false};
};

} // namespace cch::coding_agent::tui
