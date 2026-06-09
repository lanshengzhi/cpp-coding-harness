#include "Tools.hpp"

#include "PathGuard.hpp"
#include "../util/JsonSchema.hpp"

#include <fstream>
#include <sstream>

namespace cch::tools {
namespace {

std::string string_arg(const boost::json::object& args, const char* key) {
    auto* value = args.if_contains(key);
    return value && value->is_string() ? std::string(value->as_string()) : std::string{};
}

bool bool_arg(const boost::json::object& args, const char* key, bool fallback) {
    auto* value = args.if_contains(key);
    return value && value->is_bool() ? value->as_bool() : fallback;
}

class WriteFileTool final : public agent::Tool {
public:
    [[nodiscard]] agent::ToolDefinition definition() const override {
        boost::json::object props;
        props["path"] = util::string_property("Path to create or overwrite inside the workspace");
        props["content"] = util::string_property("Text content to write");
        props["create_parents"] = util::boolean_property("Create missing parent directories when true");
        return {"write_file", "Write a text file inside the workspace", util::object_schema(std::move(props), {"path", "content"})};
    }

    [[nodiscard]] agent::ToolExecutionResult execute(const boost::json::object& arguments, const agent::ToolContext& context) override {
        const auto path = string_arg(arguments, "path");
        if (path.empty()) {
            return {"write_file.path is required", true};
        }
        const auto content = string_arg(arguments, "content");
        const bool create_parents = bool_arg(arguments, "create_parents", false);
        auto guard = PathGuard::create(context.workspace);
        if (!guard) {
            return {guard.error(), true};
        }
        auto resolved = guard.value().resolve_for_write(path, create_parents);
        if (!resolved) {
            return {resolved.error(), true};
        }
        std::ofstream output(resolved.value(), std::ios::binary | std::ios::trunc);
        if (!output) {
            return {"could not open file for writing: " + path, true};
        }
        output << content;
        output.close();
        std::ostringstream result;
        result << "Wrote " << content.size() << " bytes to " << path << ".";
        return {result.str(), false};
    }
};

} // namespace

agent::ToolPtr make_write_file_tool() { return std::make_shared<WriteFileTool>(); }

} // namespace cch::tools
