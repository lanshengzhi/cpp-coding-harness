#include "Tools.hpp"

#include "OutputLimiter.hpp"
#include "../util/JsonSchema.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <sstream>
#include <utility>

#if defined(__unix__) || defined(__APPLE__)
extern char** environ;
#endif

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

bool secret_env_name(std::string name, const std::vector<std::string>& explicit_secret_names = {}) {
    for (const auto& explicit_name : explicit_secret_names) {
        if (name == explicit_name) {
            return true;
        }
    }
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    return name.find("API_KEY") != std::string::npos || name.find("TOKEN") != std::string::npos ||
           name.find("SECRET") != std::string::npos || name.find("PASSWORD") != std::string::npos ||
           name.find("OPENAI") != std::string::npos;
}

std::map<std::string, std::string> sanitized_environment(const std::vector<std::string>& explicit_secret_names = {}) {
    std::map<std::string, std::string> env;
#if defined(__unix__) || defined(__APPLE__)
    for (char** current = environ; current != nullptr && *current != nullptr; ++current) {
        std::string entry(*current);
        auto split = entry.find('=');
        if (split == std::string::npos) {
            continue;
        }
        auto key = entry.substr(0, split);
        if (!secret_env_name(key, explicit_secret_names)) {
            env[key] = entry.substr(split + 1);
        }
    }
#endif
    return env;
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
        if (!context.bash_enabled) {
            return {"bash is disabled by default; rerun with explicit bash enablement", true};
        }
        const auto command = string_arg(arguments, "command");
        if (command.empty()) {
            return {"bash.command is required", true};
        }
        const int timeout_ms = std::max(1, int_arg(arguments, "timeout_ms", int_arg(arguments, "timeout", 30000)));
        util::ProcessRequest request;
        request.command = command;
        request.working_directory = context.workspace;
        request.timeout = std::chrono::milliseconds(timeout_ms);
        request.environment = sanitized_environment(context.secret_environment_names);
        request.use_explicit_environment = true;
        auto process = runner_->run(request);
        if (!process) {
            return {process.error(), true};
        }
        auto limited = limit_output(process.value().output);
        std::ostringstream result;
        result << "exit_code: " << process.value().exit_code << "\n";
        result << "timed_out: " << (process.value().timed_out ? "true" : "false") << "\n";
        result << limited.text;
        return {result.str(), process.value().timed_out || process.value().exit_code != 0};
    }

private:
    std::shared_ptr<util::ProcessRunner> runner_;
};

} // namespace

agent::ToolPtr make_bash_tool(std::shared_ptr<util::ProcessRunner> runner) { return std::make_shared<BashTool>(std::move(runner)); }

} // namespace cch::tools
