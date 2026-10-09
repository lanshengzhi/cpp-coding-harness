#pragma once

#include <cch/ai/Tool.hpp>
#include <cch/support/JsonValue.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace cch::coding_agent::extensions {

/// pi `CODEMODE_TOOL_NAME` (`packages/coding-agent/src/extensions/codemode/tool.ts`).
inline constexpr std::string_view kCodemodeToolName = "codemode";

/// pi `ToolDefinition.label`; the same literal as the name.
inline constexpr std::string_view kCodemodeToolLabel = "codemode";

/// pi `exposure: "model-only"`: scripts must not start other scripts.
inline constexpr std::string_view kCodemodeExposure = "model-only";

/// pi `CODEMODE_STORE_ENTRY_TYPE`, the custom session entry type holding one
/// script's `store()` writes.
inline constexpr std::string_view kCodemodeStoreEntryType = "codemode-store";

/// pi `DEFAULT_CODEMODE_INLINE_BUDGET` (estimated tokens, characters / 4) for
/// the tool declarations listed in the description. Overridden by the
/// `codemode.inlineBudget` setting.
inline constexpr std::int64_t kCodemodeDefaultInlineBudget = 3000;

/// pi `codemodeToolSystemPromptContribution.snippet`.
inline constexpr std::string_view kCodemodePromptSnippet = "Run JavaScript that calls other tools";

/// pi `codemodeToolSystemPromptContribution.guidelines`.
[[nodiscard]] std::vector<std::string> codemode_prompt_guidelines();

/// pi `DESCRIPTION_INTRO` verbatim (`tool.ts`).
[[nodiscard]] std::string_view codemode_description_intro();

/// pi `describeGlobals(models)` with `models` disabled: the `Globals:` block.
/// Pike has no `models.*` namespace, so the `models` line is never emitted.
[[nodiscard]] std::string codemode_globals_text();

/// pi `createCodemodeDescription(tools)` for the no-tool case: the intro, a
/// blank line, then the globals block. This is the description the frozen
/// `pi-v1.0.4` evidence bundle captured, so a differential test compares it
/// byte for byte.
[[nodiscard]] std::string codemode_description(const std::vector<ai::Tool>& nested_tools = {});

/// pi `codemodeSchema.parameters`: one required `code` string argument.
[[nodiscard]] support::JsonValue codemode_parameters();

/// The passive model-facing `codemode` definition: name, verbatim description,
/// the single `code` argument, and the grammar-constrained sampling variant
/// (`openai_lark` → pi's `CODEMODE_SOURCE_GRAMMAR`). `execute` is added by the
/// tool source.
[[nodiscard]] ai::Tool codemode_tool_definition();

} // namespace cch::coding_agent::extensions
