#include <cch/tools/ToolFactories.hpp>

#include <cch/ai/glaze/ToolSchemaDtos.hpp>

#include "../util/Redactor.hpp"

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

struct BashArgs {
    std::string command;
    int timeout_ms{30000};
};

[[nodiscard]] agent::AsyncToolExecutionResult error_result(std::string content) {
    return agent::AsyncToolExecutionResult{std::move(content), std::nullopt, true};
}

template <typename Args>
[[nodiscard]] util::Expected<Args> parse_raw_args(std::string_view raw_arguments) {
    if (raw_arguments.empty()) {
        raw_arguments = "{}";
    }
    return util::read_json<Args>(raw_arguments);
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
        auto parsed = parse_raw_args<ReadFileArgs>(invocation.raw_arguments);
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
        co_return agent::AsyncToolExecutionResult{util::redact_text(read->content), std::nullopt, false};
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
        auto parsed = parse_raw_args<WriteFileArgs>(invocation.raw_arguments);
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
        auto parsed = parse_raw_args<EditFileArgs>(invocation.raw_arguments);
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
        auto parsed = parse_raw_args<BashArgs>(invocation.raw_arguments);
        if (!parsed || parsed->command.empty()) {
            co_return error_result("invalid bash arguments");
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
            out << "\n" << util::redact_text(shell->output);
        }
        co_return agent::AsyncToolExecutionResult{out.str(), std::nullopt, shell->exit_code != 0 || shell->timed_out};
    }
};

} // namespace

std::shared_ptr<agent::AsyncAgentTool> make_async_read_file_tool(std::shared_ptr<harness::AsyncExecutionEnv> env) {
    return std::make_shared<AsyncReadFileTool>(std::move(env));
}

std::shared_ptr<agent::AsyncAgentTool> make_async_write_file_tool(std::shared_ptr<harness::AsyncExecutionEnv> env) {
    return std::make_shared<AsyncWriteFileTool>(std::move(env));
}

std::shared_ptr<agent::AsyncAgentTool> make_async_edit_file_tool(std::shared_ptr<harness::AsyncExecutionEnv> env) {
    return std::make_shared<AsyncEditFileTool>(std::move(env));
}

std::shared_ptr<agent::AsyncAgentTool> make_async_bash_tool(std::shared_ptr<harness::AsyncExecutionEnv> env) {
    return std::make_shared<AsyncBashTool>(std::move(env));
}

} // namespace cch::tools
