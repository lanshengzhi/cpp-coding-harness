#include <cch/tui/Markdown.hpp>

#include "tui/RenderUtils.hpp"
#include "tui/UnicodeWidth.hpp"

#include <md4c.h>

#include <algorithm>
#include <cctype>
#include <exception>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::tui {

namespace {

enum class BlockKind {
    Document,
    Quote,
    UnorderedList,
    OrderedList,
    ListItem,
    Rule,
    Heading,
    Code,
    Html,
    Paragraph,
};

enum class InlineKind {
    Text,
    Emphasis,
    Strong,
    Link,
    Image,
    Code,
    Strikethrough,
};

enum class StyleRole {
    Heading,
    Quote,
    Emphasis,
    Strong,
    Strikethrough,
    InlineCode,
    Link,
};

struct InlineNode {
    InlineKind kind{InlineKind::Text};
    std::string text;
    std::string destination;
    bool autolink{false};
    bool strict_strikethrough{true};
    std::vector<std::unique_ptr<InlineNode>> children;
};

struct BlockNode {
    BlockKind kind{BlockKind::Document};
    unsigned heading_level{0};
    unsigned ordered_start{1};
    char list_marker{'-'};
    char ordered_delimiter{'.'};
    bool tight_list{true};
    bool task_item{false};
    bool task_checked{false};
    std::string language;
    std::string text;
    std::vector<std::unique_ptr<InlineNode>> inlines;
    std::vector<std::unique_ptr<BlockNode>> children;
};

struct ParseState {
    BlockNode root;
    std::vector<BlockNode*> blocks; // point into root and must not outlive the synchronous parse
    std::vector<InlineNode*> inlines; // point into root and must not outlive the synchronous parse
    std::vector<bool> strict_strikethrough;
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
        case MD_BLOCK_TABLE:
        case MD_BLOCK_THEAD:
        case MD_BLOCK_TBODY:
        case MD_BLOCK_TR:
        case MD_BLOCK_TH:
        case MD_BLOCK_TD:
            return BlockKind::Paragraph;
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
        case MD_SPAN_LATEXMATH:
        case MD_SPAN_LATEXMATH_DISPLAY:
            return InlineKind::Code;
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
    try {
        if (type == MD_BLOCK_DOC) {
            state.blocks.push_back(&state.root);
            return 0;
        }
        if (state.blocks.empty()) return 1;
        auto node = std::make_unique<BlockNode>();
        node->kind = block_kind(type);
        switch (type) {
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
    } catch (...) {
        state.failed = true;
        return 1;
    }
}

int leave_block(MD_BLOCKTYPE, void*, void* userdata) noexcept {
    auto& state = *static_cast<ParseState*>(userdata);
    if (state.blocks.empty()) return 1;
    state.blocks.pop_back();
    return 0;
}

int enter_span(MD_SPANTYPE type, void* detail, void* userdata) noexcept {
    auto& state = *static_cast<ParseState*>(userdata);
    try {
        if (state.blocks.empty()) return 1;
        auto node = std::make_unique<InlineNode>();
        node->kind = inline_kind(type);
        if (type == MD_SPAN_A) {
            const auto& link_detail = *static_cast<MD_SPAN_A_DETAIL*>(detail);
            node->destination = attribute_text(link_detail.href);
            node->autolink = link_detail.is_autolink != 0;
        } else if (type == MD_SPAN_IMG) {
            node->destination = attribute_text(static_cast<MD_SPAN_IMG_DETAIL*>(detail)->src);
        } else if (type == MD_SPAN_DEL &&
                   state.strikethrough_index < state.strict_strikethrough.size()) {
            node->strict_strikethrough =
                state.strict_strikethrough[state.strikethrough_index++];
        }
        auto* node_pointer = node.get();
        if (state.inlines.empty()) {
            state.blocks.back()->inlines.push_back(std::move(node));
        } else {
            state.inlines.back()->children.push_back(std::move(node));
        }
        state.inlines.push_back(node_pointer);
        return 0;
    } catch (...) {
        state.failed = true;
        return 1;
    }
}

int leave_span(MD_SPANTYPE, void*, void* userdata) noexcept {
    auto& state = *static_cast<ParseState*>(userdata);
    if (state.inlines.empty()) return 1;
    state.inlines.pop_back();
    return 0;
}

int append_text(MD_TEXTTYPE type, const MD_CHAR* text, MD_SIZE size, void* userdata) noexcept {
    auto& state = *static_cast<ParseState*>(userdata);
    try {
        if (state.blocks.empty()) return 1;
        auto& block = *state.blocks.back();
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
    } catch (...) {
        state.failed = true;
        return 1;
    }
}

[[nodiscard]] util::Expected<BlockNode> parse_markdown(std::string_view text) {
    ParseState state;
    state.root.kind = BlockKind::Document;
    state.strict_strikethrough = scan_strikethrough_delimiters(text);
    const MD_PARSER parser{
        .abi_version = 0,
        .flags = MD_FLAG_PERMISSIVEAUTOLINKS | MD_FLAG_STRIKETHROUGH | MD_FLAG_TASKLISTS,
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
    if (parse_result == 0 && !state.failed) return std::move(state.root);
    return std::unexpected(util::make_error(
        util::ErrorCode::Unknown,
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
    const auto input_width = detail::visible_width(text);
    auto styled = hook(std::move(text));
    return detail::visible_width(styled) == input_width ? std::move(styled) : original;
}

[[nodiscard]] std::string apply_roles(
    std::string text,
    const std::vector<StyleRole>& roles,
    MarkdownStyleConfig& style) {
    for (auto iterator = roles.rbegin(); iterator != roles.rend(); ++iterator) {
        auto* hook = role_hook(*iterator, style);
        if (hook != nullptr) text = apply_style_hook(*hook, std::move(text));
    }
    return apply_style_hook(style.text, std::move(text));
}

[[nodiscard]] std::string plain_inline_text(const InlineNode& node) {
    if (node.kind == InlineKind::Text) return node.text;
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
    auto label = render_inline_children(node.children, roles, style);
    const auto plain_label = plain_inline_text(node);
    if (style.link) return style.link(std::move(label), node.destination);
    auto comparable_destination = node.destination;
    if (comparable_destination.starts_with("mailto:")) comparable_destination.erase(0, 7);
    if (node.autolink || plain_label == node.destination || plain_label == comparable_destination) return label;
    auto destination = std::format(" ({})", node.destination);
    destination = apply_style_hook(style.link_url, std::move(destination));
    destination = apply_style_hook(style.text, std::move(destination));
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
        case InlineKind::Code:
            roles.push_back(StyleRole::InlineCode);
            return render_inline_children(node.children, roles, style);
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

[[nodiscard]] std::vector<StyleRole> block_roles(bool heading, bool quote) {
    std::vector<StyleRole> roles;
    if (heading) roles.push_back(StyleRole::Heading);
    if (quote) roles.push_back(StyleRole::Quote);
    return roles;
}

struct RenderContext {
    MarkdownStyleConfig& style; // must outlive the synchronous render operation
    SyntaxHighlightHook& syntax_highlighter; // must outlive the synchronous render operation
    bool quoted{false};
};

using RenderedLines = util::Expected<std::vector<std::string>>;

[[nodiscard]] RenderedLines add_prefix(
    const std::vector<std::string>& logical_lines,
    std::string first_prefix,
    std::string continuation_prefix,
    std::size_t width) {
    const auto prefix_width = detail::visible_width(first_prefix);
    const auto continuation_width = detail::visible_width(continuation_prefix);
    if (prefix_width >= width || continuation_width >= width) {
        std::string unprefixed;
        for (std::size_t index = 0; index < logical_lines.size(); ++index) {
            if (index > 0) unprefixed += '\n';
            unprefixed += logical_lines[index];
        }
        return detail::wrap_text(unprefixed, width);
    }
    std::vector<std::string> result;
    bool first = true;
    for (const auto& logical_line : logical_lines) {
        const auto available = width - (first ? prefix_width : continuation_width);
        if (auto wrapped = detail::wrap_text(logical_line, available); wrapped) {
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

[[nodiscard]] RenderedLines render_sequence(
    const std::vector<std::unique_ptr<BlockNode>>& blocks,
    std::size_t width,
    RenderContext context,
    bool separated) {
    std::vector<std::string> result;
    for (const auto& block : blocks) {
        if (auto rendered = render_block(*block, width, context); rendered) {
            if (separated && !result.empty() && !rendered->empty() && !result.back().empty()) {
                result.push_back({});
            }
            result.insert(result.end(), rendered->begin(), rendered->end());
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
    const auto border_roles = block_roles(false, context.quoted);
    auto opening = apply_style_hook(
        context.style.code_block_border,
        std::format("```{}", block.language));
    auto closing = apply_style_hook(context.style.code_block_border, "```");
    if (context.quoted) {
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
    if (auto wrapped_opening = detail::wrap_text(opening, width); wrapped_opening) {
        result.insert(result.end(), wrapped_opening->begin(), wrapped_opening->end());
    } else {
        return std::unexpected(wrapped_opening.error());
    }

    const auto configured_indent_width = detail::visible_width(context.style.code_block_indent);
    const auto indent = configured_indent_width < width ? context.style.code_block_indent : std::string{};
    const auto code_width = width - detail::visible_width(indent);
    if (highlighted) {
        for (const auto& code_line : code_lines) {
            if (auto wrapped = detail::wrap_text(code_line, code_width); !wrapped) {
                code_lines.clear();
                highlighted = false;
                break;
            }
        }
        if (!highlighted) code_lines = fallback_code_lines(code, context.style);
    }
    for (const auto& code_line : code_lines) {
        if (auto wrapped = detail::wrap_text(code_line, code_width); wrapped) {
            for (const auto& line : *wrapped) result.push_back(indent + line);
        } else {
            return std::unexpected(wrapped.error());
        }
    }

    if (auto wrapped_closing = detail::wrap_text(closing, width); wrapped_closing) {
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
        auto marker_width = detail::visible_width(styled_marker);
        if (marker_width >= width) {
            styled_marker.clear();
            marker_width = 0;
        }

        bool wrote_content = false;
        if (!item->inlines.empty()) {
            const auto item_text = render_inline_children(
                item->inlines,
                block_roles(false, context.quoted),
                context.style);
            if (auto item_lines = detail::wrap_text(item_text, width - marker_width); item_lines) {
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
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "TUI Markdown requires a positive content width"));
    }
    switch (block.kind) {
        case BlockKind::Document:
            return render_sequence(block.children, width, context, true);
        case BlockKind::Paragraph: {
            const auto text = render_inline_children(
                block.inlines,
                block_roles(false, context.quoted),
                context.style);
            return detail::wrap_text(text, width);
        }
        case BlockKind::Heading: {
            auto roles = block_roles(true, context.quoted);
            auto text = render_inline_children(block.inlines, roles, context.style);
            if (block.heading_level >= 3) {
                text = apply_roles(
                           std::string(block.heading_level, '#') + " ",
                           roles,
                           context.style) +
                       text;
            }
            return detail::wrap_text(text, width);
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
            return detail::wrap_text(
                apply_roles(
                    block.text,
                    block_roles(false, context.quoted),
                    context.style),
                width);
        case BlockKind::Quote: {
            constexpr std::string_view kQuotePrefix = "│ ";
            constexpr std::size_t kQuoteWidth = 2;
            const auto prefix_width = kQuoteWidth < width ? kQuoteWidth : 0;
            auto quote_context = context;
            quote_context.quoted = true;
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
            const auto styled_prefix = prefix_width == 0
                                           ? std::string{}
                                           : apply_style_hook(
                                                 context.style.quote_border,
                                                 std::string(kQuotePrefix));
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
    return trim_partial_closing_fence(std::move(normalized));
}

} // namespace

struct Markdown::Impl {
    std::string text;
    std::size_t padding_x{0};
    std::size_t padding_y{0};
    MarkdownStyleConfig style;
    SyntaxHighlightHook syntax_highlighter;
    BackgroundHook background_hook;
    std::string cached_text;
    std::size_t cached_width{0};
    std::vector<std::string> cached_lines;
    bool cache_valid{false};
};

Markdown::Markdown(
    std::string text,
    std::size_t padding_x,
    std::size_t padding_y,
    MarkdownStyleConfig style,
    SyntaxHighlightHook syntax_highlighter,
    BackgroundHook background_hook)
    : impl_(std::make_unique<Impl>()) {
    impl_->text = std::move(text);
    impl_->padding_x = padding_x;
    impl_->padding_y = padding_y;
    impl_->style = std::move(style);
    impl_->syntax_highlighter = std::move(syntax_highlighter);
    impl_->background_hook = std::move(background_hook);
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

util::Expected<RenderResult> Markdown::render(std::size_t width) {
    if (impl_->cache_valid && impl_->cached_text == impl_->text && impl_->cached_width == width) {
        return RenderResult{.lines = impl_->cached_lines};
    }
    if (width == 0) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
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
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "TUI Markdown width is too small for padding",
            std::format("width {} padding_x {}", width, impl_->padding_x)));
    }

    const auto content_width = width - impl_->padding_x - impl_->padding_x;
    const auto normalized = normalize_markdown_input(impl_->text);
    BlockNode document;
    if (auto parsed = parse_markdown(normalized); parsed) {
        document = std::move(*parsed);
    } else {
        return std::unexpected(parsed.error());
    }
    const auto render_content = [&]() -> RenderedLines {
        try {
            return render_block(
                document,
                content_width,
                RenderContext{
                    .style = impl_->style,
                    .syntax_highlighter = impl_->syntax_highlighter,
                });
        } catch (const std::exception&) {
            return std::unexpected(util::make_error(
                util::ErrorCode::Unknown,
                "TUI Markdown callback failed",
                "the styling or syntax-highlighting callback threw an exception"));
        } catch (...) {
            return std::unexpected(util::make_error(
                util::ErrorCode::Unknown,
                "TUI Markdown callback failed",
                "the styling or syntax-highlighting callback threw an unknown exception"));
        }
    };
    std::vector<std::string> content;
    if (auto rendered = render_content(); rendered) {
        content = std::move(*rendered);
    } else {
        return std::unexpected(rendered.error());
    }

    std::vector<std::string> result;
    const auto make_line = [&](std::string line) -> util::Expected<std::string> {
        line.insert(0, impl_->padding_x, ' ');
        const auto visible = detail::visible_width(line);
        if (visible < width) line.append(width - visible, ' ');
        return detail::apply_background(impl_->background_hook, std::move(line), width, "Markdown");
    };
    for (std::size_t index = 0; index < impl_->padding_y; ++index) {
        if (auto line = make_line({}); line) {
            result.push_back(std::move(*line));
        } else {
            return std::unexpected(line.error());
        }
    }
    for (const auto& line : content) {
        if (auto prepared = make_line(line); prepared) {
            result.push_back(std::move(*prepared));
        } else {
            return std::unexpected(prepared.error());
        }
    }
    for (std::size_t index = 0; index < impl_->padding_y; ++index) {
        if (auto line = make_line({}); line) {
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
