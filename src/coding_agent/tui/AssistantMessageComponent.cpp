#include "AssistantMessageComponent.hpp"
#include "coding_agent/tui/RenderResultUtils.hpp"

#include <cch/tui/Markdown.hpp>
#include <cch/tui/Text.hpp>
#include "coding_agent/BoundedText.hpp"
#include "coding_agent/tui/Theme.hpp"

#include <algorithm>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::coding_agent::tui {
namespace {

constexpr std::string_view kOsc133ZoneStart = "\x1b]133;A\x07";
constexpr std::string_view kOsc133ZoneEnd = "\x1b]133;B\x07";
constexpr std::string_view kOsc133ZoneFinal = "\x1b]133;C\x07";

[[nodiscard]] std::string safe_text(std::string text) {
    return bounded_redacted_presentation(std::move(text));
}

/// Preserve pi's hard line-break behavior while retaining fenced and indented code.
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
        const auto marker = first == std::string_view::npos ? std::string_view{} : line.substr(first);
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

[[nodiscard]] std::string italic_thinking_style(const LiveTheme& theme, std::string text) {
    return theme.foreground_hook(ThemeToken::ThinkingText)(
        std::format("\x1b[3m{}\x1b[23m", text));
}

[[nodiscard]] std::string_view strip_cr(std::string_view line) {
    if (line.ends_with('\r')) line.remove_suffix(1);
    return line;
}

[[nodiscard]] bool is_blank_line(std::string_view line) {
    return line.find_first_not_of(" \t") == std::string_view::npos;
}

struct FenceOpener {
    char fence_char;
    std::size_t fence_len;
};

[[nodiscard]] std::optional<FenceOpener> fence_opener(std::string_view line) {
    std::size_t indent = 0;
    while (indent < line.size() && line[indent] == ' ')
        ++indent;
    if (indent > 3 || indent >= line.size()) return std::nullopt;
    const char c = line[indent];
    if (c != '`' && c != '~') return std::nullopt;
    std::size_t len = 0;
    while (indent + len < line.size() && line[indent + len] == c)
        ++len;
    if (len < 3) return std::nullopt;
    if (c == '`' && line.substr(indent + len).find('`') != std::string_view::npos) {
        return std::nullopt;
    }
    return FenceOpener{.fence_char = c, .fence_len = len};
}

[[nodiscard]] bool is_closing_fence(std::string_view line, char fence_char, std::size_t fence_len) {
    std::size_t indent = 0;
    while (indent < line.size() && line[indent] == ' ')
        ++indent;
    if (indent > 3 || indent >= line.size() || line[indent] != fence_char) return false;
    std::size_t len = 0;
    while (indent + len < line.size() && line[indent + len] == fence_char)
        ++len;
    return len >= fence_len && line.substr(indent + len).find_first_not_of(" \t") == std::string_view::npos;
}

struct ListMarker {
    std::size_t indent;
};

[[nodiscard]] std::optional<ListMarker> list_marker(std::string_view line) {
    std::size_t indent = 0;
    while (indent < line.size() && line[indent] == ' ')
        ++indent;
    if (indent > 3 || indent >= line.size()) return std::nullopt;
    const char c = line[indent];
    std::size_t marker_end = 0;
    if (c == '-' || c == '+' || c == '*') {
        marker_end = indent + 1;
    } else if (c >= '0' && c <= '9') {
        std::size_t digits = 0;
        while (indent + digits < line.size() && digits < 9 && line[indent + digits] >= '0' &&
                line[indent + digits] <= '9') {
            ++digits;
        }
        const auto delimiter = indent + digits;
        if (digits == 0 || delimiter >= line.size() || (line[delimiter] != '.' && line[delimiter] != ')')) {
            return std::nullopt;
        }
        marker_end = delimiter + 1;
    } else {
        return std::nullopt;
    }
    if (marker_end < line.size() && line[marker_end] != ' ' && line[marker_end] != '\t') {
        return std::nullopt;
    }
    std::size_t non_space = 0;
    bool only_marker_chars = true;
    for (const char ch : line) {
        if (ch == ' ' || ch == '\t') continue;
        ++non_space;
        if (ch != c) {
            only_marker_chars = false;
            break;
        }
    }
    if (only_marker_chars && non_space >= 3) return std::nullopt;
    return ListMarker{.indent = indent};
}

[[nodiscard]] std::string_view first_line(std::string_view text) {
    const auto newline = text.find('\n');
    return strip_cr(text.substr(0, newline));
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

void AssistantMessageComponent::update_content(const ai::AssistantMessage& message) {
    stop_reason_ = message.stop_reason;
    error_message_ = message.error_message;
    consume_message(message);
    update_suffix();
}

void AssistantMessageComponent::set_hide_thinking_block(bool hide) {
    hide_thinking_block_ = hide;
    invalidate();
}

void AssistantMessageComponent::set_hidden_thinking_label(std::string label) {
    hidden_thinking_label_ = std::move(label);
    invalidate();
}

void AssistantMessageComponent::set_output_pad(std::size_t output_pad) {
    output_pad_ = output_pad;
    active_tail_markdown_.reset();
    built_stop_reason_ = stop_reason_ == ai::AssistantStopReason::Pending ? ai::AssistantStopReason::Stop
                                                                          : ai::AssistantStopReason::Pending;
    update_suffix();
    invalidate();
}

bool AssistantMessageComponent::has_tool_calls() const {
    return has_tool_calls_;
}

std::size_t AssistantMessageComponent::frozen_block_count() const {
    return frozen_blocks_.size();
}

bool AssistantMessageComponent::has_open_tail() const {
    return !trimmed(std::string_view{active_tail_text_}.substr(active_tail_begin_)).empty();
}

std::string_view AssistantMessageComponent::open_tail_text() const {
    return std::string_view{active_tail_text_}.substr(active_tail_begin_);
}

void AssistantMessageComponent::reset_stream_state() {
    frozen_blocks_.clear();
    tight_list_run_open_ = false;
    tail_kind_ = TailKind::None;
    active_tail_text_.clear();
    active_tail_begin_ = 0;
    tail_redacted_ = false;
    consumed_block_bytes_ = 0;
    tail_line_pos_ = 0;
    tail_in_fence_ = false;
    tail_fence_char_ = '\0';
    tail_fence_len_ = 0;
    settled_block_count_ = 0;
    active_tail_markdown_.reset();
    tail_markdown_kind_ = TailKind::None;
    has_tool_calls_ = false;
    has_visible_content_ = false;
}

void AssistantMessageComponent::consume_message(const ai::AssistantMessage& message) {
    const auto& content = message.content;
    if (content.size() < settled_block_count_) reset_stream_state();

    while (settled_block_count_ < content.size()) {
        const auto index = settled_block_count_;
        const auto& block = content[index];
        if (std::holds_alternative<ai::ToolCallContent>(block)) {
            has_tool_calls_ = true;
            ++settled_block_count_;
            continue;
        }

        const auto* text_block = std::get_if<ai::TextContent>(&block);
        const auto* thinking_block = std::get_if<ai::ThinkingContent>(&block);
        if (text_block == nullptr && thinking_block == nullptr) {
            ++settled_block_count_;
            continue;
        }
        const TailKind kind = text_block != nullptr ? TailKind::Text : TailKind::Thinking;
        const bool redacted = thinking_block != nullptr && thinking_block->redacted;
        const std::string_view block_text = text_block != nullptr ? std::string_view{text_block->text}
                                            : redacted            ? std::string_view{"[Redacted thinking]"}
                                                                  : std::string_view{thinking_block->thinking};

        if (tail_kind_ == TailKind::None) {
            tail_kind_ = kind;
            tail_redacted_ = redacted;
            consumed_block_bytes_ = 0;
            tail_line_pos_ = 0;
            tail_in_fence_ = false;
            active_tail_text_.clear();
            active_tail_begin_ = 0;
        } else if (tail_kind_ != kind || tail_redacted_ != redacted || block_text.size() < consumed_block_bytes_) {
            reset_stream_state();
            continue;
        }

        const auto delta = block_text.substr(consumed_block_bytes_);
        consumed_block_bytes_ = block_text.size();
        if (!block_text.empty()) has_visible_content_ = true;
        if (!delta.empty()) {
            active_tail_text_.append(delta);
            scan_open_tail();
        }

        const bool complete = index + 1 < content.size() || message.stop_reason != ai::AssistantStopReason::Pending;
        if (!complete) break;
        settle_tail();
        ++settled_block_count_;
    }
}

void AssistantMessageComponent::freeze_tail_prefix(std::size_t end, bool tight_list_item) {
    if (end <= active_tail_begin_) return;
    const std::string_view segment =
            std::string_view{active_tail_text_}.substr(active_tail_begin_, end - active_tail_begin_);
    if (trimmed(segment).empty()) {
        active_tail_begin_ = end;
        return;
    }
    const bool joins_list_run = tight_list_run_open_ && list_marker(first_line(segment)).has_value();
    frozen_blocks_.push_back(FrozenBlock{
            .kind = tail_kind_ == TailKind::Thinking ? SectionKind::Thinking : SectionKind::Text,
            .text = std::string{segment},
            .lines = {},
            .rendered_width = 0,
            .separator_before = !joins_list_run,
    });
    tight_list_run_open_ = tight_list_item;
    active_tail_begin_ = end;
    active_tail_markdown_.reset();
}

void AssistantMessageComponent::scan_open_tail() {
    while (tail_line_pos_ < active_tail_text_.size()) {
        const auto newline = active_tail_text_.find('\n', tail_line_pos_);
        if (newline == std::string::npos) return;
        const auto line =
                strip_cr(std::string_view{active_tail_text_}.substr(tail_line_pos_, newline - tail_line_pos_));
        const auto line_end = newline + 1;

        if (tail_in_fence_) {
            if (is_closing_fence(line, tail_fence_char_, tail_fence_len_)) {
                freeze_tail_prefix(line_end, false);
                tail_in_fence_ = false;
                tail_line_pos_ = line_end;
                while (tail_line_pos_ < active_tail_text_.size() &&
                        (active_tail_text_[tail_line_pos_] == '\r' || active_tail_text_[tail_line_pos_] == '\n')) {
                    ++tail_line_pos_;
                }
            } else {
                tail_line_pos_ = line_end;
            }
            continue;
        }

        if (is_blank_line(line)) {
            const auto prefix_end = tail_line_pos_;
            freeze_tail_prefix(prefix_end, false);
            tight_list_run_open_ = false;
            tail_line_pos_ = line_end;
            continue;
        }

        if (const auto fence = fence_opener(line)) {
            const auto prefix_end = tail_line_pos_;
            freeze_tail_prefix(prefix_end, false);
            tight_list_run_open_ = false;
            tail_in_fence_ = true;
            tail_fence_char_ = fence->fence_char;
            tail_fence_len_ = fence->fence_len;
            tail_line_pos_ = line_end;
            continue;
        }

        if (const auto marker = list_marker(line); marker && tail_line_pos_ > active_tail_begin_) {
            const auto pending_marker =
                    list_marker(first_line(std::string_view{active_tail_text_}.substr(active_tail_begin_)));
            if (!pending_marker || marker->indent < pending_marker->indent) {
                const auto prefix_end = tail_line_pos_;
                freeze_tail_prefix(prefix_end, false);
                tight_list_run_open_ = false;
                tail_line_pos_ = line_end;
            } else if (marker->indent == pending_marker->indent) {
                const auto prefix_end = tail_line_pos_;
                freeze_tail_prefix(prefix_end, true);
                tail_line_pos_ = line_end;
            } else {
                tail_line_pos_ = line_end;
            }
        } else {
            tail_line_pos_ = line_end;
        }
    }
}

void AssistantMessageComponent::settle_tail() {
    const std::string_view active_tail = std::string_view{active_tail_text_}.substr(active_tail_begin_);
    if (!trimmed(active_tail).empty()) {
        const bool joins_list_run = tight_list_run_open_ && list_marker(first_line(active_tail)).has_value();
        frozen_blocks_.push_back(FrozenBlock{
                .kind = tail_kind_ == TailKind::Thinking ? SectionKind::Thinking : SectionKind::Text,
                .text = std::string{active_tail},
                .lines = {},
                .rendered_width = 0,
                .separator_before = !joins_list_run,
        });
    }
    active_tail_text_.clear();
    active_tail_begin_ = 0;
    tight_list_run_open_ = false;
    tail_kind_ = TailKind::None;
    tail_redacted_ = false;
    consumed_block_bytes_ = 0;
    tail_line_pos_ = 0;
    tail_in_fence_ = false;
    active_tail_markdown_.reset();
    tail_markdown_kind_ = TailKind::None;
}

void AssistantMessageComponent::update_suffix() {
    if (built_stop_reason_ == stop_reason_ && built_error_message_ == error_message_ &&
            built_tool_calls_ == has_tool_calls_) {
        return;
    }
    built_stop_reason_ = stop_reason_;
    built_error_message_ = error_message_;
    built_tool_calls_ = has_tool_calls_;
    suffix_content_.clear();

    if (stop_reason_ == ai::AssistantStopReason::Length) {
        (void)suffix_content_.add_child(std::make_unique<cch::tui::Spacer>(1));
        (void)suffix_content_.add_child(std::make_unique<cch::tui::Text>(
            theme_.foreground(
                ThemeToken::Error,
                "Error: Model stopped because it reached the maximum output token limit. The response may be incomplete."),
            output_pad_,
            0));
    } else if (!has_tool_calls_) {
        std::optional<std::string> notice;
        if (stop_reason_ == ai::AssistantStopReason::Aborted) {
            const auto abort_message = error_message_ && *error_message_ != "Request was aborted"
                                               ? *error_message_
                                               : std::string{"Operation aborted"};
            notice = theme_.foreground(ThemeToken::Error, abort_message);
        } else if (stop_reason_ == ai::AssistantStopReason::Error) {
            notice = theme_.foreground(
                    ThemeToken::Error, std::format("Error: {}", error_message_.value_or("Unknown error")));
        }
        if (notice) {
            (void)suffix_content_.add_child(std::make_unique<cch::tui::Spacer>(1));
            (void)suffix_content_.add_child(std::make_unique<cch::tui::Text>(std::move(*notice), output_pad_, 0));
        }
    }
}

support::Expected<std::vector<std::string>> AssistantMessageComponent::render_section_lines(
        const FrozenBlock& block, std::size_t width) {
    const auto cleaned = trimmed(block.text);
    if (cleaned.empty()) return std::vector<std::string>{};
    auto style = theme_.markdown_style();
    if (block.kind == SectionKind::Thinking) {
        style.text = [this](std::string text) { return italic_thinking_style(theme_, std::move(text)); };
    }
    cch::tui::Markdown markdown(preserve_markdown_line_breaks(safe_text(cleaned)), output_pad_, 0, std::move(style));
    auto rendered = markdown.render(width);
    if (!rendered) return std::unexpected(rendered.error());
    return std::move(rendered->lines);
}

support::Expected<cch::tui::RenderResult> AssistantMessageComponent::render(std::size_t width) {
    if (width == 0) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "TUI AssistantMessageComponent requires a positive visible width"));
    }

    cch::tui::RenderResult result;
    bool has_spacer_at_end = false;
    bool hidden_thinking_run = false;
    auto append_lines = [&](const std::vector<std::string>& lines, bool separator_before) {
        if (lines.empty()) return;
        if (!result.lines.empty() && !has_spacer_at_end && separator_before) {
            result.lines.push_back(std::string(width, ' '));
        }
        result.lines.insert(result.lines.end(), lines.begin(), lines.end());
        has_spacer_at_end = false;
    };
    auto hidden_label = [&]() -> support::Expected<std::vector<std::string>> {
        if (hidden_label_width_ == width && !hidden_label_lines_.empty()) return hidden_label_lines_;
        cch::tui::Text label(
                theme_.foreground(ThemeToken::ThinkingText, std::format("\x1b[3m{}\x1b[23m", hidden_thinking_label_)),
                output_pad_,
                0);
        auto rendered = label.render(width);
        if (!rendered) return std::unexpected(rendered.error());
        hidden_label_lines_ = rendered->lines;
        hidden_label_width_ = width;
        return hidden_label_lines_;
    };

    if (has_visible_content_ || !frozen_blocks_.empty()) {
        result.lines.push_back(std::string(width, ' '));
        has_spacer_at_end = true;
    }

    for (auto& block : frozen_blocks_) {
        if (block.kind == SectionKind::Thinking && hide_thinking_block_) {
            if (!hidden_thinking_run) {
                const auto label = hidden_label();
                if (!label) return std::unexpected(label.error());
                append_lines(*label, block.separator_before);
            }
            hidden_thinking_run = true;
            continue;
        }
        hidden_thinking_run = false;
        if (block.rendered_width != width) {
            auto lines = render_section_lines(block, width);
            if (!lines) return std::unexpected(lines.error());
            block.lines = std::move(*lines);
            block.rendered_width = width;
        }
        append_lines(block.lines, block.separator_before);
    }

    const auto active_tail = std::string_view{active_tail_text_}.substr(active_tail_begin_);
    const auto cleaned_tail = trimmed(active_tail);
    if (!cleaned_tail.empty()) {
        if (tail_kind_ == TailKind::Thinking && hide_thinking_block_) {
            if (!hidden_thinking_run) {
                const auto label = hidden_label();
                if (!label) return std::unexpected(label.error());
                append_lines(*label, true);
            }
        } else {
            if (tail_in_fence_) {
                if (!active_tail_markdown_ || tail_markdown_kind_ != tail_kind_) {
                    active_tail_markdown_ = std::make_unique<cch::tui::Text>(safe_text(cleaned_tail), output_pad_, 0);
                    tail_markdown_kind_ = tail_kind_;
                } else if (auto* text = dynamic_cast<cch::tui::Text*>(active_tail_markdown_.get())) {
                    text->set_text(safe_text(cleaned_tail));
                }
            } else if (!active_tail_markdown_ || tail_markdown_kind_ != tail_kind_) {
                auto style = theme_.markdown_style();
                if (tail_kind_ == TailKind::Thinking) {
                    style.text = [this](std::string text) { return italic_thinking_style(theme_, std::move(text)); };
                }
                active_tail_markdown_ = std::make_unique<cch::tui::Markdown>(
                        preserve_markdown_line_breaks(safe_text(cleaned_tail)), output_pad_, 0, std::move(style));
                tail_markdown_kind_ = tail_kind_;
            } else if (auto* markdown = dynamic_cast<cch::tui::Markdown*>(active_tail_markdown_.get())) {
                markdown->set_text(preserve_markdown_line_breaks(safe_text(cleaned_tail)));
            }
            auto rendered = active_tail_markdown_->render(width);
            if (!rendered) return std::unexpected(rendered.error());
            const bool joins_list_run = tight_list_run_open_ && list_marker(first_line(cleaned_tail)).has_value();
            append_lines(rendered->lines, !joins_list_run);
        }
    }

    auto suffix_rendered = suffix_content_.render(width);
    if (!suffix_rendered) return std::unexpected(suffix_rendered.error());
    if (!suffix_rendered->lines.empty()) append_render_result(result, std::move(*suffix_rendered));

    if (!has_tool_calls_ && !result.lines.empty()) {
        result.lines.front() = std::string{kOsc133ZoneStart} + result.lines.front();
        result.lines.back() =
            std::string{kOsc133ZoneEnd} + std::string{kOsc133ZoneFinal} + result.lines.back();
    }
    return result;
}

void AssistantMessageComponent::invalidate() {
    suffix_content_.invalidate();
    for (auto& block : frozen_blocks_) {
        block.rendered_width = 0;
        block.lines.clear();
    }
    hidden_label_width_ = 0;
    hidden_label_lines_.clear();
    if (active_tail_markdown_) active_tail_markdown_->invalidate();
}

} // namespace cch::coding_agent::tui
