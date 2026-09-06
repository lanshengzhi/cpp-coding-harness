#pragma once

#include <cch/ai/Message.hpp>
#include <cch/tui/Component.hpp>
#include <cch/tui/Container.hpp>
#include <cch/support/Error.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cch::coding_agent::tui {

class LiveTheme;

/// pi `assistant-message.ts`: one assistant message. Text and thinking blocks
/// render as markdown in exact content order; thinking runs use the italic
/// `thinkingText` style (or the hidden-thinking label), terminal
/// `length`/`aborted`/`error` stop reasons render pi's notices, and OSC 133
/// A/B/C prompt zones wrap the block unless the message carries tool calls.
///
/// Streaming is incremental (#603, ADR 0051 Block Frozen Protocol):
/// `update_content` consumes only appended deltas, so per-chunk work is
/// strictly O(1) relative to the accumulated stream length. Closed syntactic
/// blocks (paragraphs ending in a blank line, closed code fences, settled
/// list items) freeze into committed sections that render once and cache
/// their lines; only the open trailing segment is parsed dynamically.
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
    /// Streaming contract: successive messages for one stream are append-only
    /// (content blocks are appended; only the trailing text/thinking block
    /// grows). Callers must not rewrite settled blocks in place.
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
    enum class SectionKind {
        Text,
        Thinking,
    };

    struct FrozenBlock {
        SectionKind kind{SectionKind::Text};
        std::string text;
        std::vector<std::string> lines{};
        std::size_t rendered_width{0};
        /// Tight list items join the previous item without a blank line.
        bool separator_before{true};
    };

    enum class TailKind {
        None,
        Text,
        Thinking,
    };

    void reset_stream_state();
    void consume_message(const ai::AssistantMessage& message);
    void scan_open_tail();
    void freeze_tail_prefix(std::size_t end, bool tight_list_item);
    void settle_tail();
    void update_suffix();
    [[nodiscard]] support::Expected<std::vector<std::string>> render_section_lines(
            const FrozenBlock& block, std::size_t width);

    const LiveTheme& theme_; // must outlive this component.
    bool hide_thinking_block_{false};
    std::string hidden_thinking_label_;
    std::size_t output_pad_{1};

    ai::AssistantStopReason stop_reason_{ai::AssistantStopReason::Stop};
    std::optional<std::string> error_message_;
    bool has_tool_calls_{false};
    bool has_visible_content_{false};

    // Committed sections in exact content order (#603).
    std::vector<FrozenBlock> frozen_blocks_;
    /// The last frozen block was a tight list item and no blank line has
    /// followed it, so a sibling list item joins it without a separator.
    bool tight_list_run_open_{false};
    TailKind tail_kind_{TailKind::None};
    std::string active_tail_text_;
    /// Absolute offset of the first byte not yet frozen in active_tail_text_.
    std::size_t active_tail_begin_{0};
    bool tail_redacted_{false};
    /// Bytes of the tail's source content block already consumed.
    std::size_t consumed_block_bytes_{0};
    /// Offset in `active_tail_text_` of the first line not yet confirmed
    /// Absolute offset of the first byte not yet confirmed by the scanner.
    std::size_t tail_line_pos_{0};
    /// End of the byte range already searched for a newline.
    std::size_t tail_probe_pos_{0};
    bool tail_in_fence_{false};
    char tail_fence_char_{'\0'};
    std::size_t tail_fence_len_{0};
    std::size_t settled_block_count_{0};
    std::unique_ptr<cch::tui::Component> active_tail_markdown_;
    TailKind tail_markdown_kind_{TailKind::None};

    cch::tui::Container suffix_content_;
    ai::AssistantStopReason built_stop_reason_{ai::AssistantStopReason::Pending};
    std::optional<std::string> built_error_message_;
    bool built_tool_calls_{false};

    std::vector<std::string> hidden_label_lines_{};
    std::size_t hidden_label_width_{0};
};

} // namespace cch::coding_agent::tui
