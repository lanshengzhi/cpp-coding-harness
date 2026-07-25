#include <cch/tools/ToolFactories.hpp>

#include "util/BoundedText.hpp"
#include "util/Json.hpp"
#include "util/OutputLimiter.hpp"

#include <algorithm>
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

struct BashArgs {
    std::string command;
    std::optional<int> timeout;  // seconds, optional (no default = no timeout)
};

[[nodiscard]] util::JsonValue typed_schema(
    std::string type,
    std::optional<std::string> description = std::nullopt) {
    util::JsonValue::object_t schema{{"type", std::move(type)}};
    if (description) {
        schema.emplace("description", std::move(*description));
    }
    return schema;
}

[[nodiscard]] util::JsonValue object_schema(
    util::JsonValue::object_t properties,
    std::vector<std::string> required) {
    util::JsonValue::array_t required_values;
    required_values.reserve(required.size());
    for (auto& name : required) {
        required_values.emplace_back(std::move(name));
    }
    return util::JsonValue::object_t{
        {"type", "object"},
        {"properties", std::move(properties)},
        {"required", std::move(required_values)},
        {"additionalProperties", false},
    };
}

[[nodiscard]] util::JsonValue array_schema(
    util::JsonValue items,
    std::optional<std::string> description = std::nullopt) {
    auto schema = typed_schema("array", std::move(description)).get_object();
    schema.emplace("items", std::move(items));
    return schema;
}

[[nodiscard]] agent::AsyncToolExecutionResult error_result(std::string content) {
    return agent::AsyncToolExecutionResult{std::vector<ai::Content>{ai::text_content(std::move(content))}, std::nullopt, true};
}

template <typename Error>
[[nodiscard]] agent::AsyncToolExecutionResult error_result_from(const Error& error) {
    return error_result(error.message);
}

[[nodiscard]] std::string combine_output(const std::string& stdout_output, const std::string& stderr_output) {
    std::string combined = stdout_output;
    if (!stderr_output.empty()) {
        if (!combined.empty() && combined.back() != '\n') {
            combined += '\n';
        }
        combined += stderr_output;
    }
    return combined;
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
            object_schema(
                {
                    {"path", typed_schema("string", "Workspace-relative file path")},
                    {"offset", typed_schema("integer", "1-based line offset")},
                    {"limit", typed_schema("integer", "Maximum number of lines to read")},
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
        auto lines = co_await (*environment)->readTextLines(parsed->path, std::nullopt);
        if (!lines) {
            co_return error_result_from(lines.error());
        }
        // offset is 1-based; limit 0 means no explicit limit.
        const auto offset = std::max(1, parsed->offset);
        const util::OutputLimit output_limit;
        std::string result;
        std::size_t bytes = 0;
        int emitted = 0;
        bool truncated = false;
        int line_number = 1;
        for (const auto& line : *lines) {
            if (line_number++ < offset) {
                continue;
            }
            if (parsed->limit > 0 && emitted >= parsed->limit) {
                break;
            }
            const auto next_bytes = bytes + line.size() + 1;
            if (static_cast<std::size_t>(emitted) >= output_limit.max_lines || next_bytes > output_limit.max_bytes) {
                truncated = true;
                break;
            }
            result += line;
            result += '\n';
            bytes = next_bytes;
            ++emitted;
        }
        if (!result.empty()) {
            result.pop_back();
        }
        // Append continuation hint when truncated
        if (truncated) {
            int next_offset = offset + emitted;
            result += "\n[output truncated]";
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
            object_schema(
                {
                    {"path", typed_schema("string", "Workspace-relative file path")},
                    {"content", typed_schema("string", "File content")},
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
        auto written = co_await (*environment)->writeFile(parsed->path, parsed->content);
        if (!written) {
            co_return error_result_from(written.error());
        }
        co_return agent::AsyncToolExecutionResult{std::vector<ai::Content>{ai::text_content("wrote " + std::to_string(parsed->content.size()) + " bytes")}, std::nullopt, false};
    }
};

class AsyncEditFileTool final : public AsyncToolBase {
public:
    using AsyncToolBase::AsyncToolBase;

    const ai::Tool& definition() const override {
        static const auto edit_entry_schema = object_schema(
            {
                {"oldText", typed_schema("string", "Exact text for one targeted replacement.")},
                {"newText", typed_schema("string", "Replacement text for this targeted edit.")},
            },
            {"oldText", "newText"});
        static const ai::Tool tool{
            "edit_file",
            "Replace exact text regions inside a workspace file with one or more edits. "
            "Each edit is matched against the original file, not incrementally. "
            "Overlapping edits are rejected.",
            object_schema(
                {
                    {"path", typed_schema("string", "Workspace-relative file path")},
                    {"edits", array_schema(edit_entry_schema, "One or more targeted replacements")},
                    {"old_text", typed_schema("string", "Legacy: exact text to replace (single edit)")},
                    {"new_text", typed_schema("string", "Legacy: replacement text (single edit)")},
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
        auto read = co_await (*environment)->readTextFile(parsed->path);
        if (!read) {
            co_return error_result_from(read.error());
        }
        const std::string original = *read;
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
                    util::bounded_redacted_text(edit.oldText, 50, "...") + "'");
            }
            if (count > 1) {
                co_return error_result("edit_file: oldText matches " + std::to_string(count) +
                    " occurrences, must be unique. Text: '" +
                    util::bounded_redacted_text(edit.oldText, 40, "...") + "'");
            }
            // Apply replacement
            working.replace(match_pos, edit.oldText.length(), edit.newText);
            applied.emplace_back(edit.oldText, edit.newText);
        }
        // Write back
        auto written = co_await (*environment)->writeFile(parsed->path, working);
        if (!written) {
            co_return error_result_from(written.error());
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
            object_schema(
                {
                    {"command", typed_schema("string", "Shell command")},
                    {"timeout", typed_schema("integer", "Timeout in seconds (optional)")},
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
        // Convert seconds to milliseconds for ExecutionEnv; zero means no timeout.
        harness::ExecOptions exec_options;
        exec_options.timeout = parsed->timeout
            ? std::chrono::milliseconds(std::chrono::seconds(*parsed->timeout))
            : std::chrono::milliseconds{0};
        std::string full_stdout;
        std::string full_stderr;
        bool received_stdout = false;
        bool received_stderr = false;
        exec_options.onStdout = [&](std::string_view chunk) {
            received_stdout = true;
            full_stdout.append(chunk);
        };
        exec_options.onStderr = [&](std::string_view chunk) {
            received_stderr = true;
            full_stderr.append(chunk);
        };
        auto shell = co_await (*environment)->exec(parsed->command, std::move(exec_options));
        if (!shell) {
            co_return error_result_from(shell.error());
        }

        std::string full_output = (received_stdout || received_stderr)
            ? strip_ansi(combine_output(full_stdout, full_stderr))
            : strip_ansi(combine_output(shell->stdout_output, shell->stderr_output));

        // Redact the complete output before splitting between model-visible and spill.
        std::string redacted_full = util::redact_text(full_output);
        const util::OutputLimit output_limit;
        auto limited_output = util::limit_output_tail(redacted_full, output_limit);
        bool truncated = limited_output.truncated;
        std::string output = std::move(limited_output.text);
        std::string full_output_path;
        if (truncated) {
            auto ts = std::chrono::system_clock::now().time_since_epoch().count();
            full_output_path = "bash-output-" + std::to_string(ts) + ".txt";
            if (auto write = co_await (*environment)->writeFile(full_output_path, redacted_full); !write) {
                full_output_path.clear();
            }
            output = "[output truncated, showing last " +
                std::to_string(output.size()) +
                " bytes]" + (!full_output_path.empty() ? " full output: " + full_output_path : "") +
                "\n" + output;
        }
        std::ostringstream out;
        out << "exit_code=" << shell->exitCode;
        if (truncated) {
            out << " truncated=true";
        }
        if (!output.empty()) {
            out << "\n" << output;
        }
        co_return agent::AsyncToolExecutionResult{
            std::vector<ai::Content>{ai::text_content(out.str())},
            std::nullopt,
            shell->exitCode != 0};
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
