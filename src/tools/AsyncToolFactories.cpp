#include "../../include/cch/tools/ToolFactories.hpp"

#include "../../include/cch/ai/glaze/ToolSchemaDtos.hpp"
#include "../../include/cch/util/Json.hpp"

#include <chrono>
#include <exception>
#include <memory>
#include <sstream>
#include <utility>

namespace cch::tools {
namespace {

struct ReadFileArgs {
    std::string path;
    int offset{1};
    int limit{0};
};

struct WriteFileArgs {
    std::string path;
    std::string content;
    bool create_parents{false};
};

struct EditFileArgs {
    std::string path;
    std::string old_text;
    std::string new_text;
};

constexpr int kDefaultBashTimeoutMs = 30000;
constexpr int kMaxBashTimeoutMs = 120000;

struct BashArgs {
    std::string command;
    int timeout_ms{kDefaultBashTimeoutMs};
};

[[nodiscard]] agent::AsyncToolExecutionResult error_result(std::string content) {
    return agent::AsyncToolExecutionResult{std::move(content), std::nullopt, true};
}

template <typename Args>
[[nodiscard]] util::Expected<Args> parse_invocation_args(const agent::ToolInvocation& invocation) {
    auto serialized = util::write_json(invocation.arguments);
    if (!serialized) {
        return std::unexpected(serialized.error());
    }
    return util::read_json<Args>(*serialized);
}

class AsyncToolBase : public agent::AsyncAgentTool {
public:
    explicit AsyncToolBase(std::shared_ptr<harness::AsyncExecutionEnv> env) : env_(std::move(env)) {}

protected:
    [[nodiscard]] util::Expected<harness::AsyncExecutionEnv*> env() const {
        if (!env_) {
            return std::unexpected(util::make_error(util::ErrorCode::Tool, "missing execution environment"));
        }
        return env_.get();
    }

    std::shared_ptr<harness::AsyncExecutionEnv> env_;
};

class AsyncReadFileTool final : public AsyncToolBase {
public:
    using AsyncToolBase::AsyncToolBase;

    const ai::Tool& definition() const override {
        static const ai::Tool tool{
            "read_file",
            "Read a text file inside the workspace",
            ai::JsonSchema::object(
                {
                    {"path", ai::JsonSchema::string("Workspace-relative file path")},
                    {"offset", ai::JsonSchema::integer("1-based line offset")},
                    {"limit", ai::JsonSchema::integer("Maximum number of lines to read")},
                },
                {"path"}),
        };
        return tool;
    }

    boost::asio::awaitable<util::Expected<agent::AsyncToolExecutionResult>> execute(
        agent::ToolInvocation invocation) override {
        auto parsed = parse_invocation_args<ReadFileArgs>(invocation);
        if (!parsed || parsed->path.empty()) {
            co_return error_result("invalid read_file arguments");
        }
        auto environment = env();
        if (!environment) {
            co_return std::unexpected(environment.error());
        }
        auto read = co_await (*environment)->read_file(parsed->path, parsed->offset, parsed->limit);
        if (!read) {
            co_return error_result(read.error().detail.empty() ? read.error().message : read.error().detail);
        }
        co_return agent::AsyncToolExecutionResult{read->content, std::nullopt, false};
    }
};

class AsyncWriteFileTool final : public AsyncToolBase {
public:
    using AsyncToolBase::AsyncToolBase;

    const ai::Tool& definition() const override {
        static const ai::Tool tool{
            "write_file",
            "Create or overwrite a text file inside the workspace",
            ai::JsonSchema::object(
                {
                    {"path", ai::JsonSchema::string("Workspace-relative file path")},
                    {"content", ai::JsonSchema::string("File content")},
                    {"create_parents", ai::JsonSchema::boolean("Create missing parent directories")},
                },
                {"path", "content"}),
        };
        return tool;
    }

    boost::asio::awaitable<util::Expected<agent::AsyncToolExecutionResult>> execute(
        agent::ToolInvocation invocation) override {
        auto parsed = parse_invocation_args<WriteFileArgs>(invocation);
        if (!parsed || parsed->path.empty()) {
            co_return error_result("invalid write_file arguments");
        }
        auto environment = env();
        if (!environment) {
            co_return std::unexpected(environment.error());
        }
        auto written = co_await (*environment)->write_file(parsed->path, parsed->content, parsed->create_parents);
        if (!written) {
            co_return error_result(written.error().detail.empty() ? written.error().message : written.error().detail);
        }
        co_return agent::AsyncToolExecutionResult{"wrote " + std::to_string(written->bytes_written) + " bytes", std::nullopt, false};
    }
};

class AsyncEditFileTool final : public AsyncToolBase {
public:
    using AsyncToolBase::AsyncToolBase;

    const ai::Tool& definition() const override {
        static const ai::Tool tool{
            "edit_file",
            "Replace one exact text region inside a workspace file",
            ai::JsonSchema::object(
                {
                    {"path", ai::JsonSchema::string("Workspace-relative file path")},
                    {"old_text", ai::JsonSchema::string("Exact text to replace")},
                    {"new_text", ai::JsonSchema::string("Replacement text")},
                },
                {"path", "old_text", "new_text"}),
        };
        return tool;
    }

    boost::asio::awaitable<util::Expected<agent::AsyncToolExecutionResult>> execute(
        agent::ToolInvocation invocation) override {
        auto parsed = parse_invocation_args<EditFileArgs>(invocation);
        if (!parsed || parsed->path.empty() || parsed->old_text.empty()) {
            co_return error_result("invalid edit_file arguments");
        }
        auto environment = env();
        if (!environment) {
            co_return std::unexpected(environment.error());
        }
        auto edited = co_await (*environment)->edit_file(parsed->path, parsed->old_text, parsed->new_text);
        if (!edited) {
            co_return error_result(edited.error().detail.empty() ? edited.error().message : edited.error().detail);
        }
        co_return agent::AsyncToolExecutionResult{"edited " + parsed->path, std::nullopt, false};
    }
};

class AsyncBashTool final : public AsyncToolBase {
public:
    using AsyncToolBase::AsyncToolBase;

    const ai::Tool& definition() const override {
        static const ai::Tool tool{
            "bash",
            "Run a shell command in the workspace when explicitly enabled",
            ai::JsonSchema::object(
                {
                    {"command", ai::JsonSchema::string("Shell command")},
                    {"timeout_ms", ai::JsonSchema::integer("Timeout in milliseconds")},
                },
                {"command"}),
        };
        return tool;
    }

    boost::asio::awaitable<util::Expected<agent::AsyncToolExecutionResult>> execute(
        agent::ToolInvocation invocation) override {
        auto parsed = parse_invocation_args<BashArgs>(invocation);
        if (!parsed || parsed->command.empty()) {
            co_return error_result("invalid bash arguments");
        }
        if (parsed->timeout_ms <= 0) {
            co_return error_result("invalid bash arguments: timeout_ms must be positive");
        }
        if (parsed->timeout_ms > kMaxBashTimeoutMs) {
            parsed->timeout_ms = kMaxBashTimeoutMs;
        }
        auto environment = env();
        if (!environment) {
            co_return std::unexpected(environment.error());
        }
        auto shell = co_await (*environment)->run_shell(parsed->command, std::chrono::milliseconds(parsed->timeout_ms));
        if (!shell) {
            co_return error_result(shell.error().detail.empty() ? shell.error().message : shell.error().detail);
        }
        std::ostringstream out;
        out << "exit_code=" << shell->exit_code;
        if (shell->timed_out) {
            out << " timed_out=true";
        }
        if (!shell->output.empty()) {
            out << "\n" << shell->output;
        }
        co_return agent::AsyncToolExecutionResult{out.str(), std::nullopt, shell->exit_code != 0 || shell->timed_out};
    }
};

} // namespace

std::unique_ptr<agent::AsyncAgentTool> make_async_read_file_tool(std::shared_ptr<harness::AsyncExecutionEnv> env) {
    return std::make_unique<AsyncReadFileTool>(std::move(env));
}

std::unique_ptr<agent::AsyncAgentTool> make_async_write_file_tool(std::shared_ptr<harness::AsyncExecutionEnv> env) {
    return std::make_unique<AsyncWriteFileTool>(std::move(env));
}

std::unique_ptr<agent::AsyncAgentTool> make_async_edit_file_tool(std::shared_ptr<harness::AsyncExecutionEnv> env) {
    return std::make_unique<AsyncEditFileTool>(std::move(env));
}

std::unique_ptr<agent::AsyncAgentTool> make_async_bash_tool(std::shared_ptr<harness::AsyncExecutionEnv> env) {
    return std::make_unique<AsyncBashTool>(std::move(env));
}

} // namespace cch::tools
