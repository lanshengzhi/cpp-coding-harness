#pragma once

#include <cch/support/Error.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace cch::coding_agent::extensions {

/// pi `CODEMODE_OPTIONS_PREFIX` (`packages/codemode/src/source.ts`).
inline constexpr std::string_view kCodemodeOptionsPrefix = "// @options:";

/// pi `CODEMODE_SOURCE_GRAMMAR` (`packages/codemode/src/source.ts`), byte for
/// byte: the model-facing `codemode` tool's grammar-constrained `code`
/// argument. It only fixes the shape of the optional options line; the options
/// JSON and the code are checked by `parse_codemode_source`.
inline constexpr std::string_view kCodemodeSourceGrammar = "\n"
                                                           "start: options_source | plain_source\n"
                                                           "options_source: OPTIONS_LINE NEWLINE SOURCE\n"
                                                           "plain_source: SOURCE\n"
                                                           "\n"
                                                           "OPTIONS_LINE: /[ \\t]*\\/\\/ @options:[^\\r\\n]*/\n"
                                                           "NEWLINE: /\\r?\\n/\n"
                                                           "SOURCE: /[\\s\\S]+/\n";

/// pi `CodemodeSourceOptions`: the parsed `// @options:` fields. Both are
/// optional; a missing field falls back to the sandbox's own default.
struct CodemodeSourceOptions {
    std::optional<std::int64_t> max_output_tokens;
    std::optional<std::int64_t> timeout_ms;
};

/// pi `ParsedCodemodeSource`: the script body with the options line replaced
/// by an empty line (so line numbers are unchanged) plus the parsed options.
struct CodemodeSource {
    std::string code;
    CodemodeSourceOptions options;
};

/// pi `parseCodemodeSource`: split an optional first-line `// @options: {...}`
/// from the script and validate it. Empty input, invalid options JSON, unknown
/// fields, out-of-range values, and an options line with no following code are
/// typed Validation errors (never a silent fallback to "no options").
[[nodiscard]] support::Expected<CodemodeSource> parse_codemode_source(std::string_view input);

} // namespace cch::coding_agent::extensions
