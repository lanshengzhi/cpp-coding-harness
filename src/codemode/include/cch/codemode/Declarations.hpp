#pragma once

#include <cch/ai/Tool.hpp>
#include <cch/support/JsonValue.hpp>

#include <cstddef>
#include <string>
#include <string_view>

namespace cch::codemode {

/// pi `toCodemodeIdentifier` (`packages/codemode/src/identifier.ts`): characters
/// invalid in an identifier become `_`; an empty name becomes `_`.
[[nodiscard]] std::string to_codemode_identifier(std::string_view name);

/// pi `schemaToType`: a JSON Schema to a TypeScript type expression. A result
/// longer than `max_chars` renders as `unknown`.
[[nodiscard]] std::string schema_to_type(const support::JsonValue& schema, std::size_t max_chars);

/// pi `renderToolSample`: the tool's description followed by its TS declaration.
[[nodiscard]] std::string render_tool_sample(const ai::Tool& tool);

} // namespace cch::codemode
