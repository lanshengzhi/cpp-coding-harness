#include "../../third_party/catch2/catch_test_macros.hpp"

#include "../support/ShellQuoting.hpp"
#include "../support/TempWorkspace.hpp"
#include "../support/TextHelpers.hpp"

#include <cch/coding_agent/Sdk.hpp>
#include "coding_agent/runtime/AsyncCliRuntime.hpp"
#include "util/Json.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/wait.h>
#endif

#ifndef CCH_BINARY
#define CCH_BINARY "./cpp_harness"
#endif

namespace {

using cch::tests::count_occurrences;
using cch::tests::shell_quote;

struct CommandResult {
    int exit_code{0};
    std::string output;
};

struct SplitCommandResult {
    int exit_code{0};
    std::string stdout_text;
    std::string stderr_text;
};

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

CommandResult run_command(const std::string& command) {
    std::array<char, 256> buffer{};
    std::string output;
    FILE* pipe = popen((command + " 2>&1").c_str(), "r");
    if (pipe == nullptr) {
        return {127, "popen failed"};
    }
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        output += buffer.data();
    }
    int status = pclose(pipe);
#if defined(__unix__) || defined(__APPLE__)
    int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : status;
#else
    int exit_code = status;
#endif
    return {exit_code, output};
}

SplitCommandResult run_command_split(const std::string& command) {
    cch::tests::TempWorkspace capture;
    const auto stdout_path = capture.path() / "stdout.txt";
    const auto stderr_path = capture.path() / "stderr.txt";
    int status = std::system((command + " > '" + stdout_path.string() + "' 2> '" + stderr_path.string() + "'").c_str());
#if defined(__unix__) || defined(__APPLE__)
    int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : status;
#else
    int exit_code = status;
#endif
    return {exit_code, read_file(stdout_path), read_file(stderr_path)};
}

SplitCommandResult run_command_split_with_input(const std::string& command, const std::string& input) {
    cch::tests::TempWorkspace capture;
    const auto input_path = capture.path() / "stdin.jsonl";
    const auto stdout_path = capture.path() / "stdout.txt";
    const auto stderr_path = capture.path() / "stderr.txt";
    std::ofstream(input_path, std::ios::binary) << input;
    int status = std::system((command + " < '" + input_path.string() + "' > '" + stdout_path.string() + "' 2> '" + stderr_path.string() + "'").c_str());
#if defined(__unix__) || defined(__APPLE__)
    int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : status;
#else
    int exit_code = status;
#endif
    return {exit_code, read_file(stdout_path), read_file(stderr_path)};
}

std::string bin() { return shell_quote(CCH_BINARY); }

std::vector<std::string> non_empty_lines(const std::string& text) {
    std::vector<std::string> result;
    std::istringstream input{text};
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty()) {
            result.push_back(line);
        }
    }
    return result;
}

cch::util::JsonValue parse_json_line(const std::string& line) {
    auto parsed = cch::util::read_json<cch::util::JsonValue>(line);
    REQUIRE(parsed.has_value());
    return *parsed;
}

const cch::util::JsonValue::object_t& as_object(const cch::util::JsonValue& value) {
    return value.get<cch::util::JsonValue::object_t>();
}

std::string json_string_at(const cch::util::JsonValue::object_t& object, const std::string& key) {
    return object.at(key).get<std::string>();
}

bool has_json_event_type(const std::vector<std::string>& lines, const std::string& type) {
    for (const auto& line : lines) {
        if (json_string_at(as_object(parse_json_line(line)), "type") == type) {
            return true;
        }
    }
    return false;
}

std::vector<cch::util::JsonValue::object_t> parse_json_objects(const std::string& output) {
    std::vector<cch::util::JsonValue::object_t> records;
    for (const auto& line : non_empty_lines(output)) {
        records.push_back(as_object(parse_json_line(line)));
    }
    return records;
}

const cch::util::JsonValue::object_t* find_response(
    const std::vector<cch::util::JsonValue::object_t>& records,
    const std::string& command) {
    for (const auto& record : records) {
        if (json_string_at(record, "type") == "response" && json_string_at(record, "command") == command) {
            return &record;
        }
    }
    return nullptr;
}

// Black-box mirror of pi's readable workspace key: remove one leading root
// separator, replace path and drive separators with '-', frame with '--'.
// SessionPathPolicyTest pins the encoding rule at the pure seam; this helper
// only composes the expected black-box location for CLI smoke assertions.
std::string expected_workspace_key(const std::filesystem::path& canonical_workspace) {
    auto encoded = canonical_workspace.string();
    if (!encoded.empty() && (encoded.front() == '/' || encoded.front() == '\\')) {
        encoded.erase(encoded.begin());
    }
    for (auto& character : encoded) {
        if (character == '/' || character == '\\' || character == ':') {
            character = '-';
        }
    }
    return "--" + encoded + "--";
}

std::vector<std::filesystem::path> jsonl_files_under(const std::filesystem::path& root) {
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    if (!std::filesystem::exists(root, ec)) {
        return files;
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root, ec)) {
        if (entry.is_regular_file(ec) && entry.path().extension() == ".jsonl") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

const std::regex kAutomaticFilename{
    R"(^([0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}-[0-9]{2}-[0-9]{2}-[0-9]{3}Z)_([0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12})\.jsonl$)"};

struct AutomaticSessionFile {
    std::filesystem::path path;
    std::string file_timestamp;
    std::string file_session_id;
    cch::util::JsonValue::object_t header;
};

/// Verify one automatic session file at path: pi-shaped filename whose UTC
/// timestamp and UUID match the durable header identity and the canonical
/// workspace metadata.
AutomaticSessionFile verify_automatic_session_file(
    const std::filesystem::path& path,
    const std::filesystem::path& canonical_workspace) {
    std::smatch match;
    const auto filename = path.filename().string();
    REQUIRE(std::regex_match(filename, match, kAutomaticFilename));

    std::ifstream input(path, std::ios::binary);
    std::string first_line;
    REQUIRE(static_cast<bool>(std::getline(input, first_line)));

    AutomaticSessionFile created;
    created.path = path;
    created.file_timestamp = match[1].str();
    created.file_session_id = match[2].str();
    created.header = as_object(parse_json_line(first_line));
    CHECK(json_string_at(created.header, "type") == "session");
    CHECK(json_string_at(created.header, "id") == created.file_session_id);
    CHECK(json_string_at(created.header, "cwd") == canonical_workspace.string());

    // The filename timestamp is the header's UTC instant with punctuation
    // normalized, proving file identity and header identity share one creation.
    auto normalized_timestamp = json_string_at(created.header, "timestamp");
    REQUIRE_FALSE(normalized_timestamp.empty());
    for (auto& character : normalized_timestamp) {
        if (character == ':' || character == '.') {
            character = '-';
        }
    }
    CHECK(normalized_timestamp == created.file_timestamp);
    return created;
}

/// Require exactly one automatic session below the sessions root, placed in the
/// workspace-keyed directory, with a pi-shaped filename whose UTC timestamp and
/// UUID match the durable header identity.
AutomaticSessionFile require_single_automatic_session(
    const std::filesystem::path& sessions_root,
    const std::filesystem::path& canonical_workspace) {
    const auto files = jsonl_files_under(sessions_root);
    REQUIRE(files.size() == 1);
    const auto expected_directory = sessions_root / expected_workspace_key(canonical_workspace);
    CHECK(files.front().parent_path() == expected_directory);
    return verify_automatic_session_file(files.front(), canonical_workspace);
}

/// Require exactly one automatic session directly inside a CLI directory
/// override: no workspace-key component is added (pi: --session-dir).
AutomaticSessionFile require_single_session_in_directory(
    const std::filesystem::path& directory,
    const std::filesystem::path& canonical_workspace) {
    const auto files = jsonl_files_under(directory);
    REQUIRE(files.size() == 1);
    CHECK(files.front().parent_path() == directory);
    return verify_automatic_session_file(files.front(), canonical_workspace);
}

}

TEST_CASE("CLI fake one-shot prints transcript and writes session", "[cli][u6]") {
    cch::tests::TempWorkspace workspace;
    auto session = workspace.path() / "one-shot.jsonl";
    auto result = run_command(bin() + " --fake --workspace " + shell_quote(workspace.path()) + " --session " + shell_quote(session) + " hello");

    REQUIRE(result.exit_code == 0);
    CHECK(result.output.find("[model-request]") != std::string::npos);
    CHECK(result.output.find("[assistant] fake: hello") != std::string::npos);
    CHECK(count_occurrences(result.output, "fake: hello") == 1);
    CHECK(result.output.find("[completed]") != std::string::npos);
    CHECK(std::filesystem::exists(session));
}

TEST_CASE("CLI text one-shot displays /help without invoking the model", "[cli][commands]") {
    cch::tests::TempWorkspace workspace;
    auto session = workspace.path() / "help-one-shot.jsonl";
    auto result = run_command(
        bin() + " --fake --workspace " + shell_quote(workspace.path()) + " --session " + shell_quote(session) + " /help");

    REQUIRE(result.exit_code == 0);
    CHECK(result.output.find("Available commands:") != std::string::npos);
    CHECK(result.output.find("/commands") != std::string::npos);
    CHECK(result.output.find("/help [command]") != std::string::npos);
    CHECK(result.output.find("[model-request]") == std::string::npos);
}

TEST_CASE("CLI text one-shot unknown /help target does not invoke the model", "[cli][commands]") {
    cch::tests::TempWorkspace workspace;
    auto session = workspace.path() / "help-unknown.jsonl";
    auto result = run_command(
        bin() + " --fake --workspace " + shell_quote(workspace.path()) + " --session " + shell_quote(session) +
        " '/help missing'");

    REQUIRE(result.exit_code == 0);
    CHECK(result.output.find("Unknown command: /missing") != std::string::npos);
    CHECK(result.output.find("[model-request]") == std::string::npos);
    CHECK(result.output.find("[assistant]") == std::string::npos);
}

TEST_CASE("CLI text REPL dispatches /commands as /help", "[cli][commands]") {
    cch::tests::TempWorkspace workspace;
    auto session = workspace.path() / "commands-repl.jsonl";
    auto result = run_command(
        "printf '/commands\\nquit\\n' | " + bin() + " --fake --repl --workspace " +
        shell_quote(workspace.path()) + " --session " + shell_quote(session));

    REQUIRE(result.exit_code == 0);
    CHECK(result.output.find("Available commands:") != std::string::npos);
    CHECK(result.output.find("Alias for /help") != std::string::npos);
    CHECK(result.output.find("[model-request]") == std::string::npos);
}

TEST_CASE("CLI text one-shot sends unmatched slash input to the model", "[cli][commands]") {
    cch::tests::TempWorkspace workspace;
    auto session = workspace.path() / "unknown-slash.jsonl";
    auto result = run_command(
        bin() + " --fake --workspace " + shell_quote(workspace.path()) + " --session " + shell_quote(session) + " /missing");

    REQUIRE(result.exit_code == 0);
    CHECK(result.output.find("fake: /missing") != std::string::npos);
    CHECK(result.output.find("Unknown command") == std::string::npos);
}

TEST_CASE("CLI text REPL temporarily intercepts user bash while one-shot does not", "[cli][commands][user-bash]") {
    cch::tests::TempWorkspace repl_workspace;
    auto repl_session = repl_workspace.path() / "repl-user-bash.jsonl";
    auto repl = run_command(
        "printf '!echo hi\\n!!echo hidden\\nquit\\n' | " + bin() + " --fake --repl --workspace " +
        shell_quote(repl_workspace.path()) + " --session " + shell_quote(repl_session));

    REQUIRE(repl.exit_code == 0);
    CHECK(count_occurrences(repl.output, "Shell passthrough (!) is not yet implemented.") == 2);
    CHECK(repl.output.find("fake: !echo hi") == std::string::npos);
    CHECK(repl.output.find("fake: !!echo hidden") == std::string::npos);

    cch::tests::TempWorkspace one_shot_workspace;
    auto one_shot_session = one_shot_workspace.path() / "oneshot-user-bash.jsonl";
    auto one_shot = run_command(
        bin() + " --fake --workspace " + shell_quote(one_shot_workspace.path()) + " --session " +
        shell_quote(one_shot_session) + " '!echo hi'");

    REQUIRE(one_shot.exit_code == 0);
    CHECK(one_shot.output.find("fake: !echo hi") != std::string::npos);

    cch::tests::TempWorkspace double_bang_workspace;
    auto double_bang_session = double_bang_workspace.path() / "oneshot-double-bang.jsonl";
    auto double_bang = run_command(
        bin() + " --fake --workspace " + shell_quote(double_bang_workspace.path()) + " --session " +
        shell_quote(double_bang_session) + " '!!echo hidden'");

    REQUIRE(double_bang.exit_code == 0);
    CHECK(double_bang.output.find("fake: !!echo hidden") != std::string::npos);
}

TEST_CASE("CLI text one-shot /clear emits terminal controls without invoking the model", "[cli][commands]") {
    cch::tests::TempWorkspace workspace;
    auto session = workspace.path() / "clear-one-shot.jsonl";
    auto result = run_command_split(
        bin() + " --fake --workspace " + shell_quote(workspace.path()) + " --session " + shell_quote(session) + " /clear");

    REQUIRE(result.exit_code == 0);
    CHECK(result.stderr_text.empty());
    CHECK(result.stdout_text == "\033[2J\033[H");
    CHECK(result.stdout_text.find("[model-request]") == std::string::npos);
}

TEST_CASE("CLI text REPL /clear stays in the frontend and /exit displays shutdown text", "[cli][commands]") {
    cch::tests::TempWorkspace workspace;
    auto session = workspace.path() / "clear-exit-repl.jsonl";
    auto result = run_command(
        "printf '/clear\\n/exit\\nignored\\n' | " + bin() + " --fake --repl --workspace " +
        shell_quote(workspace.path()) + " --session " + shell_quote(session));

    REQUIRE(result.exit_code == 0);
    CHECK(result.output.find("\033[2J\033[H") != std::string::npos);
    CHECK(result.output.find("Shutting down.") != std::string::npos);
    CHECK(result.output.find("fake: ignored") == std::string::npos);
    CHECK(result.output.find("[model-request]") == std::string::npos);
}

TEST_CASE("CLI text one-shot /exit displays shutdown text", "[cli][commands]") {
    cch::tests::TempWorkspace workspace;
    auto session = workspace.path() / "exit-one-shot.jsonl";
    auto result = run_command(
        bin() + " --fake --workspace " + shell_quote(workspace.path()) + " --session " + shell_quote(session) + " /exit");

    REQUIRE(result.exit_code == 0);
    CHECK(result.output.find("Shutting down.") != std::string::npos);
    CHECK(result.output.find("[model-request]") == std::string::npos);
}

TEST_CASE("CLI fake one-shot streams through the current event path", "[cli][u8]") {
    cch::tests::TempWorkspace workspace;
    auto session = workspace.path() / "event-path.jsonl";
    auto result = run_command(bin() + " --fake --workspace " + shell_quote(workspace.path()) + " --session " + shell_quote(session) + " hello");

    REQUIRE(result.exit_code == 0);
    CHECK(result.output.find("[model-request]") != std::string::npos);
    CHECK(result.output.find("[assistant] fake: hello") != std::string::npos);
    CHECK(count_occurrences(result.output, "fake: hello") == 1);
    CHECK(result.output.find("[completed]") != std::string::npos);
    CHECK(std::filesystem::exists(session));
}

TEST_CASE("CLI text tool flow renders subscription events without duplicate presentation", "[cli][presentation]") {
    cch::tests::TempWorkspace workspace;
    std::ofstream(workspace.path() / "note.txt") << "subscription text";
    auto session = workspace.path() / "text-tool-flow.jsonl";
    auto result = run_command(
        bin() + " --fake --workspace " + shell_quote(workspace.path()) + " --session " + shell_quote(session) +
        " 'read note.txt'");

    REQUIRE(result.exit_code == 0);
    CHECK(count_occurrences(result.output, "[tool-call] read#fake-read-1") == 1);
    CHECK(count_occurrences(result.output, "[tool-success] fake-read-1") == 1);
    CHECK(result.output.find("[assistant] fake observed: subscription text") != std::string::npos);
    CHECK(count_occurrences(result.output, "fake observed: subscription text") == 1);
    CHECK(std::filesystem::exists(session));
}

TEST_CASE("CLI rejects removed async compatibility flag before model request", "[cli][u8]") {
    cch::tests::TempWorkspace workspace;
    auto session = workspace.path() / "removed-async.jsonl";
    auto result = run_command(bin() + " --async --fake --workspace " + shell_quote(workspace.path()) + " --session " + shell_quote(session) + " hello");

    REQUIRE(result.exit_code != 0);
    CHECK(result.output.find("unknown option: --async") != std::string::npos);
    CHECK(result.output.find("[model-request]") == std::string::npos);
}

TEST_CASE("CLI help no longer advertises compatibility-only async flag", "[cli][u8]") {
    auto result = run_command(bin() + " --help");

    REQUIRE(result.exit_code == 0);
    CHECK(result.output.find("--async") == std::string::npos);
    CHECK(result.output.find("--fake") != std::string::npos);
    CHECK(result.output.find("--mode") != std::string::npos);
}

TEST_CASE("CLI rejects unsupported JSON mode combinations before model request", "[cli][json]") {
    cch::tests::TempWorkspace workspace;
    auto session = workspace.path() / "json-repl.jsonl";
    auto result = run_command_split(bin() + " --fake --mode json --repl --workspace " + shell_quote(workspace.path()) + " --session " + shell_quote(session));

    REQUIRE(result.exit_code != 0);
    CHECK(result.stdout_text.empty());
    CHECK(result.stderr_text.find("--mode json cannot be combined with --repl") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(session));
}

TEST_CASE("CLI rejects RPC positional prompt before session creation", "[cli][rpc]") {
    cch::tests::TempWorkspace workspace;
    auto session = workspace.path() / "rpc-positional.jsonl";
    auto result = run_command_split(bin() + " --fake --mode rpc --workspace " + shell_quote(workspace.path()) + " --session " + shell_quote(session) + " hello");

    REQUIRE(result.exit_code != 0);
    CHECK(result.stdout_text.empty());
    CHECK(result.stderr_text.find("--mode rpc reads prompts from stdin") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(session));
}

TEST_CASE("CLI rejects RPC repl before session creation", "[cli][rpc]") {
    cch::tests::TempWorkspace workspace;
    auto session = workspace.path() / "rpc-repl.jsonl";
    auto result = run_command_split(bin() + " --fake --mode rpc --repl --workspace " + shell_quote(workspace.path()) + " --session " + shell_quote(session));

    REQUIRE(result.exit_code != 0);
    CHECK(result.stdout_text.empty());
    CHECK(result.stderr_text.find("--mode rpc cannot be combined with --repl") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(session));
}

TEST_CASE("CLI JSON fake one-shot emits a header followed by direct session events", "[cli][json]") {
    cch::tests::TempWorkspace workspace;
    auto session = workspace.path() / "json-one-shot.jsonl";
    auto result = run_command_split(bin() + " --fake --mode json --workspace " + shell_quote(workspace.path()) + " --session " + shell_quote(session) + " hello");

    REQUIRE(result.exit_code == 0);
    CHECK(result.stderr_text.empty());
    auto output_lines = non_empty_lines(result.stdout_text);
    REQUIRE(output_lines.size() >= 5);
    auto records = parse_json_objects(result.stdout_text);
    const auto& first = records.front();
    CHECK(first.size() == 5);
    CHECK(json_string_at(first, "type") == "session");
    CHECK(static_cast<int>(first.at("version").get<double>()) == 3);
    CHECK(json_string_at(first, "id").empty() == false);
    for (std::size_t index = 1; index < records.size(); ++index) {
        const auto& record = records[index];
        CHECK_FALSE(record.contains("schemaVersion"));
        CHECK_FALSE(record.contains("seq"));
        CHECK_FALSE(record.contains("contentStatus"));
        CHECK(json_string_at(record, "type") != "runtime_terminal");
    }
    CHECK(has_json_event_type(output_lines, "turn_start"));
    CHECK(has_json_event_type(output_lines, "message_update"));
    CHECK(has_json_event_type(output_lines, "message_end"));

    std::size_t assistant_start = records.size();
    std::size_t assistant_update = records.size();
    for (std::size_t index = 1; index < records.size(); ++index) {
        const auto& record = records[index];
        const auto type = json_string_at(record, "type");
        if ((type == "message_start" || type == "message_update" || type == "message_end") &&
            record.contains("message")) {
            const auto& message = record.at("message").get<cch::util::JsonValue::object_t>();
            if (json_string_at(message, "role") == "assistant") {
                CHECK(message.contains("api"));
                CHECK(message.contains("provider"));
                CHECK(message.contains("model"));
                CHECK(message.contains("usage"));
                CHECK(json_string_at(message, "stopReason") != "unknown");
                if (type == "message_start" && assistant_start == records.size()) {
                    assistant_start = index;
                }
                if (type == "message_update" && assistant_update == records.size()) {
                    assistant_update = index;
                }
            }
        }
    }
    REQUIRE(assistant_start < records.size());
    REQUIRE(assistant_update < records.size());
    CHECK(assistant_start < assistant_update);

    const auto& last = records.back();
    CHECK(json_string_at(last, "type") == "agent_end");
    CHECK(last.contains("messages"));
    CHECK(result.stdout_text.find("[assistant]") == std::string::npos);
    CHECK(std::filesystem::exists(session));
}

TEST_CASE("CLI JSON frontend commands emit no synthetic terminal records", "[cli][json][commands]") {
    for (const std::string command : {"/help", "/exit", "/clear"}) {
        cch::tests::TempWorkspace workspace;
        auto session = workspace.path() / "json-command.jsonl";
        auto result = run_command_split(
            bin() + " --fake --mode json --workspace " + shell_quote(workspace.path()) +
            " --session " + shell_quote(session) + " " + command);

        REQUIRE(result.exit_code == 0);
        CHECK(result.stderr_text.empty());
        const auto records = parse_json_objects(result.stdout_text);
        REQUIRE(records.size() == 1);
        CHECK(json_string_at(records.front(), "type") == "session");
        CHECK(result.stdout_text.find("runtime_terminal") == std::string::npos);
        CHECK(result.stdout_text.find("[model-request]") == std::string::npos);
        CHECK(result.stdout_text.find('\033') == std::string::npos);
    }
}

TEST_CASE("CLI JSON fake tool flow emits correlated tool events", "[cli][json]") {
    cch::tests::TempWorkspace workspace;
    auto session = workspace.path() / "json-tool.jsonl";
    auto result = run_command_split(bin() + " --fake --mode json --workspace " + shell_quote(workspace.path()) + " --session " + shell_quote(session) + " read missing.txt");

    REQUIRE(result.exit_code == 0);
    auto output_lines = non_empty_lines(result.stdout_text);
    REQUIRE(has_json_event_type(output_lines, "tool_execution_start"));
    REQUIRE(has_json_event_type(output_lines, "tool_execution_end"));
    bool saw_tool_end = false;
    for (const auto& line : output_lines) {
        auto record = as_object(parse_json_line(line));
        if (json_string_at(record, "type") == "tool_execution_end") {
            saw_tool_end = true;
            CHECK(json_string_at(record, "toolCallId") == "fake-read-1");
            CHECK(json_string_at(record, "toolName") == "read");
            const auto& tool_result = record.at("result").get<cch::util::JsonValue::object_t>();
            CHECK(tool_result.contains("content"));
            CHECK(record.find("contentStatus") == record.end());
        }
    }
    CHECK(saw_tool_end);
    auto last = as_object(parse_json_line(output_lines.back()));
    CHECK(json_string_at(last, "type") == "agent_end");
}

TEST_CASE("CLI JSON max-turn flow ends with the direct agent event", "[cli][json]") {
    cch::tests::TempWorkspace workspace;
    auto session = workspace.path() / "json-max-turn.jsonl";
    auto result = run_command_split(bin() + " --fake --mode json --workspace " + shell_quote(workspace.path()) + " --session " + shell_quote(session) + " --max-turns 1 read missing.txt");

    REQUIRE(result.exit_code == 1);
    auto output_lines = non_empty_lines(result.stdout_text);
    REQUIRE(output_lines.size() >= 2);
    for (const auto& line : output_lines) {
        (void)parse_json_line(line);
    }
    auto last = as_object(parse_json_line(output_lines.back()));
    CHECK(json_string_at(last, "type") == "agent_end");
    CHECK(last.contains("messages"));
    CHECK(result.stdout_text.find("runtime_terminal") == std::string::npos);
    CHECK(result.stderr_text.find("max turns exceeded") != std::string::npos);
}

TEST_CASE("CLI RPC wires stdin and JSONL stdout to a session", "[cli][rpc]") {
    cch::tests::TempWorkspace workspace;
    auto session = workspace.path() / "rpc-loop.jsonl";
    const std::string input =
        "{\"id\":\"p1\",\"type\":\"prompt\",\"message\":\"hello\"}\n"
        "{\"id\":\"q1\",\"type\":\"shutdown\"}\n";
    auto result = run_command_split_with_input(
        bin() + " --fake --mode rpc --workspace " + shell_quote(workspace.path()) + " --session " + shell_quote(session),
        input);

    REQUIRE(result.exit_code == 0);
    CHECK(result.stderr_text.empty());
    const auto records = parse_json_objects(result.stdout_text);
    REQUIRE(find_response(records, "prompt") != nullptr);
    REQUIRE(find_response(records, "shutdown") != nullptr);
    REQUIRE(records.size() >= 3);
    CHECK(json_string_at(records.front(), "type") == "response");
    CHECK(json_string_at(records.front(), "command") == "prompt");
    CHECK(json_string_at(records[1], "type") == "agent_start");
    CHECK(has_json_event_type(non_empty_lines(result.stdout_text), "message_update"));
    CHECK_FALSE(has_json_event_type(non_empty_lines(result.stdout_text), "runtime_terminal"));
    CHECK_FALSE(has_json_event_type(non_empty_lines(result.stdout_text), "session"));
    CHECK(result.stdout_text.find("[assistant]") == std::string::npos);
    CHECK(result.stdout_text.find('\033') == std::string::npos);
    CHECK(std::filesystem::exists(session));
}

TEST_CASE("CLI JSON creation validation errors do not write stdout", "[cli][json]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace home;
    auto session = workspace.path() / "json-real.jsonl";
    auto result = run_command_split(
        "HOME=" + shell_quote(home.path()) + " env -u CCH_TEST_MISSING_KEY " + bin() +
        " --mode json --workspace " + shell_quote(workspace.path()) +
        " --session " + shell_quote(session) +
        " --api-key-env CCH_TEST_MISSING_KEY hello");

    REQUIRE(result.exit_code == 2);
    CHECK(result.stdout_text.empty());
    CHECK(result.stderr_text.find("could not create session") != std::string::npos);
    CHECK(result.stderr_text.find("missing API key") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(session));
}

TEST_CASE("CLI creation failure after malformed settings keeps the settings warning visible", "[cli][settings]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace agent_root;
    const auto agent_dir = agent_root.path() / "agent";
    std::filesystem::create_directories(agent_dir);
    {
        std::ofstream settings(agent_dir / "settings.json");
        settings << "{not valid json";
    }
    auto session = workspace.path() / "settings-fallback-failure.jsonl";
    auto result = run_command_split(
        "CCH_CODING_AGENT_DIR=" + shell_quote(agent_dir) + " env -u CCH_TEST_MISSING_KEY " + bin() +
        " --workspace " + shell_quote(workspace.path()) +
        " --session " + shell_quote(session) +
        " --api-key-env CCH_TEST_MISSING_KEY hello");

    REQUIRE(result.exit_code == 2);
    CHECK(result.stdout_text.empty());
    CHECK(result.stderr_text.find("missing API key") != std::string::npos);
    CHECK(result.stderr_text.find("could not load user settings") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(session));
}

TEST_CASE("CLI rejects an unresolvable workspace before model request", "[cli][assembly]") {
    cch::tests::TempWorkspace workspace;
    auto session = workspace.path() / "workspace-invalid.jsonl";
    auto result = run_command_split(
        bin() + " --fake --workspace " + shell_quote(workspace.path() / "missing-workspace") +
        " --session " + shell_quote(session) + " hello");

    REQUIRE(result.exit_code == 2);
    CHECK(result.stdout_text.empty());
    CHECK(result.stderr_text.find("could not create session") != std::string::npos);
    CHECK(result.stderr_text.find("workspace") != std::string::npos);
    CHECK(result.stderr_text.find("[model-request]") == std::string::npos);
    CHECK_FALSE(std::filesystem::exists(session));
}

TEST_CASE("CLI fake REPL preserves process history for two prompts", "[cli][u6]") {
    cch::tests::TempWorkspace workspace;
    auto session = workspace.path() / "repl.jsonl";
    auto command = "printf 'one\\ntwo\\nexit\\n' | " + bin() + " --fake --repl --workspace " + shell_quote(workspace.path()) + " --session " + shell_quote(session);
    auto result = run_command(command);

    REQUIRE(result.exit_code == 0);
    CHECK(result.output.find("[assistant] fake: one") != std::string::npos);
    CHECK(result.output.find("[assistant] fake: two") != std::string::npos);
}

TEST_CASE("CLI resume appends to an existing redacted session", "[cli][u6]") {
    cch::tests::TempWorkspace workspace;
    auto session = workspace.path() / "resume.jsonl";
    auto first = run_command(bin() + " --fake --workspace " + shell_quote(workspace.path()) + " --session " + shell_quote(session) + " first");
    REQUIRE(first.exit_code == 0);

    auto second = run_command(bin() + " --fake --workspace " + shell_quote(workspace.path()) + " --resume " + shell_quote(session) + " second");

    REQUIRE(second.exit_code == 0);
    CHECK(second.output.find("[assistant] fake: second") != std::string::npos);
}

TEST_CASE("CLI resume uses session workspace when workspace is omitted", "[cli][u6]") {
    cch::tests::TempWorkspace original;
    cch::tests::TempWorkspace other;
    original.write("note.txt", "from-session-workspace");
    auto session = original.path() / "resume-workspace.jsonl";
    auto first = run_command(bin() + " --fake --workspace " + shell_quote(original.path()) + " --session " + shell_quote(session) + " first");
    REQUIRE(first.exit_code == 0);

    auto resumed = run_command(bin() + " --fake --resume " + shell_quote(session) + " read note.txt");

    REQUIRE(resumed.exit_code == 0);
    CHECK(resumed.output.find("from-session-workspace") != std::string::npos);
}

TEST_CASE("CLI resume rejects explicit workspace mismatch", "[cli][u6]") {
    cch::tests::TempWorkspace original;
    cch::tests::TempWorkspace other;
    auto session = original.path() / "resume-mismatch.jsonl";
    auto first = run_command(bin() + " --fake --workspace " + shell_quote(original.path()) + " --session " + shell_quote(session) + " first");
    REQUIRE(first.exit_code == 0);

    auto resumed = run_command(bin() + " --fake --workspace " + shell_quote(other.path()) + " --resume " + shell_quote(session) + " second");

    REQUIRE(resumed.exit_code == 2);
    CHECK(resumed.output.find("could not resume session") != std::string::npos);
    CHECK(resumed.output.find("resume workspace does not match session metadata") != std::string::npos);
    CHECK(resumed.output.find("[model-request]") == std::string::npos);
}

TEST_CASE("CLI RPC resume exposes committed history without activating text output", "[cli][rpc]") {
    cch::tests::TempWorkspace workspace;
    auto session = workspace.path() / "rpc-resume.jsonl";
    auto first = run_command(bin() + " --fake --workspace " + shell_quote(workspace.path()) + " --session " + shell_quote(session) + " first");
    REQUIRE(first.exit_code == 0);

    auto resumed = run_command_split_with_input(
        bin() + " --fake --mode rpc --resume " + shell_quote(session),
        "{\"id\":\"l1\",\"type\":\"get_last_assistant_text\"}\n{\"type\":\"shutdown\"}\n");

    REQUIRE(resumed.exit_code == 0);
    CHECK(resumed.stderr_text.empty());
    auto records = parse_json_objects(resumed.stdout_text);
    auto* last_text = find_response(records, "get_last_assistant_text");
    REQUIRE(last_text != nullptr);
    auto data = last_text->at("data").get<cch::util::JsonValue::object_t>();
    CHECK(json_string_at(data, "text") == "fake: first");
    CHECK(resumed.stdout_text.find("[assistant]") == std::string::npos);
}

TEST_CASE("CLI RPC resume workspace mismatch fails before loop activation", "[cli][rpc]") {
    cch::tests::TempWorkspace original;
    cch::tests::TempWorkspace other;
    auto session = original.path() / "rpc-resume-mismatch.jsonl";
    auto first = run_command(bin() + " --fake --workspace " + shell_quote(original.path()) + " --session " + shell_quote(session) + " first");
    REQUIRE(first.exit_code == 0);

    auto resumed = run_command_split_with_input(
        bin() + " --fake --mode rpc --workspace " + shell_quote(other.path()) + " --resume " + shell_quote(session),
        "{\"type\":\"get_state\"}\n");

    REQUIRE(resumed.exit_code == 2);
    CHECK(resumed.stdout_text.empty());
    CHECK(resumed.stderr_text.find("could not resume session") != std::string::npos);
    CHECK(resumed.stderr_text.find("resume workspace does not match session metadata") != std::string::npos);
}

TEST_CASE("CLI rejects session and resume together before model request", "[cli][u6]") {
    cch::tests::TempWorkspace workspace;
    auto session = workspace.path() / "exclusive.jsonl";
    auto result = run_command(bin() + " --fake --workspace " + shell_quote(workspace.path()) + " --session " + shell_quote(session) + " --resume " + shell_quote(session) + " hello");

    REQUIRE(result.exit_code != 0);
    CHECK(result.output.find("use either --session or --resume") != std::string::npos);
    CHECK(result.output.find("[model-request]") == std::string::npos);
}

TEST_CASE("CLI text mode without prompt defaults to REPL", "[cli][u6]") {
    cch::tests::TempWorkspace workspace;
    auto session = workspace.path() / "default-repl.jsonl";
    auto result = run_command("printf '' | " + bin() + " --fake --workspace " + shell_quote(workspace.path()) + " --session " + shell_quote(session));

    REQUIRE(result.exit_code == 0);
    CHECK(result.output.find("prompt is required") == std::string::npos);
    CHECK(std::filesystem::exists(session));
}

TEST_CASE("CLI blocks existing session path without resume before model request", "[cli][u6]") {
    cch::tests::TempWorkspace workspace;
    auto session = workspace.path() / "exists.jsonl";
    std::ofstream(session) << "already here";
    auto result = run_command(bin() + " --fake --workspace " + shell_quote(workspace.path()) + " --session " + shell_quote(session) + " hello");

    REQUIRE(result.exit_code == 2);
    CHECK(result.output.find("could not create session") != std::string::npos);
    CHECK(result.output.find("already exists") != std::string::npos);
    CHECK(result.output.find("[model-request]") == std::string::npos);
}

TEST_CASE("CLI rejects invalid max turns before model request", "[cli][u6]") {
    cch::tests::TempWorkspace workspace;
    auto session = workspace.path() / "bad-turns.jsonl";
    auto result = run_command(bin() + " --fake --workspace " + shell_quote(workspace.path()) + " --session " + shell_quote(session) + " --max-turns nope hello");

    REQUIRE(result.exit_code != 0);
    CHECK((result.output.find("--max-turns") != std::string::npos ||
           result.output.find("Could not parse") != std::string::npos));
    CHECK(result.output.find("[model-request]") == std::string::npos);
}

TEST_CASE("CLI fake bash request is blocked by default", "[cli][u6]") {
    cch::tests::TempWorkspace workspace;
    auto session = workspace.path() / "bash.jsonl";
    auto result = run_command(bin() + " --fake --workspace " + shell_quote(workspace.path()) + " --session " + shell_quote(session) + " bash echo hi");

    REQUIRE(result.exit_code == 0);
    // With bash disabled, the bash tool is not registered. The scripted fake
    // client still asks for bash, so the executor reports an unknown tool.
    CHECK(result.output.find("[tool-call] bash#fake-bash-1") != std::string::npos);
    CHECK(result.output.find("[tool-error] fake-bash-1") != std::string::npos);
    CHECK(result.output.find("bash is disabled") == std::string::npos);
    CHECK(result.output.find("unknown tool: bash") != std::string::npos);
}

TEST_CASE("CLI fake read loop prints max-turn marker when turn budget is exhausted", "[cli][u1]") {
    cch::tests::TempWorkspace workspace;
    auto session = workspace.path() / "max-turn.jsonl";
    auto result = run_command(bin() + " --fake --workspace " + shell_quote(workspace.path()) + " --session " + shell_quote(session) + " --max-turns 1 read missing.txt");

    REQUIRE(result.exit_code == 1);
    CHECK(result.output.find("[model-request]") != std::string::npos);
    CHECK(result.output.find("[tool-call] read#fake-read-1") != std::string::npos);
    CHECK(result.output.find("[tool-error] fake-read-1") != std::string::npos);
    CHECK(result.output.find("loop failed: max turns exceeded") != std::string::npos);
}

TEST_CASE("CLI real-provider mode reports missing API key before model request", "[cli][u6]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace home;
    auto session = workspace.path() / "real.jsonl";
    auto result = run_command(
        "HOME=" + shell_quote(home.path()) + " env -u CCH_TEST_MISSING_KEY " + bin() +
        " --workspace " + shell_quote(workspace.path()) +
        " --session " + shell_quote(session) +
        " --api-key-env CCH_TEST_MISSING_KEY hello");

    REQUIRE(result.exit_code == 2);
    CHECK(result.output.find("missing API key") != std::string::npos);
    CHECK(result.output.find("[model-request]") == std::string::npos);
}

TEST_CASE("CLI Kimi path validates KIMI_API_KEY before model request and session creation", "[cli][kimi][u3]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace home;
    auto session = workspace.path() / "kimi-missing-key.jsonl";
    auto result = run_command(
        "HOME=" + shell_quote(home.path()) + " env -u KIMI_API_KEY " + bin() +
        " --workspace " + shell_quote(workspace.path()) +
        " --session " + shell_quote(session) +
        " --base-url https://api.kimi.com/coding/v1"
        " --model kimi-for-coding"
        " --api-key-env KIMI_API_KEY hello");

    REQUIRE(result.exit_code == 2);
    CHECK(result.output.find("missing API key") != std::string::npos);
    CHECK(result.output.find("KIMI_API_KEY") != std::string::npos);
    CHECK(result.output.find("[model-request]") == std::string::npos);
    CHECK_FALSE(std::filesystem::exists(session));
}

TEST_CASE("CLI skips project skills by default when project trust is unknown", "[cli][project-trust]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace home;
    workspace.write(".cpp-harness/skills/demo/SKILL.md",
                    "---\n"
                    "name: demo\n"
                    "description: Demo skill.\n"
                    "---\n"
                    "# Demo Skill\n\n"
                    "Do demo.\n");
    auto session = workspace.path() / "untrusted-skills.jsonl";

    auto result = run_command_split(
        "HOME=" + shell_quote(home.path()) + " " + bin() +
        " --fake --workspace " + shell_quote(workspace.path()) +
        " --session " + shell_quote(session) + " /skill:demo");

    REQUIRE(result.exit_code == 0);
    CHECK(result.stderr_text.find("project_skills skipped: untrusted") != std::string::npos);
    CHECK(result.stdout_text.find("Do demo.") == std::string::npos);
}

TEST_CASE("CLI project-controlled default trust store cannot authorize project skills", "[cli][project-trust]") {
    cch::tests::TempWorkspace workspace;
    workspace.write(".cpp-harness/skills/demo/SKILL.md",
                    "---\n"
                    "name: demo\n"
                    "description: Demo skill.\n"
                    "---\n"
                    "# Demo Skill\n\n"
                    "Do demo.\n");
    workspace.write(
        ".cpp-harness/agent/trust.json",
        "{\"" + std::filesystem::weakly_canonical(workspace.path()).string() + "\":true}\n");
    auto session = workspace.path() / "project-controlled-trust.jsonl";

    auto result = run_command_split(
        "HOME=" + shell_quote(workspace.path()) + " " + bin() +
        " --fake --workspace " + shell_quote(workspace.path()) +
        " --session " + shell_quote(session) + " /skill:demo");

    REQUIRE(result.exit_code == 0);
    CHECK(result.stderr_text.find("[trust:warn] trust_store_unavailable") != std::string::npos);
    CHECK(result.stderr_text.find("project_skills skipped: untrusted") != std::string::npos);
    CHECK(result.stdout_text.find("Do demo.") == std::string::npos);
}

TEST_CASE("CLI approve loads project skills for one run", "[cli][project-trust]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace home;
    workspace.write(".cpp-harness/skills/demo/SKILL.md",
                    "---\n"
                    "name: demo\n"
                    "description: Demo skill.\n"
                    "---\n"
                    "# Demo Skill\n\n"
                    "Do demo.\n");
    auto session = workspace.path() / "trusted-skills.jsonl";

    auto result = run_command_split(
        "HOME=" + shell_quote(home.path()) + " " + bin() +
        " --fake --approve --workspace " + shell_quote(workspace.path()) +
        " --session " + shell_quote(session) + " /skill:demo");

    REQUIRE(result.exit_code == 0);
    CHECK(result.stderr_text.find("project_skills skipped") == std::string::npos);
    CHECK(result.stdout_text.find("<skill name=\"demo\"") != std::string::npos);
    CHECK(result.stdout_text.find("Do demo.") != std::string::npos);
}

TEST_CASE("CLI no-approve skips project skills for one run", "[cli][project-trust]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace home;
    workspace.write(".cpp-harness/skills/demo/SKILL.md",
                    "---\n"
                    "name: demo\n"
                    "description: Demo skill.\n"
                    "---\n"
                    "# Demo Skill\n\n"
                    "Do demo.\n");
    auto session = workspace.path() / "no-approve-skills.jsonl";

    auto result = run_command_split(
        "HOME=" + shell_quote(home.path()) + " " + bin() +
        " --fake --no-approve --workspace " + shell_quote(workspace.path()) +
        " --session " + shell_quote(session) + " /skill:demo");

    REQUIRE(result.exit_code == 0);
    CHECK(result.stderr_text.find("project_skills skipped: untrusted") != std::string::npos);
    CHECK(result.stdout_text.find("Do demo.") == std::string::npos);
}

TEST_CASE("CLI approve loads project prompt templates for one run", "[cli][project-resources]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace home;
    workspace.write(".cpp-harness/prompts/greet.md",
                    "---\n"
                    "description: Greet someone\n"
                    "---\n"
                    "Project hello $1.\n");
    auto session = workspace.path() / "trusted-prompts.jsonl";

    auto result = run_command_split(
        "HOME=" + shell_quote(home.path()) + " " + bin() +
        " --fake --approve --workspace " + shell_quote(workspace.path()) +
        " --session " + shell_quote(session) + " /greet Ada");

    REQUIRE(result.exit_code == 0);
    CHECK(result.stderr_text.find("project_prompts skipped") == std::string::npos);
    CHECK(result.stdout_text.find("Project hello Ada.") != std::string::npos);
}

TEST_CASE("CLI no-skills disables project skills even when approved", "[cli][project-trust]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace home;
    workspace.write(".cpp-harness/skills/demo/SKILL.md",
                    "---\n"
                    "name: demo\n"
                    "description: Demo skill.\n"
                    "---\n"
                    "# Demo Skill\n\n"
                    "Do demo.\n");
    auto session = workspace.path() / "disabled-skills.jsonl";

    auto result = run_command_split(
        "HOME=" + shell_quote(home.path()) + " " + bin() +
        " --fake --approve --no-skills --workspace " + shell_quote(workspace.path()) +
        " --session " + shell_quote(session) + " /skill:demo");

    REQUIRE(result.exit_code == 0);
    CHECK(result.stderr_text.find("project_skills skipped: disabled") != std::string::npos);
    CHECK(result.stdout_text.find("Do demo.") == std::string::npos);
}

TEST_CASE("CLI no-skills disables project prompt templates even when approved", "[cli][project-resources]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace home;
    workspace.write(".cpp-harness/prompts/greet.md",
                    "---\n"
                    "description: Greet someone\n"
                    "---\n"
                    "Project hello $1.\n");
    auto session = workspace.path() / "disabled-prompts.jsonl";

    auto result = run_command_split(
        "HOME=" + shell_quote(home.path()) + " " + bin() +
        " --fake --approve --no-skills --workspace " + shell_quote(workspace.path()) +
        " --session " + shell_quote(session) + " /greet Ada");

    REQUIRE(result.exit_code == 0);
    CHECK(result.stderr_text.find("project_prompts skipped: disabled") != std::string::npos);
    CHECK(result.stdout_text.find("Project hello Ada.") == std::string::npos);
}

TEST_CASE("CLI explicit prompt template file loads through resource inputs", "[cli][project-resources]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace home;
    workspace.write("custom.md",
                    "---\n"
                    "description: Custom greeting\n"
                    "---\n"
                    "Custom hello $1.\n");
    auto session = workspace.path() / "explicit-template.jsonl";

    auto result = run_command_split(
        "HOME=" + shell_quote(home.path()) + " " + bin() +
        " --fake --workspace " + shell_quote(workspace.path()) +
        " --session " + shell_quote(session) +
        " --prompt-template custom.md /custom Ada");

    REQUIRE(result.exit_code == 0);
    CHECK(result.stderr_text.find("template:warn") == std::string::npos);
    CHECK(result.stdout_text.find("Custom hello Ada.") != std::string::npos);
}

TEST_CASE("CLI no-prompt-templates disables explicit prompt template files", "[cli][project-resources]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace home;
    workspace.write("custom.md",
                    "---\n"
                    "description: Custom greeting\n"
                    "---\n"
                    "Custom hello $1.\n");
    auto session = workspace.path() / "explicit-template-disabled.jsonl";

    auto result = run_command_split(
        "HOME=" + shell_quote(home.path()) + " " + bin() +
        " --fake --workspace " + shell_quote(workspace.path()) +
        " --session " + shell_quote(session) +
        " --prompt-template custom.md --no-prompt-templates /custom Ada");

    REQUIRE(result.exit_code == 0);
    CHECK(result.stdout_text.find("Custom hello Ada.") == std::string::npos);
}

TEST_CASE("CLI JSON project trust diagnostics stay on stderr", "[cli][json][project-trust]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace home;
    workspace.write(".cpp-harness/skills/demo/SKILL.md",
                    "---\n"
                    "name: demo\n"
                    "description: Demo skill.\n"
                    "---\n"
                    "# Demo Skill\n\n"
                    "Do demo.\n");
    auto session = workspace.path() / "json-untrusted-skills.jsonl";

    auto result = run_command_split(
        "HOME=" + shell_quote(home.path()) + " " + bin() +
        " --fake --mode json --workspace " + shell_quote(workspace.path()) +
        " --session " + shell_quote(session) + " hello");

    REQUIRE(result.exit_code == 0);
    CHECK(result.stderr_text.find("project_skills skipped: untrusted") != std::string::npos);
    for (const auto& line : non_empty_lines(result.stdout_text)) {
        (void)parse_json_line(line);
    }
}

TEST_CASE("CLI text mode shows malformed project resource diagnostics on stderr", "[cli][project-resources]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace home;
    workspace.write(".cpp-harness/skills/bad/SKILL.md",
                    "---\n"
                    "name: bad\n"
                    "---\n"
                    "Bad skill body.\n");
    workspace.write(".cpp-harness/prompts/bad.md",
                    "---\n"
                    "bad line without colon\n"
                    "---\n"
                    "Bad prompt body.\n");
    auto session = workspace.path() / "malformed-resources.jsonl";

    auto result = run_command_split(
        "HOME=" + shell_quote(home.path()) + " " + bin() +
        " --fake --approve --workspace " + shell_quote(workspace.path()) +
        " --session " + shell_quote(session) + " hello");

    REQUIRE(result.exit_code == 0);
    CHECK(result.stderr_text.find("[skill:warn] invalid_metadata") != std::string::npos);
    CHECK(result.stderr_text.find("[template:warn] parse_failed") != std::string::npos);
    CHECK(result.stdout_text.find("[assistant] fake: hello") != std::string::npos);
}

TEST_CASE("CLI JSON resource diagnostics stay off stdout for malformed resources", "[cli][json][project-resources]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace home;
    workspace.write(".cpp-harness/prompts/bad.md",
                    "---\n"
                    "bad line without colon\n"
                    "---\n"
                    "Bad prompt body.\n");
    auto session = workspace.path() / "json-malformed-resources.jsonl";

    auto result = run_command_split(
        "HOME=" + shell_quote(home.path()) + " " + bin() +
        " --fake --approve --mode json --workspace " + shell_quote(workspace.path()) +
        " --session " + shell_quote(session) + " hello");

    REQUIRE(result.exit_code == 0);
    CHECK(result.stderr_text.find("[template:warn] parse_failed") != std::string::npos);
    for (const auto& line : non_empty_lines(result.stdout_text)) {
        (void)parse_json_line(line);
    }
    CHECK(result.stdout_text.find("[template:warn]") == std::string::npos);
}

TEST_CASE("CLI JSON duplicate resource diagnostics stay off stdout", "[cli][json][project-resources]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace home;
    workspace.write(".cpp-harness/prompts/dupe.md",
                    "---\n"
                    "description: Project duplicate\n"
                    "---\n"
                    "Project duplicate.\n");
    workspace.write("dupe.md",
                    "---\n"
                    "description: Explicit duplicate\n"
                    "---\n"
                    "Explicit duplicate.\n");
    auto session = workspace.path() / "json-duplicate-resources.jsonl";

    auto result = run_command_split(
        "HOME=" + shell_quote(home.path()) + " " + bin() +
        " --fake --approve --mode json --workspace " + shell_quote(workspace.path()) +
        " --session " + shell_quote(session) + " --prompt-template dupe.md hello");

    REQUIRE(result.exit_code == 0);
    CHECK(result.stderr_text.find("[duplicate:info] duplicate_template_skipped") != std::string::npos);
    CHECK(count_occurrences(result.stderr_text, "duplicate_template_skipped") == 1);
    for (const auto& line : non_empty_lines(result.stdout_text)) {
        (void)parse_json_line(line);
    }
    CHECK(result.stdout_text.find("duplicate_template_skipped") == std::string::npos);
}

TEST_CASE("CLI RPC resource diagnostics stay off command stream", "[cli][rpc][project-resources]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace home;
    workspace.write(".cpp-harness/prompts/bad.md",
                    "---\n"
                    "bad line without colon\n"
                    "---\n"
                    "Bad prompt body.\n");
    auto session = workspace.path() / "rpc-malformed-resources.jsonl";
    const std::string input =
        "{\"id\":\"s1\",\"type\":\"get_state\"}\n"
        "{\"id\":\"q1\",\"type\":\"shutdown\"}\n";

    auto result = run_command_split_with_input(
        "HOME=" + shell_quote(home.path()) + " " + bin() +
        " --fake --approve --mode rpc --workspace " + shell_quote(workspace.path()) +
        " --session " + shell_quote(session),
        input);

    REQUIRE(result.exit_code == 0);
    CHECK(result.stderr_text.find("[template:warn] parse_failed") != std::string::npos);
    auto records = parse_json_objects(result.stdout_text);
    REQUIRE(find_response(records, "get_state") != nullptr);
    REQUIRE(find_response(records, "shutdown") != nullptr);
    CHECK(result.stdout_text.find("[template:warn]") == std::string::npos);
}

TEST_CASE("CLI applies settings.json model when CLI omits --model", "[cli][settings]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace home;
    std::filesystem::create_directories(home.path() / ".cpp-harness" / "agent");
    std::ofstream(home.path() / ".cpp-harness" / "agent" / "settings.json") << R"({"model":"config-model-name"})";
    auto session = workspace.path() / "settings-model-session.jsonl";

    auto result = run_command(
        "HOME=" + shell_quote(home.path()) + " " + bin() +
        " --fake --workspace " + shell_quote(workspace.path()) +
        " --session " + shell_quote(session) + " hello");

    REQUIRE(result.exit_code == 0);
    const auto header = parse_json_line(read_file(session));
    const auto& object = as_object(header);
    CHECK(json_string_at(object, "type") == "session");
    CHECK(json_string_at(object, "model") == "config-model-name");
}

TEST_CASE("CLI accepts settings.json api_key_env chain without explicit --api-key-env", "[cli][settings]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace home;
    std::filesystem::create_directories(home.path() / ".cpp-harness" / "agent");
    std::ofstream(home.path() / ".cpp-harness" / "agent" / "settings.json") << R"({"api_key_env":["CUSTOM_KEY"]})";
    auto session = workspace.path() / "settings-key-session.jsonl";

    auto result = run_command(
        "env -u OPENAI_API_KEY CUSTOM_KEY=test HOME=" + shell_quote(home.path()) + " " + bin() +
        " --workspace " + shell_quote(workspace.path()) +
        " --session " + shell_quote(session) + " hello");

    CHECK(result.output.find("missing API key") == std::string::npos);
    CHECK(std::filesystem::exists(session));
}

TEST_CASE("CLI invalid explicit prompt template fails before session creation", "[cli][project-resources][assembly]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace home;
    auto session = workspace.path() / "explicit-fail.jsonl";

    auto result = run_command_split(
        "HOME=" + shell_quote(home.path()) + " " + bin() +
        " --fake --workspace " + shell_quote(workspace.path()) +
        " --session " + shell_quote(session) +
        " --prompt-template missing.md hello");

    REQUIRE(result.exit_code != 0);
    CHECK(result.stdout_text.empty());
    CHECK(result.stderr_text.find("template") != std::string::npos ||
          result.stderr_text.find("explicit") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(session));
}

TEST_CASE("CLI rejects an explicit prompt template with an unsupported file type", "[cli][project-resources][assembly]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace home;
    workspace.write("not-a-template.txt", "plain text\n");
    auto session = workspace.path() / "explicit-type-fail.jsonl";

    auto result = run_command_split(
        "HOME=" + shell_quote(home.path()) + " " + bin() +
        " --fake --workspace " + shell_quote(workspace.path()) +
        " --session " + shell_quote(session) +
        " --prompt-template not-a-template.txt hello");

    REQUIRE(result.exit_code != 0);
    CHECK(result.stdout_text.empty());
    CHECK(result.stderr_text.find("template") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(session));
}

TEST_CASE("CLI rejects each explicit prompt template input that has no loadable templates", "[cli][project-resources][assembly]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace home;
    workspace.write("valid.md", "Valid template.\n");
    workspace.write("empty-prompts/notes.txt", "not a template\n");
    auto session = workspace.path() / "explicit-empty-dir-fail.jsonl";

    auto result = run_command_split(
        "HOME=" + shell_quote(home.path()) + " " + bin() +
        " --fake --workspace " + shell_quote(workspace.path()) +
        " --session " + shell_quote(session) +
        " --prompt-template valid.md --prompt-template empty-prompts hello");

    REQUIRE(result.exit_code != 0);
    CHECK(result.stdout_text.empty());
    CHECK(result.stderr_text.find("no loadable .md files") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(session));
}

TEST_CASE("CLI applies settings.json provider identity when no explicit provider", "[cli][settings][provider-resolution]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace home;
    std::filesystem::create_directories(home.path() / ".cpp-harness" / "agent");
    std::ofstream(home.path() / ".cpp-harness" / "agent" / "settings.json") << R"({"provider":"kimi-coding","model":"kimi-for-coding"})";
    auto session = workspace.path() / "settings-provider-rpc.jsonl";

    auto result = run_command_split_with_input(
        "env -u OPENAI_API_KEY CCH_CONFIG_PROVIDER_KEY=unused HOME=" + shell_quote(home.path()) + " " + bin() +
        " --workspace " + shell_quote(workspace.path()) +
        " --session " + shell_quote(session) +
        " --api-key-env CCH_CONFIG_PROVIDER_KEY --mode rpc",
        "{\"type\":\"get_state\"}\n{\"type\":\"shutdown\"}\n");

    REQUIRE(result.exit_code == 0);
    auto records = parse_json_objects(result.stdout_text);
    auto* state = find_response(records, "get_state");
    REQUIRE(state != nullptr);
    auto data = state->at("data").get<cch::util::JsonValue::object_t>();
    CHECK(json_string_at(data, "provider") == "kimi-coding");
    CHECK(json_string_at(data, "model") == "kimi-for-coding");
}

TEST_CASE("CLI resume with explicit model override reports diagnostic and uses override", "[cli][provider-resolution]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace home;
    auto session = workspace.path() / "resume-override.jsonl";

    auto first = run_command(
        "HOME=" + shell_quote(home.path()) + " " + bin() +
        " --fake --workspace " + shell_quote(workspace.path()) +
        " --session " + shell_quote(session) + " first");
    REQUIRE(first.exit_code == 0);

    auto second = run_command_split_with_input(
        "HOME=" + shell_quote(home.path()) + " " + bin() +
        " --fake --workspace " + shell_quote(workspace.path()) +
        " --resume " + shell_quote(session) +
        " --model override-model --mode rpc",
        "{\"type\":\"get_state\"}\n{\"type\":\"shutdown\"}\n");

    REQUIRE(second.exit_code == 0);
    CHECK(second.stderr_text.find("resume_provider_override") != std::string::npos);
    auto records = parse_json_objects(second.stdout_text);
    auto* state = find_response(records, "get_state");
    REQUIRE(state != nullptr);
    auto data = state->at("data").get<cch::util::JsonValue::object_t>();
    CHECK(json_string_at(data, "model") == "override-model");
}

TEST_CASE("CLI resume without override retains stored provider and model", "[cli][provider-resolution]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace home;
    auto session = workspace.path() / "resume-retain.jsonl";

    auto first = run_command(
        "HOME=" + shell_quote(home.path()) + " " + bin() +
        " --fake --workspace " + shell_quote(workspace.path()) +
        " --session " + shell_quote(session) + " first");
    REQUIRE(first.exit_code == 0);

    auto second = run_command_split_with_input(
        "HOME=" + shell_quote(home.path()) + " " + bin() +
        " --fake --workspace " + shell_quote(workspace.path()) +
        " --resume " + shell_quote(session) + " --mode rpc",
        "{\"type\":\"get_state\"}\n{\"type\":\"shutdown\"}\n");

    REQUIRE(second.exit_code == 0);
    CHECK(second.stderr_text.find("resume_provider_override") == std::string::npos);
    auto records = parse_json_objects(second.stdout_text);
    auto* state = find_response(records, "get_state");
    REQUIRE(state != nullptr);
    auto data = state->at("data").get<cch::util::JsonValue::object_t>();
    CHECK(json_string_at(data, "provider") == "fake");
    CHECK(json_string_at(data, "model") == "fake-model");
}

TEST_CASE("CLI text command is resolved by the adapter and does not reach AgentSession", "[cli][commands]") {
    cch::tests::TempWorkspace workspace;
    auto session = workspace.path() / "command-adapter.jsonl";
    auto result = run_command(
        bin() + " --fake --workspace " + shell_quote(workspace.path()) + " --session " + shell_quote(session) + " /help");

    REQUIRE(result.exit_code == 0);
    CHECK(result.output.find("Available commands:") != std::string::npos);
    CHECK(result.output.find("[model-request]") == std::string::npos);

    const auto content = read_file(session);
    const auto lines = non_empty_lines(content);
    REQUIRE(lines.size() >= 1);
    auto header = parse_json_line(lines.front());
    CHECK(json_string_at(as_object(header), "type") == "session");

    for (std::size_t i = 1; i < lines.size(); ++i) {
        auto record = parse_json_line(lines[i]);
        CHECK(json_string_at(as_object(record), "type") != "message");
    }
}

TEST_CASE("CLI default creation stores the session under the workspace-keyed agent config directory", "[cli][default-session]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace agent_root;
    const auto agent_dir = agent_root.path() / "agent";
    const auto canonical_workspace = std::filesystem::canonical(workspace.path());

    auto result = run_command_split(
        "CCH_CODING_AGENT_DIR=" + shell_quote(agent_dir) + " " + bin() +
        " --fake --workspace " + shell_quote(workspace.path()) + " hello");

    REQUIRE(result.exit_code == 0);
    CHECK(result.stdout_text.find("[assistant] fake: hello") != std::string::npos);

    const auto created = require_single_automatic_session(agent_dir / "sessions", canonical_workspace);
    CHECK(json_string_at(created.header, "provider") == "fake");
    CHECK_FALSE(std::filesystem::exists(workspace.path() / ".cpp-harness" / "sessions"));
}

TEST_CASE("CLI default creation follows the explicit workspace rather than the launch directory", "[cli][default-session]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace launch_dir;
    cch::tests::TempWorkspace agent_root;
    const auto agent_dir = agent_root.path() / "agent";
    const auto canonical_workspace = std::filesystem::canonical(workspace.path());

    auto result = run_command_split(
        "cd " + shell_quote(launch_dir.path()) + " && CCH_CODING_AGENT_DIR=" + shell_quote(agent_dir) + " " + bin() +
        " --fake --workspace " + shell_quote(workspace.path()) + " hello");

    REQUIRE(result.exit_code == 0);
    require_single_automatic_session(agent_dir / "sessions", canonical_workspace);
    CHECK_FALSE(std::filesystem::exists(launch_dir.path() / ".cpp-harness" / "sessions"));
    CHECK_FALSE(std::filesystem::exists(workspace.path() / ".cpp-harness" / "sessions"));
}

TEST_CASE("CLI default creation shares storage across symbolic-link workspace aliases", "[cli][default-session]") {
#if defined(__unix__) || defined(__APPLE__)
    cch::tests::TempWorkspace real;
    cch::tests::TempWorkspace alias_root;
    cch::tests::TempWorkspace agent_root;
    const auto agent_dir = agent_root.path() / "agent";
    const auto alias = alias_root.path() / "workspace-link";
    std::filesystem::create_directory_symlink(std::filesystem::canonical(real.path()), alias);
    const auto canonical_workspace = std::filesystem::canonical(real.path());

    auto direct = run_command_split(
        "CCH_CODING_AGENT_DIR=" + shell_quote(agent_dir) + " " + bin() +
        " --fake --workspace " + shell_quote(real.path()) + " first");
    REQUIRE(direct.exit_code == 0);
    auto aliased = run_command_split(
        "CCH_CODING_AGENT_DIR=" + shell_quote(agent_dir) + " " + bin() +
        " --fake --workspace " + shell_quote(alias) + " second");
    REQUIRE(aliased.exit_code == 0);

    const auto sessions_root = agent_dir / "sessions";
    const auto expected_directory = sessions_root / expected_workspace_key(canonical_workspace);
    std::vector<std::filesystem::path> workspace_directories;
    for (const auto& entry : std::filesystem::directory_iterator(sessions_root)) {
        if (entry.is_directory()) {
            workspace_directories.push_back(entry.path());
        }
    }
    REQUIRE(workspace_directories.size() == 1);
    CHECK(workspace_directories.front() == expected_directory);

    const auto files = jsonl_files_under(sessions_root);
    REQUIRE(files.size() == 2);
    for (const auto& file : files) {
        CHECK(file.parent_path() == expected_directory);
        std::ifstream input(file, std::ios::binary);
        std::string first_line;
        REQUIRE(static_cast<bool>(std::getline(input, first_line)));
        const auto header = as_object(parse_json_line(first_line));
        CHECK(json_string_at(header, "cwd") == canonical_workspace.string());
    }
#else
    SUCCEED("symbolic-link alias coverage requires POSIX directory symlinks");
#endif
}

TEST_CASE("CLI JSON mode propagates the same default persisted target", "[cli][default-session][json]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace agent_root;
    const auto agent_dir = agent_root.path() / "agent";
    const auto canonical_workspace = std::filesystem::canonical(workspace.path());

    auto result = run_command_split(
        "CCH_CODING_AGENT_DIR=" + shell_quote(agent_dir) + " " + bin() +
        " --fake --mode json --workspace " + shell_quote(workspace.path()) + " hello");

    REQUIRE(result.exit_code == 0);
    const auto records = parse_json_objects(result.stdout_text);
    REQUIRE(records.size() > 1);
    CHECK(json_string_at(records.front(), "type") == "session");

    const auto created = require_single_automatic_session(agent_dir / "sessions", canonical_workspace);
    CHECK(json_string_at(records.front(), "id") == created.file_session_id);
}

TEST_CASE("CLI RPC mode propagates the same default persisted target", "[cli][default-session][rpc]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace agent_root;
    const auto agent_dir = agent_root.path() / "agent";
    const auto canonical_workspace = std::filesystem::canonical(workspace.path());

    const std::string input =
        "{\"id\":\"p1\",\"type\":\"prompt\",\"message\":\"hello\"}\n"
        "{\"id\":\"q1\",\"type\":\"shutdown\"}\n";
    auto result = run_command_split_with_input(
        "CCH_CODING_AGENT_DIR=" + shell_quote(agent_dir) + " " + bin() +
        " --fake --mode rpc --workspace " + shell_quote(workspace.path()),
        input);

    REQUIRE(result.exit_code == 0);
    const auto records = parse_json_objects(result.stdout_text);
    REQUIRE(find_response(records, "prompt") != nullptr);
    REQUIRE(find_response(records, "shutdown") != nullptr);
    require_single_automatic_session(agent_dir / "sessions", canonical_workspace);
}

TEST_CASE("CLI text REPL propagates the same default persisted target", "[cli][default-session]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace agent_root;
    const auto agent_dir = agent_root.path() / "agent";
    const auto canonical_workspace = std::filesystem::canonical(workspace.path());

    auto result = run_command_split(
        "printf 'hello\\nexit\\n' | CCH_CODING_AGENT_DIR=" + shell_quote(agent_dir) + " " + bin() +
        " --fake --repl --workspace " + shell_quote(workspace.path()));

    REQUIRE(result.exit_code == 0);
    CHECK(result.stdout_text.find("[assistant] fake: hello") != std::string::npos);
    require_single_automatic_session(agent_dir / "sessions", canonical_workspace);
}

TEST_CASE("CLI explicit session targets keep their exact paths outside the default root", "[cli][default-session]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace agent_root;
    const auto agent_dir = agent_root.path() / "agent";
    const auto explicit_session = workspace.path() / "explicit.jsonl";

    auto created = run_command_split(
        "CCH_CODING_AGENT_DIR=" + shell_quote(agent_dir) + " " + bin() +
        " --fake --workspace " + shell_quote(workspace.path()) + " --session " + shell_quote(explicit_session) + " first");
    REQUIRE(created.exit_code == 0);
    CHECK(std::filesystem::exists(explicit_session));
    CHECK_FALSE(std::filesystem::exists(agent_dir / "sessions"));

    auto resumed = run_command_split(
        "CCH_CODING_AGENT_DIR=" + shell_quote(agent_dir) + " " + bin() +
        " --fake --workspace " + shell_quote(workspace.path()) + " --resume " + shell_quote(explicit_session) + " second");
    REQUIRE(resumed.exit_code == 0);
    CHECK(resumed.stdout_text.find("fake: second") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(agent_dir / "sessions"));
}

TEST_CASE("CLI default creation ignores the old project-local sessions directory", "[cli][default-session][legacy]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace agent_root;
    const auto agent_dir = agent_root.path() / "agent";
    const auto legacy_dir = workspace.path() / ".cpp-harness" / "sessions";
    std::filesystem::create_directories(legacy_dir);
    const auto legacy_file = legacy_dir / "legacy.jsonl";
    const auto canonical_workspace = std::filesystem::canonical(workspace.path());

    auto seed = run_command_split(
        "CCH_CODING_AGENT_DIR=" + shell_quote(agent_dir) + " " + bin() +
        " --fake --workspace " + shell_quote(workspace.path()) + " --session " + shell_quote(legacy_file) + " legacy-seed");
    REQUIRE(seed.exit_code == 0);
    const auto legacy_before = read_file(legacy_file);

    auto result = run_command_split(
        "CCH_CODING_AGENT_DIR=" + shell_quote(agent_dir) + " " + bin() +
        " --fake --workspace " + shell_quote(workspace.path()) + " hello");
    REQUIRE(result.exit_code == 0);
    CHECK(read_file(legacy_file) == legacy_before);
    CHECK(jsonl_files_under(legacy_dir).size() == 1);
    require_single_automatic_session(agent_dir / "sessions", canonical_workspace);

    // A valid old file remains usable only through the explicit resume contract.
    auto resumed = run_command_split(
        "CCH_CODING_AGENT_DIR=" + shell_quote(agent_dir) + " " + bin() +
        " --fake --workspace " + shell_quote(workspace.path()) + " --resume " + shell_quote(legacy_file) + " second");
    REQUIRE(resumed.exit_code == 0);
    CHECK(resumed.stdout_text.find("fake: second") != std::string::npos);
    CHECK(read_file(legacy_file) != legacy_before);
    CHECK(jsonl_files_under(agent_dir / "sessions").size() == 1);
}

TEST_CASE("CLI default creation fails explicitly when default storage is unsafe", "[cli][default-session]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace blocker_root;
    const auto blocker = blocker_root.path() / "not-a-directory";
    {
        std::ofstream output(blocker, std::ios::binary);
        output << "regular file";
    }

    auto result = run_command_split(
        "CCH_CODING_AGENT_DIR=" + shell_quote(blocker) + " " + bin() +
        " --fake --workspace " + shell_quote(workspace.path()) + " hello");

    REQUIRE(result.exit_code != 0);
    CHECK(result.stdout_text.empty());
    CHECK(result.stderr_text.find("could not") != std::string::npos);
    CHECK(result.stderr_text.find(blocker.string()) != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(workspace.path() / ".cpp-harness" / "sessions"));
}

TEST_CASE("CLI default creation fails explicitly when no user-level root can be resolved", "[cli][default-session]") {
    cch::tests::TempWorkspace workspace;

    auto result = run_command_split(
        "env -u HOME -u USERPROFILE -u CCH_CODING_AGENT_DIR " + bin() +
        " --fake --workspace " + shell_quote(workspace.path()) + " hello");

    REQUIRE(result.exit_code != 0);
    CHECK(result.stdout_text.empty());
    CHECK(result.stderr_text.find("Agent Config Directory sessions root could not be resolved") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(workspace.path() / ".cpp-harness" / "sessions"));
}

TEST_CASE("CLI help describes automatic user-level session storage", "[cli][default-session]") {
    auto result = run_command(bin() + " --help");

    REQUIRE(result.exit_code == 0);
    CHECK(result.output.find("CCH_CODING_AGENT_DIR") != std::string::npos);
    CHECK(result.output.find("sessions") != std::string::npos);
    CHECK(result.output.find("--session-id") == std::string::npos);
}

TEST_CASE("CLI failed assembly publishes no default session file", "[cli][default-session][assembly]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace agent_root;
    const auto agent_dir = agent_root.path() / "agent";

    auto result = run_command_split(
        "CCH_CODING_AGENT_DIR=" + shell_quote(agent_dir) + " " + bin() +
        " --fake --workspace " + shell_quote(workspace.path()) +
        " --prompt-template missing.md hello");

    REQUIRE(result.exit_code != 0);
    CHECK(result.stdout_text.empty());
    CHECK(result.stderr_text.find("template") != std::string::npos ||
          result.stderr_text.find("explicit") != std::string::npos);
    CHECK(jsonl_files_under(agent_dir / "sessions").empty());
    CHECK_FALSE(std::filesystem::exists(workspace.path() / ".cpp-harness" / "sessions"));
}

/// An explicit in-memory run must publish nothing: no agent config sessions
/// root, no workspace-local sessions directory, and no transcript file.
void require_no_session_filesystem_state(
    const std::filesystem::path& agent_dir,
    const std::filesystem::path& workspace) {
    CHECK_FALSE(std::filesystem::exists(agent_dir / "sessions"));
    CHECK_FALSE(std::filesystem::exists(workspace / ".cpp-harness" / "sessions"));
    CHECK(jsonl_files_under(agent_dir).empty());
    CHECK(jsonl_files_under(workspace).empty());
}

TEST_CASE("CLI --no-session runs a text prompt without publishing session state", "[cli][no-session]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace agent_root;
    const auto agent_dir = agent_root.path() / "agent";

    auto result = run_command_split(
        "CCH_CODING_AGENT_DIR=" + shell_quote(agent_dir) + " " + bin() +
        " --fake --no-session --workspace " + shell_quote(workspace.path()) + " hello");

    REQUIRE(result.exit_code == 0);
    CHECK(result.stdout_text.find("[model-request]") != std::string::npos);
    CHECK(result.stdout_text.find("[assistant] fake: hello") != std::string::npos);
    CHECK(result.stdout_text.find("[completed]") != std::string::npos);
    require_no_session_filesystem_state(agent_dir, workspace.path());
}

TEST_CASE("CLI --no-session JSON mode propagates the in-memory target", "[cli][no-session][json]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace agent_root;
    const auto agent_dir = agent_root.path() / "agent";

    auto result = run_command_split(
        "CCH_CODING_AGENT_DIR=" + shell_quote(agent_dir) + " " + bin() +
        " --fake --no-session --mode json --workspace " + shell_quote(workspace.path()) + " hello");

    REQUIRE(result.exit_code == 0);
    const auto records = parse_json_objects(result.stdout_text);
    REQUIRE(records.size() > 1);
    CHECK(json_string_at(records.front(), "type") == "session");
    CHECK(has_json_event_type(non_empty_lines(result.stdout_text), "agent_end"));
    require_no_session_filesystem_state(agent_dir, workspace.path());
}

TEST_CASE("CLI --no-session RPC mode propagates the in-memory target", "[cli][no-session][rpc]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace agent_root;
    const auto agent_dir = agent_root.path() / "agent";

    const std::string input =
        "{\"id\":\"p1\",\"type\":\"prompt\",\"message\":\"hello\"}\n"
        "{\"id\":\"s1\",\"type\":\"get_state\"}\n"
        "{\"id\":\"q1\",\"type\":\"shutdown\"}\n";
    auto result = run_command_split_with_input(
        "CCH_CODING_AGENT_DIR=" + shell_quote(agent_dir) + " " + bin() +
        " --fake --no-session --mode rpc --workspace " + shell_quote(workspace.path()),
        input);

    REQUIRE(result.exit_code == 0);
    const auto records = parse_json_objects(result.stdout_text);
    REQUIRE(find_response(records, "prompt") != nullptr);
    REQUIRE(find_response(records, "shutdown") != nullptr);
    const auto* state = find_response(records, "get_state");
    REQUIRE(state != nullptr);
    const auto& data = as_object(state->at("data"));
    // In-memory sessions keep a real Session ID but never report a file.
    CHECK_FALSE(json_string_at(data, "sessionId").empty());
    CHECK_FALSE(data.contains("sessionFile"));
    CHECK(data.at("messageCount").get<double>() == 2);
    require_no_session_filesystem_state(agent_dir, workspace.path());
}

TEST_CASE("CLI --no-session text REPL propagates the in-memory target", "[cli][no-session]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace agent_root;
    const auto agent_dir = agent_root.path() / "agent";

    auto result = run_command_split(
        "printf 'hello\\n/session\\nexit\\n' | CCH_CODING_AGENT_DIR=" + shell_quote(agent_dir) + " " + bin() +
        " --fake --no-session --repl --workspace " + shell_quote(workspace.path()));

    REQUIRE(result.exit_code == 0);
    CHECK(result.stdout_text.find("[assistant] fake: hello") != std::string::npos);
    // /session names the in-memory state instead of an ambiguous empty path.
    CHECK(result.stdout_text.find("File: In-memory") != std::string::npos);
    require_no_session_filesystem_state(agent_dir, workspace.path());
}

TEST_CASE("CLI /session shows the persisted file for a default session", "[cli][no-session]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace agent_root;
    const auto agent_dir = agent_root.path() / "agent";
    const auto canonical_workspace = std::filesystem::canonical(workspace.path());

    auto result = run_command_split(
        "CCH_CODING_AGENT_DIR=" + shell_quote(agent_dir) + " " + bin() +
        " --fake --workspace " + shell_quote(workspace.path()) + " /session");

    REQUIRE(result.exit_code == 0);
    CHECK(result.stdout_text.find("[model-request]") == std::string::npos);
    const auto created = require_single_automatic_session(agent_dir / "sessions", canonical_workspace);
    CHECK(result.stdout_text.find("File: " + created.path.string()) != std::string::npos);
    CHECK(result.stdout_text.find("In-memory") == std::string::npos);
}

TEST_CASE("CLI rejects --no-session combined with explicit create or resume before model work", "[cli][no-session]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace agent_root;
    const auto agent_dir = agent_root.path() / "agent";
    const auto explicit_session = workspace.path() / "explicit.jsonl";

    auto created = run_command_split(
        "CCH_CODING_AGENT_DIR=" + shell_quote(agent_dir) + " " + bin() +
        " --fake --no-session --session " + shell_quote(explicit_session) +
        " --workspace " + shell_quote(workspace.path()) + " hello");
    REQUIRE(created.exit_code != 0);
    CHECK(created.stdout_text.empty());
    CHECK(created.stderr_text.find("--no-session cannot be combined with --session") != std::string::npos);

    auto resumed = run_command_split(
        "CCH_CODING_AGENT_DIR=" + shell_quote(agent_dir) + " " + bin() +
        " --fake --no-session --resume " + shell_quote(explicit_session) +
        " --workspace " + shell_quote(workspace.path()) + " hello");
    REQUIRE(resumed.exit_code != 0);
    CHECK(resumed.stdout_text.empty());
    CHECK(resumed.stderr_text.find("--no-session cannot be combined with --resume") != std::string::npos);

    require_no_session_filesystem_state(agent_dir, workspace.path());
}

TEST_CASE("CLI --no-session does not consult default storage", "[cli][no-session]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace blocker_root;
    const auto blocker = blocker_root.path() / "not-a-directory";
    {
        std::ofstream output(blocker, std::ios::binary);
        output << "regular file";
    }

    auto result = run_command_split(
        "CCH_CODING_AGENT_DIR=" + shell_quote(blocker) + " " + bin() +
        " --fake --no-session --workspace " + shell_quote(workspace.path()) + " hello");

    // In-memory operation is explicit, so unusable default storage is never
    // inspected and cannot fail or redirect the run.
    REQUIRE(result.exit_code == 0);
    CHECK(result.stdout_text.find("[assistant] fake: hello") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(workspace.path() / ".cpp-harness" / "sessions"));
}

TEST_CASE("CLI --no-session preserves tool execution and events", "[cli][no-session]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace agent_root;
    const auto agent_dir = agent_root.path() / "agent";
    std::ofstream(workspace.path() / "note.txt") << "in-memory tool text";

    auto result = run_command_split(
        "CCH_CODING_AGENT_DIR=" + shell_quote(agent_dir) + " " + bin() +
        " --fake --no-session --workspace " + shell_quote(workspace.path()) + " 'read note.txt'");

    REQUIRE(result.exit_code == 0);
    CHECK(count_occurrences(result.stdout_text, "[tool-call] read#fake-read-1") == 1);
    CHECK(count_occurrences(result.stdout_text, "[tool-success] fake-read-1") == 1);
    CHECK(result.stdout_text.find("[assistant] fake observed: in-memory tool text") != std::string::npos);
    require_no_session_filesystem_state(agent_dir, workspace.path());
}

TEST_CASE("CLI --no-session publishes no filesystem state after a startup failure", "[cli][no-session][assembly]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace agent_root;
    const auto agent_dir = agent_root.path() / "agent";

    auto result = run_command_split(
        "CCH_CODING_AGENT_DIR=" + shell_quote(agent_dir) + " " + bin() +
        " --fake --no-session --workspace " + shell_quote(workspace.path()) +
        " --prompt-template missing.md hello");

    REQUIRE(result.exit_code != 0);
    CHECK(result.stdout_text.empty());
    require_no_session_filesystem_state(agent_dir, workspace.path());
}

TEST_CASE("CLI --session-dir redirects automatic storage for one run", "[cli][session-dir]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace agent_root;
    cch::tests::TempWorkspace override_root;
    const auto agent_dir = agent_root.path() / "agent";
    const auto override_dir = override_root.path() / "sessions";
    const auto canonical_workspace = std::filesystem::canonical(workspace.path());

    auto result = run_command_split(
        "CCH_CODING_AGENT_DIR=" + shell_quote(agent_dir) + " " + bin() +
        " --fake --workspace " + shell_quote(workspace.path()) +
        " --session-dir " + shell_quote(override_dir) + " hello");

    REQUIRE(result.exit_code == 0);
    CHECK(result.stdout_text.find("[assistant] fake: hello") != std::string::npos);

    const auto created = require_single_session_in_directory(override_dir, canonical_workspace);
    CHECK(json_string_at(created.header, "provider") == "fake");
    CHECK_FALSE(std::filesystem::exists(agent_dir / "sessions"));
    CHECK_FALSE(std::filesystem::exists(workspace.path() / ".cpp-harness" / "sessions"));
}

TEST_CASE("CLI session directory precedence is flag over environment over settings", "[cli][session-dir]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace agent_root;
    cch::tests::TempWorkspace flag_root;
    cch::tests::TempWorkspace env_root;
    cch::tests::TempWorkspace settings_root;
    const auto agent_dir = agent_root.path() / "agent";
    std::filesystem::create_directories(agent_dir);
    const auto flag_dir = flag_root.path() / "flag-sessions";
    const auto env_dir = env_root.path() / "env-sessions";
    const auto settings_dir = settings_root.path() / "settings-sessions";
    {
        std::ofstream settings(agent_dir / "settings.json");
        settings << "{\"sessionDir\":\"" << settings_dir.string() << "\"}";
    }

    auto flagged = run_command_split(
        "CCH_CODING_AGENT_DIR=" + shell_quote(agent_dir) +
        " CCH_CODING_AGENT_SESSION_DIR=" + shell_quote(env_dir) + " " + bin() +
        " --fake --workspace " + shell_quote(workspace.path()) +
        " --session-dir " + shell_quote(flag_dir) + " first");
    REQUIRE(flagged.exit_code == 0);
    CHECK(jsonl_files_under(flag_dir).size() == 1);
    CHECK_FALSE(std::filesystem::exists(env_dir));
    CHECK_FALSE(std::filesystem::exists(settings_dir));

    auto from_env = run_command_split(
        "CCH_CODING_AGENT_DIR=" + shell_quote(agent_dir) +
        " CCH_CODING_AGENT_SESSION_DIR=" + shell_quote(env_dir) + " " + bin() +
        " --fake --workspace " + shell_quote(workspace.path()) + " second");
    REQUIRE(from_env.exit_code == 0);
    CHECK(jsonl_files_under(env_dir).size() == 1);
    CHECK_FALSE(std::filesystem::exists(settings_dir));

    auto from_settings = run_command_split(
        "CCH_CODING_AGENT_DIR=" + shell_quote(agent_dir) + " " + bin() +
        " --fake --workspace " + shell_quote(workspace.path()) + " third");
    REQUIRE(from_settings.exit_code == 0);
    CHECK(jsonl_files_under(settings_dir).size() == 1);
    CHECK_FALSE(std::filesystem::exists(agent_dir / "sessions"));
}

TEST_CASE("CLI relative --session-dir resolves against the final workspace", "[cli][session-dir]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace launch_dir;
    cch::tests::TempWorkspace agent_root;
    const auto agent_dir = agent_root.path() / "agent";
    const auto canonical_workspace = std::filesystem::canonical(workspace.path());

    auto result = run_command_split(
        "cd " + shell_quote(launch_dir.path()) + " && CCH_CODING_AGENT_DIR=" + shell_quote(agent_dir) + " " + bin() +
        " --fake --workspace " + shell_quote(workspace.path()) +
        " --session-dir my-sessions hello");

    REQUIRE(result.exit_code == 0);
    require_single_session_in_directory(canonical_workspace / "my-sessions", canonical_workspace);
    CHECK_FALSE(std::filesystem::exists(launch_dir.path() / "my-sessions"));
    CHECK_FALSE(std::filesystem::exists(agent_dir / "sessions"));

    // The workspace itself determines resolution: the same relative value from
    // another launch directory lands in the same workspace-relative place.
    auto again = run_command_split(
        "cd " + shell_quote(agent_root.path()) + " && CCH_CODING_AGENT_DIR=" + shell_quote(agent_dir) + " " + bin() +
        " --fake --workspace " + shell_quote(workspace.path()) +
        " --session-dir my-sessions again");
    REQUIRE(again.exit_code == 0);
    CHECK(jsonl_files_under(canonical_workspace / "my-sessions").size() == 2);
    CHECK_FALSE(std::filesystem::exists(agent_root.path() / "my-sessions"));
}

TEST_CASE("CLI session directory override expands a leading home marker", "[cli][session-dir]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace agent_root;
    cch::tests::TempWorkspace home_root;
    const auto agent_dir = agent_root.path() / "agent";
    const auto canonical_workspace = std::filesystem::canonical(workspace.path());

    auto result = run_command_split(
        "HOME=" + shell_quote(home_root.path()) +
        " CCH_CODING_AGENT_DIR=" + shell_quote(agent_dir) + " " + bin() +
        " --fake --workspace " + shell_quote(workspace.path()) +
        " --session-dir '~/tilde-sessions' hello");

    REQUIRE(result.exit_code == 0);
    require_single_session_in_directory(home_root.path() / "tilde-sessions", canonical_workspace);
    CHECK_FALSE(std::filesystem::exists(canonical_workspace / "~" / "tilde-sessions"));
    CHECK_FALSE(std::filesystem::exists(agent_dir / "sessions"));
}

TEST_CASE("CLI explicit create and resume targets ignore session directory overrides", "[cli][session-dir]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace agent_root;
    cch::tests::TempWorkspace override_root;
    const auto agent_dir = agent_root.path() / "agent";
    const auto override_dir = override_root.path() / "sessions";
    const auto explicit_session = workspace.path() / "explicit.jsonl";

    auto created = run_command_split(
        "CCH_CODING_AGENT_DIR=" + shell_quote(agent_dir) + " " + bin() +
        " --fake --workspace " + shell_quote(workspace.path()) +
        " --session-dir " + shell_quote(override_dir) +
        " --session " + shell_quote(explicit_session) + " first");
    REQUIRE(created.exit_code == 0);
    CHECK(std::filesystem::exists(explicit_session));
    CHECK_FALSE(std::filesystem::exists(override_dir));

    auto resumed = run_command_split(
        "CCH_CODING_AGENT_DIR=" + shell_quote(agent_dir) +
        " CCH_CODING_AGENT_SESSION_DIR=" + shell_quote(override_dir) + " " + bin() +
        " --fake --workspace " + shell_quote(workspace.path()) +
        " --resume " + shell_quote(explicit_session) + " second");
    REQUIRE(resumed.exit_code == 0);
    CHECK(resumed.stdout_text.find("fake: second") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(override_dir));
    CHECK_FALSE(std::filesystem::exists(agent_dir / "sessions"));
}

TEST_CASE("CLI --no-session ignores session directory overrides and publishes nothing", "[cli][session-dir]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace agent_root;
    cch::tests::TempWorkspace override_root;
    const auto agent_dir = agent_root.path() / "agent";
    const auto override_dir = override_root.path() / "sessions";

    auto result = run_command_split(
        "CCH_CODING_AGENT_DIR=" + shell_quote(agent_dir) + " " + bin() +
        " --fake --no-session --workspace " + shell_quote(workspace.path()) +
        " --session-dir " + shell_quote(override_dir) + " hello");

    REQUIRE(result.exit_code == 0);
    CHECK(result.stdout_text.find("[assistant] fake: hello") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(override_dir));
    CHECK(jsonl_files_under(agent_dir).empty());
    CHECK(jsonl_files_under(workspace.path()).empty());
}

TEST_CASE("CLI ignores a non-string settings sessionDir and uses the default root", "[cli][session-dir]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace agent_root;
    const auto agent_dir = agent_root.path() / "agent";
    std::filesystem::create_directories(agent_dir);
    const auto canonical_workspace = std::filesystem::canonical(workspace.path());
    {
        std::ofstream settings(agent_dir / "settings.json");
        settings << "{\"sessionDir\":42}";
    }

    auto result = run_command_split(
        "CCH_CODING_AGENT_DIR=" + shell_quote(agent_dir) + " " + bin() +
        " --fake --workspace " + shell_quote(workspace.path()) + " hello");

    REQUIRE(result.exit_code == 0);
    require_single_automatic_session(agent_dir / "sessions", canonical_workspace);
}

TEST_CASE("CLI malformed settings keep default session storage with a warning", "[cli][session-dir]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace agent_root;
    const auto agent_dir = agent_root.path() / "agent";
    std::filesystem::create_directories(agent_dir);
    const auto canonical_workspace = std::filesystem::canonical(workspace.path());
    {
        std::ofstream settings(agent_dir / "settings.json");
        settings << "{not valid json";
    }

    auto result = run_command_split(
        "CCH_CODING_AGENT_DIR=" + shell_quote(agent_dir) + " " + bin() +
        " --fake --workspace " + shell_quote(workspace.path()) + " hello");

    REQUIRE(result.exit_code == 0);
    CHECK(result.stderr_text.find("could not load user settings") != std::string::npos);
    require_single_automatic_session(agent_dir / "sessions", canonical_workspace);
}

TEST_CASE("CLI unavailable session directory override fails explicitly without fallback", "[cli][session-dir]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace agent_root;
    cch::tests::TempWorkspace blocker_root;
    const auto agent_dir = agent_root.path() / "agent";
    const auto blocker = blocker_root.path() / "not-a-directory";
    {
        std::ofstream output(blocker, std::ios::binary);
        output << "regular file";
    }

    auto result = run_command_split(
        "CCH_CODING_AGENT_DIR=" + shell_quote(agent_dir) + " " + bin() +
        " --fake --workspace " + shell_quote(workspace.path()) +
        " --session-dir " + shell_quote(blocker) + " hello");

    REQUIRE(result.exit_code != 0);
    CHECK(result.stdout_text.empty());
    CHECK(result.stderr_text.find("could not") != std::string::npos);
    CHECK(result.stderr_text.find(blocker.string()) != std::string::npos);
    // No fallback: neither the default root nor the workspace gains a transcript.
    CHECK_FALSE(std::filesystem::exists(agent_dir / "sessions"));
    CHECK_FALSE(std::filesystem::exists(workspace.path() / ".cpp-harness" / "sessions"));
}
