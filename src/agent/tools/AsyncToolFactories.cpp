#include <cch/agent/tools/ToolFactories.hpp>

#include "agent/tools/EditDiff.hpp"
#include "support/AsyncResultBridge.hpp"
#include "support/BoundedText.hpp"
#include "support/Json.hpp"
#include "support/JsonGlaze.hpp"
#include "agent/harness/OutputLimiter.hpp"
#include "agent/harness/session/RandomHex.hpp"
#include "agent/tools/TerminalText.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>

namespace cch::tools {
// The tool-argument DTOs must have external linkage: Glaze reflects them
// through glz::detail::external, which Clang rejects for anonymous-namespace
// types (issue #487; the Clang conformance build).
namespace detail {

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

} // namespace detail

namespace {

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

[[nodiscard]] support::Error missing_capability_error() {
    return support::make_error(support::ErrorCode::Tool, "missing capability");
}

[[nodiscard]] const char* truncation_kind_name(harness::OutputTruncationKind kind) {
    return kind == harness::OutputTruncationKind::Lines ? "lines" : "bytes";
}

/// pi's `ReadToolDetails` / `BashToolDetails` carry the whole `TruncationResult`
/// minus `content`, which stays the model-facing text (pi `read.ts:27-29`,
/// `bash.ts:50-53`, `truncate.ts:15-38`).
[[nodiscard]] support::JsonValue truncation_details(const harness::OutputTruncation& truncation) {
    const auto number = [](std::size_t value) { return support::JsonValue(static_cast<double>(value)); };
    support::JsonValue::object_t object{
            {"truncated", support::JsonValue(truncation.truncated)},
            {"truncatedBy",
                    truncation.truncated_by ? support::JsonValue(truncation_kind_name(*truncation.truncated_by))
                                            : support::JsonValue(nullptr)},
            {"totalLines", number(truncation.total_lines)},
            {"totalBytes", number(truncation.total_bytes)},
            {"outputLines", number(truncation.output_lines)},
            {"outputBytes", number(truncation.output_bytes)},
            {"lastLinePartial", support::JsonValue(truncation.last_line_partial)},
            {"firstLineExceedsLimit", support::JsonValue(truncation.first_line_exceeds_limit)},
            {"maxLines", number(truncation.max_lines)},
            {"maxBytes", number(truncation.max_bytes)},
    };
    return support::JsonValue{std::move(object)};
}

/// pi `appendStatus` (`bash.ts:341`): a blank line separates the output from
/// the status, and an empty output leaves the status standing alone.
[[nodiscard]] std::string append_status(const std::string& text, const std::string& status) {
    return text.empty() ? status : text + "\n\n" + status;
}

/// The model-facing outcome of one `bash` execution: the clean output the model
/// reads, plus the truncation facts and the spill path when output was dropped.
struct BashOutput {
    std::string text;
    std::optional<harness::OutputTruncation> truncation;
    std::optional<std::string> full_output_path;
};

/// pi's `OutputAccumulator` spill path (`output-accumulator.ts:19-22`):
/// `<tmpdir>/<prefix>-<unique-id>.log`. The prefix is pike's own, matching the
/// User Bash spill artifact (ADR 0061), not pi's `pi-bash`.
[[nodiscard]] std::optional<std::string> bash_spill_path() {
    std::error_code error;
    const auto directory = std::filesystem::temp_directory_path(error);
    if (error) {
        return std::nullopt;
    }
    return (directory / ("cch-bash-" + harness::session::random_hex_id(8) + ".log")).string();
}

/// pi `formatOutput` (`bash.ts:321-339`): the truncated tail plus one
/// `\n\n`-separated summary line naming the spill file, and an `(no output)`
/// default the failure paths suppress. Every branch produces the whole text, so
/// a test asserting one branch can never pass on an empty result.
boost::asio::awaitable<support::Expected<BashOutput>> format_bash_output(
        std::shared_ptr<harness::AsyncFileSystem> filesystem,
        std::string redacted_full_output,
        std::string empty_text,
        std::stop_token stop_token) {
    const harness::OutputLimit output_limit;
    const auto truncation = harness::truncate_output_tail(redacted_full_output, output_limit);
    // pi `output.getLastLineBytes()`: the byte size of the final line of the
    // complete output, needed before the text is moved into the spill write.
    const auto last_newline = redacted_full_output.rfind('\n');
    const auto last_line_bytes = last_newline == std::string::npos ? redacted_full_output.size()
                                                                   : redacted_full_output.size() - last_newline - 1;
    BashOutput output{
            .text = truncation.text.empty() ? std::move(empty_text) : truncation.text,
            .truncation = std::nullopt,
            .full_output_path = std::nullopt,
    };
    if (!truncation.truncated) {
        co_return output;
    }

    // The complete redacted output goes to the OS temp directory; the model is
    // told the same path `details.fullOutputPath` carries.
    const auto spill_path = bash_spill_path();
    if (!spill_path) {
        co_return std::unexpected(
                support::make_error(support::ErrorCode::Workspace, "bash spill temporary directory is unavailable"));
    }
    if (auto written = co_await support::detail::await_async_result(
                filesystem->writeFile(*spill_path, std::move(redacted_full_output), stop_token));
            !written) {
        // pi closes the spill file inside `finishOutput` and a write failure
        // rejects the tool, so a missing "Full output" promise is never sent.
        co_return std::unexpected(harness::to_util_error(written.error()));
    }

    const auto total_lines = truncation.total_lines;
    const auto end_line = total_lines;
    const auto start_line = total_lines - truncation.output_lines + 1;
    const auto to_line = [](std::size_t line) { return std::to_string(line); };
    if (truncation.last_line_partial) {
        output.text += "\n\n[Showing last " + harness::format_output_size(truncation.output_bytes) + " of line " +
                       to_line(end_line) + " (line is " + harness::format_output_size(last_line_bytes) +
                       "). Full output: " + *spill_path + "]";
    } else if (truncation.truncated_by == harness::OutputTruncationKind::Lines) {
        output.text += "\n\n[Showing lines " + to_line(start_line) + "-" + to_line(end_line) + " of " +
                       to_line(total_lines) + ". Full output: " + *spill_path + "]";
    } else {
        output.text += "\n\n[Showing lines " + to_line(start_line) + "-" + to_line(end_line) + " of " +
                       to_line(total_lines) + " (" + harness::format_output_size(output_limit.max_bytes) +
                       " limit). Full output: " + *spill_path + "]";
    }
    output.truncation = truncation;
    output.full_output_path = *spill_path;
    co_return output;
}

/// Build a pending `AsyncResult` whose producer runs `body` (a fresh
/// awaitable) on the consuming coroutine's executor (the Agent loop's
/// serialized domain) and bridges its terminal outcome. The executor is read
/// from the initiating-executor thread-local the `await_async_result` bridge
/// publishes during producer initiation (ADR 0040); the coroutine runs on the
/// private completion bridge so its setup and body outcomes stay on the
/// typed `Expected` channel without an exception path.
template <typename Body>
[[nodiscard]] agent::ToolExecuteResult make_tool_result(Body body) {
    return agent::ToolExecuteResult{[body = std::move(body)](
                                            agent::ToolExecuteResult::completion_type completion) mutable noexcept {
        auto executor = support::detail::t_initiating_executor;
        if (!executor) {
            completion(std::unexpected(
                    support::make_error(support::ErrorCode::Tool, "tool execution has no initiating executor")));
            return;
        }
        auto bridged = support::detail::make_async_result_on(executor,
                [body = std::move(
                         body)]() -> boost::asio::awaitable<support::Expected<agent::AsyncToolExecutionResult>> {
                    co_return co_await body();
                });
        std::move(bridged).start(std::move(completion));
    }};
}

// ---------------------------------------------------------------------------
// Built-in tool coroutine bodies (pi `core/tools/*.ts`).
// ---------------------------------------------------------------------------

boost::asio::awaitable<support::Expected<agent::AsyncToolExecutionResult>> read_file_execute(
        std::shared_ptr<harness::AsyncFileSystem> filesystem,
        agent::ToolInvocation invocation,
        std::stop_token stop_token) {
    auto parsed = parse_invocation_args<detail::ReadFileArgs>(invocation);
    if (!parsed || parsed->path.empty()) {
        co_return error_result("invalid read arguments");
    }
    if (!filesystem) {
        co_return std::unexpected(missing_capability_error());
    }
    // pi `read.ts:132-137` reads the file and splits the whole text on "\n", so
    // the line count (and the out-of-range `offset` count) covers the trailing
    // empty line a newline-terminated file ends with.
    auto text = co_await support::detail::await_async_result(filesystem->readTextFile(parsed->path, stop_token));
    if (!text) {
        co_return error_result_from(text.error());
    }
    const auto all_lines = harness::split_lines(*text);
    const std::size_t total_file_lines = all_lines.size();

    // pi `read.ts:139-142`: offset is 1-indexed on input, 0-indexed on the
    // buffer, and an offset past the end is an error.
    const int start_line = std::max(0, parsed->offset - 1);
    if (static_cast<std::size_t>(start_line) >= total_file_lines) {
        co_return error_result("Offset " + std::to_string(parsed->offset) + " is beyond end of file (" +
                               std::to_string(total_file_lines) + " lines total)");
    }
    const auto start_line_display = static_cast<std::size_t>(start_line) + 1;

    // pi `read.ts:143-154`: a user limit narrows the slice before truncation.
    std::string selected;
    std::optional<std::size_t> user_limited_lines;
    if (parsed->limit > 0) {
        const auto end_line = std::min(
                static_cast<std::size_t>(start_line) + static_cast<std::size_t>(parsed->limit), total_file_lines);
        user_limited_lines = end_line - static_cast<std::size_t>(start_line);
    } else {
        user_limited_lines = std::nullopt;
    }
    const std::size_t selected_end =
            user_limited_lines ? static_cast<std::size_t>(start_line) + *user_limited_lines : total_file_lines;
    for (std::size_t index = static_cast<std::size_t>(start_line); index < selected_end; ++index) {
        if (index > static_cast<std::size_t>(start_line)) {
            selected += '\n';
        }
        selected.append(all_lines[index]);
    }

    const harness::OutputLimit output_limit;
    const auto truncation = harness::truncate_output_head(selected, output_limit);
    const auto max_bytes_size = harness::format_output_size(output_limit.max_bytes);
    std::string content = truncation.text;
    std::optional<harness::OutputTruncation> details_truncation;
    if (truncation.first_line_exceeds_limit) {
        // pi `read.ts:158-162`: the notice is the whole content, and the
        // suggested command carries the model-supplied path and the numeric cap.
        const auto first_line_size =
                harness::format_output_size(all_lines[static_cast<std::size_t>(start_line)].size());
        content = "[Line " + std::to_string(start_line_display) + " is " + first_line_size + ", exceeds " +
                  max_bytes_size + " limit. Use bash: sed -n '" + std::to_string(start_line_display) + "p' " +
                  parsed->path + " | head -c " + std::to_string(output_limit.max_bytes) + "]";
        details_truncation = truncation;
    } else if (truncation.truncated) {
        // pi `read.ts:163-173`.
        const auto end_line_display = start_line_display + truncation.output_lines - 1;
        const auto next_offset = end_line_display + 1;
        content += "\n\n[Showing lines " + std::to_string(start_line_display) + "-" + std::to_string(end_line_display) +
                   " of " + std::to_string(total_file_lines);
        if (truncation.truncated_by != harness::OutputTruncationKind::Lines) {
            content += " (" + max_bytes_size + " limit)";
        }
        content += ". Use offset=" + std::to_string(next_offset) + " to continue.]";
        details_truncation = truncation;
    } else if (user_limited_lines && static_cast<std::size_t>(start_line) + *user_limited_lines < total_file_lines) {
        // pi `read.ts:174-178`: the limit stopped early but the file has more.
        // pi sets no `details` in this branch, so one here would be a defect.
        const auto consumed = static_cast<std::size_t>(start_line) + *user_limited_lines;
        content += "\n\n[" + std::to_string(total_file_lines - consumed) +
                   " more lines in file. Use offset=" + std::to_string(consumed + 1) + " to continue.]";
    }
    std::optional<support::JsonValue> details;
    if (details_truncation) {
        details = support::JsonValue{
                support::JsonValue::object_t{{"truncation", truncation_details(*details_truncation)}}};
    }
    co_return agent::AsyncToolExecutionResult{
            .content = std::vector<ai::Content>{ai::text_content(std::move(content))},
            .details = std::move(details),
    };
}

boost::asio::awaitable<support::Expected<agent::AsyncToolExecutionResult>> write_file_execute(
        std::shared_ptr<harness::AsyncFileSystem> filesystem,
        agent::ToolInvocation invocation,
        std::stop_token stop_token) {
    auto parsed = parse_invocation_args<detail::WriteFileArgs>(invocation);
    if (!parsed || parsed->path.empty()) {
        co_return error_result("invalid write arguments");
    }
    if (!filesystem) {
        co_return std::unexpected(missing_capability_error());
    }
    auto written = co_await support::detail::await_async_result(
            filesystem->writeFile(parsed->path, parsed->content, stop_token));
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
        std::shared_ptr<harness::AsyncFileSystem> filesystem,
        agent::ToolInvocation invocation,
        std::stop_token stop_token) {
    auto parsed = parse_invocation_args<detail::EditArgs>(invocation);
    if (!parsed || parsed->path.empty()) {
        co_return error_result("invalid edit arguments: missing path");
    }
    if (parsed->edits.empty()) {
        co_return error_result("Edit tool input is invalid. edits must contain at least one replacement.");
    }
    if (!filesystem) {
        co_return std::unexpected(missing_capability_error());
    }

    // pi edit.ts: strip the BOM, detect and preserve the dominant line
    // ending, then apply every edit against the LF-normalized content. The
    // read resolves through the same pi resolveToCwd path resolution as the
    // write below (ADR 0057), so edit can address any path writeFile can.
    auto read = co_await support::detail::await_async_result(filesystem->readTextFile(parsed->path, stop_token));
    if (!read) {
        co_return error_result_from(read.error());
    }
    // Bind through a named local: a structured binding that lifetime-extends
    // a temporary across the co_await suspensions below has been observed to
    // drop the string destructor on the coroutine frame under GCC 16
    // (LeakSanitizer finding on the ASan+UBSan lane, issue #522).
    const auto stripped = tools::strip_bom(*read);
    const auto& [bom, text] = stripped;
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
    auto written = co_await support::detail::await_async_result(
            filesystem->writeFile(parsed->path, final_content, stop_token));
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
        std::shared_ptr<harness::AsyncShell> shell,
        std::shared_ptr<harness::AsyncFileSystem> filesystem,
        std::shared_ptr<BashSessionEnvironment> session_environment,
        agent::ToolInvocation invocation,
        std::stop_token stop_token) {
    auto parsed = parse_invocation_args<detail::BashArgs>(invocation);
    if (!parsed || parsed->command.empty()) {
        co_return error_result("invalid bash arguments");
    }
    if (!shell || !filesystem) {
        co_return std::unexpected(missing_capability_error());
    }
    // Convert seconds to milliseconds for the Shell capability; zero means no timeout.
    harness::ExecOptions exec_options;
    exec_options.stop_token = stop_token;
    exec_options.timeout = parsed->timeout
        ? std::chrono::milliseconds(std::chrono::seconds(*parsed->timeout))
        : std::chrono::milliseconds{0};
    // pi `resolveSpawnContext` (`core/tools/bash.ts`): delete the five
    // PI_* variables from the inherited environment, then re-set the ones
    // present in the live session context. The Shell capability shadows base
    // keys through the override map; absent facts shadow with an empty value
    // (the closest analogue of pi's delete).
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
    // The Shell capability owns these callbacks only until the awaited exec
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
    auto shell_result =
            co_await support::detail::await_async_result(shell->exec(parsed->command, std::move(exec_options)));
    if (!shell_result) {
        // pi `bash.ts:352-366`: only the abort and timeout sentinels become pi's
        // status strings; every other execution error keeps its own text.
        const auto& error = shell_result.error();
        if (error.code != harness::ExecutionErrorCode::Aborted && error.code != harness::ExecutionErrorCode::Timeout) {
            co_return error_result_from(error);
        }
        // The output streamed before the stop is still the model's, but the
        // `(no output)` default is suppressed so the status stands alone.
        const auto partial = tools::strip_terminal_escape_sequences(combine_output(full_stdout, full_stderr));
        auto formatted = co_await format_bash_output(
                filesystem, support::redact_text(partial), std::string{}, std::stop_token{});
        if (!formatted) {
            co_return error_result_from(formatted.error());
        }
        // The execution error carries no seconds (pi recovers them from its
        // `timeout:<secs>` sentinel); the requested timeout is the only source.
        const std::string status =
                error.code == harness::ExecutionErrorCode::Aborted
                        ? "Command aborted"
                        : "Command timed out after " + std::to_string(parsed->timeout.value_or(0)) + " seconds";
        co_return error_result(append_status(formatted->text, status));
    }

    // Streamed callbacks carry pre-truncation output. When the Shell capability
    // never fires them, the result fields are the complete output it has.
    const bool streamed = received_stdout || received_stderr;
    const std::string& stdout_source = streamed ? full_stdout : shell_result->stdout_output;
    const std::string& stderr_source = streamed ? full_stderr : shell_result->stderr_output;
    const std::string full_output =
            tools::strip_terminal_escape_sequences(combine_output(stdout_source, stderr_source));

    // Redact the complete output before splitting between model-visible and spill.
    auto formatted =
            co_await format_bash_output(filesystem, support::redact_text(full_output), "(no output)", stop_token);
    if (!formatted) {
        co_return error_result_from(formatted.error());
    }

    // pi `bash.ts:367-373`: "no exit code" is decided before a non-zero exit.
    // The Shell capability reports an unknown child status as a negative code
    // (`harness::process_exit_code`), which is this tool's null exit code.
    if (shell_result->exitCode < 0) {
        co_return error_result(append_status(formatted->text, "Command terminated without an exit code"));
    }
    if (shell_result->exitCode != 0) {
        co_return error_result(
                append_status(formatted->text, "Command exited with code " + std::to_string(shell_result->exitCode)));
    }

    std::optional<support::JsonValue> details;
    if (formatted->truncation) {
        support::JsonValue value{support::JsonValue::object_t{
                {"truncation", truncation_details(*formatted->truncation)},
                {"fullOutputPath", support::JsonValue(*formatted->full_output_path)},
        }};
        details = std::move(value);
    }
    co_return agent::AsyncToolExecutionResult{
            .content = std::vector<ai::Content>{ai::text_content(std::move(formatted->text))},
            .details = std::move(details),
            .is_error = false,
    };
}

} // namespace

agent::Tool make_async_read_file_tool(std::shared_ptr<harness::AsyncFileSystem> filesystem) {
    agent::Tool tool;
    tool.definition = ai::Tool{
            "read",
            "Read a text file at a relative or absolute path",
            object_schema(
                    {
                            {"path", typed_schema("string", "Path to the file to read (relative or absolute)")},
                            {"offset", typed_schema("integer", "1-based line offset")},
                            {"limit", typed_schema("integer", "Maximum number of lines to read")},
                    },
                    {"path"}),
    };
    // pi `core/tools/read.ts` promptSnippet/promptGuidelines (verbatim).
    tool.prompt_snippet = "Read file contents";
    tool.prompt_guidelines = {"Use read to examine files instead of cat or sed."};
    tool.execute = [filesystem](agent::ToolInvocation invocation,
                           std::stop_token stop_token,
                           agent::ToolUpdateSink) -> agent::ToolExecuteResult {
        return make_tool_result([filesystem, invocation = std::move(invocation), stop_token]() {
            return read_file_execute(filesystem, std::move(invocation), stop_token);
        });
    };
    return tool;
}

agent::Tool make_async_write_file_tool(std::shared_ptr<harness::AsyncFileSystem> filesystem) {
    agent::Tool tool;
    tool.definition = ai::Tool{
            "write",
            // pi `core/tools/write.ts` description and schema wording (verbatim).
            "Write content to a file. Creates the file if it doesn't exist, overwrites if it does. "
            "Automatically creates parent directories.",
            object_schema(
                    {
                            {"path", typed_schema("string", "Path to the file to write (relative or absolute)")},
                            {"content", typed_schema("string", "Content to write to the file")},
                    },
                    {"path", "content"}),
    };
    // pi `core/tools/write.ts` promptSnippet/promptGuidelines (verbatim).
    tool.prompt_snippet = "Create or overwrite files";
    tool.prompt_guidelines = {"Use write only for new files or complete rewrites."};
    tool.execute = [filesystem](agent::ToolInvocation invocation,
                           std::stop_token stop_token,
                           agent::ToolUpdateSink) -> agent::ToolExecuteResult {
        return make_tool_result([filesystem, invocation = std::move(invocation), stop_token]() {
            return write_file_execute(filesystem, std::move(invocation), stop_token);
        });
    };
    return tool;
}

agent::Tool make_async_edit_tool(std::shared_ptr<harness::AsyncFileSystem> filesystem) {
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
    tool.execute = [filesystem](agent::ToolInvocation invocation,
                           std::stop_token stop_token,
                           agent::ToolUpdateSink) -> agent::ToolExecuteResult {
        return make_tool_result([filesystem, invocation = std::move(invocation), stop_token]() {
            return edit_execute(filesystem, std::move(invocation), stop_token);
        });
    };
    return tool;
}

agent::Tool make_async_bash_tool(std::shared_ptr<harness::AsyncShell> shell,
        std::shared_ptr<harness::AsyncFileSystem> filesystem,
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
    tool.execute = [shell, filesystem, session_environment](agent::ToolInvocation invocation,
                           std::stop_token stop_token,
                           agent::ToolUpdateSink) -> agent::ToolExecuteResult {
        return make_tool_result(
                [shell, filesystem, session_environment, invocation = std::move(invocation), stop_token]() {
                    return bash_execute(shell, filesystem, session_environment, std::move(invocation), stop_token);
                });
    };
    return tool;
}

} // namespace cch::tools
