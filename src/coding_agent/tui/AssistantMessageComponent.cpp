#include "AssistantMessageComponent.hpp"

#include <cch/tui/Markdown.hpp>
#include <cch/tui/Text.hpp>
#include <cch/tui/Utils.hpp>
#include "coding_agent/BoundedText.hpp"
#include "coding_agent/tui/Theme.hpp"

#include <algorithm>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::coding_agent::tui {
namespace {

// pi `assistant-message.ts` OSC 133 prompt zones.
constexpr std::string_view kOsc133ZoneStart = "\x1b]133;A\x07";
constexpr std::string_view kOsc133ZoneEnd = "\x1b]133;B\x07";
constexpr std::string_view kOsc133ZoneFinal = "\x1b]133;C\x07";

[[nodiscard]] std::string safe_text(std::string text) {
    return bounded_redacted_presentation(std::move(text));
}

/// The C++ Markdown collapses single newlines; pi's markdown-transform
/// preserves them by emitting hard breaks outside fences and indented code.
[[nodiscard]] std::string preserve_markdown_line_breaks(std::string text) {
    std::string rendered;
    rendered.reserve(text.size() + 16);
    bool in_fence = false;
    std::size_t start = 0;
    while (start < text.size()) {
        const auto newline = text.find('\n', start);
        const auto end = newline == std::string::npos ? text.size() : newline;
        const auto line = std::string_view{text}.substr(start, end - start);
        const auto first = line.find_first_not_of(' ');
        const auto marker = first == std::string_view::npos
            ? std::string_view{}
            : line.substr(first);
        const bool fence = first <= 3 &&
            (marker.starts_with("```") || marker.starts_with("~~~"));
        const bool indented_code = first >= 4 && first != std::string_view::npos;

        rendered.append(line);
        if (newline != std::string_view::npos) {
            const bool already_hard = line.ends_with("  ") || line.ends_with('\\');
            if (!in_fence && !fence && !indented_code && !line.empty() && !already_hard) {
                rendered += "  ";
            }
            rendered.push_back('\n');
        }
        if (fence) in_fence = !in_fence;
        if (newline == std::string_view::npos) break;
        start = newline + 1;
    }
    return rendered;
}

[[nodiscard]] bool visible_block(const ai::AssistantContent& block) {
    if (const auto* text = std::get_if<ai::TextContent>(&block)) {
        return !text->text.empty();
    }
    if (const auto* thinking = std::get_if<ai::ThinkingContent>(&block)) {
        return thinking->redacted || !thinking->thinking.empty();
    }
    return false;
}

[[nodiscard]] std::string trimmed(std::string_view text) {
    std::size_t begin = 0;
    while (begin < text.size() && (text[begin] == ' ' || text[begin] == '\t' ||
                                      text[begin] == '\r' || text[begin] == '\n')) {
        ++begin;
    }
    std::size_t end = text.size();
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' ||
                              text[end - 1] == '\r' || text[end - 1] == '\n')) {
        --end;
    }
    return std::string{text.substr(begin, end - begin)};
}

[[nodiscard]] std::string italic_thinking_style(
    const LiveTheme& theme,
    std::string text) {
    // pi assistant-message.ts: `{ color: thinkingText, italic: true }`.
    return theme.foreground_hook(ThemeToken::ThinkingText)(
        std::format("\x1b[3m{}\x1b[23m", text));
}

} // namespace

AssistantMessageComponent::AssistantMessageComponent(
    const LiveTheme& theme,
    bool hide_thinking_block,
    std::string hidden_thinking_label,
    std::size_t output_pad)
    : theme_(theme),
      hide_thinking_block_(hide_thinking_block),
      hidden_thinking_label_(std::move(hidden_thinking_label)),
      output_pad_(output_pad) {}

AssistantMessageComponent::~AssistantMessageComponent() = default;

void AssistantMessageComponent::update_content(
    const ai::AssistantMessage& message) {
    message_ = message;
    has_tool_calls_ = std::ranges::any_of(
        message_->content,
        [](const auto& block) {
            return std::holds_alternative<ai::ToolCallContent>(block);
        });
    rebuild();
}

void AssistantMessageComponent::set_hide_thinking_block(bool hide) {
    hide_thinking_block_ = hide;
    if (message_) rebuild();
}

void AssistantMessageComponent::set_hidden_thinking_label(std::string label) {
    hidden_thinking_label_ = std::move(label);
    if (message_) rebuild();
}

void AssistantMessageComponent::set_output_pad(std::size_t output_pad) {
    output_pad_ = output_pad;
    if (message_) rebuild();
}

bool AssistantMessageComponent::has_tool_calls() const {
    return has_tool_calls_;
}

void AssistantMessageComponent::rebuild() {
    content_.clear();
    if (!message_) return;

    const auto& message = *message_;
    const auto has_visible_content = std::ranges::any_of(
        message.content,
        [](const auto& block) { return visible_block(block); });
    if (has_visible_content) {
        (void)content_.add_child(std::make_unique<cch::tui::Spacer>(1));
    }

    for (std::size_t index = 0; index < message.content.size(); ++index) {
        const auto& block = message.content[index];
        if (const auto* text = std::get_if<ai::TextContent>(&block);
            text != nullptr && !text->text.empty()) {
            auto style = theme_.markdown_style();
            auto markdown = std::make_unique<cch::tui::Markdown>(
                preserve_markdown_line_breaks(safe_text(trimmed(text->text))),
                output_pad_,
                0,
                std::move(style));
            (void)content_.add_child(std::move(markdown));
            continue;
        }
        if (std::holds_alternative<ai::ToolCallContent>(block)) continue;
        // Only thinking blocks enter the run collector (pi's `else if`); any
        // other content (empty text, redacted markers) is skipped here.
        if (!std::holds_alternative<ai::ThinkingContent>(block)) continue;

        // Consecutive thinking blocks render as one run (pi assistant-message.ts).
        std::vector<std::string> thinking_blocks;
        for (; index < message.content.size(); ++index) {
            const auto& thinking_block = message.content[index];
            if (!std::holds_alternative<ai::ThinkingContent>(thinking_block)) break;
            const auto& thinking = std::get<ai::ThinkingContent>(thinking_block);
            if (thinking.redacted) {
                thinking_blocks.emplace_back("[Redacted thinking]");
            } else if (!thinking.thinking.empty()) {
                thinking_blocks.emplace_back(thinking.thinking);
            }
        }
        --index;

        if (thinking_blocks.empty()) continue;

        const bool has_visible_content_after = std::ranges::any_of(
            std::span{message.content}.subspan(index + 1),
            [](const auto& block) { return visible_block(block); });

        if (hide_thinking_block_) {
            // One static label for each run of thinking blocks when hidden.
            auto label = std::make_unique<cch::tui::Text>(
                theme_.foreground(
                    ThemeToken::ThinkingText,
                    std::format("\x1b[3m{}\x1b[23m", hidden_thinking_label_)),
                output_pad_,
                0);
            (void)content_.add_child(std::move(label));
        } else {
            std::string joined;
            for (const auto& block : thinking_blocks) {
                if (!joined.empty()) joined += "\n\n";
                joined += safe_text(block);
            }
            auto style = theme_.markdown_style();
            style.text = [this](std::string text) {
                return italic_thinking_style(theme_, std::move(text));
            };
            auto markdown = std::make_unique<cch::tui::Markdown>(
                preserve_markdown_line_breaks(std::move(joined)),
                output_pad_,
                0,
                std::move(style));
            (void)content_.add_child(std::move(markdown));
        }
        if (has_visible_content_after) {
            (void)content_.add_child(std::make_unique<cch::tui::Spacer>(1));
        }
    }

    // Incomplete/failed outcomes surface after partial content (pi
    // assistant-message.ts); tool calls suppress the notices because the
    // tool-execution components show their own outcomes.
    if (message.stop_reason == ai::AssistantStopReason::Length) {
        (void)content_.add_child(std::make_unique<cch::tui::Spacer>(1));
        (void)content_.add_child(std::make_unique<cch::tui::Text>(
            theme_.foreground(
                ThemeToken::Error,
                "Error: Model stopped because it reached the maximum output token limit. The response may be incomplete."),
            output_pad_,
            0));
    } else if (!has_tool_calls_) {
        std::optional<std::string> notice;
        if (message.stop_reason == ai::AssistantStopReason::Aborted) {
            const auto abort_message =
                message.error_message && *message.error_message != "Request was aborted"
                ? *message.error_message
                : std::string{"Operation aborted"};
            notice = theme_.foreground(ThemeToken::Error, abort_message);
        } else if (message.stop_reason == ai::AssistantStopReason::Error) {
            const auto error_message = message.error_message.value_or("Unknown error");
            notice = theme_.foreground(
                ThemeToken::Error,
                std::format("Error: {}", error_message));
        }
        if (notice) {
            (void)content_.add_child(std::make_unique<cch::tui::Spacer>(1));
            (void)content_.add_child(std::make_unique<cch::tui::Text>(
                std::move(*notice),
                output_pad_,
                0));
        }
    }
}

util::Expected<cch::tui::RenderResult> AssistantMessageComponent::render(std::size_t width) {
    auto rendered = content_.render(width);
    if (!rendered) return std::unexpected(rendered.error());
    if (!has_tool_calls_ && !rendered->lines.empty()) {
        // pi assistant-message.ts: the zones wrap the rendered block.
        rendered->lines.front() = std::string{kOsc133ZoneStart} + rendered->lines.front();
        rendered->lines.back() =
            std::string{kOsc133ZoneEnd} + std::string{kOsc133ZoneFinal} + rendered->lines.back();
    }
    return rendered;
}

void AssistantMessageComponent::invalidate() {
    content_.invalidate();
}

} // namespace cch::coding_agent::tui
