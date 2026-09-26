#include "RenderUtils.hpp"

#include <cch/tui/Text.hpp>

#include <array>
#include <cstddef>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::coding_agent::tui {

std::vector<std::string> split_lines(std::string_view text) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (true) {
        const auto end = text.find('\n', start);
        if (end == std::string_view::npos) {
            lines.emplace_back(text.substr(start));
            return lines;
        }
        lines.emplace_back(text.substr(start, end - start));
        start = end + 1;
    }
}

std::string bold_foreground(const LiveTheme& theme, ThemeToken token, std::string_view text) {
    return theme.foreground(token, std::format("\x1b[1m{}\x1b[22m", text));
}

std::optional<std::string_view> string_argument(
        const support::JsonValue& args, std::string_view primary_key, std::string_view secondary_key) {
    const auto* object = args.get_if<support::JsonValue::object_t>();
    if (object == nullptr) return std::string_view{};
    const std::array<std::string, 2> keys{std::string{primary_key}, std::string{secondary_key}};
    for (const auto& key : keys) {
        if (key.empty()) continue;
        const auto found = object->find(key);
        // pi's `??` chain skips a missing key and a null value alike.
        if (found == object->end() || found->second.holds<support::JsonValue::null_t>()) continue;
        if (const auto* text = found->second.get_if<std::string>()) return *text;
        return std::nullopt;
    }
    return std::string_view{};
}

std::string replace_tabs(std::string_view text) {
    std::string replaced;
    replaced.reserve(text.size());
    for (const auto character : text) {
        if (character == '\t') {
            replaced.append("   ");
            continue;
        }
        replaced.push_back(character);
    }
    return replaced;
}

std::string normalize_display_text(std::string_view text) {
    std::string normalized;
    normalized.reserve(text.size());
    for (const auto character : text) {
        if (character == '\r') continue;
        normalized.push_back(character);
    }
    return normalized;
}

HeadFold fold_head_lines(std::string_view text, std::size_t max_lines) {
    auto lines = split_lines(text);
    while (!lines.empty() && lines.back().empty())
        lines.pop_back();
    if (lines.size() <= max_lines) return HeadFold{.lines = std::move(lines), .remaining = 0};
    const auto remaining = lines.size() - max_lines;
    lines.resize(max_lines);
    return HeadFold{.lines = std::move(lines), .remaining = remaining};
}

std::string fold_hint(const ToolRenderContext& context, std::size_t remaining, std::optional<std::size_t> total_lines) {
    const auto count = std::to_string(remaining);
    const auto total = total_lines.has_value() ? std::format(", {} total", *total_lines) : std::string{};
    auto hint = context.theme.foreground(ThemeToken::Muted, std::format("\n... ({} more lines,{}", count, total));
    hint += " ";
    hint += context.expand_hint;
    hint += context.theme.foreground(ThemeToken::Muted, ")");
    return hint;
}

support::Expected<VisualFold> fold_tail_visual_lines(
        std::string_view text, std::size_t max_visual_lines, std::size_t width, std::size_t padding_x) {
    if (text.empty()) return VisualFold{};
    cch::tui::Text component(std::string{text}, padding_x, 0);
    auto rendered = component.render(width);
    if (!rendered) return std::unexpected(rendered.error());
    auto all = std::move(rendered->lines);
    if (all.size() <= max_visual_lines) return VisualFold{.lines = std::move(all), .skipped = 0};
    const auto skipped = all.size() - max_visual_lines;
    VisualFold folded;
    folded.skipped = skipped;
    folded.lines.reserve(max_visual_lines);
    for (auto row = all.end() - static_cast<std::ptrdiff_t>(max_visual_lines); row != all.end(); ++row) {
        folded.lines.push_back(std::move(*row));
    }
    return folded;
}

} // namespace cch::coding_agent::tui
