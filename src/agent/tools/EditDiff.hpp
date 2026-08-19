#pragma once

#include <cch/support/Error.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::tools {

/// One targeted replacement (pi `edit.ts` `replaceEditSchema`: `oldText` /
/// `newText`). Fields are snake_case because this is cch-native machinery;
/// the JSON contract keeps pi's camelCase spelling.
struct EditReplacement {
    std::string old_text;
    std::string new_text;
};

/// Result of applying one or more edits to LF-normalized content (pi
/// `edit-diff.ts` `applyEditsToNormalizedContent`).
struct AppliedEdits {
    std::string base_content;
    std::string new_content;
};

/// The display-oriented diff plus the first changed new-file line number (pi
/// `edit-diff.ts` `generateDiffString`).
struct DisplayDiff {
    std::string diff;
    std::optional<int> first_changed_line;
};

/// Dominant line ending: the first "\r\n" vs first "\n" (pi `detectLineEnding`).
[[nodiscard]] std::string_view detect_line_ending(std::string_view content);

[[nodiscard]] std::string normalize_to_lf(std::string_view text);
[[nodiscard]] std::string restore_line_endings(
    std::string_view text,
    std::string_view ending);

/// Split off a UTF-8 BOM. Returns {bom, text without bom} (pi `stripBom`).
[[nodiscard]] std::pair<std::string, std::string> strip_bom(
    std::string_view content);

/// Apply pi's edit-diff semantics to LF-normalized content: every edit is
/// matched against the original content (not incrementally), fuzzy matching
/// only when exact matching fails, duplicates and overlaps are rejected, and
/// an unchanged result is an error. Error messages reproduce pi's.
[[nodiscard]] support::Expected<AppliedEdits> apply_edits_to_normalized_content(
    std::string_view normalized_content,
    const std::vector<EditReplacement>& edits,
    std::string_view path);

/// Display-oriented diff with line numbers and context (pi
/// `edit-diff.ts` `generateDiffString`).
[[nodiscard]] DisplayDiff generate_diff_string(
    std::string_view old_content,
    std::string_view new_content,
    std::size_t context_lines = 4);

/// Unified patch in jsdiff `createTwoFilesPatch` `FILE_HEADERS_ONLY` format
/// (pi `edit-diff.ts` `generateUnifiedPatch`).
[[nodiscard]] std::string generate_unified_patch(
    std::string_view path,
    std::string_view old_content,
    std::string_view new_content,
    std::size_t context_lines = 4);

} // namespace cch::tools
