#pragma once

#include <cch/ai/Tool.hpp>
#include <cch/support/Error.hpp>
#include <cch/support/JsonValue.hpp>

#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace cch::coding_agent::extensions {

/// pi `DEFAULT_TOOL_SEARCH_LIMIT` (`extensions/tool-search/tool.ts`).
inline constexpr std::size_t kDefaultToolSearchLimit = 8;

/// pi `toCodemodeIdentifier` (`packages/codemode/src/identifier.ts`): characters
/// invalid in an identifier become `_`; an empty name becomes `_`.
[[nodiscard]] std::string to_codemode_identifier(std::string_view name);

/// pi `tokenize`: camelCase-split, lowercased, non-alphanumeric-split terms
/// without stop words, naively stemmed.
[[nodiscard]] std::vector<std::string> tokenize_tool_text(std::string_view text);

/// pi `createToolSearchDocument`: the text the ranker sees for one tool.
[[nodiscard]] std::string tool_search_document_text(const ai::Tool& tool);

/// pi `Bm25Ranker.rank`: Okapi BM25 over `documents`, ties keeping document
/// order. `query` is tokenized the same way.
[[nodiscard]] std::vector<std::string> bm25_rank(
        std::string_view query, const std::vector<ai::Tool>& tools, std::size_t limit);

/// pi `schemaToType`: a JSON Schema to a TypeScript type expression. A result
/// longer than `max_chars` renders as `unknown`.
[[nodiscard]] std::string schema_to_type(const support::JsonValue& schema, std::size_t max_chars);

/// pi `renderToolSample`: the tool's description followed by its TS declaration.
[[nodiscard]] std::string render_tool_sample(const ai::Tool& tool);

/// pi `isNamespaceName`.
[[nodiscard]] bool is_namespace_name(std::string_view namespace_name, std::string_view query);

/// pi `createDiscoveryGlobals`: `searchTools()`, `describeTool()`, and
/// `describeNamespace()` over one callable tool set. The tool set is fixed for
/// one script run, resolved from the live session registry at call time.
class CodemodeDiscovery {
public:
    explicit CodemodeDiscovery(std::vector<ai::Tool> tools);

    /// The `globalsJson` entries for the sandbox prelude (all `spread: true`).
    [[nodiscard]] support::JsonValue globals_json() const;

    /// Handle one discovery global call. `name` is `searchTools`, `describeTool`,
    /// or `describeNamespace`; `arguments` is the spread argument array. The
    /// result is the JSON text the script receives, or a rejected-call error.
    [[nodiscard]] support::Expected<std::string> handle(
            std::string_view name, const support::JsonValue& arguments) const;

private:
    std::vector<ai::Tool> tools_;
    std::map<std::string, std::string, std::less<>> samples_;
};

} // namespace cch::coding_agent::extensions
