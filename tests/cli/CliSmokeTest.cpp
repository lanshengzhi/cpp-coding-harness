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

TEST_CASE("CLI blocks existing session path without resume before model request", "[cli][u6]") {
    cch::tests::TempWorkspace workspace;
    auto session = workspace.path() / "exists.jsonl";
    std::ofstream(session) << "already here";
    auto result = run_command(bin() + " --fake --workspace " + q(workspace.path()) + " --session " + q(session) + " hello");

    REQUIRE(result.exit_code != 0);
    CHECK(result.output.find("already exists") != std::string::npos);
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

TEST_CASE("CLI real-provider mode reports missing API key before model request", "[cli][u6]") {
    cch::tests::TempWorkspace workspace;
    auto session = workspace.path() / "real.jsonl";
    auto result = run_command("env -u CCH_TEST_MISSING_KEY " + bin() + " --workspace " + q(workspace.path()) + " --session " + q(session) + " --api-key-env CCH_TEST_MISSING_KEY hello");

    REQUIRE(result.exit_code != 0);
    CHECK(result.output.find("missing API key") != std::string::npos);
    CHECK(result.output.find("[model-request]") == std::string::npos);
}
