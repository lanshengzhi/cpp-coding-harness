#include "Tools.hpp"

#include "PathGuard.hpp"
#include "../util/JsonSchema.hpp"

#include <fstream>
#include <sstream>

namespace cch::tools {
namespace {

std::string string_arg_any(const boost::json::object& args, const char* first, const char* second = nullptr) {
    if (auto* value = args.if_contains(first); value && value->is_string()) {
        return std::string(value->as_string());
    }
    if (second != nullptr) {
        if (auto* value = args.if_contains(second); value && value->is_string()) {
            return std::string(value->as_string());
        }
    }
    return {};
}

std::size_t count_occurrences(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) {
        return 0;
    }
    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

class EditFileTool final : public agent::Tool {
public:
    [[nodiscard]] agent::ToolDefinition definition() const override {
        boost::json::object props;
        props["path"] = util::string_property("Path to edit inside the workspace");
        props["old_text"] = util::string_property("Exact text to replace; must occur exactly once");
        props["new_text"] = util::string_property("Replacement text");
        return {"edit_file", "Replace one unique exact text block inside a workspace file", util::object_schema(std::move(props), {"path", "old_text", "new_text"})};
    }

    [[nodiscard]] agent::ToolExecutionResult execute(const boost::json::object& arguments, const agent::ToolContext& context) override {
        const auto path = string_arg_any(arguments, "path");
        const auto old_text = string_arg_any(arguments, "old_text", "oldText");
        const auto new_text = string_arg_any(arguments, "new_text", "newText");
        if (path.empty() || old_text.empty()) {
            return {"edit_file requires path and non-empty old_text", true};
        }
        auto guard = PathGuard::create(context.workspace);
        if (!guard) {
            return {guard.error(), true};
        }
        auto resolved = guard.value().resolve_existing_file(path);
        if (!resolved) {
            return {resolved.error(), true};
        }
        std::ifstream input(resolved.value(), std::ios::binary);
        if (!input) {
            return {"could not open file for editing: " + path, true};
        }
        std::ostringstream buffer;
        buffer << input.rdbuf();
        auto content = buffer.str();
        const auto matches = count_occurrences(content, old_text);
        if (matches == 0) {
            return {"old_text did not match any text in " + path, true};
        }
        if (matches > 1) {
            return {"old_text matched multiple regions in " + path + "; edit is ambiguous", true};
        }
        const auto pos = content.find(old_text);
        content.replace(pos, old_text.size(), new_text);
        std::ofstream output(resolved.value(), std::ios::binary | std::ios::trunc);
        if (!output) {
            return {"could not write edited file: " + path, true};
        }
        output << content;
        std::ostringstream result;
        result << "Successfully replaced 1 block(s) in " << path << ".\n";
        result << "-" << old_text.substr(0, 80) << "\n+" << new_text.substr(0, 80);
        return {result.str(), false};
    }
};

} // namespace

agent::ToolPtr make_edit_file_tool() { return std::make_shared<EditFileTool>(); }

} // namespace cch::tools
