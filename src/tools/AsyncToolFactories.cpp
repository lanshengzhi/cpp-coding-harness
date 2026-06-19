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
};

struct EditEntry {
    std::string oldText;
    std::string newText;
};

struct EditFileArgs {
    std::string path;
    std::vector<EditEntry> edits;
    // Legacy single-arg fallback
    std::optional<std::string> old_text;
    std::optional<std::string> new_text;
};

constexpr int kDefaultMaxOutputLines = 2000;
constexpr std::size_t kDefaultMaxOutputBytes = 50 * 1024;

struct BashArgs {
    std::string command;
    std::optional<int> timeout;  // seconds, optional (no default = no timeout)
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
            "read",
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
            co_return error_result("invalid read arguments");
        }
        auto environment = env();
        if (!environment) {
            co_return std::unexpected(environment.error());
        }
        auto read = co_await (*environment)->read_file(parsed->path, parsed->offset, parsed->limit);
        if (!read) {
            co_return error_result(read.error().detail.empty() ? read.error().message : read.error().detail);
        }
        // Append continuation hint when truncated
        std::string result = read->content;
        if (read->truncated) {
            int next_offset = parsed->offset + read->lines_read;
            result += "\n\n[Output truncated. Use offset=" + std::to_string(next_offset) + " to continue.]";
        }
        co_return agent::AsyncToolExecutionResult{std::vector<ai::Content>{ai::text_content(std::move(result))}, std::nullopt, false};
    }
};

class AsyncWriteFileTool final : public AsyncToolBase {
public:
    using AsyncToolBase::AsyncToolBase;

    const ai::Tool& definition() const override {
        static const ai::Tool tool{
            "write",
            "Create or overwrite a text file inside the workspace. Parent directories are created automatically.",
            ai::JsonSchema::object(
                {
                    {"path", ai::JsonSchema::string("Workspace-relative file path")},
                    {"content", ai::JsonSchema::string("File content")},
                },
                {"path", "content"}),
        };
        return tool;
    }

    boost::asio::awaitable<util::Expected<agent::AsyncToolExecutionResult>> execute(
        agent::ToolInvocation invocation) override {
        auto parsed = parse_invocation_args<WriteFileArgs>(invocation);
        if (!parsed || parsed->path.empty()) {
            co_return error_result("invalid write arguments");
        }
        auto environment = env();
        if (!environment) {
            co_return std::unexpected(environment.error());
        }
        auto written = co_await (*environment)->write_file(parsed->path, parsed->content, true);
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
                    {"oldText", ai::JsonSchema::string("Exact text for one targeted replacement.")},
                    {"newText", ai::JsonSchema::string("Replacement text for this targeted edit.")},
                },
                {"oldText", "newText"},
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
            if (edit.oldText.empty()) {
                co_return error_result("invalid edit_file arguments: empty oldText in edits");
            }
            // Count occurrences
            size_t pos = 0;
            int count = 0;
            size_t match_pos = std::string::npos;
            while ((pos = working.find(edit.oldText, pos)) != std::string::npos) {
                if (count == 0) match_pos = pos;
                ++count;
                pos += edit.oldText.length();
            }
            if (count == 0) {
                co_return error_result("edit_file: oldText not found in file: '" +
                    (edit.oldText.length() > 50 ? edit.oldText.substr(0, 47) + "..." : edit.oldText) + "'");
            }
            if (count > 1) {
                co_return error_result("edit_file: oldText matches " + std::to_string(count) +
                    " occurrences, must be unique. Text: '" +
                    (edit.oldText.length() > 40 ? edit.oldText.substr(0, 37) + "..." : edit.oldText) + "'");
            }
            // Apply replacement
            working.replace(match_pos, edit.oldText.length(), edit.newText);
            applied.emplace_back(edit.oldText, edit.newText);
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
                    {"timeout", ai::JsonSchema::integer("Timeout in seconds (optional)")},
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
        auto environment = env();
        if (!environment) {
            co_return std::unexpected(environment.error());
        }
        // Convert seconds to milliseconds for ExecutionEnv
        auto timeout_ms = parsed->timeout
            ? std::optional<std::chrono::milliseconds>(std::chrono::seconds(*parsed->timeout))
            : std::optional<std::chrono::milliseconds>{};
        auto shell = co_await (*environment)->run_shell(
            parsed->command,
            timeout_ms.value_or(std::chrono::milliseconds::zero()));
        if (!shell) {
            co_return error_result(shell.error().detail.empty() ? shell.error().message : shell.error().detail);
        }
        // Strip ANSI CSI escape sequences
        std::string output = strip_ansi(shell->output);
        // Apply truncation
        bool truncated = false;
        std::string full_output_path;
        {
            std::size_t line_count = 1;
            std::size_t byte_count = 0;
            for (char c : output) {
                if (c == '\n') ++line_count;
                ++byte_count;
            }
            if (line_count > kDefaultMaxOutputLines || byte_count > kDefaultMaxOutputBytes) {
                truncated = true;
                auto ts = std::chrono::system_clock::now().time_since_epoch().count();
                full_output_path = "bash-output-" + std::to_string(ts) + ".txt";
                if (auto write = co_await (*environment)->write_file(full_output_path, output, true); !write) {
                    full_output_path.clear();
                }
                // Truncate: keep last 2000 lines / 50KB (tail truncation)
                std::size_t keep_bytes = 0;
                std::size_t keep_lines = 0;
                std::size_t cut_pos = output.size();
                for (std::size_t i = output.size(); i > 0; --i) {
                    char c = output[i - 1];
                    if (c == '\n') ++keep_lines;
                    keep_bytes += 1;
                    if (keep_lines >= kDefaultMaxOutputLines || keep_bytes >= kDefaultMaxOutputBytes) {
                        cut_pos = i;
                        break;
                    }
                }
                output = "[output truncated, showing last " +
                    std::to_string(static_cast<int>(output.size() - cut_pos)) +
                    " bytes]" + (!full_output_path.empty() ? " full output: " + full_output_path : "") +
                    "\n" + output.substr(cut_pos);
            }
        }
        std::ostringstream out;
        out << "exit_code=" << shell->exit_code;
        if (shell->timed_out) {
            out << " timed_out=true";
        }
        if (truncated) {
            out << " truncated=true";
        }
        if (!output.empty()) {
            out << "\n" << output;
        }
        co_return agent::AsyncToolExecutionResult{
            std::vector<ai::Content>{ai::text_content(out.str())},
            std::nullopt,
            shell->exit_code != 0 || shell->timed_out};
    }

private:
    static std::string strip_ansi(const std::string& input) {
        std::string output;
        output.reserve(input.size());
        for (std::size_t i = 0; i < input.size(); ++i) {
            if (input[i] == '\x1b' && i + 1 < input.size() && input[i + 1] == '[') {
                i += 2;
                while (i < input.size() && !((input[i] >= 'a' && input[i] <= 'z') ||
                    (input[i] >= 'A' && input[i] <= 'Z'))) {
                    ++i;
                }
                continue;
            }
            output += input[i];
        }
        return output;
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
