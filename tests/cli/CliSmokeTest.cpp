#include "../../third_party/catch2/catch_test_macros.hpp"

#include "../support/TempWorkspace.hpp"

#include "cch/util/Json.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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

TEST_CASE("CLI rejects rpc mode before session creation", "[cli][json]") {
    cch::tests::TempWorkspace workspace;
    auto session = workspace.path() / "rpc.jsonl";
    auto result = run_command_split(bin() + " --fake --mode rpc --workspace " + q(workspace.path()) + " --session " + q(session) + " hello");

    REQUIRE(result.exit_code != 0);
    CHECK(result.stdout_text.empty());
    CHECK(result.stderr_text.find("--mode rpc is not supported yet") != std::string::npos);
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
            CHECK(json_string_at(record, "toolName") == "read_file");
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

TEST_CASE("CLI JSON preflight errors do not write stdout", "[cli][json]") {
    cch::tests::TempWorkspace workspace;
    auto session = workspace.path() / "json-real.jsonl";
    auto result = run_command_split("env -u CCH_TEST_MISSING_KEY " + bin() + " --mode json --workspace " + q(workspace.path()) + " --session " + q(session) + " --api-key-env CCH_TEST_MISSING_KEY hello");

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

TEST_CASE("CLI rejects session and resume together before model request", "[cli][u6]") {
    cch::tests::TempWorkspace workspace;
    auto session = workspace.path() / "exclusive.jsonl";
    auto result = run_command(bin() + " --fake --workspace " + q(workspace.path()) + " --session " + q(session) + " --resume " + q(session) + " hello");

    REQUIRE(result.exit_code != 0);
    CHECK(result.output.find("use either --session or --resume") != std::string::npos);
    CHECK(result.output.find("[model-request]") == std::string::npos);
}

TEST_CASE("CLI rejects missing prompt without repl before model request", "[cli][u6]") {
    cch::tests::TempWorkspace workspace;
    auto session = workspace.path() / "missing-prompt.jsonl";
    auto result = run_command(bin() + " --fake --workspace " + q(workspace.path()) + " --session " + q(session));

    REQUIRE(result.exit_code != 0);
    CHECK(result.output.find("prompt is required unless --repl is used") != std::string::npos);
    CHECK(result.output.find("[model-request]") == std::string::npos);
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
    CHECK(result.output.find("[tool-call] bash#fake-bash-1") != std::string::npos);
    CHECK(result.output.find("[tool-error] fake-bash-1") != std::string::npos);
    CHECK(result.output.find("bash is disabled") != std::string::npos);
}

TEST_CASE("CLI fake read loop prints max-turn marker when turn budget is exhausted", "[cli][u1]") {
    cch::tests::TempWorkspace workspace;
    auto session = workspace.path() / "max-turn.jsonl";
    auto result = run_command(bin() + " --fake --workspace " + q(workspace.path()) + " --session " + q(session) + " --max-turns 1 read missing.txt");

    REQUIRE(result.exit_code != 0);
    CHECK(result.output.find("[model-request] turn 1") != std::string::npos);
    CHECK(result.output.find("[tool-call] read_file#fake-read-1") != std::string::npos);
    CHECK(result.output.find("[tool-error] fake-read-1") != std::string::npos);
    CHECK(result.output.find("[max-turns] max_turns_exceeded") != std::string::npos);
    CHECK(result.output.find("loop failed: max_turns_exceeded") != std::string::npos);
}

TEST_CASE("CLI real-provider mode reports missing API key before model request", "[cli][u6]") {
    cch::tests::TempWorkspace workspace;
    auto session = workspace.path() / "real.jsonl";
    auto result = run_command("env -u CCH_TEST_MISSING_KEY " + bin() + " --workspace " + q(workspace.path()) + " --session " + q(session) + " --api-key-env CCH_TEST_MISSING_KEY hello");

    REQUIRE(result.exit_code != 0);
    CHECK(result.output.find("missing API key") != std::string::npos);
    CHECK(result.output.find("[model-request]") == std::string::npos);
}

TEST_CASE("CLI Kimi path validates KIMI_API_KEY before model request and session creation", "[cli][kimi][u3]") {
    cch::tests::TempWorkspace workspace;
    auto session = workspace.path() / "kimi-missing-key.jsonl";
    auto result = run_command(
        "env -u KIMI_API_KEY " + bin() +
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
