#include <cch/tools/ToolFactories.hpp>

#include "tools/EditDiff.hpp"
#include "util/BoundedText.hpp"
#include "util/Json.hpp"
#include "util/JsonGlaze.hpp"
#include "util/OutputLimiter.hpp"
#include "util/TerminalText.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <exception>
#include <map>
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

struct EditArgs {
    std::string path;
    std::vector<EditEntry> edits;
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
    return agent::AsyncToolExecutionResult{
        .content = std::vector<ai::Content>{ai::text_content(std::move(content))},
        .details = std::nullopt,
        .is_error = true,
    };
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

    // pi `core/tools/read.ts` promptSnippet/promptGuidelines (verbatim).
    [[nodiscard]] std::optional<std::string> prompt_snippet() const override {
        return "Read file contents";
    }
    [[nodiscard]] std::vector<std::string> prompt_guidelines() const override {
        return {"Use read to examine files instead of cat or sed."};
    }

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
        agent::ToolInvocation invocation,
        std::stop_token stop_token) override {
        auto parsed = parse_invocation_args<ReadFileArgs>(invocation);
        if (!parsed || parsed->path.empty()) {
            co_return error_result("invalid read arguments");
        }
        if (auto environment = env(); !environment) {
            co_return std::unexpected(environment.error());
        }
        auto lines = co_await env_->readTextLines(parsed->path, std::nullopt, stop_token);
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
        co_return agent::AsyncToolExecutionResult{
            .content = std::vector<ai::Content>{ai::text_content(std::move(result))},
            .details = std::nullopt,
        };
    }
};

class AsyncWriteFileTool final : public AsyncToolBase {
public:
    using AsyncToolBase::AsyncToolBase;

    // pi `core/tools/write.ts` promptSnippet/promptGuidelines (verbatim).
    [[nodiscard]] std::optional<std::string> prompt_snippet() const override {
        return "Create or overwrite files";
    }
    [[nodiscard]] std::vector<std::string> prompt_guidelines() const override {
        return {"Use write only for new files or complete rewrites."};
    }

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
        agent::ToolInvocation invocation,
        std::stop_token stop_token) override {
        auto parsed = parse_invocation_args<WriteFileArgs>(invocation);
        if (!parsed || parsed->path.empty()) {
            co_return error_result("invalid write arguments");
        }
        if (auto environment = env(); !environment) {
            co_return std::unexpected(environment.error());
        }
        auto written = co_await env_->writeFile(parsed->path, parsed->content, stop_token);
        if (!written) {
            co_return error_result_from(written.error());
        }
        co_return agent::AsyncToolExecutionResult{
            .content = std::vector<ai::Content>{
                ai::text_content("wrote " + std::to_string(parsed->content.size()) + " bytes")},
            .details = std::nullopt,
        };
    }
};

class AsyncEditTool final : public AsyncToolBase {
public:
    using AsyncToolBase::AsyncToolBase;

    // pi `core/tools/edit.ts` promptSnippet/promptGuidelines (verbatim): the
    // snippet plus the four edit guidelines.
    [[nodiscard]] std::optional<std::string> prompt_snippet() const override {
        return "Make precise file edits with exact text replacement, including "
               "multiple disjoint edits in one call";
    }
    [[nodiscard]] std::vector<std::string> prompt_guidelines() const override {
        return {
            "Use edit for precise changes (edits[].oldText must match exactly)",
            "When changing multiple separate locations in one file, use one "
            "edit call with multiple entries in edits[] instead of multiple "
            "edit calls",
            "Each edits[].oldText is matched against the original file, not "
            "after earlier edits are applied. Do not emit overlapping or "
            "nested edits. Merge nearby changes into one edit.",
            "Keep edits[].oldText as small as possible while still being "
            "unique in the file. Do not pad with large unchanged regions.",
        };
    }

    const ai::Tool& definition() const override {
        static const auto edit_entry_schema = object_schema(
            {
                {"oldText", typed_schema("string",
                    "Exact text for one targeted replacement. It must be unique in the original file "
                    "and must not overlap with any other edits[].oldText in the same call.")},
                {"newText", typed_schema("string", "Replacement text for this targeted edit.")},
            },
            {"oldText", "newText"});
        static const auto edits_schema = [] {
            auto schema = array_schema(edit_entry_schema,
                "One or more targeted replacements. Each edit is matched against the original file, "
                "not incrementally. Do not include overlapping or nested edits. If two changes touch "
                "the same block or nearby lines, merge them into one edit instead.");
            // Execution requires at least one replacement, so the contract does too.
            schema.get_object().emplace("minItems", 1);
            return schema;
        }();
        static const ai::Tool tool{
            "edit",
            "Edit a single file using exact text replacement. Every edits[].oldText must match a "
            "unique, non-overlapping region of the original file. If two changes affect the same "
            "block or nearby lines, merge them into one edit instead of emitting overlapping edits. "
            "Do not include large unchanged regions just to connect distant changes.",
            object_schema(
                {
                    {"path", typed_schema("string", "Path to the file to edit (relative or absolute)")},
                    {"edits", edits_schema},
                },
                {"path", "edits"}),
        };
        return tool;
    }

    boost::asio::awaitable<util::Expected<agent::AsyncToolExecutionResult>> execute(
        agent::ToolInvocation invocation,
        std::stop_token stop_token) override {
        auto parsed = parse_invocation_args<EditArgs>(invocation);
        if (!parsed || parsed->path.empty()) {
            co_return error_result("invalid edit arguments: missing path");
        }
        if (parsed->edits.empty()) {
            co_return error_result("Edit tool input is invalid. edits must contain at least one replacement.");
        }
        if (auto environment = env(); !environment) {
            co_return std::unexpected(environment.error());
        }

        // pi edit.ts: strip the BOM, detect and preserve the dominant line
        // ending, then apply every edit against the LF-normalized content.
        auto read = co_await env_->readTextFile(parsed->path, stop_token);
        if (!read) {
            co_return error_result_from(read.error());
        }
        const auto [bom, text] = tools::strip_bom(*read);
        const auto original_ending = tools::detect_line_ending(text);
        const auto normalized_content = tools::normalize_to_lf(text);
        std::vector<tools::EditReplacement> replacements;
        replacements.reserve(parsed->edits.size());
        for (const auto& edit : parsed->edits) {
            replacements.push_back(tools::EditReplacement{
                .old_text = edit.oldText,
                .new_text = edit.newText,
            });
        }
        auto applied = tools::apply_edits_to_normalized_content(
            normalized_content, replacements, parsed->path);
        if (!applied) {
            co_return error_result(applied.error().message);
        }

        const std::string final_content =
            bom + tools::restore_line_endings(applied->new_content, original_ending);
        auto written = co_await env_->writeFile(parsed->path, final_content, stop_token);
        if (!written) {
            co_return error_result_from(written.error());
        }

        // pi edit.ts details: the display diff, a unified patch, and the first
        // changed new-file line number.
        const auto diff_result = tools::generate_diff_string(
            applied->base_content, applied->new_content);
        const std::string patch = tools::generate_unified_patch(
            parsed->path, applied->base_content, applied->new_content);
        util::JsonValue details{util::JsonValue::object_t{}};
        auto& detail_object = details.get_object();
        detail_object.emplace("diff", util::JsonValue(diff_result.diff));
        detail_object.emplace("patch", util::JsonValue(patch));
        if (diff_result.first_changed_line) {
            detail_object.emplace(
                "firstChangedLine",
                util::JsonValue(*diff_result.first_changed_line));
        }
        co_return agent::AsyncToolExecutionResult{
            .content = std::vector<ai::Content>{ai::text_content(
                "Successfully replaced " + std::to_string(parsed->edits.size()) +
                " block(s) in " + parsed->path + ".")},
            .details = std::move(details),
        };
    }
};

class AsyncBashTool final : public AsyncToolBase {
public:
    AsyncBashTool(
        std::shared_ptr<harness::AsyncExecutionEnv> env,
        std::shared_ptr<BashSessionEnvironment> session_environment = {})
        : AsyncToolBase(std::move(env)),
          session_environment_(std::move(session_environment)) {}

    // pi `core/tools/bash.ts` promptSnippet/promptGuidelines (verbatim): the
    // guideline is the PI_* environment-exposure note, supplied exactly like
    // pi's `exposeSessionEnvironment ? [...] : undefined` — only when the
    // session facts holder is wired in.
    [[nodiscard]] std::optional<std::string> prompt_snippet() const override {
        return "Execute bash commands (ls, grep, find, etc.)";
    }
    [[nodiscard]] std::vector<std::string> prompt_guidelines() const override {
        if (!session_environment_) {
            return {};
        }
        return {
            "Inspect PI_* environment variables for current model and session "
            "details.",
        };
    }

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
        agent::ToolInvocation invocation,
        std::stop_token stop_token) override {
        auto parsed = parse_invocation_args<BashArgs>(invocation);
        if (!parsed || parsed->command.empty()) {
            co_return error_result("invalid bash arguments");
        }
        if (auto environment = env(); !environment) {
            co_return std::unexpected(environment.error());
        }
        // Convert seconds to milliseconds for ExecutionEnv; zero means no timeout.
        harness::ExecOptions exec_options;
        exec_options.stop_token = stop_token;
        exec_options.timeout = parsed->timeout
            ? std::chrono::milliseconds(std::chrono::seconds(*parsed->timeout))
            : std::chrono::milliseconds{0};
        // pi `resolveSpawnContext` (`core/tools/bash.ts`): delete the five
        // PI_* variables from the inherited environment, then re-set the ones
        // present in the live session context. The execution environment
        // shadows base keys through the override map; absent facts shadow
        // with an empty value (the closest analogue of pi's delete).
        if (session_environment_) {
            const auto& session = *session_environment_;
            std::map<std::string, std::string> pi_environment;
            pi_environment["PI_SESSION_ID"] = session.session_id;
            pi_environment["PI_SESSION_FILE"] =
                session.session_file.value_or("");
            pi_environment["PI_PROVIDER"] = session.provider;
            pi_environment["PI_MODEL"] = session.model;
            pi_environment["PI_REASONING_LEVEL"] =
                session.reasoning_level.value_or("");
            exec_options.env = std::move(pi_environment);
        }
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
        auto shell = co_await env_->exec(parsed->command, std::move(exec_options));
        if (!shell) {
            co_return error_result_from(shell.error());
        }

        // Streamed callbacks carry pre-truncation output. When the environment
        // never fires them, the result fields are already capped at the
        // execution layer and no complete output exists to spill.
        const bool streamed = received_stdout || received_stderr;
        const std::string& stdout_source = streamed ? full_stdout : shell->stdout_output;
        const std::string& stderr_source = streamed ? full_stderr : shell->stderr_output;
        std::string full_output = util::strip_terminal_escape_sequences(
            combine_output(stdout_source, stderr_source));

        // Redact the complete output before splitting between model-visible and spill.
        std::string redacted_full = util::redact_text(full_output);
        auto limited_output = util::limit_output_tail(redacted_full);
        bool truncated = limited_output.truncated;
        std::string output = std::move(limited_output.text);
        if (truncated && streamed) {
            std::string full_output_path;
            auto ts = std::chrono::system_clock::now().time_since_epoch().count();
            full_output_path = "bash-output-" + std::to_string(ts) + ".txt";
            if (auto write = co_await env_->writeFile(full_output_path, redacted_full, stop_token); !write) {
                full_output_path.clear();
            }
            output = "[output truncated, showing last " +
                std::to_string(output.size()) +
                " bytes]" + (!full_output_path.empty() ? " full output: " + full_output_path : "") +
                "\n" + output;
        } else if (truncated) {
            output = "[output capped at execution layer, showing last " +
                std::to_string(output.size()) + " bytes]\n" + output;
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
            .content = std::vector<ai::Content>{ai::text_content(out.str())},
            .details = std::nullopt,
            .is_error = shell->exitCode != 0,
        };
    }

    std::shared_ptr<BashSessionEnvironment> session_environment_;
};

} // namespace

std::unique_ptr<agent::AsyncAgentTool> make_async_read_file_tool(std::shared_ptr<harness::AsyncExecutionEnv> env) {
    return std::make_unique<AsyncReadFileTool>(std::move(env));
}

std::unique_ptr<agent::AsyncAgentTool> make_async_write_file_tool(std::shared_ptr<harness::AsyncExecutionEnv> env) {
    return std::make_unique<AsyncWriteFileTool>(std::move(env));
}

std::unique_ptr<agent::AsyncAgentTool> make_async_edit_tool(std::shared_ptr<harness::AsyncExecutionEnv> env) {
    return std::make_unique<AsyncEditTool>(std::move(env));
}

std::unique_ptr<agent::AsyncAgentTool> make_async_bash_tool(
    std::shared_ptr<harness::AsyncExecutionEnv> env,
    std::shared_ptr<BashSessionEnvironment> session_environment) {
    return std::make_unique<AsyncBashTool>(
        std::move(env), std::move(session_environment));
}

} // namespace cch::tools
