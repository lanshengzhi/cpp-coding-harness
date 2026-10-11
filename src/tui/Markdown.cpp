#include <cch/tui/Markdown.hpp>

#include <cch/tui/Latex.hpp>
#include <cch/tui/TerminalImage.hpp>
#include <cch/tui/Utils.hpp>

#include "tui/RenderUtils.hpp"
#include "tui/UnicodeWidth.hpp"

#include <cch/support/Error.hpp>
#include <md4c.h>
#include <utf8proc.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <exception>
#include <format>
#include <memory>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::tui {

namespace {

enum class BlockKind {
    Document,
    Latex,
    Quote,
    UnorderedList,
    OrderedList,
    ListItem,
    Rule,
    Heading,
    Code,
    Html,
    Paragraph,
    Table,
    TableHead,
    TableBody,
    TableRow,
    TableHeaderCell,
    TableCell,
};

enum class InlineKind {
    Text,
    Emphasis,
    Strong,
    Link,
    Image,
    Code,
    Strikethrough,
    Latex,
};

enum class StyleRole {
    Heading,
    Quote,
    Emphasis,
    Strong,
    Underline,
    Strikethrough,
    InlineCode,
    Link,
};

struct InlineNode {
    InlineKind kind{InlineKind::Text};
    std::string text;
    std::string source;
    std::string destination;
    bool latex_display{false};
    bool latex_block{false};
    bool latex_enabled{true};
    bool latex_pending{false};
    bool autolink{false};
    bool strict_strikethrough{true};
    std::vector<std::unique_ptr<InlineNode>> children;
};

// Forward declaration used by the math-block promotion below.
[[nodiscard]] std::string plain_inline_text(const InlineNode& node);

struct BlockNode {
    BlockKind kind{BlockKind::Document};
    unsigned heading_level{0};
    unsigned ordered_start{1};
    unsigned column_count{0};
    char list_marker{'-'};
    char ordered_delimiter{'.'};
    bool tight_list{true};
    bool task_item{false};
    bool task_checked{false};
    bool latex_pending{false};
    bool latex_display{false};
    std::string language;
    std::string raw;
    std::string text;
    std::vector<std::unique_ptr<InlineNode>> inlines;
    std::vector<std::unique_ptr<BlockNode>> children;
};

struct MathToken {
    std::string marker;
    std::string source;
    std::string text;
    bool display{false};
    bool block{false};
    bool pending{false};
};

struct ProtectedMarkdown {
    std::string text;
    std::vector<MathToken> math_tokens;
};

void append_math_token(ProtectedMarkdown& protected_markdown, MathToken token);
void scan_pending_display_math(std::string_view source_tail, ProtectedMarkdown& protected_markdown);

struct ParseState {
    BlockNode root;
    std::vector<MathToken> math_tokens;
    std::vector<BlockNode*> blocks; // point into root and must not outlive the synchronous parse
    std::vector<InlineNode*> inlines; // point into root and must not outlive the synchronous parse
    std::vector<bool> strict_strikethrough;
    std::vector<std::string> table_raw;
    std::size_t table_index{0};
    std::size_t strikethrough_index{0};
    bool failed{false};
};

[[nodiscard]] bool escaped_at(std::string_view text, std::size_t position) {
    std::size_t backslashes = 0;
    while (position > backslashes && text[position - backslashes - 1] == '\\') ++backslashes;
    return backslashes % 2 != 0;
}

[[nodiscard]] bool strict_strikethrough_contents(std::string_view contents) {
    if (contents.empty() || contents.front() == '~' ||
        std::isspace(static_cast<unsigned char>(contents.front())) != 0) {
        return false;
    }
    const auto last = contents.size() - 1;
    if (contents[last] == '~' || std::isspace(static_cast<unsigned char>(contents[last])) != 0) {
        return escaped_at(contents, last);
    }
    return contents[last] != '\\' || escaped_at(contents, last);
}

[[nodiscard]] std::vector<bool> scan_strikethrough_delimiters(std::string_view text) {
    std::vector<bool> result;
    for (std::size_t index = 0; index + 1 < text.size();) {
        if (text[index] == '`') {
            std::size_t run = 1;
            while (index + run < text.size() && text[index + run] == '`') ++run;
            const auto closing = text.find(std::string(run, '`'), index + run);
            index = closing == std::string_view::npos ? text.size() : closing + run;
            continue;
        }
        if (index + 2 < text.size() && text.substr(index, 3) == "~~~") {
            const auto closing = text.find("~~~", index + 3);
            index = closing == std::string_view::npos ? text.size() : closing + 3;
            continue;
        }
        if (text.substr(index, 2) != "~~" || escaped_at(text, index)) {
            ++index;
            continue;
        }
        auto closing = index + 2;
        while (closing + 1 < text.size() &&
               (text.substr(closing, 2) != "~~" || escaped_at(text, closing))) {
            ++closing;
        }
        if (closing + 1 >= text.size()) break;
        result.push_back(strict_strikethrough_contents(
            text.substr(index + 2, closing - index - 2)));
        index = closing + 2;
    }
    return result;
}

[[nodiscard]] std::string attribute_text(const MD_ATTRIBUTE& attribute) {
    if (attribute.text == nullptr || attribute.size == 0) return {};
    return std::string(attribute.text, attribute.size);
}

[[nodiscard]] BlockKind block_kind(MD_BLOCKTYPE type) {
    switch (type) {
        case MD_BLOCK_DOC:
            return BlockKind::Document;
        case MD_BLOCK_QUOTE:
            return BlockKind::Quote;
        case MD_BLOCK_UL:
            return BlockKind::UnorderedList;
        case MD_BLOCK_OL:
            return BlockKind::OrderedList;
        case MD_BLOCK_LI:
            return BlockKind::ListItem;
        case MD_BLOCK_HR:
            return BlockKind::Rule;
        case MD_BLOCK_H:
            return BlockKind::Heading;
        case MD_BLOCK_CODE:
            return BlockKind::Code;
        case MD_BLOCK_HTML:
            return BlockKind::Html;
        case MD_BLOCK_P:
            return BlockKind::Paragraph;
        case MD_BLOCK_TABLE:
            return BlockKind::Table;
        case MD_BLOCK_THEAD:
            return BlockKind::TableHead;
        case MD_BLOCK_TBODY:
            return BlockKind::TableBody;
        case MD_BLOCK_TR:
            return BlockKind::TableRow;
        case MD_BLOCK_TH:
            return BlockKind::TableHeaderCell;
        case MD_BLOCK_TD:
            return BlockKind::TableCell;
    }
    return BlockKind::Paragraph;
}

[[nodiscard]] InlineKind inline_kind(MD_SPANTYPE type) {
    switch (type) {
        case MD_SPAN_EM:
            return InlineKind::Emphasis;
        case MD_SPAN_STRONG:
            return InlineKind::Strong;
        case MD_SPAN_A:
            return InlineKind::Link;
        case MD_SPAN_IMG:
            return InlineKind::Image;
        case MD_SPAN_CODE:
            return InlineKind::Code;
        case MD_SPAN_LATEXMATH:
        case MD_SPAN_LATEXMATH_DISPLAY:
            return InlineKind::Latex;
        case MD_SPAN_DEL:
            return InlineKind::Strikethrough;
        case MD_SPAN_WIKILINK:
        case MD_SPAN_U:
            return InlineKind::Text;
    }
    return InlineKind::Text;
}

int enter_block(MD_BLOCKTYPE type, void* detail, void* userdata) noexcept {
    auto& state = *static_cast<ParseState*>(userdata);
    if (type == MD_BLOCK_DOC) {
        state.blocks.push_back(&state.root);
        return 0;
    }
        if (state.blocks.empty()) return 1;
        auto node = std::make_unique<BlockNode>();
        node->kind = block_kind(type);
        switch (type) {
        case MD_BLOCK_TABLE: {
            node->column_count = static_cast<MD_BLOCK_TABLE_DETAIL*>(detail)->col_count;
            if (state.table_index < state.table_raw.size()) {
                node->raw = std::move(state.table_raw[state.table_index]);
            }
            ++state.table_index;
            break;
        }
            case MD_BLOCK_H:
                node->heading_level = static_cast<MD_BLOCK_H_DETAIL*>(detail)->level;
                break;
            case MD_BLOCK_UL: {
                const auto& list_detail = *static_cast<MD_BLOCK_UL_DETAIL*>(detail);
                node->list_marker = list_detail.mark;
                node->tight_list = list_detail.is_tight != 0;
                break;
            }
            case MD_BLOCK_OL: {
                const auto& list_detail = *static_cast<MD_BLOCK_OL_DETAIL*>(detail);
                node->ordered_start = list_detail.start;
                node->ordered_delimiter = list_detail.mark_delimiter;
                node->tight_list = list_detail.is_tight != 0;
                break;
            }
            case MD_BLOCK_LI: {
                const auto& item_detail = *static_cast<MD_BLOCK_LI_DETAIL*>(detail);
                node->task_item = item_detail.is_task != 0;
                node->task_checked = item_detail.task_mark == 'x' || item_detail.task_mark == 'X';
                break;
            }
            case MD_BLOCK_CODE:
                node->language = attribute_text(static_cast<MD_BLOCK_CODE_DETAIL*>(detail)->lang);
                break;
            default:
                break;
        }
        auto* node_pointer = node.get();
        state.blocks.back()->children.push_back(std::move(node));
        state.blocks.push_back(node_pointer);
        return 0;
}

int leave_block(MD_BLOCKTYPE, void*, void* userdata) noexcept {
    auto& state = *static_cast<ParseState*>(userdata);
    if (state.blocks.empty()) return 1;
    state.blocks.pop_back();
    return 0;
}

int enter_span(MD_SPANTYPE type, void* detail, void* userdata) noexcept {
    auto& state = *static_cast<ParseState*>(userdata);
    if (state.blocks.empty()) return 1;
    auto node = std::make_unique<InlineNode>();
    node->kind = inline_kind(type);
    if (type == MD_SPAN_A) {
        const auto& link_detail = *static_cast<MD_SPAN_A_DETAIL*>(detail);
        node->destination = attribute_text(link_detail.href);
        node->autolink = link_detail.is_autolink != 0;
    } else if (type == MD_SPAN_IMG) {
        node->destination = attribute_text(static_cast<MD_SPAN_IMG_DETAIL*>(detail)->src);
    } else if (type == MD_SPAN_DEL && state.strikethrough_index < state.strict_strikethrough.size()) {
        node->strict_strikethrough = state.strict_strikethrough[state.strikethrough_index++];
    } else if (type == MD_SPAN_LATEXMATH || type == MD_SPAN_LATEXMATH_DISPLAY) {
        node->latex_display = type == MD_SPAN_LATEXMATH_DISPLAY;
        node->latex_block = node->latex_display;
    }
        auto* node_pointer = node.get();
        if (state.inlines.empty()) {
            state.blocks.back()->inlines.push_back(std::move(node));
        } else {
            state.inlines.back()->children.push_back(std::move(node));
        }
        state.inlines.push_back(node_pointer);
        return 0;
}

int leave_span(MD_SPANTYPE, void*, void* userdata) noexcept {
    auto& state = *static_cast<ParseState*>(userdata);
    if (state.inlines.empty()) return 1;
    state.inlines.pop_back();
    return 0;
}

int append_text(MD_TEXTTYPE type, const MD_CHAR* text, MD_SIZE size, void* userdata) noexcept {
    auto& state = *static_cast<ParseState*>(userdata);
    if (state.blocks.empty()) return 1;
    auto& block = *state.blocks.back();
    if (!state.inlines.empty() && state.inlines.back()->kind == InlineKind::Latex) {
        if (type == MD_TEXT_NULLCHAR) {
            state.inlines.back()->text += "\xef\xbf\xbd";
        } else {
            state.inlines.back()->text.append(text, size);
        }
        return 0;
    }
    if (block.kind == BlockKind::Code || block.kind == BlockKind::Html) {
        if (type == MD_TEXT_NULLCHAR) {
            block.text += "\xef\xbf\xbd";
        } else {
            block.text.append(text, size);
        }
        return 0;
    }

        auto node = std::make_unique<InlineNode>();
        node->kind = InlineKind::Text;
        if (type == MD_TEXT_NULLCHAR) {
            node->text = "\xef\xbf\xbd";
        } else if (type == MD_TEXT_BR) {
            node->text = "\n";
        } else if (type == MD_TEXT_SOFTBR) {
            node->text = " ";
        } else {
            node->text.assign(text, size);
        }
        if (state.inlines.empty()) {
            block.inlines.push_back(std::move(node));
        } else {
            state.inlines.back()->children.push_back(std::move(node));
        }
        return 0;
}

struct MarkdownFence {
    char marker{'\0'};
    std::size_t length{0};
};

[[nodiscard]] bool update_markdown_fence(std::string_view line, MarkdownFence& fence) {
    while (!line.empty() && line.front() == ' ')
        line.remove_prefix(1);
    if (line.empty() || (line.front() != '`' && line.front() != '~')) return fence.marker != '\0';
    const auto marker = line.front();
    std::size_t length = 0;
    while (length < line.size() && line[length] == marker)
        ++length;
    if (fence.marker == '\0') {
        if (length >= 3) fence = {.marker = marker, .length = length};
        return fence.marker != '\0';
    }
    if (marker == fence.marker && length >= fence.length &&
            std::all_of(line.begin() + static_cast<std::ptrdiff_t>(length), line.end(), [](char character) {
                return character == ' ' || character == '\t' || character == '\r';
            })) {
        fence = {};
    }
    return true;
}

[[nodiscard]] bool looks_like_pending_dollar_math(std::string_view source) {
    constexpr std::string_view kMathCharacters{"_^=+*/<>()[|+-"};
    constexpr std::array<std::string_view, 12> kUnicodeMathCharacters{
            "±", "≤", "≥", "≠", "≈", "∈", "→", "⇒", "∞", "∫", "∑", "√"};
    if (source.find('\\') != std::string_view::npos ||
            source.find_first_of(kMathCharacters) != std::string_view::npos) {
        return true;
    }
    return std::any_of(kUnicodeMathCharacters.begin(),
            kUnicodeMathCharacters.end(),
            [source](std::string_view character) { return source.find(character) != std::string_view::npos; });
}

[[nodiscard]] bool escaped_math_delimiter(std::string_view source, std::size_t position) {
    return escaped_at(source, position);
}

[[nodiscard]] std::size_t find_math_closing(std::string_view source, std::string_view delimiter, std::size_t start) {
    auto position = source.find(delimiter, start);
    while (position != std::string_view::npos && escaped_math_delimiter(source, position)) {
        position = source.find(delimiter, position + delimiter.size());
    }
    return position;
}

void append_math_token(ProtectedMarkdown& protected_markdown, MathToken token) {
    token.marker = std::format("CCHMATHPLACEHOLDER{}END", protected_markdown.math_tokens.size());
    protected_markdown.text += token.marker;
    protected_markdown.math_tokens.push_back(std::move(token));
}

[[nodiscard]] bool valid_inline_dollar_math(
        std::string_view source, std::size_t opening_length, std::size_t closing_position) {
    const auto contents = source.substr(opening_length, closing_position - opening_length);
    if (contents.empty() || std::isspace(static_cast<unsigned char>(contents.back())) != 0 ||
            contents.find('`') != std::string_view::npos) {
        return false;
    }
    auto following = closing_position + opening_length;
    if (following < source.size() && std::isdigit(static_cast<unsigned char>(source[following])) != 0) return false;
    if (!contents.empty() && std::isupper(static_cast<unsigned char>(contents.front())) != 0 &&
            std::all_of(contents.begin(),
                    contents.end(),
                    [](char character) {
                        return std::isupper(static_cast<unsigned char>(character)) != 0 || character == '_' ||
                               std::isdigit(static_cast<unsigned char>(character)) != 0;
                    }) &&
            following < source.size() && std::isalpha(static_cast<unsigned char>(source[following])) != 0) {
        return false;
    }
    return true;
}

void protect_inline_math_line(
        std::string_view line, ProtectedMarkdown& protected_markdown, std::size_t& code_tick_length) {
    for (std::size_t index = 0; index < line.size();) {
        if (line[index] == '`') {
            std::size_t run = 1;
            while (index + run < line.size() && line[index + run] == '`')
                ++run;
            protected_markdown.text.append(line.substr(index, run));
            if (code_tick_length == 0) {
                code_tick_length = run;
            } else if (code_tick_length == run) {
                code_tick_length = 0;
            }
            index += run;
            continue;
        }
        if (code_tick_length != 0) {
            protected_markdown.text += line[index++];
            continue;
        }

        std::string_view opening;
        std::string_view closing;
        if (line[index] == '$' && !escaped_at(line, index)) {
            const auto opening_length = line.substr(index).starts_with("$$") ? 2U : 1U;
            if (opening_length == 1 && index + 1 < line.size() &&
                    std::isspace(static_cast<unsigned char>(line[index + 1])) != 0) {
                protected_markdown.text += line[index++];
                continue;
            }
            opening = opening_length == 2 ? "$$" : "$";
            closing = opening;
        } else if (line[index] == '\\' && index + 1 < line.size() &&
                   (line[index + 1] == '(' || line[index + 1] == '[') && !escaped_math_delimiter(line, index)) {
            opening = line[index + 1] == '(' ? "\\(" : "\\[";
            closing = line[index + 1] == '(' ? "\\)" : "\\]";
        }

        if (opening.empty()) {
            if (line[index] == '\\' && index + 1 < line.size() && line[index + 1] == '$') {
                protected_markdown.text += line[index++];
                protected_markdown.text += line[index++];
            } else if (line[index] == '\\' && index + 1 < line.size() && line[index + 1] == '\\' &&
                       index + 2 < line.size() && (line[index + 2] == '(' || line[index + 2] == '[')) {
                protected_markdown.text += line[index++];
                protected_markdown.text += line[index++];
                protected_markdown.text += line[index++];
            } else {
                protected_markdown.text += line[index++];
            }
            continue;
        }

        const auto opening_length = opening.size();
        const auto closing_position = find_math_closing(line, closing, index + opening_length);
        if (closing_position != std::string_view::npos && opening == "$" &&
                !valid_inline_dollar_math(line.substr(index), opening_length, closing_position - index)) {
            protected_markdown.text += line[index++];
            continue;
        }
        if (closing_position != std::string_view::npos) {
            const auto text = line.substr(index + opening_length, closing_position - index - opening_length);
            if (text.empty()) {
                protected_markdown.text += line[index++];
                continue;
            }
            const auto end = closing_position + closing.size();
            if (opening == "\\(" || opening == "\\[") {
                append_math_token(protected_markdown,
                        MathToken{
                                {},
                                std::string{line.substr(index, end - index)},
                                std::string{text},
                                opening == "\\[",
                                opening == "\\[",
                                false,
                        });
            } else {
                protected_markdown.text.append(line.substr(index, end - index));
            }
            index = end;
            continue;
        }

        if (opening == "\\(" || opening == "\\[") {
            const auto pending_text = line.substr(index + opening_length);
            append_math_token(protected_markdown,
                    MathToken{
                            {},
                            std::string{line.substr(index)},
                            std::string{pending_text},
                            opening == "\\[",
                            opening == "\\[",
                            true,
                    });
            break;
        }
        protected_markdown.text += line[index++];
    }
}

[[nodiscard]] ProtectedMarkdown protect_markdown_math(std::string_view input) {
    ProtectedMarkdown result;
    MarkdownFence fence;
    std::size_t code_tick_length = 0;
    std::size_t line_start = 0;
    while (line_start < input.size()) {
        const auto newline = input.find('\n', line_start);
        const auto line_end = newline == std::string_view::npos ? input.size() : newline;
        const auto line = input.substr(line_start, line_end - line_start);
        const bool was_fenced = fence.marker != '\0';
        const bool fence_line = update_markdown_fence(line, fence);
        if (was_fenced || fence_line) {
            result.text.append(line);
        } else {
            const auto indent = line.find_first_not_of(' ');
            const auto content_start = indent == std::string_view::npos ? line.size() : indent;
            const auto content = line.substr(content_start);
            const bool display_dollar = content.starts_with("$$") && indent <= 3 && !escaped_at(line, content_start);
            const bool display_bracket =
                    content.starts_with("\\[") && indent <= 3 && !escaped_math_delimiter(line, content_start);
            if ((display_dollar || display_bracket) && code_tick_length == 0) {
                const std::string_view delimiter = display_dollar ? "$$" : "\\[";
                const std::string_view closing = display_dollar ? "$$" : "\\]";
                std::size_t closing_line_end = std::string_view::npos;
                std::size_t closing_position = std::string_view::npos;
                std::size_t scan_line_start = line_start;
                while (scan_line_start <= input.size()) {
                    const auto scan_newline = input.find('\n', scan_line_start);
                    const auto scan_line_end = scan_newline == std::string_view::npos ? input.size() : scan_newline;
                    const auto scan_line = input.substr(scan_line_start, scan_line_end - scan_line_start);
                    auto candidate = scan_line.find(closing);
                    while (candidate != std::string_view::npos &&
                            (escaped_math_delimiter(scan_line, candidate) ||
                                    (scan_line_start == line_start && candidate < content_start + delimiter.size()))) {
                        candidate = scan_line.find(closing, candidate + closing.size());
                    }
                    if (candidate != std::string_view::npos &&
                            scan_line.substr(candidate + closing.size()).find_first_not_of(" \t\r") ==
                                    std::string_view::npos) {
                        closing_line_end = scan_line_end;
                        closing_position = scan_line_start + candidate;
                        break;
                    }
                    if (scan_newline == std::string_view::npos) break;
                    scan_line_start = scan_newline + 1;
                }
                if (closing_position != std::string_view::npos &&
                        closing_position >= line_start + content_start + delimiter.size()) {
                    const auto source_end = closing_line_end < input.size() ? closing_line_end + 1 : closing_line_end;
                    const auto source = input.substr(line_start, source_end - line_start);
                    const auto content_start_offset = line_start + content_start + delimiter.size();
                    const auto content_end = closing_position;
                    auto text = input.substr(content_start_offset, content_end - content_start_offset);
                    while (!text.empty() && (text.front() == '\n' || text.front() == '\r'))
                        text.remove_prefix(1);
                    auto trimmed_text = text;
                    while (!trimmed_text.empty() &&
                            (trimmed_text.back() == '\n' || trimmed_text.back() == '\r' || trimmed_text.back() == ' ' ||
                                    trimmed_text.back() == '\t')) {
                        trimmed_text.remove_suffix(1);
                    }
                    if (!trimmed_text.empty()) {
                        if (!result.text.empty() && !result.text.ends_with("\n\n")) {
                            result.text += result.text.ends_with('\n') ? "\n" : "\n\n";
                        }
                        append_math_token(result,
                                MathToken{
                                        {},
                                        std::string{source},
                                        std::string{trimmed_text},
                                        true,
                                        true,
                                        false,
                                });
                        result.text += "\n\n";
                        line_start = source_end;
                        code_tick_length = 0;
                        continue;
                    }
                } else {
                    const auto pending_source = input.substr(line_start);
                    const auto pending_math = pending_source.substr(content_start + delimiter.size());
                    if (display_bracket || (!pending_math.empty() && looks_like_pending_dollar_math(pending_math))) {
                        if (!result.text.empty() && !result.text.ends_with("\n\n")) {
                            result.text += result.text.ends_with('\n') ? "\n" : "\n\n";
                        }
                        append_math_token(result,
                                MathToken{
                                        {},
                                        std::string{pending_source},
                                        std::string{pending_math},
                                        true,
                                        true,
                                        true,
                                });
                        line_start = input.size();
                        break;
                    }
                }
            }
            protect_inline_math_line(line, result, code_tick_length);
        }
        if (newline != std::string_view::npos) result.text += '\n';
        if (newline == std::string_view::npos) {
            scan_pending_display_math(result.text, result);
            break;
        }
        line_start = newline + 1;
    }
    return result;
}

void restore_math_tokens(BlockNode& block, const std::vector<MathToken>& math_tokens) {
    for (auto& child : block.children)
        restore_math_tokens(*child, math_tokens);
    std::vector<std::unique_ptr<InlineNode>> restored;
    for (auto& node : block.inlines) {
        if (node->kind != InlineKind::Text) {
            restored.push_back(std::move(node));
            continue;
        }
        std::size_t position = 0;
        while (position < node->text.size()) {
            std::size_t token_position = std::string::npos;
            const MathToken* matched_token = nullptr;
            for (const auto& token : math_tokens) {
                const auto found = node->text.find(token.marker, position);
                if (found != std::string::npos && found < token_position) {
                    token_position = found;
                    matched_token = &token;
                }
            }
            if (matched_token == nullptr) {
                auto text = std::make_unique<InlineNode>();
                text->text = node->text.substr(position);
                restored.push_back(std::move(text));
                break;
            }
            if (token_position > position) {
                auto text = std::make_unique<InlineNode>();
                text->text = node->text.substr(position, token_position - position);
                restored.push_back(std::move(text));
            }
            auto math = std::make_unique<InlineNode>();
            math->kind = InlineKind::Latex;
            math->text = matched_token->text;
            math->source = matched_token->source;
            math->latex_display = matched_token->display;
            math->latex_block = matched_token->block;
            math->latex_pending = matched_token->pending;
            restored.push_back(std::move(math));
            position = token_position + matched_token->marker.size();
        }
        if (node->text.empty()) restored.push_back(std::move(node));
    }
    block.inlines = std::move(restored);
    for (auto& node : block.inlines) {
        if (node->kind == InlineKind::Latex && node->source.empty()) {
            const std::string delimiter = node->latex_display ? "$$" : "$";
            node->source = delimiter + node->text + delimiter;
        }
    }
    for (auto& node : block.inlines) {
        if (node->kind != InlineKind::Latex) continue;
        for (auto& child : node->children) {
            if (child->kind == InlineKind::Text) child->kind = InlineKind::Code;
        }
    }
    if (block.kind != BlockKind::Paragraph || block.inlines.size() != 1 ||
            block.inlines.front()->kind != InlineKind::Latex || !block.inlines.front()->latex_block) {
        return;
    }
    const auto& math = *block.inlines.front();
    block.kind = BlockKind::Latex;
    block.text = math.text;
    block.raw = math.source;
    block.latex_display = math.latex_display;
    block.latex_pending = math.latex_pending;
    block.inlines.clear();
}

[[nodiscard]] bool is_table_delimiter(std::string_view line) {
    bool has_pipe = false;
    bool has_dash = false;
    for (const auto character : line) {
        if (character == '|') {
            has_pipe = true;
        } else if (character == '-') {
            has_dash = true;
        } else if (character != ':' && character != ' ' && character != '\t' && character != '\r') {
            return false;
        }
    }
    return has_pipe && has_dash;
}

[[nodiscard]] std::vector<std::string> raw_tables(std::string_view text) {
    std::vector<std::string> tables;
    std::vector<std::string_view> lines;
    std::size_t start = 0;
    while (start <= text.size()) {
        const auto end = text.find('\n', start);
        lines.push_back(text.substr(start, end == std::string_view::npos ? text.size() - start : end - start));
        if (end == std::string_view::npos) break;
        start = end + 1;
    }

    MarkdownFence fence;
    std::vector<bool> fenced(lines.size(), false);
    for (std::size_t index = 0; index < lines.size(); ++index)
        fenced[index] = update_markdown_fence(lines[index], fence);

    for (std::size_t index = 0; index + 1 < lines.size(); ++index) {
        if (fenced[index] || fenced[index + 1] || lines[index].find('|') == std::string_view::npos ||
                !is_table_delimiter(lines[index + 1])) {
            continue;
        }
        std::string table;
        auto row = index;
        while (row < lines.size() && !fenced[row] && lines[row].find('|') != std::string_view::npos &&
                !lines[row].empty()) {
            if (!table.empty()) table += '\n';
            table += lines[row];
            ++row;
        }
        tables.push_back(std::move(table));
        index = row > index ? row - 1 : index;
    }
    return tables;
}

[[nodiscard]] support::Expected<BlockNode> parse_markdown(
        std::string_view text, const std::vector<MathToken>& math_tokens, bool render_latex) {
    ParseState state;
    state.math_tokens = math_tokens;
    state.root.kind = BlockKind::Document;
    state.strict_strikethrough = scan_strikethrough_delimiters(text);
    state.table_raw = raw_tables(text);
    const MD_PARSER parser{
            .abi_version = 0,
            .flags = MD_FLAG_PERMISSIVEAUTOLINKS | MD_FLAG_STRIKETHROUGH | MD_FLAG_TASKLISTS | MD_FLAG_TABLES |
                     MD_FLAG_LATEXMATHSPANS,
            .enter_block = enter_block,
            .leave_block = leave_block,
            .enter_span = enter_span,
            .leave_span = leave_span,
            .text = append_text,
            .debug_log = nullptr,
            .syntax = nullptr,
    };
    const auto parse_result = md_parse(
        text.data(),
        static_cast<MD_SIZE>(text.size()),
        &parser,
        &state);
    if (parse_result == 0 && !state.failed) {
        restore_math_tokens(state.root, state.math_tokens);
        const auto set_latex_enabled = [&](auto&& self, BlockNode& block) -> void {
            for (auto& node : block.inlines) {
                const auto set_inline = [&](auto&& inline_self, InlineNode& inline_node) -> void {
                    if (inline_node.kind == InlineKind::Latex) inline_node.latex_enabled = render_latex;
                    for (auto& child : inline_node.children)
                        inline_self(inline_self, *child);
                };
                set_inline(set_inline, *node);
            }
            for (auto& child : block.children)
                self(self, *child);
        };
        set_latex_enabled(set_latex_enabled, state.root);
        return std::move(state.root);
    }
    return std::unexpected(support::make_error(
        support::ErrorCode::Unknown,
        "TUI Markdown parsing failed",
        state.failed
            ? "a parser callback threw an exception"
            : "the Markdown parser aborted unexpectedly"));
}

[[nodiscard]] MarkdownStyleHook* role_hook(StyleRole role, MarkdownStyleConfig& style) {
    switch (role) {
        case StyleRole::Heading:
            return &style.heading;
        case StyleRole::Quote:
            return &style.quote;
        case StyleRole::Emphasis:
            return &style.emphasis;
        case StyleRole::Strong:
            return &style.strong;
        case StyleRole::Underline:
            return &style.underline;
        case StyleRole::Strikethrough:
            return &style.strikethrough;
        case StyleRole::InlineCode:
            return &style.inline_code;
        case StyleRole::Link:
            return &style.link_text;
    }
    return nullptr;
}

[[nodiscard]] std::string apply_style_hook(MarkdownStyleHook& hook, std::string text) {
    if (!hook) return text;
    const auto original = text;
    const auto input_width = visible_width(text);
    auto styled = hook(std::move(text));
    return visible_width(styled) == input_width ? std::move(styled) : original;
}

[[nodiscard]] std::string apply_roles(
    std::string text,
    const std::vector<StyleRole>& roles,
    MarkdownStyleConfig& style) {
    const auto has_contextual_style = std::any_of(roles.begin(), roles.end(), [](StyleRole role) {
        return role == StyleRole::Quote || role == StyleRole::Heading;
    });
    for (auto iterator = roles.rbegin(); iterator != roles.rend(); ++iterator) {
        auto* hook = role_hook(*iterator, style);
        if (hook != nullptr) text = apply_style_hook(*hook, std::move(text));
    }
    return has_contextual_style ? text : apply_style_hook(style.text, std::move(text));
}

[[nodiscard]] std::string role_style_prefix(const std::vector<StyleRole>& roles, MarkdownStyleConfig& style) {
    constexpr std::string_view kSentinel{"\0", 1};
    auto styled = apply_roles(std::string{kSentinel}, roles, style);
    const auto sentinel = styled.find(kSentinel);
    return sentinel == std::string::npos ? std::string{} : styled.substr(0, sentinel);
}

[[nodiscard]] std::string plain_inline_text(const InlineNode& node) {
    if (node.kind == InlineKind::Text || node.kind == InlineKind::Latex) return node.text;
    std::string result;
    for (const auto& child : node.children) result += plain_inline_text(*child);
    return result;
}

[[nodiscard]] std::string render_inline_node(
    const InlineNode& node,
    std::vector<StyleRole> roles,
    MarkdownStyleConfig& style);

[[nodiscard]] std::string render_inline_children(
    const std::vector<std::unique_ptr<InlineNode>>& nodes,
    const std::vector<StyleRole>& roles,
    MarkdownStyleConfig& style) {
    std::string result;
    for (const auto& node : nodes) result += render_inline_node(*node, roles, style);
    return result;
}

[[nodiscard]] std::string render_link(
    const InlineNode& node,
    std::vector<StyleRole> roles,
    MarkdownStyleConfig& style) {
    roles.push_back(StyleRole::Link);
    roles.push_back(StyleRole::Underline);
    auto label = render_inline_children(node.children, roles, style);
    const auto plain_label = plain_inline_text(node);
    if (style.link) return style.link(std::move(label), node.destination);
    if (get_image_capabilities().hyperlinks) return hyperlink(label, node.destination);
    auto comparable_destination = node.destination;
    if (comparable_destination.starts_with("mailto:")) comparable_destination.erase(0, 7);
    if (node.autolink || plain_label == node.destination || plain_label == comparable_destination) return label;
    auto destination = std::format(" ({})", node.destination);
    destination = apply_style_hook(style.link_url, std::move(destination));
    destination += role_style_prefix(roles, style);
    return label + destination;
}

[[nodiscard]] std::string render_inline_node(
    const InlineNode& node,
    std::vector<StyleRole> roles,
    MarkdownStyleConfig& style) {
    switch (node.kind) {
        case InlineKind::Text:
            return apply_roles(node.text, roles, style);
        case InlineKind::Emphasis:
            roles.push_back(StyleRole::Emphasis);
            return render_inline_children(node.children, roles, style);
        case InlineKind::Strong:
            roles.push_back(StyleRole::Strong);
            return render_inline_children(node.children, roles, style);
        case InlineKind::Code: {
            const auto code = plain_inline_text(node);
            const auto styled_code = apply_style_hook(style.inline_code, code);
            return styled_code + role_style_prefix(roles, style);
        }
        case InlineKind::Latex: {
            const auto rendered = node.latex_enabled && !node.latex_pending
                                          ? cch::tui::render_latex(node.text, {.display = node.latex_display})
                                          : std::nullopt;
            const auto text = rendered.value_or(node.source);
            std::string result;
            std::size_t start = 0;
            while (start <= text.size()) {
                const auto newline = text.find('\n', start);
                result += apply_roles(std::string{text.substr(start,
                                              newline == std::string::npos ? text.size() - start : newline - start)},
                        roles,
                        style);
                if (newline == std::string::npos) break;
                result += '\n';
                start = newline + 1;
            }
            return result;
        }
        case InlineKind::Strikethrough: {
            if (!node.strict_strikethrough) {
                return apply_roles("~~", roles, style) +
                       render_inline_children(node.children, roles, style) +
                       apply_roles("~~", roles, style);
            }
            roles.push_back(StyleRole::Strikethrough);
            return render_inline_children(node.children, roles, style);
        }
        case InlineKind::Link:
            return render_link(node, roles, style);
        case InlineKind::Image: {
            auto label = render_inline_children(node.children, roles, style);
            return node.destination.empty() ? label : std::format("{} ({})", label, node.destination);
        }
    }
    return {};
}

[[nodiscard]] std::vector<StyleRole> block_roles(bool heading, std::size_t quote_depth) {
    std::vector<StyleRole> roles;
    for (std::size_t depth = 0; depth < quote_depth; ++depth) {
        roles.push_back(StyleRole::Quote);
        roles.push_back(StyleRole::Emphasis);
    }
    if (heading) roles.push_back(StyleRole::Heading);
    return roles;
}

struct RenderContext {
    MarkdownStyleConfig& style; // must outlive the synchronous render operation
    SyntaxHighlightHook& syntax_highlighter; // must outlive the synchronous render operation
    bool render_latex{true};
    std::size_t quote_depth{0};
};

using RenderedLines = support::Expected<std::vector<std::string>>;

[[nodiscard]] RenderedLines add_prefix(
    const std::vector<std::string>& logical_lines,
    std::string first_prefix,
    std::string continuation_prefix,
    std::size_t width) {
    const auto prefix_width = visible_width(first_prefix);
    const auto continuation_width = visible_width(continuation_prefix);
    if (prefix_width >= width || continuation_width >= width) {
        std::string unprefixed;
        for (std::size_t index = 0; index < logical_lines.size(); ++index) {
            if (index > 0) unprefixed += '\n';
            unprefixed += logical_lines[index];
        }
        return wrap_text(unprefixed, width);
    }
    std::vector<std::string> result;
    bool first = true;
    for (const auto& logical_line : logical_lines) {
        const auto available = width - (first ? prefix_width : continuation_width);
        if (auto wrapped = wrap_text(logical_line, available); wrapped) {
            for (const auto& line : *wrapped) {
                auto& prefix = first ? first_prefix : continuation_prefix;
                result.push_back(prefix + line);
                first = false;
            }
        } else {
            return std::unexpected(wrapped.error());
        }
    }
    if (result.empty()) result.push_back(std::move(first_prefix));
    return result;
}

[[nodiscard]] bool is_list(const BlockNode& block) {
    return block.kind == BlockKind::UnorderedList || block.kind == BlockKind::OrderedList;
}

[[nodiscard]] RenderedLines render_block(const BlockNode& block, std::size_t width, RenderContext context);

[[nodiscard]] std::size_t longest_word_width(std::string_view text, std::size_t max_width) {
    auto tokens = detail::tokenize_terminal_output(text);
    if (!tokens) return 0;
    std::size_t longest = 0;
    std::size_t word_width = 0;
    for (const auto& token : *tokens) {
        if (token.kind == detail::TerminalTokenKind::Newline) {
            longest = std::max(longest, word_width);
            word_width = 0;
            continue;
        }
        if (token.kind != detail::TerminalTokenKind::Grapheme) continue;
        const auto [codepoint, bytes] = detail::decode_utf8(token.text, 0);
        const auto category =
                bytes == 0 ? UTF8PROC_CATEGORY_CN : utf8proc_category(static_cast<utf8proc_int32_t>(codepoint));
        const auto is_space = codepoint == ' ' || category == UTF8PROC_CATEGORY_ZS ||
                              category == UTF8PROC_CATEGORY_ZL || category == UTF8PROC_CATEGORY_ZP ||
                              codepoint == '\t' || codepoint == '\n' || codepoint == '\r' || codepoint == '\f' ||
                              codepoint == '\v';
        if (is_space) {
            longest = std::max(longest, word_width);
            word_width = 0;
        } else {
            word_width += token.width;
        }
    }
    return std::min(std::max(longest, word_width), max_width);
}

[[nodiscard]] RenderedLines wrap_cell_text(
        std::string_view text, std::size_t column_width, std::size_t padded_width, std::string_view style_prefix) {
    if (auto wrapped = wrap_text(text, std::max<std::size_t>(1, column_width)); wrapped) {
        constexpr std::string_view kStyleReset{"\x1b[22;23;24;25;27;28;29;39m"};
        std::vector<std::string> result;
        result.reserve(wrapped->size());
        for (std::size_t index = 0; index < wrapped->size(); ++index) {
            auto line = std::move((*wrapped)[index]);
            if (index + 1 < wrapped->size()) line += kStyleReset;
            line += style_prefix;
            const auto line_width = visible_width(line);
            if (line_width < padded_width) line.append(padded_width - line_width, ' ');
            result.push_back(std::move(line));
        }
        return result;
    } else {
        return std::unexpected(wrapped.error());
    }
}

[[nodiscard]] RenderedLines render_table(const BlockNode& table, std::size_t width, RenderContext context) {
    const auto num_columns = static_cast<std::size_t>(table.column_count);
    if (num_columns == 0) return std::vector<std::string>{};

    const auto border_overhead = 3 * num_columns + 1;
    const auto available_for_cells = width - std::min(width, border_overhead);
    if (available_for_cells < num_columns) {
        if (table.raw.empty()) return std::vector<std::string>{};
        return wrap_text(table.raw, width);
    }

    constexpr std::size_t kMaxUnbrokenWordWidth = 30;
    const BlockNode* header_row = nullptr;
    const BlockNode* body = nullptr;
    for (const auto& section : table.children) {
        if (section->kind == BlockKind::TableHead && !section->children.empty())
            header_row = section->children.front().get();
        if (section->kind == BlockKind::TableBody) body = section.get();
    }
    if (header_row == nullptr || header_row->children.empty()) return std::vector<std::string>{};

    std::vector<std::vector<const BlockNode*>> table_rows;
    table_rows.push_back({});
    for (const auto& cell : header_row->children)
        table_rows.front().push_back(cell.get());
    if (body != nullptr) {
        for (const auto& row : body->children) {
            if (row->kind != BlockKind::TableRow) continue;
            std::vector<const BlockNode*> cells;
            for (const auto& cell : row->children)
                cells.push_back(cell.get());
            table_rows.push_back(std::move(cells));
        }
    }

    const auto cell_text = [&](const BlockNode* cell) {
        return cell == nullptr
                       ? std::string{}
                       : render_inline_children(cell->inlines, block_roles(false, context.quote_depth), context.style);
    };
    std::vector<std::size_t> natural_widths(num_columns, 0);
    std::vector<std::size_t> min_word_widths(num_columns, 1);
    for (const auto& row : table_rows) {
        for (std::size_t column = 0; column < std::min(num_columns, row.size()); ++column) {
            const auto text = cell_text(row[column]);
            natural_widths[column] = std::max(natural_widths[column], visible_width(text));
            min_word_widths[column] =
                    std::max(min_word_widths[column], longest_word_width(text, kMaxUnbrokenWordWidth));
        }
    }

    auto min_column_widths = min_word_widths;
    auto min_cells_width = std::accumulate(min_column_widths.begin(), min_column_widths.end(), std::size_t{0});
    if (min_cells_width > available_for_cells) {
        min_column_widths.assign(num_columns, 1);
        const auto remaining = available_for_cells - num_columns;
        if (remaining > 0) {
            std::size_t total_weight = 0;
            for (const auto min_word_width : min_word_widths)
                total_weight += min_word_width - 1;
            std::vector<std::size_t> growth(num_columns, 0);
            if (total_weight > 0) {
                for (std::size_t column = 0; column < num_columns; ++column) {
                    growth[column] = ((min_word_widths[column] - 1) * remaining) / total_weight;
                    min_column_widths[column] += growth[column];
                }
            }
            const auto allocated = std::accumulate(growth.begin(), growth.end(), std::size_t{0});
            for (auto leftover = remaining - allocated; leftover > 0; --leftover) {
                ++min_column_widths[remaining - allocated - leftover];
            }
        }
        min_cells_width = std::accumulate(min_column_widths.begin(), min_column_widths.end(), std::size_t{0});
    }

    const auto total_natural_width =
            std::accumulate(natural_widths.begin(), natural_widths.end(), std::size_t{0}) + border_overhead;
    std::vector<std::size_t> column_widths;
    if (total_natural_width <= width) {
        column_widths.resize(num_columns);
        for (std::size_t column = 0; column < num_columns; ++column) {
            column_widths[column] = std::max(natural_widths[column], min_column_widths[column]);
        }
    } else {
        std::size_t total_grow_potential = 0;
        for (std::size_t column = 0; column < num_columns; ++column) {
            total_grow_potential += natural_widths[column] > min_column_widths[column]
                                            ? natural_widths[column] - min_column_widths[column]
                                            : 0;
        }
        const auto extra_width = available_for_cells > min_cells_width ? available_for_cells - min_cells_width : 0;
        column_widths.resize(num_columns);
        for (std::size_t column = 0; column < num_columns; ++column) {
            const auto delta = natural_widths[column] > min_column_widths[column]
                                       ? natural_widths[column] - min_column_widths[column]
                                       : 0;
            const auto grow = total_grow_potential > 0 ? (delta * extra_width) / total_grow_potential : 0;
            column_widths[column] = min_column_widths[column] + grow;
        }
        auto allocated = std::accumulate(column_widths.begin(), column_widths.end(), std::size_t{0});
        auto remaining = available_for_cells - allocated;
        while (remaining > 0) {
            bool grew = false;
            for (std::size_t column = 0; column < num_columns && remaining > 0; ++column) {
                if (column_widths[column] < natural_widths[column]) {
                    ++column_widths[column];
                    --remaining;
                    grew = true;
                }
            }
            if (!grew) break;
        }
    }

    const auto append_rule = [](std::string& line, std::size_t width) {
        for (std::size_t index = 0; index < width; ++index)
            line += "─";
    };
    const auto top_border = [&] {
        std::string line{"┌─"};
        for (std::size_t column = 0; column < num_columns; ++column) {
            if (column > 0) line += "─┬─";
            append_rule(line, column_widths[column]);
        }
        return line + "─┐";
    };
    const auto separator = [&] {
        std::string line{"├─"};
        for (std::size_t column = 0; column < num_columns; ++column) {
            if (column > 0) line += "─┼─";
            append_rule(line, column_widths[column]);
        }
        return line + "─┤";
    };
    const auto bottom_border = [&] {
        std::string line{"└─"};
        for (std::size_t column = 0; column < num_columns; ++column) {
            if (column > 0) line += "─┴─";
            append_rule(line, column_widths[column]);
        }
        return line + "─┘";
    };

    std::vector<std::string> lines{top_border()};
    const auto style_prefix = role_style_prefix(block_roles(false, context.quote_depth), context.style);
    for (std::size_t row_index = 0; row_index < table_rows.size(); ++row_index) {
        std::vector<std::vector<std::string>> wrapped_cells;
        wrapped_cells.reserve(num_columns);
        for (std::size_t column = 0; column < num_columns; ++column) {
            const auto* cell = column < table_rows[row_index].size() ? table_rows[row_index][column] : nullptr;
            const auto text = cell_text(cell);
            if (auto wrapped = wrap_cell_text(text, column_widths[column], column_widths[column], style_prefix);
                    wrapped) {
                wrapped_cells.push_back(std::move(*wrapped));
            } else {
                return std::unexpected(wrapped.error());
            }
        }
        std::size_t row_line_count = 1;
        for (const auto& cell : wrapped_cells)
            row_line_count = std::max(row_line_count, cell.size());
        for (std::size_t line_index = 0; line_index < row_line_count; ++line_index) {
            std::string line{"│ "};
            for (std::size_t column = 0; column < num_columns; ++column) {
                if (column > 0) line += " │ ";
                auto text = line_index < wrapped_cells[column].size() ? wrapped_cells[column][line_index]
                                                                      : std::string(column_widths[column], ' ');
                if (row_index == 0) {
                    if (context.style.strong) text = apply_style_hook(context.style.strong, std::move(text));
                    text = "\x1b[1m" + text + "\x1b[22m";
                }
                line += text;
            }
            line += " │";
            lines.push_back(std::move(line));
        }
        if (row_index == 0)
            lines.push_back(separator());
        else if (row_index + 1 < table_rows.size())
            lines.push_back(separator());
    }
    lines.push_back(bottom_border());
    return lines;
}

[[nodiscard]] RenderedLines render_sequence(
    const std::vector<std::unique_ptr<BlockNode>>& blocks,
    std::size_t width,
    RenderContext context,
    bool separated) {
    std::vector<std::string> result;
    for (std::size_t index = 0; index < blocks.size(); ++index) {
        const auto& block = blocks[index];
        if (block->kind == BlockKind::Table) {
            if (auto rendered = render_table(*block, width, context); rendered) {
                if (separated && !result.empty() && !rendered->empty() && !result.back().empty()) {
                    result.push_back({});
                }
                result.insert(result.end(), rendered->begin(), rendered->end());
                if (!separated && index + 1 < blocks.size() && !result.empty() && !result.back().empty()) {
                    result.push_back({});
                }
            } else {
                return std::unexpected(rendered.error());
            }
            continue;
        }
        if (auto rendered = render_block(*block, width, context); rendered) {
            if (separated && !result.empty() && !rendered->empty() && !result.back().empty()) {
                result.push_back({});
            }
            result.insert(result.end(), rendered->begin(), rendered->end());
            if (!separated && block->kind == BlockKind::Table && index + 1 < blocks.size() && !result.empty() &&
                    !result.back().empty()) {
                result.push_back({});
            }
        } else {
            return std::unexpected(rendered.error());
        }
    }
    while (!result.empty() && result.back().empty()) result.pop_back();
    return result;
}

[[nodiscard]] std::vector<std::string> fallback_code_lines(
    std::string_view code,
    MarkdownStyleConfig& style) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start <= code.size()) {
        const auto end = code.find('\n', start);
        const auto length = end == std::string_view::npos ? code.size() - start : end - start;
        lines.push_back(apply_style_hook(style.code_block, std::string(code.substr(start, length))));
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    if (lines.empty()) lines.push_back({});
    return lines;
}

[[nodiscard]] RenderedLines render_code(const BlockNode& block, std::size_t width, RenderContext context) {
    const auto code = block.text.ends_with('\n')
                          ? block.text.substr(0, block.text.size() - 1)
                          : block.text;
    const auto border_roles = block_roles(false, context.quote_depth);
    auto opening = apply_style_hook(
        context.style.code_block_border,
        std::format("```{}", block.language));
    auto closing = apply_style_hook(context.style.code_block_border, "```");
    if (!border_roles.empty()) {
        opening = apply_roles(std::move(opening), border_roles, context.style);
        closing = apply_roles(std::move(closing), border_roles, context.style);
    }

    std::vector<std::string> code_lines;
    bool highlighted = false;
    if (context.syntax_highlighter) {
        auto candidate = context.syntax_highlighter(code, block.language);
        if (candidate && (!candidate->empty() || code.empty())) {
            code_lines = std::move(*candidate);
            highlighted = true;
        }
    }
    if (!highlighted) code_lines = fallback_code_lines(code, context.style);

    std::vector<std::string> result;
    if (auto wrapped_opening = wrap_text(opening, width); wrapped_opening) {
        result.insert(result.end(), wrapped_opening->begin(), wrapped_opening->end());
    } else {
        return std::unexpected(wrapped_opening.error());
    }

    const auto configured_indent_width = visible_width(context.style.code_block_indent);
    const auto indent = configured_indent_width < width ? context.style.code_block_indent : std::string{};
    const auto code_width = width - visible_width(indent);
    if (highlighted) {
        for (const auto& code_line : code_lines) {
            if (auto wrapped = wrap_text(code_line, code_width); !wrapped) {
                code_lines.clear();
                highlighted = false;
                break;
            }
        }
        if (!highlighted) code_lines = fallback_code_lines(code, context.style);
    }
    for (const auto& code_line : code_lines) {
        if (auto wrapped = wrap_text(code_line, code_width); wrapped) {
            for (const auto& line : *wrapped) {
                auto rendered_line = indent + line;
                if (!border_roles.empty())
                    rendered_line = apply_roles(std::move(rendered_line), border_roles, context.style);
                result.push_back(std::move(rendered_line));
            }
        } else {
            return std::unexpected(wrapped.error());
        }
    }

    if (auto wrapped_closing = wrap_text(closing, width); wrapped_closing) {
        result.insert(result.end(), wrapped_closing->begin(), wrapped_closing->end());
    } else {
        return std::unexpected(wrapped_closing.error());
    }
    return result;
}

[[nodiscard]] RenderedLines render_list(const BlockNode& block, std::size_t width, RenderContext context) {
    std::vector<std::string> result;
    std::size_t item_index = 0;
    for (const auto& item : block.children) {
        if (item->kind != BlockKind::ListItem) continue;
        std::string marker;
        if (block.kind == BlockKind::OrderedList) {
            marker = std::format("{}{} ", block.ordered_start + item_index, block.ordered_delimiter);
        } else {
            marker = std::format("{} ", block.list_marker);
        }
        if (item->task_item) marker += item->task_checked ? "[x] " : "[ ] ";
        auto styled_marker = apply_style_hook(context.style.list_marker, marker);
        const auto marker_roles = block_roles(false, context.quote_depth);
        if (!marker_roles.empty()) styled_marker = apply_roles(std::move(styled_marker), marker_roles, context.style);
        auto marker_width = visible_width(styled_marker);
        if (marker_width >= width) {
            styled_marker.clear();
            marker_width = 0;
        }

        bool wrote_content = false;
        if (!item->inlines.empty()) {
            const auto item_text =
                    render_inline_children(item->inlines, block_roles(false, context.quote_depth), context.style);
            if (auto item_lines = wrap_text(item_text, width - marker_width); item_lines) {
                if (auto prefixed = add_prefix(
                        *item_lines,
                        styled_marker,
                        std::string(marker_width, ' '),
                        width);
                    prefixed) {
                    result.insert(result.end(), prefixed->begin(), prefixed->end());
                    wrote_content = true;
                } else {
                    return std::unexpected(prefixed.error());
                }
            } else {
                return std::unexpected(item_lines.error());
            }
        }
        for (const auto& child : item->children) {
            if (!block.tight_list && wrote_content && !is_list(*child) &&
                !result.empty() && !result.back().empty()) {
                result.push_back({});
            }
            if (is_list(*child)) {
                constexpr std::size_t kPreferredNestedIndent = 4;
                const auto nested_indent = std::min(kPreferredNestedIndent, width - 1);
                if (auto nested = render_block(*child, width - nested_indent, context); nested) {
                    for (const auto& line : *nested) {
                        result.push_back(std::string(nested_indent, ' ') + line);
                    }
                    wrote_content = true;
                    continue;
                } else {
                    return std::unexpected(nested.error());
                }
            }
            if (auto child_lines = render_block(*child, width - marker_width, context); child_lines) {
                if (auto prefixed = add_prefix(
                        *child_lines,
                        wrote_content ? std::string(marker_width, ' ') : styled_marker,
                        std::string(marker_width, ' '),
                        width);
                    prefixed) {
                    result.insert(result.end(), prefixed->begin(), prefixed->end());
                    wrote_content = true;
                } else {
                    return std::unexpected(prefixed.error());
                }
            } else {
                return std::unexpected(child_lines.error());
            }
        }
        if (!wrote_content) result.push_back(std::move(styled_marker));
        if (!block.tight_list && item_index + 1 < block.children.size()) result.push_back({});
        ++item_index;
    }
    return result;
}

[[nodiscard]] RenderedLines render_block(const BlockNode& block, std::size_t width, RenderContext context) {
    if (width == 0) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "TUI Markdown requires a positive content width"));
    }
    switch (block.kind) {
        case BlockKind::Document:
            return render_sequence(block.children, width, context, true);
        case BlockKind::Latex: {
            const auto rendered = context.render_latex && !block.latex_pending
                                          ? cch::tui::render_latex(block.text, {.display = block.latex_display})
                                          : std::nullopt;
            const auto text = rendered.value_or(block.raw);
            std::vector<std::string> lines;
            std::size_t start = 0;
            while (start <= text.size()) {
                const auto newline = text.find('\n', start);
                const auto line = std::string{
                        text.substr(start, newline == std::string::npos ? text.size() - start : newline - start)};
                if (auto wrapped = wrap_text(
                            apply_roles(line, block_roles(false, context.quote_depth), context.style), width);
                        wrapped) {
                    lines.insert(lines.end(), wrapped->begin(), wrapped->end());
                } else {
                    return std::unexpected(wrapped.error());
                }
                if (newline == std::string::npos) break;
                start = newline + 1;
            }
            return lines;
        }
        case BlockKind::Paragraph: {
            const auto text =
                    render_inline_children(block.inlines, block_roles(false, context.quote_depth), context.style);
            return wrap_text(text, width);
        }
        case BlockKind::Table:
            return render_table(block, width, context);
        case BlockKind::TableHead:
        case BlockKind::TableBody:
        case BlockKind::TableRow:
            return render_sequence(block.children, width, context, false);
        case BlockKind::TableHeaderCell:
        case BlockKind::TableCell: {
            const auto text =
                    render_inline_children(block.inlines, block_roles(false, context.quote_depth), context.style);
            return wrap_text(text, width);
        }
        case BlockKind::Heading: {
            auto roles = block_roles(true, context.quote_depth);
            roles.push_back(StyleRole::Strong);
            if (block.heading_level == 1) roles.push_back(StyleRole::Underline);
            auto text = render_inline_children(block.inlines, roles, context.style);
            if (block.heading_level >= 3) {
                text = apply_roles(
                           std::string(block.heading_level, '#') + " ",
                           roles,
                           context.style) +
                       text;
            }
            return wrap_text(text, width);
        }
        case BlockKind::Rule: {
            std::string rule;
            for (std::size_t index = 0; index < std::min<std::size_t>(width, 80); ++index) rule += "─";
            rule = apply_style_hook(context.style.horizontal_rule, std::move(rule));
            return std::vector<std::string>{std::move(rule)};
        }
        case BlockKind::Code:
            return render_code(block, width, context);
        case BlockKind::Html:
            return wrap_text(apply_roles(block.text, block_roles(false, context.quote_depth), context.style), width);
        case BlockKind::Quote: {
            constexpr std::string_view kQuotePrefix = "│ ";
            constexpr std::size_t kQuoteWidth = 2;
            const auto prefix_width = kQuoteWidth < width ? kQuoteWidth : 0;
            auto quote_context = context;
            ++quote_context.quote_depth;
            std::vector<std::string> quote_lines;
            if (auto content = render_sequence(
                    block.children,
                    width - prefix_width,
                    quote_context,
                    true);
                content) {
                quote_lines = std::move(*content);
            } else {
                return std::unexpected(content.error());
            }
            auto styled_prefix = prefix_width == 0
                                         ? std::string{}
                                         : apply_style_hook(context.style.quote_border, std::string(kQuotePrefix));
            const auto parent_quote_roles = block_roles(false, context.quote_depth);
            if (!parent_quote_roles.empty()) {
                styled_prefix = apply_roles(std::move(styled_prefix), parent_quote_roles, context.style);
            }
            std::vector<std::string> result;
            for (const auto& line : quote_lines) result.push_back(styled_prefix + line);
            if (result.empty()) result.push_back(std::move(styled_prefix));
            return result;
        }
        case BlockKind::UnorderedList:
        case BlockKind::OrderedList:
            return render_list(block, width, context);
        case BlockKind::ListItem:
            return render_sequence(block.children, width, context, false);
    }
    return std::vector<std::string>{};
}

struct FenceRun {
    char marker{'\0'};
    std::size_t length{0};
};

void scan_pending_display_math(const std::string_view source_tail, ProtectedMarkdown& protected_markdown) {
    constexpr std::size_t kMaxOpenerLength = 2;
    for (std::size_t length = kMaxOpenerLength; length >= 1; --length) {
        if (source_tail.size() < length) continue;
        const auto suffix = source_tail.substr(source_tail.size() - length);
        if (suffix == "$$" || suffix == "\\[") {
            const auto position = source_tail.size() - length;
            if (position > 0 && source_tail[position - 1] == '\\') continue;
            append_math_token(protected_markdown,
                    MathToken{
                            {},
                            std::string{suffix},
                            {},
                            true,
                            true,
                            true,
                    });
            break;
        }
    }
}

[[nodiscard]] std::string_view strip_container_prefixes(std::string_view line) {
    while (!line.empty()) {
        while (!line.empty() && line.front() == ' ') line.remove_prefix(1);
        if (line.starts_with('>')) {
            line.remove_prefix(1);
            if (line.starts_with(' ')) line.remove_prefix(1);
            continue;
        }
        if (line.size() >= 2 && (line[0] == '-' || line[0] == '+' || line[0] == '*') &&
            line[1] == ' ') {
            line.remove_prefix(2);
            continue;
        }
        std::size_t digits = 0;
        while (digits < line.size() && digits < 9 &&
               std::isdigit(static_cast<unsigned char>(line[digits])) != 0) {
            ++digits;
        }
        if (digits > 0 && digits + 1 < line.size() &&
            (line[digits] == '.' || line[digits] == ')') && line[digits + 1] == ' ') {
            line.remove_prefix(digits + 2);
            continue;
        }
        break;
    }
    while (!line.empty() && line.front() == ' ') line.remove_prefix(1);
    return line;
}

[[nodiscard]] FenceRun fence_run(std::string_view line) {
    line = strip_container_prefixes(line);
    if (line.empty() || (line.front() != '`' && line.front() != '~')) return {};
    const auto marker = line.front();
    std::size_t length = 0;
    while (length < line.size() && line[length] == marker) ++length;
    return {.marker = marker, .length = length};
}

[[nodiscard]] std::string trim_partial_closing_fence(std::string text) {
    const auto candidate_start = text.rfind('\n');
    const auto line_start = candidate_start == std::string::npos ? 0 : candidate_start + 1;
    auto candidate = std::string_view(text).substr(line_start);
    if (candidate.ends_with('\r')) candidate.remove_suffix(1);
    candidate = strip_container_prefixes(candidate);
    if (candidate.empty() || (candidate.front() != '`' && candidate.front() != '~')) return text;
    const auto marker = candidate.front();
    if (!std::all_of(candidate.begin(), candidate.end(), [&](char character) {
            return character == marker;
        })) {
        return text;
    }

    FenceRun active;
    std::size_t position = 0;
    while (position < line_start) {
        const auto end = text.find('\n', position);
        const auto length = end == std::string::npos ? line_start - position : end - position;
        const auto run = fence_run(std::string_view(text).substr(position, length));
        if (active.length == 0 && run.length >= 3) {
            active = run;
        } else if (active.length > 0 && run.marker == active.marker && run.length >= active.length) {
            active = {};
        }
        if (end == std::string::npos) break;
        position = end + 1;
    }
    if (active.marker == marker && candidate.size() < active.length) text.erase(line_start);
    return text;
}

[[nodiscard]] std::string separate_table_following_content(std::string_view text) {
    std::vector<std::string_view> lines;
    std::size_t start = 0;
    while (start <= text.size()) {
        const auto end = text.find('\n', start);
        lines.push_back(text.substr(start, end == std::string_view::npos ? text.size() - start : end - start));
        if (end == std::string_view::npos) break;
        start = end + 1;
    }

    std::vector<bool> insert_blank_before(lines.size(), false);
    MarkdownFence fence;
    std::vector<bool> fenced(lines.size(), false);
    for (std::size_t index = 0; index < lines.size(); ++index)
        fenced[index] = update_markdown_fence(lines[index], fence);

    for (std::size_t index = 0; index + 1 < lines.size(); ++index) {
        if (fenced[index] || fenced[index + 1] || lines[index].find('|') == std::string_view::npos ||
                !is_table_delimiter(lines[index + 1])) {
            continue;
        }
        auto row = index + 2;
        while (row < lines.size() && !fenced[row] && !lines[row].empty() &&
                lines[row].find('|') != std::string_view::npos) {
            ++row;
        }
        if (row < lines.size() && !lines[row].empty()) insert_blank_before[row] = true;
        index = row > index ? row - 1 : index;
    }

    std::string result;
    for (std::size_t index = 0; index < lines.size(); ++index) {
        if (index > 0) result += '\n';
        if (insert_blank_before[index]) result += '\n';
        result += lines[index];
    }
    return result;
}

[[nodiscard]] std::string normalize_markdown_input(std::string_view input) {
    std::string normalized;
    normalized.reserve(input.size());
    for (const auto character : input) {
        if (character == '\t') {
            normalized += "   ";
        } else {
            normalized += character;
        }
    }
    return trim_partial_closing_fence(separate_table_following_content(normalized));
}

} // namespace

struct Markdown::Impl {
    std::string text;
    std::size_t padding_x{0};
    std::size_t padding_y{0};
    MarkdownStyleConfig style;
    SyntaxHighlightHook syntax_highlighter;
    MarkdownOptions options;
    BackgroundHook background_hook;
    std::string cached_text;
    std::size_t cached_width{0};
    std::vector<std::string> cached_lines;
    bool cache_valid{false};
};

Markdown::Markdown(std::string text,
        std::size_t padding_x,
        std::size_t padding_y,
        MarkdownStyleConfig style,
        SyntaxHighlightHook syntax_highlighter,
        BackgroundHook background_hook,
        MarkdownOptions options)
    : impl_(std::make_unique<Impl>()) {
    impl_->text = std::move(text);
    impl_->padding_x = padding_x;
    impl_->padding_y = padding_y;
    impl_->style = std::move(style);
    impl_->syntax_highlighter = std::move(syntax_highlighter);
    impl_->background_hook = std::move(background_hook);
    impl_->options = options;
}

Markdown::Markdown(Markdown&&) noexcept = default;
Markdown& Markdown::operator=(Markdown&&) noexcept = default;
Markdown::~Markdown() = default;

void Markdown::set_text(std::string text) {
    impl_->text = std::move(text);
    invalidate();
}

std::string_view Markdown::text() const {
    return impl_->text;
}

void Markdown::set_padding_x(std::size_t padding_x) {
    impl_->padding_x = padding_x;
    invalidate();
}

void Markdown::set_padding_y(std::size_t padding_y) {
    impl_->padding_y = padding_y;
    invalidate();
}

void Markdown::set_style(MarkdownStyleConfig style) {
    impl_->style = std::move(style);
    invalidate();
}

void Markdown::set_syntax_highlighter(SyntaxHighlightHook syntax_highlighter) {
    impl_->syntax_highlighter = std::move(syntax_highlighter);
    invalidate();
}

void Markdown::set_background_hook(BackgroundHook background_hook) {
    impl_->background_hook = std::move(background_hook);
    invalidate();
}

support::Expected<RenderResult> Markdown::render(std::size_t width) {
    if (impl_->cache_valid && impl_->cached_text == impl_->text && impl_->cached_width == width) {
        return RenderResult{.lines = impl_->cached_lines};
    }
    if (width == 0) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "TUI Markdown requires a positive visible width"));
    }
    if (impl_->text.empty() || impl_->text.find_first_not_of(" \t\r\n") == std::string::npos) {
        impl_->cached_text = impl_->text;
        impl_->cached_width = width;
        impl_->cached_lines.clear();
        impl_->cache_valid = true;
        return RenderResult{.lines = impl_->cached_lines};
    }
    if (impl_->padding_x >= width || impl_->padding_x >= width - impl_->padding_x) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "TUI Markdown width is too small for padding",
            std::format("width {} padding_x {}", width, impl_->padding_x)));
    }

    const auto content_width = width - impl_->padding_x - impl_->padding_x;
    const auto normalized = normalize_markdown_input(impl_->text);
    auto protected_markdown = protect_markdown_math(normalized);
    BlockNode document;
    if (auto parsed = parse_markdown(
                protected_markdown.text, protected_markdown.math_tokens, impl_->options.render_latex);
            parsed) {
        document = std::move(*parsed);
    } else {
        return std::unexpected(parsed.error());
    }
    const auto render_content = [&]() -> RenderedLines {
        return render_block(document,
                content_width,
                RenderContext{
                        .style = impl_->style,
                        .syntax_highlighter = impl_->syntax_highlighter,
                        .render_latex = impl_->options.render_latex,
                });
    };
    std::vector<std::string> content;
    if (auto rendered = render_content(); rendered) {
        content = std::move(*rendered);
    } else {
        return std::unexpected(rendered.error());
    }

    std::vector<std::string> result;
    const auto make_line = [&](std::string line, std::size_t line_width) -> support::Expected<std::string> {
        if (line_width > width) {
            return std::unexpected(support::make_error(
                    support::ErrorCode::Validation, "TUI Markdown composed a line wider than its width bound"));
        }
        line.append(width - line_width, ' ');
        return detail::apply_background(impl_->background_hook,
                detail::PreparedRenderedLine{.text = std::move(line), .width = width},
                width,
                "Markdown");
    };
    for (std::size_t index = 0; index < impl_->padding_y; ++index) {
        if (auto line = make_line(std::string(width, ' '), width); line) {
            result.push_back(std::move(*line));
        } else {
            return std::unexpected(line.error());
        }
    }
    for (const auto& line : content) {
        auto prepared_content = detail::prepare_rendered_line(line, width);
        if (!prepared_content) return std::unexpected(prepared_content.error());
        if (auto prepared = make_line(std::string(impl_->padding_x, ' ') + prepared_content->text,
                    impl_->padding_x + prepared_content->width);
                prepared) {
            result.push_back(std::move(*prepared));
        } else {
            return std::unexpected(prepared.error());
        }
    }
    for (std::size_t index = 0; index < impl_->padding_y; ++index) {
        if (auto line = make_line(std::string(width, ' '), width); line) {
            result.push_back(std::move(*line));
        } else {
            return std::unexpected(line.error());
        }
    }

    impl_->cached_text = impl_->text;
    impl_->cached_width = width;
    impl_->cached_lines = result;
    impl_->cache_valid = true;
    return RenderResult{.lines = std::move(result)};
}

void Markdown::invalidate() {
    impl_->cache_valid = false;
}

} // namespace cch::tui
