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

struct EditEntry {
    std::string old_text;
    std::string new_text;
};

struct EditFileArgs {
    std::string path;
    std::vector<EditEntry> edits;
    // Legacy single-arg fallback
    std::optional<std::string> old_text;
    std::optional<std::string> new_text;
};

constexpr int kDefaultBashTimeoutMs = 30000;
constexpr int kMaxBashTimeoutMs = 120000;

struct BashArgs {
    std::string command;
    int timeout_ms{kDefaultBashTimeoutMs};
};

[[nodiscard]] agent::AsyncToolExecutionResult error_result(std::string content) {
    return agent::AsyncToolExecutionResult{std::vector<ai::Content>{ai::text_content(std::move(content))}, std::nullopt, true};
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
        co_return agent::AsyncToolExecutionResult{std::vector<ai::Content>{ai::text_content(read->content)}, std::nullopt, false};
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
        co_return agent::AsyncToolExecutionResult{std::vector<ai::Content>{ai::text_content("wrote " + std::to_string(written->bytes_written) + " bytes")}, std::nullopt, false};
    }
};

class AsyncEditFileTool final : public AsyncToolBase {
public:
    using AsyncToolBase::AsyncToolBase;

    const ai::Tool& definition() const override {
        static const auto edit_entry_schema = std::make_shared<ai::JsonSchema>(
            ai::JsonSchema::object(
                {
                    {"old_text", ai::JsonSchema::string("Exact text for one targeted replacement.")},
                    {"new_text", ai::JsonSchema::string("Replacement text for this targeted edit.")},
                },
                {"old_text", "new_text"},
                std::nullopt,
                false));
        static const ai::Tool tool{
            "edit_file",
            "Replace exact text regions inside a workspace file with one or more edits. "
            "Each edit is matched against the original file, not incrementally. "
            "Overlapping edits are rejected.",
            ai::JsonSchema::object(
                {
                    {"path", ai::JsonSchema::string("Workspace-relative file path")},
                    {"edits", ai::JsonSchema::array(
                        std::make_shared<ai::JsonSchema>(*edit_entry_schema),
                        "One or more targeted replacements")},
                    {"old_text", ai::JsonSchema::string("Legacy: exact text to replace (single edit)")},
                    {"new_text", ai::JsonSchema::string("Legacy: replacement text (single edit)")},
                },
                {"path", "edits"}),
        };
        return tool;
    }

    boost::asio::awaitable<util::Expected<agent::AsyncToolExecutionResult>> execute(
        agent::ToolInvocation invocation) override {
        auto parsed = parse_invocation_args<EditFileArgs>(invocation);
        if (!parsed || parsed->path.empty()) {
            co_return error_result("invalid edit_file arguments: missing path");
        }
        // Backward-compatible legacy conversion: single old_text/new_text → edits[]
        if (parsed->edits.empty() && parsed->old_text && !parsed->old_text->empty()) {
            if (!parsed->new_text) {
                co_return error_result("invalid edit_file arguments: old_text without new_text");
            }
            parsed->edits.push_back(EditEntry{*parsed->old_text, *parsed->new_text});
        }
        if (parsed->edits.empty()) {
            co_return error_result("invalid edit_file arguments: edits must contain at least one replacement");
        }
        auto environment = env();
        if (!environment) {
            co_return std::unexpected(environment.error());
        }
        // Read the original file
        auto read = co_await (*environment)->read_file(parsed->path, 1, 0);
        if (!read) {
            co_return error_result(read.error().detail.empty() ? read.error().message : read.error().detail);
        }
        const std::string original = read->content;
        // Validate all edits against the original content
        std::string working = original;
        std::vector<std::pair<std::string, std::string>> applied;
        for (const auto& edit : parsed->edits) {
            if (edit.old_text.empty()) {
                co_return error_result("invalid edit_file arguments: empty oldText in edits");
            }
            // Count occurrences
            size_t pos = 0;
            int count = 0;
            size_t match_pos = std::string::npos;
            while ((pos = working.find(edit.old_text, pos)) != std::string::npos) {
                if (count == 0) match_pos = pos;
                ++count;
                pos += edit.old_text.length();
            }
            if (count == 0) {
                co_return error_result("edit_file: oldText not found in file: '" +
                    (edit.old_text.length() > 50 ? edit.old_text.substr(0, 47) + "..." : edit.old_text) + "'");
            }
            if (count > 1) {
                co_return error_result("edit_file: oldText matches " + std::to_string(count) +
                    " occurrences, must be unique. Text: '" +
                    (edit.old_text.length() > 40 ? edit.old_text.substr(0, 37) + "..." : edit.old_text) + "'");
            }
            // Apply replacement
            working.replace(match_pos, edit.old_text.length(), edit.new_text);
            applied.emplace_back(edit.old_text, edit.new_text);
        }
        // Write back
        auto written = co_await (*environment)->write_file(parsed->path, working, true);
        if (!written) {
            co_return error_result(written.error().detail.empty() ? written.error().message : written.error().detail);
        }
        // Build a simple result message
        std::string result_text = "Successfully replaced " + std::to_string(applied.size()) + " block(s) in " + parsed->path + ".";
        // Generate a basic line-diff preview
        if (original != working) {
            result_text += "\n--- before\n+++ after\n";
            // Simple line-by-line diff: show first changed region
            auto orig_lines = split_lines(original);
            auto new_lines = split_lines(working);
            size_t i = 0;
            while (i < orig_lines.size() && i < new_lines.size() && orig_lines[i] == new_lines[i]) ++i;
            if (i < orig_lines.size() || i < new_lines.size()) {
                // Show context: up to 3 lines before and after the first change
                size_t ctx_start = (i > 3) ? i - 3 : 0;
                size_t ctx_end_orig = std::min(i + 3, orig_lines.size());
                size_t ctx_end_new = std::min(i + 3, new_lines.size());
                for (size_t j = ctx_start; j < ctx_end_orig && j < orig_lines.size(); ++j) {
                    result_text += (j >= i && (j - i < new_lines.size() - i) ? "-" : " ") + orig_lines[j] + "\n";
                }
                for (size_t j = ctx_start; j < ctx_end_new && j < new_lines.size(); ++j) {
                    result_text += (j >= i ? "+" : " ") + new_lines[j] + "\n";
                }
            }
        }
        co_return agent::AsyncToolExecutionResult{
            std::vector<ai::Content>{ai::text_content(result_text)},
            std::nullopt, false};
    }

private:
    static std::vector<std::string> split_lines(const std::string& text) {
        std::vector<std::string> lines;
        size_t start = 0;
        for (size_t i = 0; i < text.size(); ++i) {
            if (text[i] == '\n') {
                lines.push_back(text.substr(start, i - start));
                start = i + 1;
            }
        }
        if (start < text.size()) lines.push_back(text.substr(start));
        return lines;
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
        co_return agent::AsyncToolExecutionResult{std::vector<ai::Content>{ai::text_content(out.str())}, std::nullopt, shell->exit_code != 0 || shell->timed_out};
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
