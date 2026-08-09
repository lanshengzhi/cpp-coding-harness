#pragma once

#include "coding_agent/tui/SharedKeybindings.hpp"

#include <cch/agent/AgentEvent.hpp>
#include <cch/ai/Message.hpp>
#include <cch/coding_agent/AgentSessionSnapshot.hpp>
#include <cch/tui/Component.hpp>
#include <cch/util/Error.hpp>

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace cch::coding_agent::tui {

class LiveTheme;

/// The chat container of the pi main-screen composition: one ordered
/// sequence of pi-shaped message components (user-message, assistant-message,
/// tool-execution, bash-execution, custom/compaction/branch summaries) plus
/// frontend notices and diagnostics. Owns the streaming assistant update
/// path and the tool-call/result settlement that pi's interactive-mode chat
/// container performs.
class ChatContainer final : public cch::tui::Component {
public:
    /// The theme must outlive this container; keybindings resolve through
    /// the shared slot (ADR 0035, #418).
    explicit ChatContainer(
        const LiveTheme& theme,
        std::shared_ptr<const SharedKeybindings> keybindings);
    ChatContainer(ChatContainer&&) noexcept;
    ChatContainer& operator=(ChatContainer&&) noexcept;
    ~ChatContainer() override;

    ChatContainer(const ChatContainer&) = delete;
    ChatContainer& operator=(const ChatContainer&) = delete;

    void initialize(const AgentSessionSnapshot& snapshot);
    void apply_event(const agent::AgentLifecycleEvent& event);
    /// Append one runtime-confirmed passive message that has no lifecycle event.
    void append_committed_message(ai::MessageVariant message);
    void clear();
    void append_frontend_message(std::string text);
    void append_diagnostic(std::string text);
    /// Append one pi-shaped boot warning (`Warning: <text>`, warning token) —
    /// the `modelFallbackMessage` presentation of interactive boot.
    void append_warning(std::string text);
    /// Append pi's untrusted-project boot warning (`renderProjectTrustWarningIfNeeded`):
    /// a Spacer row above a plain warning-token line (no `Warning:` prefix).
    void append_trust_warning(std::string text);
    /// Append or replace one pi `showStatus` dim status line: the status is
    /// replaced while it is still the newest chat item, matching pi's live
    /// tail-status behavior.
    void append_status_message(std::string text);
    /// Append one User Bash diagnostic. ADR 0028: User Bash command, output,
    /// and error-diagnostic values pass through raw like pi — bounded, never
    /// redacted.
    void append_user_bash_diagnostic(std::string text);
    void toggle_tool_output();
    /// Whether tool and User Bash output currently renders in full.
    [[nodiscard]] bool tools_expanded() const;

    /// pi `hideThinkingBlock` render setting: update the flag and apply it to
    /// every rendered assistant message in place (pi
    /// `AssistantMessageComponent.setHideThinkingBlock`).
    void set_hide_thinking_block(bool hide);
    /// pi `outputPad` render setting: update the padding and rebuild the
    /// user/assistant message components so the new padding applies (pi
    /// `setOutputPad` + `rebuildChatFromMessages`).
    void set_output_pad(std::size_t output_pad);

    [[nodiscard]] util::Expected<cch::tui::RenderResult> render(std::size_t width) override;
    void invalidate() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cch::coding_agent::tui
