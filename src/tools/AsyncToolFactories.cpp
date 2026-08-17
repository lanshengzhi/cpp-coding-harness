#include <cch/agent/tools/ToolFactories.hpp>

#include "tools/EditDiff.hpp"
#include "ai/AsyncResultBridge.hpp"
#include "ai/BoundedText.hpp"
#include "support/Json.hpp"
#include "support/JsonGlaze.hpp"
#include "harness/OutputLimiter.hpp"
#include "tools/TerminalText.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>

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

[[nodiscard]] support::JsonValue typed_schema(
    std::string type,
    std::optional<std::string> description = std::nullopt) {
    support::JsonValue::object_t schema{{"type", std::move(type)}};
    if (description) {
        schema.emplace("description", std::move(*description));
    }
    return schema;
}

[[nodiscard]] support::JsonValue object_schema(
    support::JsonValue::object_t properties,
    std::vector<std::string> required) {
    support::JsonValue::array_t required_values;
    required_values.reserve(required.size());
    for (auto& name : required) {
        required_values.emplace_back(std::move(name));
    }
    return support::JsonValue::object_t{
        {"type", "object"},
        {"properties", std::move(properties)},
        {"required", std::move(required_values)},
        {"additionalProperties", false},
    };
}

[[nodiscard]] support::JsonValue array_schema(
    support::JsonValue items,
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
[[nodiscard]] support::Expected<Args> parse_invocation_args(const agent::ToolInvocation& invocation) {
    auto serialized = support::write_json(invocation.arguments);
    if (!serialized) {
        return std::unexpected(serialized.error());
    }
    return support::read_json<Args>(*serialized);
}

[[nodiscard]] support::Error missing_env_error() {
    return support::make_error(support::ErrorCode::Tool, "missing execution environment");
}

/// Build a pending `AsyncResult` whose producer runs `body` (a fresh
/// awaitable) on the consuming coroutine's executor (the Agent loop's
/// serialized domain) and bridges its terminal outcome. The executor is read
/// from the initiating-executor thread-local the `await_async_result` bridge
/// publishes during producer initiation (ADR 0040).
template <typename Body>
[[nodiscard]] agent::ToolExecuteResult make_tool_result(Body body) {
    return agent::ToolExecuteResult{
        [body = std::move(body)](
            agent::ToolExecuteResult::completion_type completion) mutable noexcept {
            auto executor = ai::detail::t_initiating_executor;
            if (!executor) {
                completion(std::unexpected(support::make_error(
                    support::ErrorCode::Tool, "tool execution has no initiating executor")));
                return;
            }
            try {
                boost::asio::co_spawn(
                    executor,
                    body(),
                    [completion = std::move(completion)](
                        std::exception_ptr eptr,
                        support::Expected<agent::AsyncToolExecutionResult> result) mutable noexcept {
                        if (eptr) {
                            completion(std::unexpected(support::make_error(
                                support::ErrorCode::Tool, "tool execution failed")));
                        } else {
                            completion(std::move(result));
                        }
                    });
            } catch (...) {
                completion(std::unexpected(support::make_error(
                    support::ErrorCode::Tool, "tool execution failed")));
            }
        }};
}

// ---------------------------------------------------------------------------
// Built-in tool coroutine bodies (pi `core/tools/*.ts`).
// ---------------------------------------------------------------------------

boost::asio::awaitable<support::Expected<agent::AsyncToolExecutionResult>> read_file_execute(
    std::shared_ptr<harness::AsyncExecutionEnv> env,
    agent::ToolInvocation invocation,
    std::stop_token stop_token) {
    auto parsed = parse_invocation_args<ReadFileArgs>(invocation);
    if (!parsed || parsed->path.empty()) {
        co_return error_result("invalid read arguments");
    }
    if (!env) {
        co_return std::unexpected(missing_env_error());
    }
    auto lines = co_await ai::detail::await_async_result(
        env->readTextLines(parsed->path, std::nullopt, stop_token));
    if (!lines) {
        co_return error_result_from(lines.error());
    }
    // offset is 1-based; limit 0 means no explicit limit.
    const auto offset = std::max(1, parsed->offset);
    const harness::OutputLimit output_limit;
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

boost::asio::awaitable<support::Expected<agent::AsyncToolExecutionResult>> write_file_execute(
    std::shared_ptr<harness::AsyncExecutionEnv> env,
    agent::ToolInvocation invocation,
    std::stop_token stop_token) {
    auto parsed = parse_invocation_args<WriteFileArgs>(invocation);
    if (!parsed || parsed->path.empty()) {
        co_return error_result("invalid write arguments");
    }
    if (!env) {
        co_return std::unexpected(missing_env_error());
    }
    auto written = co_await ai::detail::await_async_result(
        env->writeFile(parsed->path, parsed->content, stop_token));
    if (!written) {
        co_return error_result_from(written.error());
    }
    co_return agent::AsyncToolExecutionResult{
        .content = std::vector<ai::Content>{
            ai::text_content("wrote " + std::to_string(parsed->content.size()) + " bytes")},
        .details = std::nullopt,
    };
}

boost::asio::awaitable<support::Expected<agent::AsyncToolExecutionResult>> edit_execute(
    std::shared_ptr<harness::AsyncExecutionEnv> env,
    agent::ToolInvocation invocation,
    std::stop_token stop_token) {
    auto parsed = parse_invocation_args<EditArgs>(invocation);
    if (!parsed || parsed->path.empty()) {
        co_return error_result("invalid edit arguments: missing path");
    }
    if (parsed->edits.empty()) {
        co_return error_result("Edit tool input is invalid. edits must contain at least one replacement.");
    }
    if (!env) {
        co_return std::unexpected(missing_env_error());
    }

    // pi edit.ts: strip the BOM, detect and preserve the dominant line
    // ending, then apply every edit against the LF-normalized content.
    auto read = co_await ai::detail::await_async_result(
        env->readTextFile(parsed->path, stop_token));
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
    auto written = co_await ai::detail::await_async_result(
        env->writeFile(parsed->path, final_content, stop_token));
    if (!written) {
        co_return error_result_from(written.error());
    }

    // pi edit.ts details: the display diff, a unified patch, and the first
    // changed new-file line number.
    const auto diff_result = tools::generate_diff_string(
        applied->base_content, applied->new_content);
    const std::string patch = tools::generate_unified_patch(
        parsed->path, applied->base_content, applied->new_content);
    support::JsonValue details{support::JsonValue::object_t{}};
    auto& detail_object = details.get_object();
    detail_object.emplace("diff", support::JsonValue(diff_result.diff));
    detail_object.emplace("patch", support::JsonValue(patch));
    if (diff_result.first_changed_line) {
        detail_object.emplace(
            "firstChangedLine",
            support::JsonValue(*diff_result.first_changed_line));
    }
    co_return agent::AsyncToolExecutionResult{
        .content = std::vector<ai::Content>{ai::text_content(
            "Successfully replaced " + std::to_string(parsed->edits.size()) +
            " block(s) in " + parsed->path + ".")},
        .details = std::move(details),
    };
}

boost::asio::awaitable<support::Expected<agent::AsyncToolExecutionResult>> bash_execute(
    std::shared_ptr<harness::AsyncExecutionEnv> env,
    std::shared_ptr<BashSessionEnvironment> session_environment,
    agent::ToolInvocation invocation,
    std::stop_token stop_token) {
    auto parsed = parse_invocation_args<BashArgs>(invocation);
    if (!parsed || parsed->command.empty()) {
        co_return error_result("invalid bash arguments");
    }
    if (!env) {
        co_return std::unexpected(missing_env_error());
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
    if (session_environment) {
        const auto& session = *session_environment;
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
    // The environment owns these callbacks only until the awaited exec
    // completes; the references point into this coroutine frame.
    exec_options.onStdout = [&](std::string_view chunk) -> support::ExpectedVoid {
        received_stdout = true;
        full_stdout.append(chunk);
        return {};
    };
    exec_options.onStderr = [&](std::string_view chunk) -> support::ExpectedVoid {
        received_stderr = true;
        full_stderr.append(chunk);
        return {};
    };
    auto shell = co_await ai::detail::await_async_result(
        env->exec(parsed->command, std::move(exec_options)));
    if (!shell) {
        co_return error_result_from(shell.error());
    }

    // Streamed callbacks carry pre-truncation output. When the environment
    // never fires them, the result fields are already capped at the
    // execution layer and no complete output exists to spill.
    const bool streamed = received_stdout || received_stderr;
    const std::string& stdout_source = streamed ? full_stdout : shell->stdout_output;
    const std::string& stderr_source = streamed ? full_stderr : shell->stderr_output;
    std::string full_output = tools::strip_terminal_escape_sequences(
        combine_output(stdout_source, stderr_source));

    // Redact the complete output before splitting between model-visible and spill.
    std::string redacted_full = ai::redact_text(full_output);
    auto limited_output = harness::limit_output_tail(redacted_full);
    bool truncated = limited_output.truncated;
    std::string output = std::move(limited_output.text);
    if (truncated && streamed) {
        std::string full_output_path;
        auto ts = std::chrono::system_clock::now().time_since_epoch().count();
        full_output_path = "bash-output-" + std::to_string(ts) + ".txt";
        if (auto write = co_await ai::detail::await_async_result(
                env->writeFile(full_output_path, redacted_full, stop_token)); !write) {
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

} // namespace

agent::Tool make_async_read_file_tool(std::shared_ptr<harness::AsyncExecutionEnv> env) {
    agent::Tool tool;
    tool.definition = ai::Tool{
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
    // pi `core/tools/read.ts` promptSnippet/promptGuidelines (verbatim).
    tool.prompt_snippet = "Read file contents";
    tool.prompt_guidelines = {"Use read to examine files instead of cat or sed."};
    tool.execute = [env](
        agent::ToolInvocation invocation,
        std::stop_token stop_token,
        agent::ToolUpdateSink) -> agent::ToolExecuteResult {
        return make_tool_result(
            [env, invocation = std::move(invocation), stop_token]() {
                return read_file_execute(env, std::move(invocation), stop_token);
            });
    };
    return tool;
}

agent::Tool make_async_write_file_tool(std::shared_ptr<harness::AsyncExecutionEnv> env) {
    agent::Tool tool;
    tool.definition = ai::Tool{
        "write",
        "Create or overwrite a text file inside the workspace. Parent directories are created automatically.",
        object_schema(
            {
                {"path", typed_schema("string", "Workspace-relative file path")},
                {"content", typed_schema("string", "File content")},
            },
            {"path", "content"}),
    };
    // pi `core/tools/write.ts` promptSnippet/promptGuidelines (verbatim).
    tool.prompt_snippet = "Create or overwrite files";
    tool.prompt_guidelines = {"Use write only for new files or complete rewrites."};
    tool.execute = [env](
        agent::ToolInvocation invocation,
        std::stop_token stop_token,
        agent::ToolUpdateSink) -> agent::ToolExecuteResult {
        return make_tool_result(
            [env, invocation = std::move(invocation), stop_token]() {
                return write_file_execute(env, std::move(invocation), stop_token);
            });
    };
    return tool;
}

agent::Tool make_async_edit_tool(std::shared_ptr<harness::AsyncExecutionEnv> env) {
    agent::Tool tool;
    const auto edit_entry_schema = object_schema(
        {
            {"oldText", typed_schema("string",
                "Exact text for one targeted replacement. It must be unique in the original file "
                "and must not overlap with any other edits[].oldText in the same call.")},
            {"newText", typed_schema("string", "Replacement text for this targeted edit.")},
        },
        {"oldText", "newText"});
    const auto edits_schema = [&] {
        auto schema = array_schema(
            edit_entry_schema,
            "One or more targeted replacements. Each edit is matched against the original file, "
            "not incrementally. Do not include overlapping or nested edits. If two changes touch "
            "the same block or nearby lines, merge them into one edit instead.");
        // Execution requires at least one replacement, so the contract does too.
        schema.get_object().emplace("minItems", 1);
        return schema;
    }();
    tool.definition = ai::Tool{
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
    // pi `core/tools/edit.ts` promptSnippet/promptGuidelines (verbatim): the
    // snippet plus the four edit guidelines.
    tool.prompt_snippet = "Make precise file edits with exact text replacement, including "
                          "multiple disjoint edits in one call";
    tool.prompt_guidelines = {
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
    tool.execute = [env](
        agent::ToolInvocation invocation,
        std::stop_token stop_token,
        agent::ToolUpdateSink) -> agent::ToolExecuteResult {
        return make_tool_result(
            [env, invocation = std::move(invocation), stop_token]() {
                return edit_execute(env, std::move(invocation), stop_token);
            });
    };
    return tool;
}

agent::Tool make_async_bash_tool(
    std::shared_ptr<harness::AsyncExecutionEnv> env,
    std::shared_ptr<BashSessionEnvironment> session_environment) {
    agent::Tool tool;
    tool.definition = ai::Tool{
        "bash",
        "Run a shell command in the workspace when explicitly enabled",
        object_schema(
            {
                {"command", typed_schema("string", "Shell command")},
                {"timeout", typed_schema("integer", "Timeout in seconds (optional)")},
            },
            {"command"}),
    };
    // pi `core/tools/bash.ts` promptSnippet/promptGuidelines (verbatim): the
    // guideline is the PI_* environment-exposure note, supplied exactly like
    // pi's `exposeSessionEnvironment ? [...] : undefined` — only when the
    // session facts holder is wired in.
    tool.prompt_snippet = "Execute bash commands (ls, grep, find, etc.)";
    if (session_environment) {
        tool.prompt_guidelines = {
            "Inspect PI_* environment variables for current model and session "
            "details.",
        };
    }
    tool.execute = [env, session_environment](
        agent::ToolInvocation invocation,
        std::stop_token stop_token,
        agent::ToolUpdateSink) -> agent::ToolExecuteResult {
        return make_tool_result(
            [env, session_environment, invocation = std::move(invocation), stop_token]() {
                return bash_execute(env, session_environment, std::move(invocation), stop_token);
            });
    };
    return tool;
}

} // namespace cch::tools
