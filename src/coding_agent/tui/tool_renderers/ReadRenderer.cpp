#include "ReadRenderer.hpp"

#include "PathPresentation.hpp"
#include "RenderUtils.hpp"
#include "agent/harness/OutputLimiter.hpp"

#include <cch/coding_agent/AgentConfigDir.hpp>
#include <cch/support/JsonValue.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace cch::coding_agent::tui {
namespace {

/// pi `read.ts:26` `COMPACT_RESOURCE_FILE_NAMES`: five spellings, matched on
/// the whole file name.
constexpr std::array<std::string_view, 5> kCompactResourceFileNames{
        "AGENTS.override.md",
        "AGENTS.md",
        "AGENTS.MD",
        "CLAUDE.md",
        "CLAUDE.MD",
};

/// pi `read.ts:129`: the collapsed read preview is 10 LINES taken from the
/// head. The fold only ever applies to the collapsed-error path, because a
/// collapsed non-error result already returned the empty string below it.
constexpr std::size_t kCollapsedPreviewLines = 10;

/// pi `truncate.ts:11-12` `DEFAULT_MAX_LINES` / `DEFAULT_MAX_BYTES`, carried so
/// a truncation object that omits them still renders pi's fallback text. The
/// size formatting itself is pi's `formatSize`, reused as
/// `harness::format_output_size` rather than restated here.
constexpr double kDefaultMaxLines = 2000.0;
constexpr double kDefaultMaxBytes = 50.0 * 1024.0;

[[nodiscard]] bool field_present(const support::JsonValue::object_t& object, std::string_view key) {
    return object.find(std::string{key}) != object.end();
}

/// A numeric `details.truncation` field. Absent, JSON null, and non-numeric
/// all read as "no value" and take `fallback`: pi resolves `maxBytes` and
/// `maxLines` with `??` and the tool always emits the line counts, so the
/// fallback is pi's documented default where pi has one and `0` where it does
/// not.
[[nodiscard]] double number_field(const support::JsonValue::object_t& object, std::string_view key, double fallback) {
    const auto found = object.find(std::string{key});
    if (found == object.end()) return fallback;
    const auto* value = found->second.get_if<double>();
    return value == nullptr ? fallback : *value;
}

/// A JSON boolean field; a missing or non-boolean one is `false`, so a caller
/// distinguishes the three warning cases by which field it asks for.
[[nodiscard]] bool boolean_field(const support::JsonValue::object_t& object, std::string_view key) {
    const auto found = object.find(std::string{key});
    if (found == object.end()) return false;
    const auto* value = found->second.get_if<bool>();
    return value != nullptr && *value;
}

[[nodiscard]] std::string_view string_field(const support::JsonValue::object_t& object, std::string_view key) {
    const auto found = object.find(std::string{key});
    if (found == object.end()) return {};
    const auto* value = found->second.get_if<std::string>();
    return value == nullptr ? std::string_view{} : std::string_view{*value};
}

/// pi `read.ts:28-33` `formatReadLineRange`: the warning-coloured `:start-end`
/// suffix, emitted only when `offset` or `limit` is present. A read with
/// neither gets no suffix at all — not `:1`. Visible forms are `:1`, `:5`, and
/// `:5-104`.
[[nodiscard]] std::string read_line_range(const support::JsonValue& args, const LiveTheme& theme) {
    const auto* object = args.get_if<support::JsonValue::object_t>();
    if (object == nullptr) return {};
    const bool has_offset = field_present(*object, "offset");
    const bool has_limit = field_present(*object, "limit");
    if (!has_offset && !has_limit) return {};
    const auto start = number_field(*object, "offset", 1.0);
    // pi's `endLine` is `startLine + limit - 1` when a limit is present and the
    // empty string otherwise, and a computed `0` is falsy in JavaScript, so a
    // `limit` of 0 also leaves the range open.
    const auto end = has_limit ? start + number_field(*object, "limit", 0.0) - 1.0 : 0.0;
    const auto range = end == 0.0 ? std::format(":{}", start) : std::format(":{}-{}", start, end);
    return theme.foreground(ThemeToken::Warning, range);
}

/// pi `path-utils.ts:19` `resolveToCwd`: home-marker expansion, then an
/// absolute path honored as is and a relative one resolved against the tool
/// execution's cwd. Lexical only, so the classification performs no filesystem
/// I/O.
[[nodiscard]] std::string resolve_to_cwd(std::string_view path, std::string_view cwd) {
    std::string expanded{path};
    const auto home = home_directory().string();
    if (!home.empty() && (expanded == "~" || expanded.starts_with("~/"))) {
        expanded = home + expanded.substr(1);
    }
    const std::filesystem::path candidate{expanded};
    if (candidate.is_absolute()) return candidate.lexically_normal().string();
    return (std::filesystem::path{cwd} / candidate).lexically_normal().string();
}

/// pi `utils/paths.ts:57-60` `formatPathRelativeToCwdOrAbsolute`: the path
/// relative to the cwd when it is inside it and the absolute path otherwise,
/// both with POSIX separators.
[[nodiscard]] std::string format_path_relative_to_cwd_or_absolute(
        std::string_view absolute_path, std::string_view cwd) {
    const std::filesystem::path base{cwd.empty() ? absolute_path : cwd};
    const auto relative = std::filesystem::path{absolute_path}.lexically_relative(base);
    const bool inside = !relative.empty() && *relative.begin() != "..";
    const auto value = inside ? relative.generic_string() : std::filesystem::path{absolute_path}.generic_string();
    return value.empty() ? "." : value;
}

/// pi `read.ts:26, 48-88` `getCompactReadClassification`, with pi's pi-docs
/// branch (`README.md`, `docs/**`, `examples/**` under pi's own package root)
/// deliberately absent: pike ships no runtime docs tree, so that branch would
/// be dead code. Do not re-add it without such a tree.
struct CompactReadClassification {
    enum class Kind {
        Skill,
        Resource,
    };
    Kind kind{Kind::Resource};
    std::string label{};
};

[[nodiscard]] std::optional<CompactReadClassification> compact_read_classification(const ToolRenderContext& context) {
    // pi reads `args.path` only, which is why this helper is not symmetric with
    // the title's `file_path ?? path` precedence: a `file_path`-only read stays
    // on the plain `read <path>` form.
    const auto raw_path = string_argument(context.args, "path");
    if (!raw_path || raw_path->empty()) return std::nullopt;

    const std::filesystem::path candidate{resolve_to_cwd(*raw_path, context.cwd)};
    const auto file_name = candidate.filename().string();
    if (file_name == "SKILL.md") {
        // The containing directory's name, falling back to the file name when
        // the path has no directory component.
        const auto directory_name = candidate.parent_path().filename().string();
        return CompactReadClassification{
                .kind = CompactReadClassification::Kind::Skill,
                .label = directory_name.empty() ? file_name : directory_name,
        };
    }

    if (std::ranges::find(kCompactResourceFileNames, file_name) == kCompactResourceFileNames.end()) {
        return std::nullopt;
    }
    return CompactReadClassification{
            .kind = CompactReadClassification::Kind::Resource,
            .label = format_path_relative_to_cwd_or_absolute(candidate.string(), context.cwd),
    };
}

/// pi `formatCompactReadCall` (`read.ts:88-110`): the skill label or the
/// resource label, then the line range, then the expand hint — in that order.
/// The compact hint is a different string from the fold hint: it carries no
/// line count and has a leading space inside the parens.
[[nodiscard]] std::string compact_read_call(const CompactReadClassification& classification,
        const support::JsonValue& args,
        const ToolRenderContext& context) {
    const auto line_range = read_line_range(args, context.theme);
    const auto expand_hint =
            context.theme.foreground(ThemeToken::Dim, std::format(" ({} to expand)", context.expand_key));
    if (classification.kind == CompactReadClassification::Kind::Skill) {
        // pi styles the skill label with a literal bold escape pair rather than
        // `theme.bold`, so the text is exactly `\x1b[1m[skill]\x1b[22m `.
        return context.theme.foreground(ThemeToken::CustomMessageLabel, "\x1b[1m[skill]\x1b[22m ") +
               context.theme.foreground(ThemeToken::CustomMessageText, classification.label) + line_range + expand_hint;
    }
    return bold_foreground(context.theme, ThemeToken::ToolTitle, "read resource") + " " +
           context.theme.foreground(ThemeToken::Accent, classification.label) + line_range + expand_hint;
}

/// pi `formatReadCall` (`read.ts:34-37`): the bold tool name, a space, the
/// presented path, then the optional line range.
[[nodiscard]] std::string normal_read_call(const ToolRenderContext& context) {
    return bold_foreground(context.theme, ThemeToken::ToolTitle, "read") + " " +
           render_tool_path(context.theme, context.cwd, string_argument(context.args, "file_path", "path")) +
           read_line_range(context.args, context.theme);
}

/// pi `read.ts:137-146`: the three distinct truncation warnings, verbatim and in
/// pi's order. A read whose `details` carries no `truncation` draws no warning
/// at all, which is also what keeps an old session file — whose marker is baked
/// into the result text — rendering as plain text with no migration.
[[nodiscard]] std::optional<std::string> truncation_warning(const support::JsonValue* details) {
    if (details == nullptr) return std::nullopt;
    const auto* object = details->get_if<support::JsonValue::object_t>();
    if (object == nullptr) return std::nullopt;
    const auto truncation = object->find("truncation");
    if (truncation == object->end()) return std::nullopt;
    const auto* fields = truncation->second.get_if<support::JsonValue::object_t>();
    if (fields == nullptr || !boolean_field(*fields, "truncated")) return std::nullopt;

    if (boolean_field(*fields, "firstLineExceedsLimit")) {
        return std::format("[First line exceeds {} limit]",
                harness::format_output_size(
                        static_cast<std::size_t>(number_field(*fields, "maxBytes", kDefaultMaxBytes))));
    }
    if (string_field(*fields, "truncatedBy") == "lines") {
        return std::format("[Truncated: showing {} of {} lines ({} line limit)]",
                number_field(*fields, "outputLines", 0.0),
                number_field(*fields, "totalLines", 0.0),
                static_cast<std::size_t>(number_field(*fields, "maxLines", kDefaultMaxLines)));
    }
    return std::format("[Truncated: {} lines shown ({} limit)]",
            number_field(*fields, "outputLines", 0.0),
            harness::format_output_size(static_cast<std::size_t>(number_field(*fields, "maxBytes", kDefaultMaxBytes))));
}

} // namespace

ToolRenderer make_read_renderer() {
    return ToolRenderer{
            .render_call = [](const ToolRenderContext& context) -> ToolRenderedText {
                // pi `read.ts:154`: the compact classification is attempted only
                // while collapsed, so an expanded read shows the plain
                // `read <path>` form.
                const auto classification = context.expanded ? std::optional<CompactReadClassification>{}
                                                             : compact_read_classification(context);
                return ToolRenderedText{
                        .title = classification ? compact_read_call(*classification, context.args, context)
                                                : normal_read_call(context),
                };
            },
            .render_result = [](const ToolRenderedResult& result,
                                     const ToolRenderContext& context) -> ToolRenderedText {
                // pi `read.ts:111-115`: a collapsed, non-error read result
                // renders nothing at all — the title line is the whole block.
                if (!context.expanded && !context.is_error) return {};

                const auto max_lines =
                        context.expanded ? std::numeric_limits<std::size_t>::max() : kCollapsedPreviewLines;
                const auto fold = fold_head_lines(result.output, max_lines);
                std::string body = "\n";
                bool first = true;
                for (const auto& line : fold.lines) {
                    if (!first) body.push_back('\n');
                    // No syntax highlighting: this repository has none, and adding
                    // one is a new dependency (deferred, recorded in the ADR).
                    // Every line therefore takes the `toolOutput` token.
                    body += context.theme.foreground(ThemeToken::ToolOutput, replace_tabs(line));
                    first = false;
                }
                if (fold.remaining > 0) body += fold_hint(context, fold.remaining);
                if (const auto warning = truncation_warning(result.details ? &*result.details : nullptr)) {
                    body += "\n" + context.theme.foreground(ThemeToken::Warning, *warning);
                }
                return ToolRenderedText{.blocks = {std::move(body)}};
            },
    };
}

} // namespace cch::coding_agent::tui
