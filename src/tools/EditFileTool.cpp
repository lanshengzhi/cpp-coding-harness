#include "Tools.hpp"

#include "../harness/LocalExecutionEnv.hpp"
#include "../util/JsonSchema.hpp"

#include <memory>
#include <sstream>

namespace cch::tools {
namespace {

util::Result<std::string> required_string_arg_any(
    const boost::json::object& args,
    const char* first,
    const char* second,
    const char* label,
    bool allow_empty) {
    auto read_key = [&](const char* key) -> util::Result<std::string> {
        if (key == nullptr) {
            return util::Result<std::string>::failure(std::string(label) + " is required");
        }
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
    };

    if (args.if_contains(first) != nullptr) {
        return read_key(first);
    }
    if (second != nullptr && args.if_contains(second) != nullptr) {
        return read_key(second);
    }
    return util::Result<std::string>::failure(std::string(label) + " is required");
}

std::shared_ptr<harness::ExecutionEnv> execution_env_for(const agent::ToolContext& context) {
    if (context.execution_env) {
        return context.execution_env;
    }
    return std::make_shared<harness::LocalExecutionEnv>(context.workspace, context.bash_enabled, context.secret_environment_names);
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
        auto path = required_string_arg_any(arguments, "path", nullptr, "edit_file.path", false);
        if (!path) {
            return {path.error(), true};
        }
        auto old_text = required_string_arg_any(arguments, "old_text", "oldText", "edit_file.old_text", false);
        if (!old_text) {
            return {old_text.error(), true};
        }
        auto new_text = required_string_arg_any(arguments, "new_text", "newText", "edit_file.new_text", true);
        if (!new_text) {
            return {new_text.error(), true};
        }
        auto edited = execution_env_for(context)->edit_file(path.value(), old_text.value(), new_text.value());
        if (!edited) {
            return {edited.error(), true};
        }
        std::ostringstream result;
        result << "Successfully replaced 1 block(s) in " << path.value() << ".\n";
        result << "-" << edited.value().old_preview << "\n+" << edited.value().new_preview;
        return {result.str(), false};
    }
};

} // namespace

agent::ToolPtr make_edit_file_tool() { return std::make_shared<EditFileTool>(); }

} // namespace cch::tools
