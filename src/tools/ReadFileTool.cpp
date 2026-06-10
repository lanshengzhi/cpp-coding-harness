#include "Tools.hpp"

#include "../harness/LocalExecutionEnv.hpp"
#include "../util/JsonSchema.hpp"

#include <algorithm>
#include <memory>
#include <sstream>

namespace cch::tools {
namespace {

std::string required_string(const boost::json::object& args, const char* key) {
    auto* value = args.if_contains(key);
    if (value == nullptr || !value->is_string()) {
        return {};
    }
    return std::string(value->as_string());
}

int optional_int(const boost::json::object& args, const char* key, int fallback) {
    auto* value = args.if_contains(key);
    if (value == nullptr) {
        return fallback;
    }
    if (value->is_int64()) {
        return static_cast<int>(value->as_int64());
    }
    return fallback;
}

std::shared_ptr<harness::ExecutionEnv> execution_env_for(const agent::ToolContext& context) {
    if (context.execution_env) {
        return context.execution_env;
    }
    return std::make_shared<harness::LocalExecutionEnv>(context.workspace, context.bash_enabled, context.secret_environment_names);
}

class ReadFileTool final : public agent::Tool {
public:
    [[nodiscard]] agent::ToolDefinition definition() const override {
        boost::json::object props;
        props["path"] = util::string_property("Path to a file inside the workspace");
        props["offset"] = util::integer_property("1-based line number to start reading from");
        props["limit"] = util::integer_property("Maximum number of lines to return");
        return {"read_file", "Read a text file inside the workspace", util::object_schema(std::move(props), {"path"})};
    }

    [[nodiscard]] agent::ToolExecutionResult execute(const boost::json::object& arguments, const agent::ToolContext& context) override {
        const auto path = required_string(arguments, "path");
        if (path.empty()) {
            return {"read_file.path is required", true};
        }
        const int offset = std::max(1, optional_int(arguments, "offset", 1));
        const int limit = optional_int(arguments, "limit", 0);
        auto read = execution_env_for(context)->read_file(path, offset, limit);
        if (!read) {
            return {read.error(), true};
        }
        std::ostringstream result;
        result << "path: " << path << "\n" << read.value().content;
        return {result.str(), false};
    }
};

} // namespace

agent::ToolPtr make_read_file_tool() { return std::make_shared<ReadFileTool>(); }

} // namespace cch::tools
