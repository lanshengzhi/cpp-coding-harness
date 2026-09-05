#include "AssistantMessageComponent.hpp"

#include <cch/tui/Markdown.hpp>
#include <cch/tui/Text.hpp>
#include <cch/tui/Utils.hpp>
#include "coding_agent/BoundedText.hpp"
#include "coding_agent/tui/Theme.hpp"

#include <cch/support/Error.hpp>
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

void append_render_result(cch::tui::RenderResult& destination, cch::tui::RenderResult rendered) {
    const auto row_offset = destination.lines.size();
    destination.lines.insert(destination.lines.end(),
            std::make_move_iterator(rendered.lines.begin()),
            std::make_move_iterator(rendered.lines.end()));
    for (auto& image : rendered.images) {
        image.region.row += row_offset;
        destination.images.push_back(std::move(image));
    }
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
    update_prefix_and_suffix();
    update_text();
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

std::size_t AssistantMessageComponent::frozen_block_count() const {
    return frozen_blocks_.size();
}

bool AssistantMessageComponent::has_open_tail() const {
    return !trimmed(active_tail_text_).empty();
}

std::string_view AssistantMessageComponent::open_tail_text() const {
    return active_tail_text_;
}

void AssistantMessageComponent::rebuild() {
    prefix_content_.clear();
    suffix_content_.clear();
    frozen_blocks_.clear();
    active_tail_text_.clear();
    consumed_text_bytes_ = 0;
    accumulated_text_.clear();
    active_tail_markdown_.reset();
    if (message_) {
        update_prefix_and_suffix();
        update_text();
    }
}

void AssistantMessageComponent::update_prefix_and_suffix() {
    prefix_content_.clear();
    suffix_content_.clear();
    if (!message_) return;

    const auto& message = *message_;
    const auto has_visible_content = std::ranges::any_of(
        message.content,
        [](const auto& block) { return visible_block(block); });
    if (has_visible_content) {
        (void)prefix_content_.add_child(std::make_unique<cch::tui::Spacer>(1));
    }

    for (std::size_t index = 0; index < message.content.size(); ++index) {
        const auto& block = message.content[index];
        if (std::holds_alternative<ai::TextContent>(block)) continue;
        if (std::holds_alternative<ai::ToolCallContent>(block)) continue;
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
            auto label = std::make_unique<cch::tui::Text>(
                theme_.foreground(
                    ThemeToken::ThinkingText,
                    std::format("\x1b[3m{}\x1b[23m", hidden_thinking_label_)),
                output_pad_,
                0);
            (void)prefix_content_.add_child(std::move(label));
        } else {
            std::string joined;
            for (const auto& tb : thinking_blocks) {
                if (!joined.empty()) joined += "\n\n";
                joined += safe_text(tb);
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
            (void)prefix_content_.add_child(std::move(markdown));
        }
        if (has_visible_content_after) {
            (void)prefix_content_.add_child(std::make_unique<cch::tui::Spacer>(1));
        }
    }

    if (message.stop_reason == ai::AssistantStopReason::Length) {
        (void)suffix_content_.add_child(std::make_unique<cch::tui::Spacer>(1));
        (void)suffix_content_.add_child(std::make_unique<cch::tui::Text>(
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
            (void)suffix_content_.add_child(std::make_unique<cch::tui::Spacer>(1));
            (void)suffix_content_.add_child(std::make_unique<cch::tui::Text>(
                std::move(*notice),
                output_pad_,
                0));
        }
    }
}

void AssistantMessageComponent::update_text() {
    if (!message_) return;

    std::string new_text;
    for (const auto& block : message_->content) {
        if (const auto* text = std::get_if<ai::TextContent>(&block)) {
            if (!text->text.empty()) {
                if (!new_text.empty()) new_text += "\n\n";
                new_text += text->text;
            }
        }
    }

    if (new_text.empty()) {
        accumulated_text_.clear();
        frozen_blocks_.clear();
        consumed_text_bytes_ = 0;
        active_tail_text_.clear();
        active_tail_markdown_.reset();
        return;
    }

    if (new_text.size() < consumed_text_bytes_ ||
        new_text.compare(0, consumed_text_bytes_, accumulated_text_.substr(0, consumed_text_bytes_)) != 0) {
        frozen_blocks_.clear();
        consumed_text_bytes_ = 0;
        active_tail_text_.clear();
    }

    accumulated_text_ = std::move(new_text);
    const bool is_finished = (message_->stop_reason != ai::AssistantStopReason::Pending);

    std::size_t scan_pos = consumed_text_bytes_;
    const auto& text = accumulated_text_;

    while (scan_pos < text.size()) {
        std::size_t block_start = scan_pos;
        while (block_start < text.size() && (text[block_start] == '\r' || text[block_start] == '\n')) {
            ++block_start;
        }
        if (block_start >= text.size()) {
            scan_pos = block_start;
            break;
        }

        // Check if block_start begins an opening code fence
        std::size_t indent = 0;
        while (block_start + indent < text.size() && text[block_start + indent] == ' ') {
            ++indent;
        }

        bool is_code_fence = false;
        char fence_char = '\0';
        std::size_t fence_len = 0;
        if (indent <= 3 && block_start + indent < text.size()) {
            const char c = text[block_start + indent];
            if (c == '`' || c == '~') {
                std::size_t flen = 0;
                while (block_start + indent + flen < text.size() && text[block_start + indent + flen] == c) {
                    ++flen;
                }
                if (flen >= 3) {
                    const auto newline = text.find('\n', block_start + indent + flen);
                    if (newline != std::string::npos) {
                        std::string_view info_line = std::string_view(text).substr(
                            block_start + indent + flen,
                            newline - (block_start + indent + flen));
                        if (c != '`' || info_line.find('`') == std::string_view::npos) {
                            is_code_fence = true;
                            fence_char = c;
                            fence_len = flen;
                        }
                    }
                }
            }
        }

        if (is_code_fence) {
            const auto opening_newline = text.find('\n', block_start);
            std::size_t search_pos = opening_newline + 1;
            bool found_closing_fence = false;
            std::size_t fence_end = text.size();

            while (search_pos < text.size()) {
                const auto line_end = text.find('\n', search_pos);
                const auto next_search_pos = (line_end == std::string::npos) ? text.size() : line_end + 1;
                std::string_view line = std::string_view(text).substr(
                    search_pos,
                    (line_end == std::string::npos ? text.size() : line_end) - search_pos);
                if (line.ends_with('\r')) line.remove_suffix(1);

                std::size_t close_indent = 0;
                while (close_indent < line.size() && line[close_indent] == ' ') {
                    ++close_indent;
                }

                if (close_indent <= 3 && close_indent < line.size() && line[close_indent] == fence_char) {
                    std::size_t close_flen = 0;
                    while (close_indent + close_flen < line.size() && line[close_indent + close_flen] == fence_char) {
                        ++close_flen;
                    }
                    if (close_flen >= fence_len) {
                        std::string_view after_fence = line.substr(close_indent + close_flen);
                        if (after_fence.find_first_not_of(" \t") == std::string_view::npos) {
                            found_closing_fence = true;
                            fence_end = next_search_pos;
                            while (fence_end < text.size() && (text[fence_end] == '\r' || text[fence_end] == '\n')) {
                                ++fence_end;
                            }
                            break;
                        }
                    }
                }

                search_pos = next_search_pos;
            }

            if (found_closing_fence) {
                frozen_blocks_.push_back(FrozenBlock{
                    .text = text.substr(block_start, fence_end - block_start),
                    .lines = {},
                    .rendered_width = 0,
                });
                scan_pos = fence_end;
                consumed_text_bytes_ = scan_pos;
                continue;
            } else {
                scan_pos = block_start;
                break;
            }
        } else {
            std::size_t line_start = block_start;
            bool found_block_end = false;
            std::size_t block_end = text.size();

            while (line_start < text.size()) {
                const auto newline = text.find('\n', line_start);
                const auto line_end = (newline == std::string::npos) ? text.size() : newline;
                const auto next_line_start = (newline == std::string::npos) ? text.size() : newline + 1;
                std::string_view line = std::string_view(text).substr(line_start, line_end - line_start);
                if (line.ends_with('\r')) line.remove_suffix(1);

                if (line_start > block_start) {
                    if (line.find_first_not_of(" \t") == std::string_view::npos) {
                        found_block_end = true;
                        block_end = next_line_start;
                        while (block_end < text.size() && (text[block_end] == '\r' || text[block_end] == '\n')) {
                            ++block_end;
                        }
                        break;
                    }

                    std::size_t l_indent = 0;
                    while (l_indent < line.size() && line[l_indent] == ' ') ++l_indent;
                    if (l_indent <= 3 && l_indent < line.size() && (line[l_indent] == '`' || line[l_indent] == '~')) {
                        const char fc = line[l_indent];
                        std::size_t flen = 0;
                        while (l_indent + flen < line.size() && line[l_indent + flen] == fc) ++flen;
                        if (flen >= 3) {
                            found_block_end = true;
                            block_end = line_start;
                            break;
                        }
                    }
                }

                if (newline == std::string::npos) break;
                line_start = next_line_start;
            }

            if (found_block_end) {
                frozen_blocks_.push_back(FrozenBlock{
                    .text = text.substr(block_start, block_end - block_start),
                    .lines = {},
                    .rendered_width = 0,
                });
                scan_pos = block_end;
                consumed_text_bytes_ = scan_pos;
                continue;
            } else {
                scan_pos = block_start;
                break;
            }
        }
    }

    if (scan_pos < text.size()) {
        active_tail_text_ = text.substr(scan_pos);
    } else {
        active_tail_text_.clear();
    }

    if (is_finished && !active_tail_text_.empty()) {
        frozen_blocks_.push_back(FrozenBlock{
            .text = std::move(active_tail_text_),
            .lines = {},
            .rendered_width = 0,
        });
        active_tail_text_.clear();
        consumed_text_bytes_ = text.size();
    }
}

support::Expected<cch::tui::RenderResult> AssistantMessageComponent::render(std::size_t width) {
    if (width == 0) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "TUI AssistantMessageComponent requires a positive visible width"));
    }

    cch::tui::RenderResult result;
    bool has_spacer_at_end = false;

    auto prefix_rendered = prefix_content_.render(width);
    if (!prefix_rendered) return std::unexpected(prefix_rendered.error());
    append_render_result(result, std::move(*prefix_rendered));
    if (!result.lines.empty() && result.lines.back().find_first_not_of(' ') == std::string::npos) {
        has_spacer_at_end = true;
    }

    auto append_block_lines = [&](const std::vector<std::string>& block_lines) {
        if (block_lines.empty()) return;
        if (!result.lines.empty() && !has_spacer_at_end) {
            result.lines.push_back(std::string(width, ' '));
        }
        result.lines.insert(result.lines.end(), block_lines.begin(), block_lines.end());
        has_spacer_at_end = false;
    };

    for (auto& block : frozen_blocks_) {
        if (block.rendered_width != width || block.lines.empty()) {
            const auto cleaned = trimmed(block.text);
            if (!cleaned.empty()) {
                auto style = theme_.markdown_style();
                cch::tui::Markdown md(
                    preserve_markdown_line_breaks(safe_text(cleaned)),
                    output_pad_,
                    0,
                    std::move(style));
                auto rendered = md.render(width);
                if (!rendered) return std::unexpected(rendered.error());
                block.lines = std::move(rendered->lines);
            } else {
                block.lines.clear();
            }
            block.rendered_width = width;
        }
        append_block_lines(block.lines);
    }

    const auto cleaned_tail = trimmed(active_tail_text_);
    if (!cleaned_tail.empty()) {
        if (!active_tail_markdown_) {
            active_tail_markdown_ = std::make_unique<cch::tui::Markdown>(
                preserve_markdown_line_breaks(safe_text(cleaned_tail)),
                output_pad_,
                0,
                theme_.markdown_style());
        } else {
            auto* md = dynamic_cast<cch::tui::Markdown*>(active_tail_markdown_.get());
            if (md != nullptr) {
                md->set_text(preserve_markdown_line_breaks(safe_text(cleaned_tail)));
            }
        }
        auto tail_rendered = active_tail_markdown_->render(width);
        if (!tail_rendered) return std::unexpected(tail_rendered.error());
        append_block_lines(tail_rendered->lines);
    }

    auto suffix_rendered = suffix_content_.render(width);
    if (!suffix_rendered) return std::unexpected(suffix_rendered.error());
    if (!suffix_rendered->lines.empty()) {
        append_render_result(result, std::move(*suffix_rendered));
    }

    if (!has_tool_calls_ && !result.lines.empty()) {
        result.lines.front() = std::string{kOsc133ZoneStart} + result.lines.front();
        result.lines.back() =
            std::string{kOsc133ZoneEnd} + std::string{kOsc133ZoneFinal} + result.lines.back();
    }

    return result;
}

void AssistantMessageComponent::invalidate() {
    prefix_content_.invalidate();
    suffix_content_.invalidate();
    for (auto& block : frozen_blocks_) {
        block.rendered_width = 0;
        block.lines.clear();
    }
    if (active_tail_markdown_) {
        active_tail_markdown_->invalidate();
    }
}

} // namespace cch::coding_agent::tui
