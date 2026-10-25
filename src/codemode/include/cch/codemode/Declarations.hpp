#pragma once

#include <cch/ai/Tool.hpp>
#include <cch/support/JsonValue.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace cch::codemode {

/// pi's public limit constants (`packages/codemode/src/limits.ts`), captured by
/// the frozen declarations artifact ("default and stored limits are public
/// constants"). The prelude enforces the output/store bounds guest-side.
inline constexpr std::size_t kDefaultInputSchemaMaxChars = 16'000;
inline constexpr std::size_t kMaxOutputChars = 16'777'216;
inline constexpr std::size_t kMaxOutputItems = 100'000;
inline constexpr std::size_t kMaxStoreTotalChars = 1'048'576;
inline constexpr std::size_t kMaxStoreValueChars = 262'144;

/// pi's global declarations as `renderDeclarations` consumes them: the bound
/// name (dotted names group into a namespace), an optional doc description, and
/// the callable signature without the name.
struct CodemodeGlobalDeclaration {
    std::string name;
    std::string description;
    std::string signature;
};

/// pi `toCodemodeIdentifier` (`packages/codemode/src/identifier.ts`): characters
/// invalid in an identifier become `_`; an empty name becomes `_`.
[[nodiscard]] std::string to_codemode_identifier(std::string_view name);

/// pi `schemaToType`: a JSON Schema to a TypeScript type expression. A result
/// longer than `max_chars` renders as `unknown`.
[[nodiscard]] std::string schema_to_type(const support::JsonValue& schema, std::size_t max_chars);

/// pi `renderToolDeclaration`: `<jsName>(args: <input>): Promise<<output>>;`
/// where the output type comes from the tool's `outputSchema` (`unknown` when
/// absent) and `input_max_chars` bounds the rendered input type.
[[nodiscard]] std::string render_tool_declaration(
        const ai::Tool& tool, std::size_t input_max_chars = kDefaultInputSchemaMaxChars);

/// pi `renderToolSample`: the tool's description followed by its TS declaration.
[[nodiscard]] std::string render_tool_sample(const ai::Tool& tool);

/// pi `renderDeclarations`: the `declare const tools` block, then one
/// declaration per global (dotted names grouped into `declare const <ns>`
/// blocks), with doc comments from each description.
[[nodiscard]] std::string render_declarations(
        const std::vector<ai::Tool>& tools, const std::vector<CodemodeGlobalDeclaration>& globals);

} // namespace cch::codemode
