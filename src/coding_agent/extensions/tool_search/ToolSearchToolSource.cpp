#include "coding_agent/extensions/tool_search/ToolSearchToolSource.hpp"

#include <cch/ai/Content.hpp>

#include "support/Json.hpp"

#include <format>
#include <stop_token>
#include <string>
#include <utility>

namespace cch::coding_agent::extensions {

ToolSearchToolSource::ToolSearchToolSource(Search search, Activate activate)
    : search_(std::move(search)), activate_(std::move(activate)) {}

support::Expected<std::vector<ExtensionTool>> ToolSearchToolSource::load_tools() {
    ExtensionTool tool;
    tool.definition = tool_search_definition();
    tool.default_active = false;
    tool.prompt_snippet = std::nullopt;
    tool.concurrency = agent::ToolConcurrency::Exclusive;
    tool.execute = [search = search_, activate = activate_](
                           support::JsonValue arguments, std::stop_token) -> support::AsyncResult<ExtensionToolResult> {
        ExtensionToolResult result;
        const auto* fields = arguments.get_if<support::JsonValue::object_t>();
        if (fields == nullptr) {
            result.is_error = true;
            result.content.push_back(ai::text_content("tool_search requires a string query."));
            return support::AsyncResult<ExtensionToolResult>{support::Expected<ExtensionToolResult>{std::move(result)}};
        }
        const auto query = fields->find("query");
        if (query == fields->end() || !query->second.holds<std::string>()) {
            result.is_error = true;
            result.content.push_back(ai::text_content("tool_search requires a string query."));
            return support::AsyncResult<ExtensionToolResult>{support::Expected<ExtensionToolResult>{std::move(result)}};
        }
        std::size_t limit = 5;
        if (const auto found = fields->find("limit"); found != fields->end()) {
            if (const auto* number = found->second.get_if<double>();
                    number != nullptr && *number >= 1.0 && *number <= 50.0) {
                limit = static_cast<std::size_t>(*number);
            }
        }
        auto matches = search(query->second.get_string(), limit);
        std::vector<std::string> names;
        std::string text;
        for (const auto& match : matches) {
            names.push_back(match.name);
            text += std::format("- {}: {}\n", match.name, match.description);
        }
        if (names.empty())
            text = "No matching inactive tools found.";
        else
            activate(std::move(names));
        result.content.push_back(ai::text_content(std::move(text)));
        return support::AsyncResult<ExtensionToolResult>{support::Expected<ExtensionToolResult>{std::move(result)}};
    };
    std::vector<ExtensionTool> tools;
    tools.push_back(std::move(tool));
    return tools;
}

} // namespace cch::coding_agent::extensions
