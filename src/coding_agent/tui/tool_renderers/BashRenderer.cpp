#include "BashRenderer.hpp"

#include "coding_agent/tui/tool_renderers/RenderUtils.hpp"

#include "agent/harness/OutputLimiter.hpp"

#include <cch/support/JsonValue.hpp>
#include <cch/tui/Utils.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::coding_agent::tui {
namespace {

/// pi `bash.ts:18` `BASH_PREVIEW_LINES`: the collapsed view keeps the last
/// five **visual** lines of the output.
constexpr std::size_t kPreviewLines = 5;

/// pi `truncate.ts:12` `DEFAULT_MAX_BYTES`, the limit a byte-truncated result
/// reports when it carries no limit of its own.
constexpr std::size_t kDefaultMaxBytes = 50 * 1024;

/// pi `truncationResult` as `details.truncation` carries it: the fields the
/// warning line and the footer stripping read.
struct TruncationFacts {
    bool truncated{false};
    bool truncated_by_lines{false};
    std::size_t output_lines{0};
    std::size_t total_lines{0};
    std::size_t max_bytes{kDefaultMaxBytes};
};

/// pi `bash.ts:45-46`: `const timeout = args?.timeout` and
/// `timeout ? ... : ""`, so an absent, null, or zero timeout prints no suffix
/// at all.
[[nodiscard]] std::optional<double> truthy_timeout(const support::JsonValue& args) {
    const auto* value = json_member(args, "timeout");
    if (value == nullptr) return std::nullopt;
    const auto* number = value->get_if<double>();
    if (number == nullptr || *number == 0.0) return std::nullopt;
    return *number;
}

[[nodiscard]] std::optional<std::string> full_output_path(const support::JsonValue* details) {
    if (details == nullptr) return std::nullopt;
    const auto path = json_string(*details, "fullOutputPath");
    if (!path || path->empty()) return std::nullopt;
    return std::string{*path};
}

[[nodiscard]] std::optional<TruncationFacts> truncation_facts(const support::JsonValue* details) {
    if (details == nullptr) return std::nullopt;
    const auto* value = json_member(*details, "truncation");
    if (value == nullptr) return std::nullopt;
    return TruncationFacts{
            .truncated = json_boolean(*value, "truncated"),
            .truncated_by_lines = json_string(*value, "truncatedBy") == "lines",
            .output_lines = static_cast<std::size_t>(json_number(*value, "outputLines").value_or(0.0)),
            .total_lines = static_cast<std::size_t>(json_number(*value, "totalLines").value_or(0.0)),
            .max_bytes = static_cast<std::size_t>(
                    json_number(*value, "maxBytes").value_or(static_cast<double>(kDefaultMaxBytes))),
    };
}

/// JavaScript's `trim` whitespace set, which pi's `.trim()` removes on both
/// ends of the result text.
constexpr std::string_view kWhitespace{" \t\n\r\f\v"};

/// pi `getTextOutput(result).trim()`: JavaScript `trim` drops leading and
/// trailing whitespace, newlines included.
[[nodiscard]] std::string trim_text(std::string_view text) {
    const auto first = text.find_first_not_of(kWhitespace);
    if (first == std::string_view::npos) return {};
    const auto last = text.find_last_not_of(kWhitespace);
    return std::string{text.substr(first, last - first + 1)};
}

[[nodiscard]] std::string trim_end(std::string text) {
    while (!text.empty() && kWhitespace.find(text.back()) != std::string_view::npos)
        text.pop_back();
    return text;
}

/// pi `bash.ts:64-72`: a settled result's own summary line names the spill
/// path and the renderer's warning line names it again, so pi strips the
/// baked-in one. All four conditions are required, which is what keeps it
/// from eating unrelated bracketed text.
void strip_spill_footer(std::string& output,
        bool is_partial,
        const std::optional<TruncationFacts>& facts,
        const std::optional<std::string>& path) {
    if (is_partial || !facts.has_value() || !facts->truncated || !path.has_value()) return;
    if (output.empty() || output.back() != ']') return;
    const auto footer_start = output.rfind("\n\n[");
    if (footer_start == std::string::npos) return;
    if (output.find(*path, footer_start) == std::string::npos) return;
    output = trim_end(output.substr(0, footer_start));
}

/// pi `bash.ts:32-42` `formatDuration`.
[[nodiscard]] std::string format_duration(std::int64_t milliseconds) {
    const auto seconds = static_cast<double>(milliseconds) / 1000.0;
    if (seconds < 60.0) return std::format("{:.1f}s", seconds);
    const auto total_seconds = static_cast<std::int64_t>(std::floor(seconds));
    const auto minutes = total_seconds / 60;
    const auto remainder = total_seconds % 60;
    if (minutes < 60) return std::format("{}m {}s", minutes, remainder);
    return std::format("{}h {}m {}s", minutes / 60, minutes % 60, remainder);
}

/// pi `bash.ts:43-49` `formatShellCall` for the `$` prompt. The whole
/// `"$ <command>"` is the tool-title bold and the timeout suffix sits outside
/// that styling in `muted`, with the space before the paren pi writes.
[[nodiscard]] std::string shell_call_title(const ToolRenderContext& context) {
    const auto command = string_argument(context.args, "command");
    std::string command_display;
    if (!command.has_value()) {
        command_display = context.theme.foreground(ThemeToken::Error, "[invalid arg]");
    } else if (command->empty()) {
        command_display = context.theme.foreground(ThemeToken::ToolOutput, "...");
    } else {
        command_display = *command;
    }
    auto title = bold_foreground(context.theme, ThemeToken::ToolTitle, "$ " + command_display);
    if (const auto timeout = truthy_timeout(context.args)) {
        title += context.theme.foreground(ThemeToken::Muted, std::format(" (timeout {}s)", *timeout));
    }
    return title;
}

/// pi `bash.ts:74-106`. The styled output is one `toolOutput` colour per line;
/// the expanded view is that text whole, and the collapsed view is its last
/// five **visual** lines with the earlier-lines hint above them (pi returns
/// `["", hint, ...lines]`: a blank row, then the hint, then the tail).
[[nodiscard]] std::string output_body(std::string_view output, const ToolRenderContext& context) {
    const auto styled = join_rendered_rows(split_lines(output),
            [&context](const std::string& line) { return context.theme.foreground(ThemeToken::ToolOutput, line); });
    if (context.expanded) return styled;

    // A failed measurement means the host's own render at this width fails
    // too, so showing the whole output leaves the error to surface there
    // rather than folding a tail that is not pi's.
    auto folded = fold_tail_visual_lines(styled, kPreviewLines, context.width, 0);
    if (!folded) return styled;

    std::vector<std::string> rows;
    if (folded->skipped > 0) {
        auto hint = context.theme.foreground(ThemeToken::Muted, std::format("... ({} earlier lines,", folded->skipped));
        hint += " ";
        hint += context.expand_hint;
        hint += context.theme.foreground(ThemeToken::Muted, ")");
        // pi `truncateToWidth(hint, width, "...")`: an unbounded hint would
        // wrap and change the block's height.
        if (const auto bounded = cch::tui::truncate_text(hint, context.width, "...", false)) {
            hint = *bounded;
        }
        rows.push_back(std::move(hint));
    }
    rows.reserve(rows.size() + folded->lines.size());
    for (auto& row : folded->lines) {
        // `Text::render` right-pads every row to the frame. pi keeps those
        // rows because they are final; here they go back through the host's
        // own `Text`, which pads again, so the padding is dropped rather than
        // carried inside the block's text.
        rows.push_back(trim_end(std::move(row)));
    }
    return join_rendered_rows(rows, [](const std::string& row) { return row; });
}

/// pi `bash.ts:108-123`: the `warning`-coloured bracket joining the spill path
/// and the truncation summary with `". "`. The gate is `truncated ||
/// fullOutputPath`, so a path with no truncation still warns, and the path is
/// verbatim inside the brackets: never shortened, hyperlinked, or accented.
[[nodiscard]] std::optional<std::string> warning_line(const ToolRenderContext& context,
        const std::optional<TruncationFacts>& facts,
        const std::optional<std::string>& path) {
    const auto truncated = facts.has_value() && facts->truncated;
    if (!truncated && !path.has_value()) return std::nullopt;
    std::vector<std::string> warnings;
    if (path.has_value()) warnings.push_back(std::format("Full output: {}", *path));
    if (truncated) {
        if (facts->truncated_by_lines) {
            warnings.push_back(
                    std::format("Truncated: showing {} of {} lines", facts->output_lines, facts->total_lines));
        } else {
            warnings.push_back(std::format("Truncated: {} lines shown ({} limit)",
                    facts->output_lines,
                    harness::format_output_size(facts->max_bytes)));
        }
    }
    const auto joined = [&warnings] {
        std::string text;
        for (const auto& warning : warnings) {
            if (!text.empty()) text += ". ";
            text += warning;
        }
        return text;
    }();
    return context.theme.foreground(ThemeToken::Warning, "[" + joined + "]");
}

/// pi `bash.ts:125-129`, settled half only. pi prints `Elapsed` while partial
/// against a live `Date.now()` on a 1 s redraw interval, and this tool block
/// has no loader to drive one (ADR), so a `Took` ships against the end stamp
/// the host took and no duration line renders before it exists.
[[nodiscard]] std::optional<std::string> duration_line(const ToolRenderContext& context) {
    if (!context.started_at_ms.has_value() || !context.ended_at_ms.has_value()) return std::nullopt;
    const auto elapsed = *context.ended_at_ms - *context.started_at_ms;
    return context.theme.foreground(ThemeToken::Muted, std::format("Took {}", format_duration(elapsed)));
}

} // namespace

ToolRenderer make_bash_renderer() {
    return ToolRenderer{
            .render_call = [](const ToolRenderContext& context) -> ToolRenderedText {
                return ToolRenderedText{.title = shell_call_title(context)};
            },
            .render_result = [](const ToolRenderedResult& result,
                                     const ToolRenderContext& context) -> ToolRenderedText {
                const auto* details = result.details ? &*result.details : nullptr;
                const auto facts = truncation_facts(details);
                const auto path = full_output_path(details);
                auto output = trim_text(result.output);
                strip_spill_footer(output, context.is_partial, facts, path);

                // Each of pi's three result children is a `\n`-prefixed
                // line, so a block reads as title, blank row, output,
                // warning, duration.
                std::string body;
                const auto append = [&body](std::string&& line) {
                    body += "\n";
                    body += std::move(line);
                };
                if (!output.empty()) append(output_body(output, context));
                if (auto warning = warning_line(context, facts, path)) append(std::move(*warning));
                if (auto duration = duration_line(context)) append(std::move(*duration));

                ToolRenderedText rendered;
                if (!body.empty()) rendered.blocks.push_back(std::move(body));
                return rendered;
            },
    };
}

} // namespace cch::coding_agent::tui
