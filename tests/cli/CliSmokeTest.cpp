#include "../../third_party/catch2/catch_test_macros.hpp"

#include "../support/TempWorkspace.hpp"

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

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

std::string q(const std::filesystem::path& path) {
    return "'" + path.string() + "'";
}

std::string bin() { return q(CCH_BINARY); }
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
    CHECK(result.output.find("--max-turns must be an integer") != std::string::npos);
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
