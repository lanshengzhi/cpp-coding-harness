#include "../../third_party/catch2/catch_test_macros.hpp"

#include "../support/TempWorkspace.hpp"

#include "util/Json.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/wait.h>
#endif

#ifndef CCH_BINARY
#define CCH_BINARY "./cpp_harness"
#endif

namespace {
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

std::string q(const std::filesystem::path& path) {
    return "'" + path.string() + "'";
}

std::string bin() { return q(CCH_BINARY); }

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

std::size_t count_occurrences(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) {
        return 0;
    }
    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string_view::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
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
}

TEST_CASE("CLI fake one-shot prints transcript and writes session", "[cli][u6]") {
    cch::tests::TempWorkspace workspace;
    auto session = workspace.path() / "one-shot.jsonl";
    auto result = run_command(bin() + " --fake --workspace " + q(workspace.path()) + " --session " + q(session) + " hello");

    REQUIRE(result.exit_code == 0);
    CHECK(result.output.find("[model-request]") != std::string::npos);
    CHECK(result.output.find("[assistant] fake: hello") != std::string::npos);
    CHECK(result.output.find("[completed]") != std::string::npos);
    CHECK(std::filesystem::exists(session));
}

TEST_CASE("CLI fake one-shot streams through the current event path", "[cli][u8][ae5]") {
    cch::tests::TempWorkspace workspace;
    auto session = workspace.path() / "event-path.jsonl";
    auto result = run_command(bin() + " --fake --workspace " + q(workspace.path()) + " --session " + q(session) + " hello");

    REQUIRE(result.exit_code == 0);
    CHECK(result.output.find("[model-request] turn 1") != std::string::npos);
    CHECK(result.output.find("[assistant] fake: hello") != std::string::npos);
    CHECK(result.output.find("[completed] stop") != std::string::npos);
    CHECK(std::filesystem::exists(session));
}

TEST_CASE("CLI rejects removed async compatibility flag before model request", "[cli][u8]") {
    cch::tests::TempWorkspace workspace;
    auto session = workspace.path() / "removed-async.jsonl";
    auto result = run_command(bin() + " --async --fake --workspace " + q(workspace.path()) + " --session " + q(session) + " hello");

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
    auto result = run_command_split(bin() + " --fake --mode json --repl --workspace " + q(workspace.path()) + " --session " + q(session));

    REQUIRE(result.exit_code != 0);
    CHECK(result.stdout_text.empty());
    CHECK(result.stderr_text.find("--mode json cannot be combined with --repl") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(session));
}

TEST_CASE("CLI rejects RPC positional prompt before session creation", "[cli][rpc]") {
    cch::tests::TempWorkspace workspace;
    auto session = workspace.path() / "rpc-positional.jsonl";
    auto result = run_command_split(bin() + " --fake --mode rpc --workspace " + q(workspace.path()) + " --session " + q(session) + " hello");

    REQUIRE(result.exit_code != 0);
    CHECK(result.stdout_text.empty());
    CHECK(result.stderr_text.find("--mode rpc reads prompts from stdin") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(session));
}

TEST_CASE("CLI rejects RPC repl before session creation", "[cli][rpc]") {
    cch::tests::TempWorkspace workspace;
    auto session = workspace.path() / "rpc-repl.jsonl";
    auto result = run_command_split(bin() + " --fake --mode rpc --repl --workspace " + q(workspace.path()) + " --session " + q(session));

    REQUIRE(result.exit_code != 0);
    CHECK(result.stdout_text.empty());
    CHECK(result.stderr_text.find("--mode rpc cannot be combined with --repl") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(session));
}

TEST_CASE("CLI JSON fake one-shot emits JSONL only", "[cli][json]") {
    cch::tests::TempWorkspace workspace;
    auto session = workspace.path() / "json-one-shot.jsonl";
    auto result = run_command_split(bin() + " --fake --mode json --workspace " + q(workspace.path()) + " --session " + q(session) + " hello");

    REQUIRE(result.exit_code == 0);
    CHECK(result.stderr_text.empty());
    auto output_lines = non_empty_lines(result.stdout_text);
    REQUIRE(output_lines.size() >= 5);
    for (const auto& line : output_lines) {
        (void)parse_json_line(line);
    }
    auto first = as_object(parse_json_line(output_lines.front()));
    CHECK(json_string_at(first, "type") == "session");
    CHECK(json_string_at(first, "id").empty() == false);
    CHECK(has_json_event_type(output_lines, "turn_start"));
    CHECK(has_json_event_type(output_lines, "message_update"));
    auto last = as_object(parse_json_line(output_lines.back()));
    CHECK(json_string_at(last, "type") == "runtime_terminal");
    CHECK(json_string_at(last, "code") == "completed");
    CHECK(last.at("success").get<bool>() == true);
    CHECK(result.stdout_text.find("[assistant]") == std::string::npos);
    CHECK(std::filesystem::exists(session));
}

TEST_CASE("CLI JSON fake tool flow emits correlated tool events", "[cli][json]") {
    cch::tests::TempWorkspace workspace;
    auto session = workspace.path() / "json-tool.jsonl";
    auto result = run_command_split(bin() + " --fake --mode json --workspace " + q(workspace.path()) + " --session " + q(session) + " read missing.txt");

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
            CHECK(record.find("content") == record.end());
        }
    }
    CHECK(saw_tool_end);
    auto last = as_object(parse_json_line(output_lines.back()));
    CHECK(json_string_at(last, "type") == "runtime_terminal");
    CHECK(last.at("success").get<bool>() == true);
}

TEST_CASE("CLI JSON max-turn flow emits terminal error code", "[cli][json]") {
    cch::tests::TempWorkspace workspace;
    auto session = workspace.path() / "json-max-turn.jsonl";
    auto result = run_command_split(bin() + " --fake --mode json --workspace " + q(workspace.path()) + " --session " + q(session) + " --max-turns 1 read missing.txt");

    REQUIRE(result.exit_code != 0);
    auto output_lines = non_empty_lines(result.stdout_text);
    REQUIRE(output_lines.size() >= 2);
    for (const auto& line : output_lines) {
        (void)parse_json_line(line);
    }
    auto last = as_object(parse_json_line(output_lines.back()));
    CHECK(json_string_at(last, "type") == "runtime_terminal");
    CHECK(json_string_at(last, "code") == "max_turns_exceeded");
    CHECK(last.at("success").get<bool>() == false);
}

TEST_CASE("CLI RPC fake command loop drives a session with JSONL responses and events", "[cli][rpc]") {
    cch::tests::TempWorkspace workspace;
    auto session = workspace.path() / "rpc-loop.jsonl";
    const std::string input =
        "{\"id\":\"s1\",\"type\":\"get_state\"}\n"
        "{\"id\":\"p1\",\"type\":\"prompt\",\"message\":\"hello\"}\n"
        "{\"id\":\"l1\",\"type\":\"get_last_assistant_text\"}\n"
        "{\"id\":\"q1\",\"type\":\"shutdown\"}\n";
    auto result = run_command_split_with_input(
        bin() + " --fake --mode rpc --workspace " + q(workspace.path()) + " --session " + q(session),
        input);

    REQUIRE(result.exit_code == 0);
    CHECK(result.stderr_text.empty());
    auto records = parse_json_objects(result.stdout_text);
    REQUIRE(records.size() >= 6);
    auto* state = find_response(records, "get_state");
    REQUIRE(state != nullptr);
    CHECK(state->at("success").get<bool>() == true);
    CHECK(json_string_at(*state, "id") == "s1");
    auto state_data = state->at("data").get<cch::util::JsonValue::object_t>();
    CHECK(json_string_at(state_data, "provider") == "fake");
    CHECK(state_data.find("baseUrl") == state_data.end());
    CHECK(state_data.find("apiKeyEnv") == state_data.end());
    CHECK(state_data.find("sessionPath") == state_data.end());

    REQUIRE(find_response(records, "prompt") != nullptr);
    CHECK(has_json_event_type(non_empty_lines(result.stdout_text), "message_update"));
    CHECK(has_json_event_type(non_empty_lines(result.stdout_text), "runtime_terminal"));
    auto* last_text = find_response(records, "get_last_assistant_text");
    REQUIRE(last_text != nullptr);
    auto last_data = last_text->at("data").get<cch::util::JsonValue::object_t>();
    CHECK(json_string_at(last_data, "text") == "fake: hello");
    REQUIRE(find_response(records, "shutdown") != nullptr);
    CHECK(result.stdout_text.find("[assistant]") == std::string::npos);
    CHECK(std::filesystem::exists(session));
}

TEST_CASE("CLI RPC exits cleanly on EOF without shutdown", "[cli][rpc]") {
    cch::tests::TempWorkspace workspace;
    auto session = workspace.path() / "rpc-eof.jsonl";
    auto result = run_command_split_with_input(
        bin() + " --fake --mode rpc --workspace " + q(workspace.path()) + " --session " + q(session),
        "{\"id\":\"s1\",\"type\":\"get_state\"}");

    REQUIRE(result.exit_code == 0);
    CHECK(result.stderr_text.empty());
    auto records = parse_json_objects(result.stdout_text);
    REQUIRE(records.size() == 1);
    CHECK(json_string_at(records.front(), "command") == "get_state");
}

TEST_CASE("CLI RPC parse and validation errors are recoverable responses", "[cli][rpc]") {
    cch::tests::TempWorkspace workspace;
    auto session = workspace.path() / "rpc-errors.jsonl";
    const std::string input =
        "{bad json}\n"
        "[]\n"
        "{}\n"
        "{\"id\":5,\"type\":\"get_state\"}\n"
        "{\"id\":\"p1\",\"type\":\"prompt\",\"message\":\"\"}\n"
        "{\"id\":\"u1\",\"type\":\"set_model\"}\n"
        "{\"id\":\"s1\",\"type\":\"get_state\"}\n";
    auto result = run_command_split_with_input(
        bin() + " --fake --mode rpc --workspace " + q(workspace.path()) + " --session " + q(session),
        input);

    REQUIRE(result.exit_code == 0);
    CHECK(result.stderr_text.empty());
    auto records = parse_json_objects(result.stdout_text);
    REQUIRE(records.size() == 7);
    CHECK(json_string_at(records[0], "command") == "parse");
    CHECK(records[0].at("success").get<bool>() == false);
    CHECK(json_string_at(records[1], "command") == "invalid");
    CHECK(json_string_at(records[2], "command") == "invalid");
    CHECK(json_string_at(records[3], "command") == "get_state");
    CHECK(records[3].at("success").get<bool>() == false);
    CHECK(json_string_at(records[4], "command") == "prompt");
    CHECK(records[4].at("success").get<bool>() == false);
    CHECK(json_string_at(records[5], "command") == "set_model");
    CHECK(records[5].at("success").get<bool>() == false);
    CHECK(json_string_at(records[6], "command") == "get_state");
    CHECK(records[6].at("success").get<bool>() == true);
}

TEST_CASE("CLI RPC framing treats CRLF and Unicode separators as record content", "[cli][rpc]") {
    cch::tests::TempWorkspace workspace;
    auto session = workspace.path() / "rpc-framing.jsonl";
    const std::string input =
        "{\"id\":\"p1\",\"type\":\"prompt\",\"message\":\"hello\\u2028there\"}\r\n"
        "{\"id\":\"q1\",\"type\":\"shutdown\"}\r\n";
    auto result = run_command_split_with_input(
        bin() + " --fake --mode rpc --workspace " + q(workspace.path()) + " --session " + q(session),
        input);

    REQUIRE(result.exit_code == 0);
    CHECK(result.stderr_text.empty());
    auto records = parse_json_objects(result.stdout_text);
    REQUIRE(find_response(records, "prompt") != nullptr);
    REQUIRE(find_response(records, "shutdown") != nullptr);
    CHECK(has_json_event_type(non_empty_lines(result.stdout_text), "message_update"));
}

TEST_CASE("CLI RPC event sequence is process-global across prompts", "[cli][rpc]") {
    cch::tests::TempWorkspace workspace;
    auto session = workspace.path() / "rpc-seq.jsonl";
    const std::string input =
        "{\"type\":\"prompt\",\"message\":\"one\"}\n"
        "{\"type\":\"prompt\",\"message\":\"two\"}\n";
    auto result = run_command_split_with_input(
        bin() + " --fake --mode rpc --workspace " + q(workspace.path()) + " --session " + q(session),
        input);

    REQUIRE(result.exit_code == 0);
    CHECK(result.stderr_text.empty());
    int previous = 0;
    int event_count = 0;
    for (const auto& record : parse_json_objects(result.stdout_text)) {
        if (record.find("seq") == record.end()) {
            continue;
        }
        const auto seq = static_cast<int>(record.at("seq").get<double>());
        CHECK(seq > previous);
        previous = seq;
        ++event_count;
    }
    CHECK(event_count >= 10);
}

TEST_CASE("CLI JSON preflight errors do not write stdout", "[cli][json]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace home;
    auto session = workspace.path() / "json-real.jsonl";
    auto result = run_command_split(
        "HOME=" + q(home.path()) + " env -u CCH_TEST_MISSING_KEY " + bin() +
        " --mode json --workspace " + q(workspace.path()) +
        " --session " + q(session) +
        " --api-key-env CCH_TEST_MISSING_KEY hello");

    REQUIRE(result.exit_code != 0);
    CHECK(result.stdout_text.empty());
    CHECK(result.stderr_text.find("missing API key") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(session));
}

TEST_CASE("CLI fake REPL preserves process history for two prompts", "[cli][u6]") {
    cch::tests::TempWorkspace workspace;
    auto session = workspace.path() / "repl.jsonl";
    auto command = "printf 'one\\ntwo\\nexit\\n' | " + bin() + " --fake --repl --workspace " + q(workspace.path()) + " --session " + q(session);
    auto result = run_command(command);

    REQUIRE(result.exit_code == 0);
    CHECK(result.output.find("[assistant] fake: one") != std::string::npos);
    CHECK(result.output.find("[assistant] fake: two") != std::string::npos);
}

TEST_CASE("CLI resume appends to an existing redacted session", "[cli][u6]") {
    cch::tests::TempWorkspace workspace;
    auto session = workspace.path() / "resume.jsonl";
    auto first = run_command(bin() + " --fake --workspace " + q(workspace.path()) + " --session " + q(session) + " first");
    REQUIRE(first.exit_code == 0);

    auto second = run_command(bin() + " --fake --workspace " + q(workspace.path()) + " --resume " + q(session) + " second");

    REQUIRE(second.exit_code == 0);
    CHECK(second.output.find("[assistant] fake: second") != std::string::npos);
}

TEST_CASE("CLI resume uses session workspace when workspace is omitted", "[cli][u6]") {
    cch::tests::TempWorkspace original;
    cch::tests::TempWorkspace other;
    original.write("note.txt", "from-session-workspace");
    auto session = original.path() / "resume-workspace.jsonl";
    auto first = run_command(bin() + " --fake --workspace " + q(original.path()) + " --session " + q(session) + " first");
    REQUIRE(first.exit_code == 0);

    auto resumed = run_command(bin() + " --fake --resume " + q(session) + " read note.txt");

    REQUIRE(resumed.exit_code == 0);
    CHECK(resumed.output.find("from-session-workspace") != std::string::npos);
}

TEST_CASE("CLI resume rejects explicit workspace mismatch", "[cli][u6]") {
    cch::tests::TempWorkspace original;
    cch::tests::TempWorkspace other;
    auto session = original.path() / "resume-mismatch.jsonl";
    auto first = run_command(bin() + " --fake --workspace " + q(original.path()) + " --session " + q(session) + " first");
    REQUIRE(first.exit_code == 0);

    auto resumed = run_command(bin() + " --fake --workspace " + q(other.path()) + " --resume " + q(session) + " second");

    REQUIRE(resumed.exit_code != 0);
    CHECK(resumed.output.find("resume workspace does not match session metadata") != std::string::npos);
    CHECK(resumed.output.find("[model-request]") == std::string::npos);
}

TEST_CASE("CLI RPC resume exposes committed history without activating text output", "[cli][rpc]") {
    cch::tests::TempWorkspace workspace;
    auto session = workspace.path() / "rpc-resume.jsonl";
    auto first = run_command(bin() + " --fake --workspace " + q(workspace.path()) + " --session " + q(session) + " first");
    REQUIRE(first.exit_code == 0);

    auto resumed = run_command_split_with_input(
        bin() + " --fake --mode rpc --resume " + q(session),
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
    auto first = run_command(bin() + " --fake --workspace " + q(original.path()) + " --session " + q(session) + " first");
    REQUIRE(first.exit_code == 0);

    auto resumed = run_command_split_with_input(
        bin() + " --fake --mode rpc --workspace " + q(other.path()) + " --resume " + q(session),
        "{\"type\":\"get_state\"}\n");

    REQUIRE(resumed.exit_code != 0);
    CHECK(resumed.stdout_text.empty());
    CHECK(resumed.stderr_text.find("resume workspace does not match session metadata") != std::string::npos);
}

TEST_CASE("CLI rejects session and resume together before model request", "[cli][u6]") {
    cch::tests::TempWorkspace workspace;
    auto session = workspace.path() / "exclusive.jsonl";
    auto result = run_command(bin() + " --fake --workspace " + q(workspace.path()) + " --session " + q(session) + " --resume " + q(session) + " hello");

    REQUIRE(result.exit_code != 0);
    CHECK(result.output.find("use either --session or --resume") != std::string::npos);
    CHECK(result.output.find("[model-request]") == std::string::npos);
}

TEST_CASE("CLI text mode without prompt defaults to REPL", "[cli][u6]") {
    cch::tests::TempWorkspace workspace;
    auto session = workspace.path() / "default-repl.jsonl";
    auto result = run_command("printf '' | " + bin() + " --fake --workspace " + q(workspace.path()) + " --session " + q(session));

    REQUIRE(result.exit_code == 0);
    CHECK(result.output.find("prompt is required") == std::string::npos);
    CHECK(std::filesystem::exists(session));
}

TEST_CASE("CLI blocks existing session path without resume before model request", "[cli][u6]") {
    cch::tests::TempWorkspace workspace;
    auto session = workspace.path() / "exists.jsonl";
    std::ofstream(session) << "already here";
    auto result = run_command(bin() + " --fake --workspace " + q(workspace.path()) + " --session " + q(session) + " hello");

    REQUIRE(result.exit_code != 0);
    CHECK(result.output.find("already exists") != std::string::npos);
    CHECK(result.output.find("[model-request]") == std::string::npos);
}

TEST_CASE("CLI rejects invalid max turns before model request", "[cli][u6]") {
    cch::tests::TempWorkspace workspace;
    auto session = workspace.path() / "bad-turns.jsonl";
    auto result = run_command(bin() + " --fake --workspace " + q(workspace.path()) + " --session " + q(session) + " --max-turns nope hello");

    REQUIRE(result.exit_code != 0);
    CHECK((result.output.find("--max-turns") != std::string::npos ||
           result.output.find("Could not parse") != std::string::npos));
    CHECK(result.output.find("[model-request]") == std::string::npos);
}

TEST_CASE("CLI fake bash request is blocked by default", "[cli][u6]") {
    cch::tests::TempWorkspace workspace;
    auto session = workspace.path() / "bash.jsonl";
    auto result = run_command(bin() + " --fake --workspace " + q(workspace.path()) + " --session " + q(session) + " bash echo hi");

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
    auto result = run_command(bin() + " --fake --workspace " + q(workspace.path()) + " --session " + q(session) + " --max-turns 1 read missing.txt");

    REQUIRE(result.exit_code != 0);
    CHECK(result.output.find("[model-request] turn 1") != std::string::npos);
    CHECK(result.output.find("[tool-call] read#fake-read-1") != std::string::npos);
    CHECK(result.output.find("[tool-error] fake-read-1") != std::string::npos);
    CHECK(result.output.find("[max-turns] max_turns_exceeded") != std::string::npos);
    CHECK(result.output.find("loop failed: max_turns_exceeded") != std::string::npos);
}

TEST_CASE("CLI real-provider mode reports missing API key before model request", "[cli][u6]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace home;
    auto session = workspace.path() / "real.jsonl";
    auto result = run_command(
        "HOME=" + q(home.path()) + " env -u CCH_TEST_MISSING_KEY " + bin() +
        " --workspace " + q(workspace.path()) +
        " --session " + q(session) +
        " --api-key-env CCH_TEST_MISSING_KEY hello");

    REQUIRE(result.exit_code != 0);
    CHECK(result.output.find("missing API key") != std::string::npos);
    CHECK(result.output.find("[model-request]") == std::string::npos);
}

TEST_CASE("CLI Kimi path validates KIMI_API_KEY before model request and session creation", "[cli][kimi][u3]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace home;
    auto session = workspace.path() / "kimi-missing-key.jsonl";
    auto result = run_command(
        "HOME=" + q(home.path()) + " env -u KIMI_API_KEY " + bin() +
        " --workspace " + q(workspace.path()) +
        " --session " + q(session) +
        " --base-url https://api.kimi.com/coding/v1"
        " --model kimi-for-coding"
        " --api-key-env KIMI_API_KEY hello");

    REQUIRE(result.exit_code != 0);
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
        "HOME=" + q(home.path()) + " " + bin() +
        " --fake --workspace " + q(workspace.path()) +
        " --session " + q(session) + " /skill:demo");

    REQUIRE(result.exit_code == 0);
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
        "HOME=" + q(home.path()) + " " + bin() +
        " --fake --approve --workspace " + q(workspace.path()) +
        " --session " + q(session) + " /skill:demo");

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
        "HOME=" + q(home.path()) + " " + bin() +
        " --fake --no-approve --workspace " + q(workspace.path()) +
        " --session " + q(session) + " /skill:demo");

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
        "HOME=" + q(home.path()) + " " + bin() +
        " --fake --approve --workspace " + q(workspace.path()) +
        " --session " + q(session) + " /greet Ada");

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
        "HOME=" + q(home.path()) + " " + bin() +
        " --fake --approve --no-skills --workspace " + q(workspace.path()) +
        " --session " + q(session) + " /skill:demo");

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
        "HOME=" + q(home.path()) + " " + bin() +
        " --fake --approve --no-skills --workspace " + q(workspace.path()) +
        " --session " + q(session) + " /greet Ada");

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
        "HOME=" + q(home.path()) + " " + bin() +
        " --fake --workspace " + q(workspace.path()) +
        " --session " + q(session) +
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
        "HOME=" + q(home.path()) + " " + bin() +
        " --fake --workspace " + q(workspace.path()) +
        " --session " + q(session) +
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
        "HOME=" + q(home.path()) + " " + bin() +
        " --fake --mode json --workspace " + q(workspace.path()) +
        " --session " + q(session) + " hello");

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
        "HOME=" + q(home.path()) + " " + bin() +
        " --fake --approve --workspace " + q(workspace.path()) +
        " --session " + q(session) + " hello");

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
        "HOME=" + q(home.path()) + " " + bin() +
        " --fake --approve --mode json --workspace " + q(workspace.path()) +
        " --session " + q(session) + " hello");

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
        "HOME=" + q(home.path()) + " " + bin() +
        " --fake --approve --mode json --workspace " + q(workspace.path()) +
        " --session " + q(session) + " --prompt-template dupe.md hello");

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
        "HOME=" + q(home.path()) + " " + bin() +
        " --fake --approve --mode rpc --workspace " + q(workspace.path()) +
        " --session " + q(session),
        input);

    REQUIRE(result.exit_code == 0);
    CHECK(result.stderr_text.find("[template:warn] parse_failed") != std::string::npos);
    auto records = parse_json_objects(result.stdout_text);
    REQUIRE(find_response(records, "get_state") != nullptr);
    REQUIRE(find_response(records, "shutdown") != nullptr);
    CHECK(result.stdout_text.find("[template:warn]") == std::string::npos);
}

TEST_CASE("CLI applies config.json model when CLI omits --model", "[cli][config]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace home;
    std::filesystem::create_directories(home.path() / ".cpp-harness");
    std::ofstream(home.path() / ".cpp-harness" / "config.json") << R"({"model":"config-model-name"})";
    auto session = workspace.path() / "config-model-session.jsonl";

    auto result = run_command(
        "HOME=" + q(home.path()) + " " + bin() +
        " --fake --workspace " + q(workspace.path()) +
        " --session " + q(session) + " hello");

    REQUIRE(result.exit_code == 0);
    const auto header = parse_json_line(read_file(session));
    const auto& object = as_object(header);
    CHECK(json_string_at(object, "type") == "session");
    CHECK(json_string_at(object, "model") == "config-model-name");
}

TEST_CASE("CLI accepts config.json api_key_env chain without explicit --api-key-env", "[cli][config]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace home;
    std::filesystem::create_directories(home.path() / ".cpp-harness");
    std::ofstream(home.path() / ".cpp-harness" / "config.json") << R"({"api_key_env":["CUSTOM_KEY"]})";
    auto session = workspace.path() / "config-key-session.jsonl";

    auto result = run_command(
        "env -u OPENAI_API_KEY CUSTOM_KEY=test HOME=" + q(home.path()) + " " + bin() +
        " --workspace " + q(workspace.path()) +
        " --session " + q(session) + " hello");

    CHECK(result.output.find("missing API key") == std::string::npos);
    CHECK(std::filesystem::exists(session));
}

TEST_CASE("CLI invalid explicit prompt template fails before session creation", "[cli][project-resources][assembly]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace home;
    auto session = workspace.path() / "explicit-fail.jsonl";

    auto result = run_command_split(
        "HOME=" + q(home.path()) + " " + bin() +
        " --fake --workspace " + q(workspace.path()) +
        " --session " + q(session) +
        " --prompt-template missing.md hello");

    REQUIRE(result.exit_code != 0);
    CHECK(result.stdout_text.empty());
    CHECK(result.stderr_text.find("template") != std::string::npos ||
          result.stderr_text.find("explicit") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(session));
}

TEST_CASE("CLI applies config.json provider identity when no explicit provider", "[cli][config][provider-resolution]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace home;
    std::filesystem::create_directories(home.path() / ".cpp-harness");
    std::ofstream(home.path() / ".cpp-harness" / "config.json") << R"({"provider":"kimi-coding","model":"kimi-for-coding"})";
    auto session = workspace.path() / "config-provider-rpc.jsonl";

    auto result = run_command_split_with_input(
        "env -u OPENAI_API_KEY CCH_CONFIG_PROVIDER_KEY=unused HOME=" + q(home.path()) + " " + bin() +
        " --workspace " + q(workspace.path()) +
        " --session " + q(session) +
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
        "HOME=" + q(home.path()) + " " + bin() +
        " --fake --workspace " + q(workspace.path()) +
        " --session " + q(session) + " first");
    REQUIRE(first.exit_code == 0);

    auto second = run_command_split_with_input(
        "HOME=" + q(home.path()) + " " + bin() +
        " --fake --workspace " + q(workspace.path()) +
        " --resume " + q(session) +
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
        "HOME=" + q(home.path()) + " " + bin() +
        " --fake --workspace " + q(workspace.path()) +
        " --session " + q(session) + " first");
    REQUIRE(first.exit_code == 0);

    auto second = run_command_split_with_input(
        "HOME=" + q(home.path()) + " " + bin() +
        " --fake --workspace " + q(workspace.path()) +
        " --resume " + q(session) + " --mode rpc",
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
