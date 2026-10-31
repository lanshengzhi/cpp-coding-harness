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

/// pi `codemodeSchema.parameters`: one required `code` string argument.
[[nodiscard]] support::JsonValue codemode_parameters();

/// The passive model-facing `codemode` definition: name, the empty-catalog
/// description (`createCodemodeDescription([])`), the single `code` argument,
/// and the grammar-constrained sampling variant (`openai_lark` → pi's
/// `CODEMODE_SOURCE_GRAMMAR`). `execute` is added by the tool source; the live
/// catalog replaces the description whenever the loadout changes.
[[nodiscard]] ai::Tool codemode_tool_definition();

} // namespace cch::coding_agent::extensions
