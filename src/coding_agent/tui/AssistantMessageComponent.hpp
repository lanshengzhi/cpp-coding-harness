#pragma once

#include <cch/ai/Message.hpp>
#include <cch/tui/Component.hpp>
#include <cch/tui/Container.hpp>
#include <cch/support/Error.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>

namespace cch::coding_agent::tui {

class LiveTheme;

/// pi `assistant-message.ts`: one assistant message. Text blocks render as
/// markdown without a background, consecutive thinking runs render as one
/// italic `thinkingText` section (or the hidden-thinking label), terminal
/// `length`/`aborted`/`error` stop reasons render pi's notices, and OSC 133
/// A/B/C prompt zones wrap the block unless the message carries tool calls.
class AssistantMessageComponent final : public cch::tui::Component {
public:
    /// The theme must outlive this component.
    AssistantMessageComponent(
        const LiveTheme& theme,
        bool hide_thinking_block = false,
        std::string hidden_thinking_label = "Thinking...",
        std::size_t output_pad = 1);
    ~AssistantMessageComponent() override;

    AssistantMessageComponent(const AssistantMessageComponent&) = delete;
    AssistantMessageComponent& operator=(const AssistantMessageComponent&) = delete;

    void update_content(const ai::AssistantMessage& message);
    void set_hide_thinking_block(bool hide);
    void set_hidden_thinking_label(std::string label);
    void set_output_pad(std::size_t output_pad);
    /// Whether the message carries tool calls (pi suppresses the OSC zones
    /// and notices when tools render separately).
    [[nodiscard]] bool has_tool_calls() const;

    /// Number of closed blocks that are frozen in cache.
    [[nodiscard]] std::size_t frozen_block_count() const;
    /// Whether there is currently an open trailing segment.
    [[nodiscard]] bool has_open_tail() const;
    /// The unparsed active trailing text.
    [[nodiscard]] std::string_view open_tail_text() const;

    [[nodiscard]] support::Expected<cch::tui::RenderResult> render(std::size_t width) override;
    void invalidate() override;

private:
    struct FrozenBlock {
        std::string text;
        std::vector<std::string> lines{};
        std::size_t rendered_width{0};
    };

    void rebuild();
    void update_prefix_and_suffix();
    void update_text();

    const LiveTheme& theme_; // must outlive this component.
    bool hide_thinking_block_{false};
    std::string hidden_thinking_label_;
    std::size_t output_pad_{1};
    std::optional<ai::AssistantMessage> message_;
    bool has_tool_calls_{false};

    cch::tui::Container prefix_content_;
    cch::tui::Container suffix_content_;

    std::vector<FrozenBlock> frozen_blocks_;
    std::string active_tail_text_;
    std::size_t consumed_text_bytes_{0};
    std::string accumulated_text_;
    std::unique_ptr<cch::tui::Component> active_tail_markdown_;
};

} // namespace cch::coding_agent::tui
