#pragma once

#include "coding_agent/extensions/ToolExposure.hpp"

#include <cch/ai/Tool.hpp>
#include <cch/support/Error.hpp>
#include <cch/support/JsonValue.hpp>

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::coding_agent::extensions {

struct LoadoutTool;

/// pi `DEFAULT_TOOL_SEARCH_LIMIT` (`extensions/tool-search/tool.ts`).
inline constexpr std::size_t kDefaultToolSearchLimit = 8;

/// pi `tokenize`: camelCase-split, lowercased, non-alphanumeric-split terms
/// without stop words, naively stemmed.
[[nodiscard]] std::vector<std::string> tokenize_tool_text(std::string_view text);

/// pi `createToolSearchDocument`: the text the ranker sees for one tool — the
/// name, the description, schema descriptions and property names, and the
/// namespace with its description and instructions.
[[nodiscard]] std::string tool_search_document_text(
        const ai::Tool& tool, const std::optional<ToolNamespace>& name_space);

/// pi `Bm25Ranker.rank`: Okapi BM25 over `documents`, ties keeping document
/// order. `query` is tokenized the same way. Each document is its registered
/// tool name plus the search text `tool_search_document_text` builds.
[[nodiscard]] std::vector<std::string> bm25_rank(
        std::string_view query, const std::vector<std::pair<std::string, std::string>>& documents, std::size_t limit);

/// pi `createDiscoveryGlobals`: `searchTools()`, `describeTool()`, and
/// `describeNamespace()` over one callable tool set. The tool set is fixed for
/// one script run, resolved from the live session registry at call time.
class CodemodeDiscovery {
public:
    explicit CodemodeDiscovery(std::vector<LoadoutTool> tools);

    /// The `globalsJson` entries for the sandbox prelude (all `spread: true`).
    [[nodiscard]] support::JsonValue globals_json() const;

    /// Handle one discovery global call. `name` is `searchTools`, `describeTool`,
    /// or `describeNamespace`; `arguments` is the spread argument array. The
    /// result is the JSON text the script receives, or a rejected-call error.
    [[nodiscard]] support::Expected<std::string> handle(
            std::string_view name, const support::JsonValue& arguments) const;

private:
    std::vector<LoadoutTool> tools_;
    std::map<std::string, std::string, std::less<>> samples_;
    std::vector<std::pair<std::string, std::string>> documents_;
};

} // namespace cch::coding_agent::extensions
