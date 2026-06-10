#include "Tools.hpp"

#include "../harness/LocalExecutionEnv.hpp"
#include "../util/JsonSchema.hpp"

#include <algorithm>
#include <chrono>
#include <memory>
#include <sstream>
#include <utility>

namespace cch::tools {
namespace {

std::string string_arg(const boost::json::object& args, const char* key) {
    auto* value = args.if_contains(key);
    return value && value->is_string() ? std::string(value->as_string()) : std::string{};
}

int int_arg(const boost::json::object& args, const char* key, int fallback) {
    auto* value = args.if_contains(key);
    return value && value->is_int64() ? static_cast<int>(value->as_int64()) : fallback;
}

std::shared_ptr<harness::ExecutionEnv> execution_env_for(const agent::ToolContext& context, std::shared_ptr<util::ProcessRunner> runner) {
    if (context.execution_env) {
        return context.execution_env;
    }
    return std::make_shared<harness::LocalExecutionEnv>(
        context.workspace,
        context.bash_enabled,
        context.secret_environment_names,
        std::move(runner));
}

class BashTool final : public agent::Tool {
public:
    explicit BashTool(std::shared_ptr<util::ProcessRunner> runner) : runner_(std::move(runner)) {}

    [[nodiscard]] agent::ToolDefinition definition() const override {
        boost::json::object props;
        props["command"] = util::string_property("Bash command to execute inside the workspace");
        props["timeout_ms"] = util::integer_property("Timeout in milliseconds");
        return {"bash", "Run a bash command inside the workspace when explicitly enabled", util::object_schema(std::move(props), {"command"})};
    }

    [[nodiscard]] agent::ToolExecutionResult execute(const boost::json::object& arguments, const agent::ToolContext& context) override {
        const auto command = string_arg(arguments, "command");
        if (command.empty()) {
            return {"bash.command is required", true};
        }
        const int timeout_ms = std::max(1, int_arg(arguments, "timeout_ms", int_arg(arguments, "timeout", 30000)));
        auto shell = execution_env_for(context, runner_)->run_shell(command, std::chrono::milliseconds(timeout_ms));
        if (!shell) {
            return {shell.error(), true};
        }
        std::ostringstream result;
        result << "exit_code: " << shell.value().exit_code << "\n";
        result << "timed_out: " << (shell.value().timed_out ? "true" : "false") << "\n";
        result << shell.value().output;
        return {result.str(), shell.value().timed_out || shell.value().exit_code != 0};
    }

private:
    std::shared_ptr<util::ProcessRunner> runner_;
};

} // namespace

agent::ToolPtr make_bash_tool(std::shared_ptr<util::ProcessRunner> runner) { return std::make_shared<BashTool>(std::move(runner)); }

} // namespace cch::tools
