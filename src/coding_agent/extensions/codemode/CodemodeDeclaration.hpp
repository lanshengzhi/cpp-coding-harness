#pragma once

#include <cch/support/Error.hpp>
#include <cch/support/JsonValue.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cch::coding_agent::extensions {

/// Where a project's codemode declarations live: `<workspace>/.pi/codemode`.
/// Pike's project config directory is `.pi` (as in pi), and project-local
/// resources sit under it (`ProjectResourceLoader`), so declared script tools
/// are discovered there too.
///
/// This on-disk declaration format is an **intentional divergence** from pi
/// v1.0.4 (`7c10bd43`): pi has no codemode declaration file — scripts are
/// written inline in the model's `codemode` tool call, and "exposure" is an
/// in-memory registration concept (`packages/coding-agent/src/extensions/codemode/`).
/// Pike defines the minimal project-local surface anyway so a project can
/// declare script tools before the sandbox exists (#870). See
/// `fixtures/codemode/declarations/README.md` for the format.
[[nodiscard]] std::filesystem::path codemode_declaration_directory(const std::filesystem::path& workspace);

/// pi `parseCodemodeSource` options (`packages/codemode/src/source.ts`).
struct CodemodeSourceOptions {
    std::optional<std::int64_t> max_output_tokens;
    std::optional<std::int64_t> timeout_ms;
};

/// A validated codemode script source: the JS body (the options line replaced
/// by an empty line so line numbers are unchanged) plus the parsed options.
struct CodemodeSource {
    std::string code;
    CodemodeSourceOptions options;
};

/// One validated project-local codemode declaration: the model-facing Tool
/// descriptor plus the parsed, not-yet-executed script source. `input_schema`
/// is pi `CodemodeTool.inputSchema`/Tool `parameters`; `source` is pi's
/// `parseCodemodeSource` result (#870 does not execute it).
struct CodemodeDeclaration {
    std::string name;
    std::string description;
    std::optional<support::JsonValue> input_schema;
    std::filesystem::path source_path;
    CodemodeSource source;
};

/// pi `parseCodemodeSource`: split an optional first-line `// @options: {...}`
/// from the script and validate it. Empty input, invalid options JSON, unknown
/// fields, out-of-range values, and an options line with no following code are
/// typed Validation errors (never a silent fallback to "no options").
[[nodiscard]] support::Expected<CodemodeSource> parse_codemode_source(std::string_view input);

/// Parse one declaration document (already read) whose file is
/// `declaration_path`; `source` resolves relative to that file's directory.
/// A missing/empty `name`, `description`, or `source`, an unknown field, a
/// non-object `inputSchema`, unreadable source, and source parse errors are
/// typed Validation errors.
[[nodiscard]] support::Expected<CodemodeDeclaration> parse_codemode_declaration(
        const std::filesystem::path& declaration_path, std::string_view json_text);

/// Read and parse one `*.json` declaration file.
[[nodiscard]] support::Expected<CodemodeDeclaration> load_codemode_declaration(
        const std::filesystem::path& declaration_path);

/// Load every `*.json` declaration in `declaration_directory` in file-name
/// order. A directory that does not exist contributes no declarations (a
/// project without codemode sources is not an error). Any invalid or
/// unreadable declaration aborts the load with a typed Validation error — no
/// declaration is skipped silently, and a partially loaded set is never
/// returned.
[[nodiscard]] support::Expected<std::vector<CodemodeDeclaration>> load_codemode_declarations(
        const std::filesystem::path& declaration_directory);

} // namespace cch::coding_agent::extensions
