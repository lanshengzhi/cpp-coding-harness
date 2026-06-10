#include "Tools.hpp"

#include "../harness/LocalExecutionEnv.hpp"
#include "../util/JsonSchema.hpp"

#include <memory>
#include <sstream>

namespace cch::tools {
namespace {

util::Result<std::string> required_string_arg(const boost::json::object& args, const char* key, const char* label, bool allow_empty) {
    auto* value = args.if_contains(key);
    if (value == nullptr) {
        return util::Result<std::string>::failure(std::string(label) + " is required");
    }
    if (!value->is_string()) {
        return util::Result<std::string>::failure(std::string(label) + " must be a string");
    }
    std::string text(value->as_string());
    if (!allow_empty && text.empty()) {
        return util::Result<std::string>::failure(std::string(label) + " must be non-empty");
    }
    return util::Result<std::string>::success(std::move(text));
}

bool bool_arg(const boost::json::object& args, const char* key, bool fallback) {
    auto* value = args.if_contains(key);
    return value && value->is_bool() ? value->as_bool() : fallback;
}

std::shared_ptr<harness::ExecutionEnv> execution_env_for(const agent::ToolContext& context) {
    if (context.execution_env) {
        return context.execution_env;
    }
    return std::make_shared<harness::LocalExecutionEnv>(context.workspace, context.bash_enabled, context.secret_environment_names);
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
        auto path = required_string_arg(arguments, "path", "write_file.path", false);
        if (!path) {
            return {path.error(), true};
        }
        auto content = required_string_arg(arguments, "content", "write_file.content", true);
        if (!content) {
            return {content.error(), true};
        }
        const bool create_parents = bool_arg(arguments, "create_parents", false);
        auto written = execution_env_for(context)->write_file(path.value(), content.value(), create_parents);
        if (!written) {
            return {written.error(), true};
        }
        std::ostringstream result;
        result << "Wrote " << written.value().bytes_written << " bytes to " << path.value() << ".";
        return {result.str(), false};
    }
};

} // namespace

agent::ToolPtr make_write_file_tool() { return std::make_shared<WriteFileTool>(); }

} // namespace cch::tools
