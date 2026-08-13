#include <catch2/catch_test_macros.hpp>

#include "support/CliRunFixture.hpp"
#include "support/ImageFixture.hpp"
#include "support/PseudoTerminal.hpp"
#include "support/ShellQuoting.hpp"
#include "support/TempWorkspace.hpp"
#include "support/TextHelpers.hpp"

#include "coding_agent/runtime/AsyncCliRuntime.hpp"
#include "util/Json.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/wait.h>
#include <format>
#endif

#ifndef CCH_BINARY
#define CCH_BINARY "./cpp_harness"
#endif

namespace {

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

std::string bin() { return shell_quote(CCH_BINARY); }

void write_tiny_gif(const std::filesystem::path& path) {
    const auto bytes = cch::tests::decode_base64(cch::tests::kTinyGifBase64);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
}

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
    auto parsed = cch::util::read_json(line);
    REQUIRE(parsed.has_value());
    return *parsed;
}

const cch::util::JsonValue::object_t& as_object(const cch::util::JsonValue& value) {
    return value.get<cch::util::JsonValue::object_t>();
}

std::string json_string_at(const cch::util::JsonValue::object_t& object, const std::string& key) {
    return object.at(key).get<std::string>();
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

/// Run the CLI entry chain in-process through the fake-provider seam with the
/// process cwd set to `workspace` and no ambient user configuration.
cch::tests::CliRunResult run_in_workspace(
    const cch::tests::TempWorkspace& workspace,
    std::vector<std::string> args) {
    return cch::tests::run_cli(cch::tests::CliRunOptions{
        .args = std::move(args),
        .cwd = workspace.path(),
    });
}

/// pi `formatMissingSessionCwdError` (session-cwd.ts), verbatim.
[[nodiscard]] std::string missing_cwd_error_text(
    const std::filesystem::path& session_cwd,
    const std::filesystem::path& session_file,
    const std::filesystem::path& fallback_cwd) {
    return std::format(
        "Stored session working directory does not exist: {}\n"
        "Session file: {}\n"
        "Current working directory: {}\n",
        session_cwd.string(),
        session_file.string(),
        fallback_cwd.string());
}

} // namespace

TEST_CASE(
    "CLI initial image arguments persist text before image with or without positional text",
    "[cli][smoke][issue63]") {
    cch::tests::TempWorkspace workspace;
    const auto image = workspace.path() / "misleading.data";
    write_tiny_gif(image);

    const auto described_session = workspace.path() / "described.jsonl";
    const auto described = run_in_workspace(
        workspace,
        {"--session", described_session.string(), "@" + image.string(), "describe"});
    REQUIRE(described.exit_code == 0);
    const auto described_jsonl = read_file(described_session);
    const auto wrapper = "<file name=\\\"" + image.string() + "\\\"></file>\\ndescribe";
    const auto text_position = described_jsonl.find(wrapper);
    const auto image_position = described_jsonl.find("\"type\":\"image\"");
    REQUIRE(text_position != std::string::npos);
    REQUIRE(image_position != std::string::npos);
    CHECK(text_position < image_position);
    CHECK(described_jsonl.find("\"mimeType\":\"image/gif\"") != std::string::npos);

    const auto lone_session = workspace.path() / "lone.jsonl";
    const auto lone = run_in_workspace(
        workspace,
        {"--session", lone_session.string(), "@" + image.string()});
    REQUIRE(lone.exit_code == 0);
    const auto lone_jsonl = read_file(lone_session);
    CHECK(lone_jsonl.find("<file name=\\\"" + image.string() +
        "\\\"></file>") != std::string::npos);
    CHECK(lone_jsonl.find("\"mimeType\":\"image/gif\"") != std::string::npos);

    const auto missing_session = workspace.path() / "missing.jsonl";
    const auto missing = run_in_workspace(
        workspace,
        {"--session", missing_session.string(),
         "@" + (workspace.path() / "missing.png").string()});
    CHECK(missing.exit_code == 1);
    CHECK(missing.stderr_text.find("initial file not found") != std::string::npos);
    std::error_code exists_error;
    CHECK_FALSE(std::filesystem::exists(missing_session, exists_error));
    CHECK_FALSE(exists_error);
}

TEST_CASE("CLI print mode prints only the final assistant text and writes the session", "[cli][u6]") {
    cch::tests::TempWorkspace workspace;
    auto session = workspace.path() / "one-shot.jsonl";
    auto result = run_in_workspace(workspace, {"--session", session.string(), "hello"});

    REQUIRE(result.exit_code == 0);
    CHECK(result.stdout_text == "fake: hello\n");
    CHECK(result.stderr_text.empty());
    CHECK(std::filesystem::exists(session));
}

TEST_CASE("CLI print mode has no slash commands and sends /help to the model", "[cli][commands]") {
    cch::tests::TempWorkspace workspace;
    auto session = workspace.path() / "help-one-shot.jsonl";
    auto result = run_in_workspace(workspace, {"--session", session.string(), "/help"});

    REQUIRE(result.exit_code == 0);
    CHECK(result.stdout_text == "fake: /help\n");
    CHECK(result.stderr_text.empty());
}

TEST_CASE("CLI print mode passes unknown slash text through as an ordinary prompt", "[cli][commands]") {
    cch::tests::TempWorkspace workspace;
    auto session = workspace.path() / "help-unknown.jsonl";
    auto result = run_in_workspace(
        workspace, {"--session", session.string(), "/help missing"});

    REQUIRE(result.exit_code == 0);
    CHECK(result.stdout_text == "fake: /help missing\n");
}

TEST_CASE("CLI print mode sends /commands to the model like any prompt", "[cli][commands][issue64]") {
    cch::tests::TempWorkspace workspace;
    auto session = workspace.path() / "commands.jsonl";
    auto result = run_in_workspace(
        workspace, {"--print", "--session", session.string(), "/commands"});

    REQUIRE(result.exit_code == 0);
    CHECK(result.stdout_text == "fake: /commands\n");
}

TEST_CASE("CLI text one-shot sends unmatched slash input to the model", "[cli][commands]") {
    cch::tests::TempWorkspace workspace;
    auto session = workspace.path() / "unknown-slash.jsonl";
    auto result = run_in_workspace(workspace, {"--session", session.string(), "/missing"});

    REQUIRE(result.exit_code == 0);
    CHECK(result.stdout_text == "fake: /missing\n");
}

TEST_CASE("CLI text one-shot treats user bash syntax as an ordinary prompt", "[cli][commands][user-bash][issue64]") {
    cch::tests::TempWorkspace one_shot_workspace;
    auto one_shot_session = one_shot_workspace.path() / "oneshot-user-bash.jsonl";
    auto one_shot = run_in_workspace(
        one_shot_workspace, {"--session", one_shot_session.string(), "!echo hi"});

    REQUIRE(one_shot.exit_code == 0);
    CHECK(one_shot.stdout_text.find("fake: !echo hi") != std::string::npos);

    cch::tests::TempWorkspace double_bang_workspace;
    auto double_bang_session = double_bang_workspace.path() / "oneshot-double-bang.jsonl";
    auto double_bang = run_in_workspace(
        double_bang_workspace, {"--session", double_bang_session.string(), "!!echo hidden"});

    REQUIRE(double_bang.exit_code == 0);
    CHECK(double_bang.stdout_text.find("fake: !!echo hidden") != std::string::npos);
}

TEST_CASE("CLI print mode sends /clear to the model", "[cli][commands][issue64]") {
    cch::tests::TempWorkspace workspace;
    auto session = workspace.path() / "clear-one-shot.jsonl";
    auto result = run_in_workspace(workspace, {"--session", session.string(), "/clear"});

    REQUIRE(result.exit_code == 0);
    CHECK(result.stderr_text.empty());
    CHECK(result.stdout_text == "fake: /clear\n");
}

TEST_CASE("CLI print mode sends /exit to the model", "[cli][commands]") {
    cch::tests::TempWorkspace workspace;
    auto session = workspace.path() / "exit-one-shot.jsonl";
    auto result = run_in_workspace(workspace, {"--session", session.string(), "/exit"});

    REQUIRE(result.exit_code == 0);
    CHECK(result.stdout_text == "fake: /exit\n");
}

TEST_CASE("CLI print mode outputs only the final text after a tool flow", "[cli][presentation]") {
    cch::tests::TempWorkspace workspace;
    std::ofstream(workspace.path() / "note.txt") << "subscription text";
    auto session = workspace.path() / "text-tool-flow.jsonl";
    auto result = run_in_workspace(
        workspace, {"--session", session.string(), "read note.txt"});

    REQUIRE(result.exit_code == 0);
    CHECK(result.stdout_text == "fake observed: subscription text\n");
    CHECK(result.stderr_text.empty());
    CHECK(std::filesystem::exists(session));
}

TEST_CASE("CLI bash tool is always available under the fixed tool set", "[cli][u6]") {
    cch::tests::TempWorkspace workspace;
    auto session = workspace.path() / "bash.jsonl";
    auto result = run_in_workspace(
        workspace, {"--session", session.string(), "bash echo hi"});

    REQUIRE(result.exit_code == 0);
    // The model-requested bash tool is registered without any --enable-bash
    // opt-in (pi); the scripted fake's bash request executes in the workspace
    // and its result reaches the model through the final assistant text.
    CHECK(result.stdout_text.find("unknown tool: bash") == std::string::npos);
    CHECK(result.stdout_text.find("bash is disabled") == std::string::npos);
    CHECK(result.stdout_text.find("exit_code=0") != std::string::npos);
}

TEST_CASE("CLI rejects removed compatibility flags before model request", "[cli][u8]") {
    {
        cch::tests::TempWorkspace workspace;
        auto result = run_in_workspace(workspace, {"--async", "hello"});
        REQUIRE(result.exit_code != 0);
        CHECK(result.stderr_text.find("unknown option: --async") != std::string::npos);
        CHECK(result.stdout_text.find("[model-request]") == std::string::npos);
    }
    {
        cch::tests::TempWorkspace workspace;
        auto result = run_in_workspace(workspace, {"--repl", "hello"});
        REQUIRE(result.exit_code != 0);
        CHECK(result.stderr_text.find("unknown option: --repl") != std::string::npos);
        CHECK(result.stdout_text.find("[model-request]") == std::string::npos);
    }
}

TEST_CASE("CLI rejects the deleted C++-only flags before model request", "[cli][u8]") {
    cch::tests::TempWorkspace workspace;
    auto fake_result = run_in_workspace(workspace, {"--fake", "hello"});
    REQUIRE(fake_result.exit_code != 0);
    CHECK(fake_result.stderr_text.find("unknown option: --fake") != std::string::npos);
    CHECK(fake_result.stdout_text.find("[model-request]") == std::string::npos);

    auto bash_result = run_in_workspace(workspace, {"--enable-bash", "hello"});
    REQUIRE(bash_result.exit_code != 0);
    CHECK(bash_result.stderr_text.find("unknown option: --enable-bash") != std::string::npos);

    auto turns_result = run_in_workspace(workspace, {"--max-turns", "12", "hello"});
    REQUIRE(turns_result.exit_code != 0);
    CHECK(turns_result.stderr_text.find("unknown option: --max-turns") != std::string::npos);

    auto workspace_result = run_in_workspace(
        workspace, {"--workspace", "/tmp", "hello"});
    REQUIRE(workspace_result.exit_code != 0);
    CHECK(workspace_result.stderr_text.find("unknown option: --workspace") != std::string::npos);
}

TEST_CASE("CLI rejects the removed json and rpc modes with an explicit error", "[cli][modes]") {
    {
        cch::tests::TempWorkspace workspace;
        auto result = run_in_workspace(workspace, {"--mode", "json", "hello"});
        REQUIRE(result.exit_code != 0);
        CHECK(result.stdout_text.empty());
        CHECK(result.stderr_text.find("--mode json was removed") != std::string::npos);
        CHECK(result.stderr_text.find("only --mode text is supported") != std::string::npos);
        CHECK(result.stderr_text.find("[model-request]") == std::string::npos);
    }
    {
        cch::tests::TempWorkspace workspace;
        auto result = run_in_workspace(workspace, {"--mode", "rpc", "hello"});
        REQUIRE(result.exit_code != 0);
        CHECK(result.stdout_text.empty());
        CHECK(result.stderr_text.find("--mode rpc was removed") != std::string::npos);
        CHECK(result.stderr_text.find("[model-request]") == std::string::npos);
    }
}

TEST_CASE("CLI --version prints the binary's own CMake project version", "[cli][identity]") {
    auto result = run_command(bin() + " --version");
    REQUIRE(result.exit_code == 0);
    const auto expected = std::string{cch::cli::project_version()} + "\n";
    CHECK(result.output == expected);

    auto short_result = run_command(bin() + " -v");
    REQUIRE(short_result.exit_code == 0);
    CHECK(short_result.output == expected);
}

TEST_CASE("CLI help documents the pi-aligned surface and omits deleted flags", "[cli][u8]") {
    auto result = run_command(bin() + " --help");

    REQUIRE(result.exit_code == 0);
    CHECK(result.output.find("--async") == std::string::npos);
    CHECK(result.output.find("--fake") == std::string::npos);
    CHECK(result.output.find("--enable-bash") == std::string::npos);
    CHECK(result.output.find("--max-turns") == std::string::npos);
    CHECK(result.output.find("--workspace") == std::string::npos);
    CHECK(result.output.find("--mode") != std::string::npos);
    CHECK(result.output.find("--session-id") != std::string::npos);
    CHECK(result.output.find("--no-approve, -na") != std::string::npos);
    CHECK(result.output.find("--version, -v") != std::string::npos);
}

TEST_CASE("CLI non-TTY stdin becomes one print prompt", "[cli][selection][issue64]") {
    cch::tests::TempWorkspace workspace;
    auto session = workspace.path() / "piped.jsonl";
    auto result = cch::tests::run_cli(cch::tests::CliRunOptions{
        .args = {"--session", session.string()},
        .cwd = workspace.path(),
        .stdin_text = "one\ntwo",
    });

    REQUIRE(result.exit_code == 0);
    CHECK(result.stdout_text == "fake: one\ntwo\n");
}

TEST_CASE("CLI --session open resumes and appends to an existing redacted session", "[cli][u6]") {
    cch::tests::TempWorkspace workspace;
    auto session = workspace.path() / "resume.jsonl";
    auto first = run_in_workspace(
        workspace, {"--session", session.string(), "first"});
    REQUIRE(first.exit_code == 0);

    // pi: `--resume` is the picker; opening an existing session at a path is
    // `--session <path>` (open-or-create resumes).
    auto second = run_in_workspace(
        workspace, {"--session", session.string(), "second"});
    REQUIRE(second.exit_code == 0);
    CHECK(second.stdout_text == "fake: second\n");
}

#if defined(__unix__) || defined(__APPLE__)
TEST_CASE(
    "CLI interactive boot Continue recovers a vanished session cwd",
    "[cli][startup-tui][issue417]") {
    cch::tests::TempWorkspace original;
    cch::tests::TempWorkspace storage;
    cch::tests::TempWorkspace home;
    home.write(".pi/agent/models.json", R"({
      "providers": {
        "deepseek": {
          "baseUrl": "https://api.deepseek.example/v1",
          "api": "openai-responses",
          "apiKey": "configured-key",
          "models": [{"id": "deepseek-v4-flash"}]
        }
      }
    })");
    const auto session = storage.path() / "vanished.jsonl";

    // Seed a session whose header cwd (`original`) then vanishes while the
    // file survives (pi `getMissingSessionCwdIssue`).
    auto seeded = run_command_split(
        "cd " + shell_quote(original.path()) + " && HOME=" + shell_quote(home.path()) + " env -u PI_CODING_AGENT_DIR " + bin() +
        " --session " + shell_quote(session) +
        " --model deepseek-v4-flash");
    REQUIRE(seeded.exit_code == 0);
    std::error_code ec;
    REQUIRE(std::filesystem::remove_all(original.path(), ec) > 0);
    REQUIRE_FALSE(ec);

    auto pty = cch::tests::open_pseudo_terminal(100, 40);
    REQUIRE(pty);
    const auto pid = ::fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        (void)::dup2(pty->slave.get(), STDIN_FILENO);
        (void)::dup2(pty->slave.get(), STDOUT_FILENO);
        (void)::dup2(pty->slave.get(), STDERR_FILENO);
        (void)::setenv("HOME", home.path().string().c_str(), 1);
        (void)::unsetenv("PI_CODING_AGENT_DIR");
        (void)::chdir(storage.path().string().c_str());
        ::execl(
            CCH_BINARY, "cpp_harness", "--session", session.string().c_str(),
            static_cast<char*>(nullptr));
        ::_exit(127);
    }

    // The interactive boot shows pi's startup-TUI Continue/Cancel prompt
    // with the verbatim missing-cwd text before the main TUI starts.
    std::string output;
    REQUIRE(cch::tests::wait_until(
        [&] {
            output.append(cch::tests::read_available(
                pty->master.get(), std::chrono::milliseconds(20)));
            return output.find("cwd from session file does not exist") !=
                std::string::npos;
        },
        std::chrono::seconds(5)));
    CHECK(output.find("continue in current cwd") != std::string::npos);
    CHECK(output.find(original.path().string()) != std::string::npos);
    CHECK(output.find("Continue") != std::string::npos);
    CHECK(output.find("Cancel") != std::string::npos);

    // Enter picks Continue; the main TUI boots with the resumed session
    // (its footer shows the stored model), then Ctrl+D exits.
    REQUIRE(::write(pty->master.get(), "\r", 1) == 1);
    REQUIRE(cch::tests::wait_until(
        [&] {
            output.append(cch::tests::read_available(
                pty->master.get(), std::chrono::milliseconds(20)));
            return output.find("deepseek-v4-flash") != std::string::npos;
        },
        std::chrono::seconds(5)));
    REQUIRE(::write(pty->master.get(), "\x04", 1) == 1);
    int status = 0;
    REQUIRE(cch::tests::wait_until(
        [&] {
            output.append(cch::tests::read_available(
                pty->master.get(), std::chrono::milliseconds(20)));
            return ::waitpid(pid, &status, WNOHANG) == pid;
        },
        std::chrono::seconds(5)));
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
}
#endif

#if defined(__unix__) || defined(__APPLE__)
TEST_CASE(
    "CLI --resume opens the startup-TUI picker on a real terminal",
    "[cli][startup-tui][issue417]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace home;
    home.write(".pi/agent/models.json", R"({
      "providers": {
        "deepseek": {
          "baseUrl": "https://api.deepseek.example/v1",
          "api": "openai-responses",
          "apiKey": "configured-key",
          "models": [{"id": "deepseek-v4-flash"}]
        }
      }
    })");

    // Seed an automatic session with no prompt (no network): a no-prompt
    // print run still creates the session (pi). The picker's current-folder
    // scope lists it.
    auto seeded = run_command_split(
        "cd " + shell_quote(workspace.path()) + " && HOME=" + shell_quote(home.path()) + " env -u PI_CODING_AGENT_DIR " + bin() +
        " --model deepseek-v4-flash");
    REQUIRE(seeded.exit_code == 0);

    auto pty = cch::tests::open_pseudo_terminal(100, 40);
    REQUIRE(pty);
    const auto pid = ::fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        // The child runs the real binary with the PTY as its terminal: the
        // startup-TUI picker (ProcessTerminal host) and the print-mode run
        // share it (pi `selectSession` before the frontend split).
        (void)::dup2(pty->slave.get(), STDIN_FILENO);
        (void)::dup2(pty->slave.get(), STDOUT_FILENO);
        (void)::dup2(pty->slave.get(), STDERR_FILENO);
        (void)::setenv("HOME", home.path().string().c_str(), 1);
        (void)::unsetenv("PI_CODING_AGENT_DIR");
        (void)::chdir(workspace.path().string().c_str());
        ::execl(CCH_BINARY, "cpp_harness", "--print", "--resume", static_cast<char*>(nullptr));
        ::_exit(127);
    }

    // The startup TUI renders the picker on the terminal before any session
    // machinery (pi selectSession).
    std::string output;
    REQUIRE(cch::tests::wait_until(
        [&] {
            output.append(cch::tests::read_available(
                pty->master.get(), std::chrono::milliseconds(20)));
            return output.find("Resume Session (Current Folder)") !=
                std::string::npos;
        },
        std::chrono::seconds(5)));
    CHECK(output.find("deepseek-v4-flash") == std::string::npos);

    // Enter selects the seeded session; the picker clears, the print run
    // resumes with no prompt (nothing printed, exit 0).
    REQUIRE(::write(pty->master.get(), "\r", 1) == 1);
    int status = 0;
    REQUIRE(cch::tests::wait_until(
        [&] {
            output.append(cch::tests::read_available(
                pty->master.get(), std::chrono::milliseconds(20)));
            return ::waitpid(pid, &status, WNOHANG) == pid;
        },
        std::chrono::seconds(5)));
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
}
#endif

TEST_CASE("CLI --session open uses session workspace when the launch directory differs", "[cli][u6]") {
    cch::tests::TempWorkspace original;
    cch::tests::TempWorkspace other;
    original.write("note.txt", "from-session-workspace");
    auto session = original.path() / "resume-workspace.jsonl";
    auto first = run_in_workspace(
        original, {"--session", session.string(), "first"});
    REQUIRE(first.exit_code == 0);

    auto resumed = cch::tests::run_cli(cch::tests::CliRunOptions{
        .args = {"--session", session.string(), "read note.txt"},
        .cwd = other.path(),
    });

    REQUIRE(resumed.exit_code == 0);
    CHECK(resumed.stdout_text.find("from-session-workspace") != std::string::npos);
}

TEST_CASE("CLI --session wins over --resume without a conflict error", "[cli][u6]") {
    cch::tests::TempWorkspace workspace;
    auto session = workspace.path() / "exclusive.jsonl";
    auto other = workspace.path() / "other.jsonl";
    auto result = run_in_workspace(
        workspace,
        {"--session", session.string(), "--resume", "hello"});

    // pi precedence: --session is selected before --resume; the C++-today
    // "use either --session or --resume" error is deleted. `--resume` is a
    // pure boolean flag, so nothing after it is consumed as a path.
    REQUIRE(result.exit_code == 0);
    CHECK(result.stderr_text.find("use either --session or --resume") == std::string::npos);
    CHECK(result.stdout_text == "fake: hello\n");
    CHECK(std::filesystem::exists(session));
    CHECK_FALSE(std::filesystem::exists(other));
}

TEST_CASE("CLI print mode with no prompt prints nothing and exits 0", "[cli][selection][issue64]") {
    cch::tests::TempWorkspace workspace;
    auto session = workspace.path() / "empty-print.jsonl";
    auto result = cch::tests::run_cli(cch::tests::CliRunOptions{
        .args = {"--session", session.string()},
        .cwd = workspace.path(),
    });

    // pi: no prompt-required guard; the session is still created, nothing is
    // printed, and the run exits 0.
    REQUIRE(result.exit_code == 0);
    CHECK(result.stdout_text.empty());
    CHECK(result.stderr_text.empty());
    CHECK(std::filesystem::exists(session));
}

TEST_CASE("CLI --session opens-or-creates at the target path", "[cli][u6]") {
    cch::tests::TempWorkspace workspace;
    auto session = workspace.path() / "exists.jsonl";

    // pi: --session on a missing path creates the session; the old
    // create-only "already exists" behavior is gone.
    auto first = run_in_workspace(
        workspace, {"--session", session.string(), "first"});
    REQUIRE(first.exit_code == 0);
    CHECK(std::filesystem::exists(session));

    // pi: --session on an existing path opens (resumes) it.
    auto second = run_in_workspace(
        workspace, {"--session", session.string(), "second"});
    REQUIRE(second.exit_code == 0);
    CHECK(second.stdout_text == "fake: second\n");
    const auto content = read_file(session);
    CHECK(content.find("\"text\":\"second\"") != std::string::npos);
}

TEST_CASE("CLI real-provider mode reports missing API key as a terminal auth outcome", "[cli][u6][issue338]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace home;
    auto session = workspace.path() / "real.jsonl";
    auto result = run_command_split(
        "cd " + shell_quote(workspace.path()) + " && HOME=" + shell_quote(home.path()) +
        " env -u KIMI_API_KEY " + bin() +
        " --session " + shell_quote(session) +
        " hello");

    REQUIRE(result.exit_code == 1);
    // Nothing resolves as configured: the Agent holds pi's unknown
    // kDefaultModel and fails through normal provider lookup (T04); the
    // terminal outcome's errorMessage reaches stderr with exit 1.
    CHECK(result.stdout_text.empty());
    CHECK(result.stderr_text.find("Unknown provider: unknown") != std::string::npos);
}

TEST_CASE("CLI Kimi path reports missing KIMI_API_KEY through the preflight re-auth guidance", "[cli][kimi][u3][issue338][issue360]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace home;
    auto session = workspace.path() / "kimi-missing-key.jsonl";
    auto result = run_command(
        "cd " + shell_quote(workspace.path()) + " && HOME=" + shell_quote(home.path()) +
        " env -u KIMI_API_KEY " + bin() +
        " --session " + shell_quote(session) +
        " --model kimi-for-coding hello");

    // T11 preflight (pi `prompt()` hasConfiguredAuth check): a real model
    // whose provider resolves no auth fails the prompt before any stream with
    // pi's verbatim OAuth re-auth guidance (kimi-coding is OAuth-typed).
    REQUIRE(result.exit_code == 1);
    CHECK(result.output.find("loop failed: Authentication failed for \"kimi-coding\"") !=
          std::string::npos);
    CHECK(result.output.find("Run '/login kimi-coding' to re-authenticate.") !=
          std::string::npos);
    CHECK(std::filesystem::exists(session));
}

TEST_CASE("CLI terminal auth failure after malformed settings keeps the warning visible", "[cli][settings][issue338]") {
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
        "cd " + shell_quote(workspace.path()) + " && PI_CODING_AGENT_DIR=" + shell_quote(agent_dir) +
        " env -u KIMI_API_KEY " + bin() +
        " --session " + shell_quote(session) +
        " hello");

    REQUIRE(result.exit_code == 1);
    // Nothing resolves as configured: the Agent holds kDefaultModel and the
    // terminal auth outcome names the unknown provider (T04) on stderr.
    CHECK(result.stdout_text.empty());
    CHECK(result.stderr_text.find("Unknown provider: unknown") != std::string::npos);
    CHECK(result.stderr_text.find("could not load global settings") != std::string::npos);
    CHECK(std::filesystem::exists(session));
}

TEST_CASE("CLI skips project skills by default when project trust is unknown", "[cli][project-trust]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace home;
    workspace.write(".pi/skills/demo/SKILL.md",
                    "---\n"
                    "name: demo\n"
                    "description: Demo skill.\n"
                    "---\n"
                    "# Demo Skill\n\n"
                    "Do demo.\n");
    auto session = workspace.path() / "untrusted-skills.jsonl";

    auto result = cch::tests::run_cli(cch::tests::CliRunOptions{
        .args = {"--session", session.string(), "/skill:demo"},
        .cwd = workspace.path(),
        .env = {{"HOME", home.path().string()}},
    });

    REQUIRE(result.exit_code == 0);
        CHECK(result.stdout_text.find("Do demo.") == std::string::npos);
}

TEST_CASE("CLI project-controlled default trust store cannot authorize project skills", "[cli][project-trust]") {
    cch::tests::TempWorkspace workspace;
    workspace.write(".pi/skills/demo/SKILL.md",
                    "---\n"
                    "name: demo\n"
                    "description: Demo skill.\n"
                    "---\n"
                    "# Demo Skill\n\n"
                    "Do demo.\n");
    workspace.write(
        ".pi/agent/trust.json",
        "{\"" + std::filesystem::weakly_canonical(workspace.path()).string() + "\":true}\n");
    auto session = workspace.path() / "project-controlled-trust.jsonl";

    auto result = cch::tests::run_cli(cch::tests::CliRunOptions{
        .args = {"--session", session.string(), "/skill:demo"},
        .cwd = workspace.path(),
        .env = {{"HOME", workspace.path().string()},
                {"PI_CODING_AGENT_DIR", std::nullopt}},
    });

    REQUIRE(result.exit_code == 0);
    // The project-controlled trust store is rejected at the session seam, so
    // the store error surfaces as a pi-shaped warning without authorizing.
    CHECK(result.stderr_text.find("[resource:warn] warning: trust store path is empty") != std::string::npos);
    CHECK(result.stdout_text.find("Do demo.") == std::string::npos);
}

TEST_CASE("CLI approve loads project skills for one run", "[cli][project-trust]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace home;
    workspace.write(".pi/skills/demo/SKILL.md",
                    "---\n"
                    "name: demo\n"
                    "description: Demo skill.\n"
                    "---\n"
                    "# Demo Skill\n\n"
                    "Do demo.\n");
    auto session = workspace.path() / "trusted-skills.jsonl";

    auto result = cch::tests::run_cli(cch::tests::CliRunOptions{
        .args = {"--approve", "--session", session.string(), "/skill:demo"},
        .cwd = workspace.path(),
        .env = {{"HOME", home.path().string()}},
    });

    REQUIRE(result.exit_code == 0);
    CHECK(result.stderr_text.find("[resource:warn]") == std::string::npos);
    CHECK(result.stdout_text.find("<skill name=\"demo\"") != std::string::npos);
    CHECK(result.stdout_text.find("Do demo.") != std::string::npos);
}

TEST_CASE("CLI no-approve skips project skills for one run", "[cli][project-trust]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace home;
    workspace.write(".pi/skills/demo/SKILL.md",
                    "---\n"
                    "name: demo\n"
                    "description: Demo skill.\n"
                    "---\n"
                    "# Demo Skill\n\n"
                    "Do demo.\n");
    auto session = workspace.path() / "no-approve-skills.jsonl";

    auto result = cch::tests::run_cli(cch::tests::CliRunOptions{
        .args = {"--no-approve", "--session", session.string(), "/skill:demo"},
        .cwd = workspace.path(),
        .env = {{"HOME", home.path().string()}},
    });

    REQUIRE(result.exit_code == 0);
        CHECK(result.stdout_text.find("Do demo.") == std::string::npos);
}

TEST_CASE("CLI -na short carries pi's no-approve semantics", "[cli][project-trust]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace home;
    workspace.write(".pi/skills/demo/SKILL.md",
                    "---\n"
                    "name: demo\n"
                    "description: Demo skill.\n"
                    "---\n"
                    "# Demo Skill\n\n"
                    "Do demo.\n");
    auto session = workspace.path() / "short-no-approve-skills.jsonl";

    auto result = cch::tests::run_cli(cch::tests::CliRunOptions{
        .args = {"-na", "--session", session.string(), "/skill:demo"},
        .cwd = workspace.path(),
        .env = {{"HOME", home.path().string()}},
    });

    REQUIRE(result.exit_code == 0);
        CHECK(result.stdout_text.find("Do demo.") == std::string::npos);
}

TEST_CASE("CLI approve loads project prompt templates for one run", "[cli][project-resources]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace home;
    workspace.write(".pi/prompts/greet.md",
                    "---\n"
                    "description: Greet someone\n"
                    "---\n"
                    "Project hello $1.\n");
    auto session = workspace.path() / "trusted-prompts.jsonl";

    auto result = cch::tests::run_cli(cch::tests::CliRunOptions{
        .args = {"--approve", "--session", session.string(), "/greet Ada"},
        .cwd = workspace.path(),
        .env = {{"HOME", home.path().string()}},
    });

    REQUIRE(result.exit_code == 0);
    CHECK(result.stderr_text.find("[resource:warn]") == std::string::npos);
    CHECK(result.stdout_text.find("Project hello Ada.") != std::string::npos);
}

TEST_CASE("CLI no-skills disables project skills even when approved", "[cli][project-trust]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace home;
    workspace.write(".pi/skills/demo/SKILL.md",
                    "---\n"
                    "name: demo\n"
                    "description: Demo skill.\n"
                    "---\n"
                    "# Demo Skill\n\n"
                    "Do demo.\n");
    auto session = workspace.path() / "disabled-skills.jsonl";

    auto result = cch::tests::run_cli(cch::tests::CliRunOptions{
        .args = {"--approve", "--no-skills", "--session", session.string(), "/skill:demo"},
        .cwd = workspace.path(),
        .env = {{"HOME", home.path().string()}},
    });

    REQUIRE(result.exit_code == 0);
        CHECK(result.stdout_text.find("Do demo.") == std::string::npos);
}

TEST_CASE("CLI -ns short disables project skills even when approved", "[cli][project-trust]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace home;
    workspace.write(".pi/skills/demo/SKILL.md",
                    "---\n"
                    "name: demo\n"
                    "description: Demo skill.\n"
                    "---\n"
                    "# Demo Skill\n\n"
                    "Do demo.\n");
    auto session = workspace.path() / "short-disabled-skills.jsonl";

    auto result = cch::tests::run_cli(cch::tests::CliRunOptions{
        .args = {"--approve", "-ns", "--session", session.string(), "/skill:demo"},
        .cwd = workspace.path(),
        .env = {{"HOME", home.path().string()}},
    });

    REQUIRE(result.exit_code == 0);
        CHECK(result.stdout_text.find("Do demo.") == std::string::npos);
}

TEST_CASE("CLI no-skills keeps project prompt templates", "[cli][project-resources][issue405]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace home;
    workspace.write(".pi/prompts/greet.md",
                    "---\n"
                    "description: Greet someone\n"
                    "---\n"
                    "Project hello $1.\n");
    auto session = workspace.path() / "kept-prompts.jsonl";

    // pi semantics: --no-skills drops skill discovery only; prompt templates
    // still load from the trust-gated project directory.
    auto result = cch::tests::run_cli(cch::tests::CliRunOptions{
        .args = {"--approve", "--no-skills", "--session", session.string(), "/greet Ada"},
        .cwd = workspace.path(),
        .env = {{"HOME", home.path().string()}},
    });

    REQUIRE(result.exit_code == 0);
    CHECK(result.stderr_text.find("[resource:warn]") == std::string::npos);
    CHECK(result.stdout_text.find("Project hello Ada.") != std::string::npos);
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

    auto result = cch::tests::run_cli(cch::tests::CliRunOptions{
        .args = {"--session", session.string(), "--prompt-template", "custom.md", "/custom Ada"},
        .cwd = workspace.path(),
        .env = {{"HOME", home.path().string()}},
    });

    REQUIRE(result.exit_code == 0);
    CHECK(result.stderr_text.find("[resource:warn]") == std::string::npos);
    CHECK(result.stdout_text.find("Custom hello Ada.") != std::string::npos);
}

TEST_CASE("CLI no-prompt-templates keeps explicit prompt template files", "[cli][project-resources][issue405]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace home;
    workspace.write("custom.md",
                    "---\n"
                    "description: Custom greeting\n"
                    "---\n"
                    "Custom hello $1.\n");
    auto session = workspace.path() / "explicit-template-kept.jsonl";

    // pi semantics: --no-prompt-templates drops user and project discovery
    // but keeps explicit --prompt-template paths.
    auto result = cch::tests::run_cli(cch::tests::CliRunOptions{
        .args = {"--session", session.string(), "--prompt-template", "custom.md",
                 "--no-prompt-templates", "/custom Ada"},
        .cwd = workspace.path(),
        .env = {{"HOME", home.path().string()}},
    });

    REQUIRE(result.exit_code == 0);
    CHECK(result.stdout_text.find("Custom hello Ada.") != std::string::npos);
}

TEST_CASE("CLI -np short keeps explicit prompt template files", "[cli][project-resources][issue405]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace home;
    workspace.write("custom.md",
                    "---\n"
                    "description: Custom greeting\n"
                    "---\n"
                    "Custom hello $1.\n");
    auto session = workspace.path() / "short-explicit-template-kept.jsonl";

    auto result = cch::tests::run_cli(cch::tests::CliRunOptions{
        .args = {"--session", session.string(), "--prompt-template", "custom.md",
                 "-np", "/custom Ada"},
        .cwd = workspace.path(),
        .env = {{"HOME", home.path().string()}},
    });

    REQUIRE(result.exit_code == 0);
    CHECK(result.stdout_text.find("Custom hello Ada.") != std::string::npos);
}

TEST_CASE("CLI text mode shows malformed project resource diagnostics on stderr", "[cli][project-resources]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace home;
    workspace.write(".pi/skills/bad/SKILL.md",
                    "---\n"
                    "name: bad\n"
                    "---\n"
                    "Bad skill body.\n");
    workspace.write(".pi/prompts/bad.md",
                    "---\n"
                    "bad line without colon\n"
                    "---\n"
                    "Bad prompt body.\n");
    auto session = workspace.path() / "malformed-resources.jsonl";

    auto result = cch::tests::run_cli(cch::tests::CliRunOptions{
        .args = {"--approve", "--session", session.string(), "hello"},
        .cwd = workspace.path(),
        .env = {{"HOME", home.path().string()}},
    });

    REQUIRE(result.exit_code == 0);
    // pi ResourceDiagnostic shape: no C++-invented codes, the message carries
    // the detail under a stable resource:warning code.
    CHECK(result.stderr_text.find("[resource:warn] warning: description is required") != std::string::npos);
    CHECK(result.stderr_text.find("[resource:warn] warning: YAML frontmatter parse error") != std::string::npos);
    CHECK(result.stdout_text == "fake: hello\n");
}

TEST_CASE("CLI applies settings.json model when CLI omits --model", "[cli][settings]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace home;
    home.write(".pi/agent/settings.json", R"({"defaultModel":"config-model-name"})");
    auto session = workspace.path() / "settings-model-session.jsonl";

    auto result = cch::tests::run_cli(cch::tests::CliRunOptions{
        .args = {"--session", session.string(), "hello"},
        .cwd = workspace.path(),
        .env = {{"HOME", home.path().string()},
                {"PI_CODING_AGENT_DIR", std::nullopt}},
    });

    REQUIRE(result.exit_code == 0);
    const auto header = parse_json_line(read_file(session));
    const auto& object = as_object(header);
    CHECK(json_string_at(object, "type") == "session");
    CHECK(json_string_at(object, "model") == "config-model-name");
}

TEST_CASE("CLI invalid explicit prompt template fails before session creation", "[cli][project-resources][assembly]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace home;
    auto session = workspace.path() / "explicit-fail.jsonl";

    auto result = cch::tests::run_cli(cch::tests::CliRunOptions{
        .args = {"--session", session.string(), "--prompt-template", "missing.md", "hello"},
        .cwd = workspace.path(),
        .env = {{"HOME", home.path().string()}},
    });

    REQUIRE(result.exit_code != 0);
    CHECK(result.stdout_text.empty());
    CHECK((result.stderr_text.find("template") != std::string::npos ||
          result.stderr_text.find("explicit") != std::string::npos));
    CHECK_FALSE(std::filesystem::exists(session));
}

TEST_CASE("CLI rejects an explicit prompt template with an unsupported file type", "[cli][project-resources][assembly]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace home;
    workspace.write("not-a-template.txt", "plain text\n");
    auto session = workspace.path() / "explicit-type-fail.jsonl";

    auto result = cch::tests::run_cli(cch::tests::CliRunOptions{
        .args = {"--session", session.string(), "--prompt-template", "not-a-template.txt", "hello"},
        .cwd = workspace.path(),
        .env = {{"HOME", home.path().string()}},
    });

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

    auto result = cch::tests::run_cli(cch::tests::CliRunOptions{
        .args = {"--session", session.string(),
                 "--prompt-template", "valid.md", "--prompt-template", "empty-prompts", "hello"},
        .cwd = workspace.path(),
        .env = {{"HOME", home.path().string()}},
    });

    REQUIRE(result.exit_code != 0);
    CHECK(result.stdout_text.empty());
    CHECK(result.stderr_text.find("no loadable .md files") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(session));
}

TEST_CASE("CLI --session open with explicit model override reports diagnostic and uses override", "[cli][provider-resolution]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace home;
    auto session = workspace.path() / "resume-override.jsonl";

    auto first = cch::tests::run_cli(cch::tests::CliRunOptions{
        .args = {"--session", session.string(), "first"},
        .cwd = workspace.path(),
        .env = {{"HOME", home.path().string()}},
    });
    REQUIRE(first.exit_code == 0);

    auto second = cch::tests::run_cli(cch::tests::CliRunOptions{
        .args = {"--session", session.string(), "--model", "override-model", "second"},
        .cwd = workspace.path(),
        .env = {{"HOME", home.path().string()}},
    });

    REQUIRE(second.exit_code == 0);
    CHECK(second.stderr_text.find("resume_provider_override") != std::string::npos);
    CHECK(second.stdout_text == "fake: second\n");
}

TEST_CASE("CLI --session open without override retains stored provider and model", "[cli][provider-resolution]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace home;
    auto session = workspace.path() / "resume-retain.jsonl";

    auto first = cch::tests::run_cli(cch::tests::CliRunOptions{
        .args = {"--session", session.string(), "first"},
        .cwd = workspace.path(),
        .env = {{"HOME", home.path().string()}},
    });
    REQUIRE(first.exit_code == 0);

    auto second = cch::tests::run_cli(cch::tests::CliRunOptions{
        .args = {"--session", session.string(), "second"},
        .cwd = workspace.path(),
        .env = {{"HOME", home.path().string()}},
    });

    REQUIRE(second.exit_code == 0);
    CHECK(second.stderr_text.find("resume_provider_override") == std::string::npos);
    CHECK(second.stdout_text == "fake: second\n");
}

TEST_CASE("CLI resume falls back with a diagnostic when the stored model no longer resolves", "[cli][resume][issue346]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace home;
    const auto models_path = home.path() / ".pi" / "agent" / "models.json";
    home.write(".pi/agent/models.json", R"({
      "providers": {
        "deepseek": {
          "baseUrl": "https://api.deepseek.example/v1",
          "api": "openai-responses",
          "apiKey": "configured-key",
          "models": [{"id": "deepseek-v4-flash"}]
        }
      }
    })");
    auto session = workspace.path() / "resume-unresolved.jsonl";

    // Create a session that records deepseek/deepseek-v4-flash without
    // streaming: a no-prompt print run still creates the session (pi).
    auto first = run_command_split(
        "cd " + shell_quote(workspace.path()) + " && HOME=" + shell_quote(home.path()) + " env -u PI_CODING_AGENT_DIR " + bin() +
        " --session " + shell_quote(session) +
        " --model deepseek-v4-flash");
    REQUIRE(first.exit_code == 0);

    // Remove the configured model, then resume: the stored identity no longer
    // resolves, so the chain falls back through the runtime default. The
    // fallback message is an interactive boot warning only (pi
    // `modelFallbackMessage`); print mode drops it entirely.
    std::filesystem::remove(models_path);
    auto second = run_command_split(
        "cd " + shell_quote(workspace.path()) + " && HOME=" + shell_quote(home.path()) + " env -u PI_CODING_AGENT_DIR " + bin() +
        " --session " + shell_quote(session));

    REQUIRE(second.exit_code == 0);
    CHECK(second.stderr_text.find("Could not restore model") == std::string::npos);
}

TEST_CASE("CLI --api-key installs an in-memory runtime API key override", "[cli][api-key][issue346]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace home;
    auto session = workspace.path() / "api-key-session.jsonl";

    auto result = cch::tests::run_cli(cch::tests::CliRunOptions{
        .args = {"--session", session.string(), "--model", "fake-model",
                 "--api-key", "sk-runtime-hello", "hello"},
        .cwd = workspace.path(),
        .env = {{"HOME", home.path().string()}},
    });

    REQUIRE(result.exit_code == 0);
    CHECK(result.stdout_text.find("fake: hello") != std::string::npos);
    // The runtime API key is never persisted into the session file.
    const auto content = read_file(session);
    CHECK(content.find("sk-runtime-hello") == std::string::npos);
}

TEST_CASE("CLI rejects --api-key without an explicit model", "[cli][api-key][issue346]") {
    cch::tests::TempWorkspace workspace;
    auto result = run_command_split(
        "cd " + shell_quote(workspace.path()) + " && " + bin() + " --api-key sk-nomodel hello");
    REQUIRE(result.exit_code == 1);
    CHECK(
        result.stderr_text.find("--api-key requires a model") != std::string::npos);
}

TEST_CASE("CLI print mode slash text reaches AgentSession and the session file", "[cli][commands]") {
    cch::tests::TempWorkspace workspace;
    auto session = workspace.path() / "command-adapter.jsonl";
    auto result = run_in_workspace(workspace, {"--session", session.string(), "/help"});

    REQUIRE(result.exit_code == 0);
    CHECK(result.stdout_text == "fake: /help\n");

    const auto content = read_file(session);
    const auto lines = non_empty_lines(content);
    REQUIRE(lines.size() >= 3);
    auto header = parse_json_line(lines.front());
    CHECK(json_string_at(as_object(header), "type") == "session");

    // The slash text is an ordinary prompt: the session records the user
    // message and the assistant reply.
    bool saw_message = false;
    for (std::size_t i = 1; i < lines.size(); ++i) {
        auto record = parse_json_line(lines[i]);
        if (json_string_at(as_object(record), "type") == "message") {
            saw_message = true;
        }
    }
    CHECK(saw_message);
}

TEST_CASE("CLI default creation stores the session under the workspace-keyed agent config directory", "[cli][default-session]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace agent_root;
    const auto agent_dir = agent_root.path() / "agent";
    const auto canonical_workspace = std::filesystem::canonical(workspace.path());

    auto result = cch::tests::run_cli(cch::tests::CliRunOptions{
        .args = {"hello"},
        .cwd = workspace.path(),
        .env = {{"PI_CODING_AGENT_DIR", agent_dir.string()}},
    });

    REQUIRE(result.exit_code == 0);
    CHECK(result.stdout_text == "fake: hello\n");

    const auto created = require_single_automatic_session(agent_dir / "sessions", canonical_workspace);
    CHECK(json_string_at(created.header, "provider") == "fake");
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

    auto direct = cch::tests::run_cli(cch::tests::CliRunOptions{
        .args = {"first"},
        .cwd = real.path(),
        .env = {{"PI_CODING_AGENT_DIR", agent_dir.string()}},
    });
    REQUIRE(direct.exit_code == 0);
    auto aliased = cch::tests::run_cli(cch::tests::CliRunOptions{
        .args = {"second"},
        .cwd = alias,
        .env = {{"PI_CODING_AGENT_DIR", agent_dir.string()}},
    });
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

TEST_CASE("CLI piped print propagates the same default persisted target", "[cli][default-session][issue64]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace agent_root;
    const auto agent_dir = agent_root.path() / "agent";
    const auto canonical_workspace = std::filesystem::canonical(workspace.path());

    auto result = cch::tests::run_cli(cch::tests::CliRunOptions{
        .args = {},
        .cwd = workspace.path(),
        .env = {{"PI_CODING_AGENT_DIR", agent_dir.string()}},
        .stdin_text = "hello",
    });

    REQUIRE(result.exit_code == 0);
    CHECK(result.stdout_text == "fake: hello\n");
    require_single_automatic_session(agent_dir / "sessions", canonical_workspace);
}

TEST_CASE("CLI explicit session targets keep their exact paths outside the default root", "[cli][default-session]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace agent_root;
    const auto agent_dir = agent_root.path() / "agent";
    const auto explicit_session = workspace.path() / "explicit.jsonl";

    auto created = cch::tests::run_cli(cch::tests::CliRunOptions{
        .args = {"--session", explicit_session.string(), "first"},
        .cwd = workspace.path(),
        .env = {{"PI_CODING_AGENT_DIR", agent_dir.string()}},
    });
    REQUIRE(created.exit_code == 0);
    CHECK(std::filesystem::exists(explicit_session));
    CHECK_FALSE(std::filesystem::exists(agent_dir / "sessions"));

    auto resumed = cch::tests::run_cli(cch::tests::CliRunOptions{
        .args = {"--session", explicit_session.string(), "second"},
        .cwd = workspace.path(),
        .env = {{"PI_CODING_AGENT_DIR", agent_dir.string()}},
    });
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

    auto seed = cch::tests::run_cli(cch::tests::CliRunOptions{
        .args = {"--session", legacy_file.string(), "legacy-seed"},
        .cwd = workspace.path(),
        .env = {{"PI_CODING_AGENT_DIR", agent_dir.string()}},
    });
    REQUIRE(seed.exit_code == 0);
    const auto legacy_before = read_file(legacy_file);

    auto result = cch::tests::run_cli(cch::tests::CliRunOptions{
        .args = {"hello"},
        .cwd = workspace.path(),
        .env = {{"PI_CODING_AGENT_DIR", agent_dir.string()}},
    });
    REQUIRE(result.exit_code == 0);
    CHECK(read_file(legacy_file) == legacy_before);
    CHECK(jsonl_files_under(legacy_dir).size() == 1);
    require_single_automatic_session(agent_dir / "sessions", canonical_workspace);

    // A valid old file remains usable only through the explicit open contract.
    auto resumed = cch::tests::run_cli(cch::tests::CliRunOptions{
        .args = {"--session", legacy_file.string(), "second"},
        .cwd = workspace.path(),
        .env = {{"PI_CODING_AGENT_DIR", agent_dir.string()}},
    });
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
        "cd " + shell_quote(workspace.path()) + " && PI_CODING_AGENT_DIR=" + shell_quote(blocker) + " " + bin() +
        " hello");

    REQUIRE(result.exit_code != 0);
    CHECK(result.stdout_text.empty());
    CHECK(result.stderr_text.find("could not") != std::string::npos);
    CHECK(result.stderr_text.find(blocker.string()) != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(workspace.path() / ".cpp-harness" / "sessions"));
}

TEST_CASE("CLI default creation fails explicitly when no user-level root can be resolved", "[cli][default-session]") {
    cch::tests::TempWorkspace workspace;

    auto result = run_command_split(
        "cd " + shell_quote(workspace.path()) + " && env -u HOME -u USERPROFILE -u PI_CODING_AGENT_DIR " + bin() +
        " hello");

    REQUIRE(result.exit_code != 0);
    CHECK(result.stdout_text.empty());
    CHECK(result.stderr_text.find("Agent Config Directory sessions root could not be resolved") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(workspace.path() / ".cpp-harness" / "sessions"));
}

TEST_CASE("CLI help describes automatic user-level session storage", "[cli][default-session]") {
    auto result = run_command(bin() + " --help");

    REQUIRE(result.exit_code == 0);
    CHECK(result.output.find("PI_CODING_AGENT_DIR") != std::string::npos);
    CHECK(result.output.find("sessions") != std::string::npos);
    CHECK(result.output.find("--session-id") != std::string::npos);
}

TEST_CASE("CLI failed assembly publishes no default session file", "[cli][default-session][assembly]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace agent_root;
    const auto agent_dir = agent_root.path() / "agent";

    auto result = cch::tests::run_cli(cch::tests::CliRunOptions{
        .args = {"--prompt-template", "missing.md", "hello"},
        .cwd = workspace.path(),
        .env = {{"PI_CODING_AGENT_DIR", agent_dir.string()}},
    });

    REQUIRE(result.exit_code != 0);
    CHECK(result.stdout_text.empty());
    CHECK((result.stderr_text.find("template") != std::string::npos ||
          result.stderr_text.find("explicit") != std::string::npos));
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

    auto result = cch::tests::run_cli(cch::tests::CliRunOptions{
        .args = {"--no-session", "hello"},
        .cwd = workspace.path(),
        .env = {{"PI_CODING_AGENT_DIR", agent_dir.string()}},
    });

    REQUIRE(result.exit_code == 0);
    CHECK(result.stdout_text == "fake: hello\n");
    CHECK(result.stderr_text.empty());
    require_no_session_filesystem_state(agent_dir, workspace.path());
}

TEST_CASE("CLI --no-session sends /session to the model as an ordinary prompt", "[cli][no-session][issue64]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace agent_root;
    const auto agent_dir = agent_root.path() / "agent";

    auto result = cch::tests::run_cli(cch::tests::CliRunOptions{
        .args = {"--no-session", "--print", "/session"},
        .cwd = workspace.path(),
        .env = {{"PI_CODING_AGENT_DIR", agent_dir.string()}},
    });

    REQUIRE(result.exit_code == 0);
    CHECK(result.stdout_text == "fake: /session\n");
    require_no_session_filesystem_state(agent_dir, workspace.path());
}

TEST_CASE("CLI print mode sends /session to the model under default session storage", "[cli][no-session]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace agent_root;
    const auto agent_dir = agent_root.path() / "agent";
    const auto canonical_workspace = std::filesystem::canonical(workspace.path());

    auto result = cch::tests::run_cli(cch::tests::CliRunOptions{
        .args = {"/session"},
        .cwd = workspace.path(),
        .env = {{"PI_CODING_AGENT_DIR", agent_dir.string()}},
    });

    REQUIRE(result.exit_code == 0);
    CHECK(result.stdout_text == "fake: /session\n");
    require_single_automatic_session(agent_dir / "sessions", canonical_workspace);
}

TEST_CASE("CLI --no-session short-circuits silently over explicit create and resume", "[cli][no-session]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace agent_root;
    const auto agent_dir = agent_root.path() / "agent";
    const auto explicit_session = workspace.path() / "explicit.jsonl";

    // pi: --no-session wins silently over --session/--resume; the C++-today
    // conflict errors are deleted and no session file is ever written.
    auto created = cch::tests::run_cli(cch::tests::CliRunOptions{
        .args = {"--no-session", "--session", explicit_session.string(), "hello"},
        .cwd = workspace.path(),
        .env = {{"PI_CODING_AGENT_DIR", agent_dir.string()}},
    });
    REQUIRE(created.exit_code == 0);
    CHECK(created.stdout_text == "fake: hello\n");
    CHECK(created.stderr_text.find("cannot be combined") == std::string::npos);
    CHECK_FALSE(std::filesystem::exists(explicit_session));

    auto resumed = cch::tests::run_cli(cch::tests::CliRunOptions{
        .args = {"--no-session", "--resume", "hello"},
        .cwd = workspace.path(),
        .env = {{"PI_CODING_AGENT_DIR", agent_dir.string()}},
    });
    REQUIRE(resumed.exit_code == 0);
    CHECK(resumed.stdout_text == "fake: hello\n");
    CHECK(resumed.stderr_text.find("cannot be combined") == std::string::npos);

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

    auto result = cch::tests::run_cli(cch::tests::CliRunOptions{
        .args = {"--no-session", "hello"},
        .cwd = workspace.path(),
        .env = {{"PI_CODING_AGENT_DIR", blocker.string()}},
    });

    // In-memory operation is explicit, so unusable default storage is never
    // inspected and cannot fail or redirect the run.
    REQUIRE(result.exit_code == 0);
    CHECK(result.stdout_text == "fake: hello\n");
    CHECK_FALSE(std::filesystem::exists(workspace.path() / ".cpp-harness" / "sessions"));
}

TEST_CASE("CLI --no-session preserves tool execution and events", "[cli][no-session]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace agent_root;
    const auto agent_dir = agent_root.path() / "agent";
    std::ofstream(workspace.path() / "note.txt") << "in-memory tool text";

    auto result = cch::tests::run_cli(cch::tests::CliRunOptions{
        .args = {"--no-session", "read note.txt"},
        .cwd = workspace.path(),
        .env = {{"PI_CODING_AGENT_DIR", agent_dir.string()}},
    });

    REQUIRE(result.exit_code == 0);
    // Print mode presents only the final assistant text; the tool execution
    // still happened (the observation reached the final response).
    CHECK(result.stdout_text == "fake observed: in-memory tool text\n");
    require_no_session_filesystem_state(agent_dir, workspace.path());
}

TEST_CASE("CLI --no-session publishes no filesystem state after a startup failure", "[cli][no-session][assembly]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace agent_root;
    const auto agent_dir = agent_root.path() / "agent";

    auto result = cch::tests::run_cli(cch::tests::CliRunOptions{
        .args = {"--no-session", "--prompt-template", "missing.md", "hello"},
        .cwd = workspace.path(),
        .env = {{"PI_CODING_AGENT_DIR", agent_dir.string()}},
    });

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

    auto result = cch::tests::run_cli(cch::tests::CliRunOptions{
        .args = {"--session-dir", override_dir.string(), "hello"},
        .cwd = workspace.path(),
        .env = {{"PI_CODING_AGENT_DIR", agent_dir.string()}},
    });

    REQUIRE(result.exit_code == 0);
    CHECK(result.stdout_text == "fake: hello\n");

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

    auto flagged = cch::tests::run_cli(cch::tests::CliRunOptions{
        .args = {"--session-dir", flag_dir.string(), "first"},
        .cwd = workspace.path(),
        .env = {{"PI_CODING_AGENT_DIR", agent_dir.string()},
                {"PI_CODING_AGENT_SESSION_DIR", env_dir.string()}},
    });
    REQUIRE(flagged.exit_code == 0);
    CHECK(jsonl_files_under(flag_dir).size() == 1);
    CHECK_FALSE(std::filesystem::exists(env_dir));
    CHECK_FALSE(std::filesystem::exists(settings_dir));

    auto from_env = cch::tests::run_cli(cch::tests::CliRunOptions{
        .args = {"second"},
        .cwd = workspace.path(),
        .env = {{"PI_CODING_AGENT_DIR", agent_dir.string()},
                {"PI_CODING_AGENT_SESSION_DIR", env_dir.string()}},
    });
    REQUIRE(from_env.exit_code == 0);
    CHECK(jsonl_files_under(env_dir).size() == 1);
    CHECK_FALSE(std::filesystem::exists(settings_dir));

    auto from_settings = cch::tests::run_cli(cch::tests::CliRunOptions{
        .args = {"third"},
        .cwd = workspace.path(),
        .env = {{"PI_CODING_AGENT_DIR", agent_dir.string()}},
    });
    REQUIRE(from_settings.exit_code == 0);
    CHECK(jsonl_files_under(settings_dir).size() == 1);
    CHECK_FALSE(std::filesystem::exists(agent_dir / "sessions"));
}

TEST_CASE("CLI relative --session-dir resolves against the final workspace", "[cli][session-dir]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace agent_root;
    const auto agent_dir = agent_root.path() / "agent";
    const auto canonical_workspace = std::filesystem::canonical(workspace.path());

    auto result = cch::tests::run_cli(cch::tests::CliRunOptions{
        .args = {"--session-dir", "my-sessions", "hello"},
        .cwd = workspace.path(),
        .env = {{"PI_CODING_AGENT_DIR", agent_dir.string()}},
    });
    REQUIRE(result.exit_code == 0);
    require_single_session_in_directory(canonical_workspace / "my-sessions", canonical_workspace);
    CHECK_FALSE(std::filesystem::exists(agent_dir / "sessions"));

    // The workspace itself determines resolution: the same relative value from
    // a later run lands in the same workspace-relative place.
    auto again = cch::tests::run_cli(cch::tests::CliRunOptions{
        .args = {"--session-dir", "my-sessions", "again"},
        .cwd = workspace.path(),
        .env = {{"PI_CODING_AGENT_DIR", agent_dir.string()}},
    });
    REQUIRE(again.exit_code == 0);
    CHECK(jsonl_files_under(canonical_workspace / "my-sessions").size() == 2);
}

TEST_CASE("CLI session directory override expands a leading home marker", "[cli][session-dir]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace agent_root;
    cch::tests::TempWorkspace home_root;
    const auto agent_dir = agent_root.path() / "agent";
    const auto canonical_workspace = std::filesystem::canonical(workspace.path());

    auto result = cch::tests::run_cli(cch::tests::CliRunOptions{
        .args = {"--session-dir", "~/tilde-sessions", "hello"},
        .cwd = workspace.path(),
        .env = {{"HOME", home_root.path().string()},
                {"PI_CODING_AGENT_DIR", agent_dir.string()}},
    });

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

    auto created = cch::tests::run_cli(cch::tests::CliRunOptions{
        .args = {"--session-dir", override_dir.string(), "--session", explicit_session.string(), "first"},
        .cwd = workspace.path(),
        .env = {{"PI_CODING_AGENT_DIR", agent_dir.string()}},
    });
    REQUIRE(created.exit_code == 0);
    CHECK(std::filesystem::exists(explicit_session));
    CHECK_FALSE(std::filesystem::exists(override_dir));

    auto resumed = cch::tests::run_cli(cch::tests::CliRunOptions{
        .args = {"--session", explicit_session.string(), "second"},
        .cwd = workspace.path(),
        .env = {{"PI_CODING_AGENT_DIR", agent_dir.string()},
                {"PI_CODING_AGENT_SESSION_DIR", override_dir.string()}},
    });
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

    auto result = cch::tests::run_cli(cch::tests::CliRunOptions{
        .args = {"--no-session", "--session-dir", override_dir.string(), "hello"},
        .cwd = workspace.path(),
        .env = {{"PI_CODING_AGENT_DIR", agent_dir.string()}},
    });

    REQUIRE(result.exit_code == 0);
    CHECK(result.stdout_text == "fake: hello\n");
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

    auto result = cch::tests::run_cli(cch::tests::CliRunOptions{
        .args = {"hello"},
        .cwd = workspace.path(),
        .env = {{"PI_CODING_AGENT_DIR", agent_dir.string()}},
    });

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

    auto result = cch::tests::run_cli(cch::tests::CliRunOptions{
        .args = {"hello"},
        .cwd = workspace.path(),
        .env = {{"PI_CODING_AGENT_DIR", agent_dir.string()}},
    });

    REQUIRE(result.exit_code == 0);
    CHECK(result.stderr_text.find("could not load global settings") != std::string::npos);
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

    auto result = cch::tests::run_cli(cch::tests::CliRunOptions{
        .args = {"--session-dir", blocker.string(), "hello"},
        .cwd = workspace.path(),
        .env = {{"PI_CODING_AGENT_DIR", agent_dir.string()}},
    });

    REQUIRE(result.exit_code != 0);
    CHECK(result.stdout_text.empty());
    CHECK(result.stderr_text.find("could not") != std::string::npos);
    CHECK(result.stderr_text.find(blocker.string()) != std::string::npos);
    // No fallback: neither the default root nor the workspace gains a transcript.
    CHECK_FALSE(std::filesystem::exists(agent_dir / "sessions"));
    CHECK_FALSE(std::filesystem::exists(workspace.path() / ".cpp-harness" / "sessions"));
}

TEST_CASE(
    "CLI non-interactive resume of a session with a vanished cwd prints pi's error and exits 1",
    "[cli][session-family][issue404]") {
    cch::tests::TempWorkspace original;
    cch::tests::TempWorkspace other;
    cch::tests::TempWorkspace storage;
    auto session = storage.path() / "vanished-cwd.jsonl";
    auto first = run_in_workspace(
        original, {"--session", session.string(), "first"});
    REQUIRE(first.exit_code == 0);

    // The session header cwd vanishes while the session file survives.
    std::error_code ec;
    REQUIRE(std::filesystem::remove_all(original.path(), ec) > 0);
    CHECK_FALSE(ec);

    auto resumed = cch::tests::run_cli(cch::tests::CliRunOptions{
        .args = {"--session", session.string(), "second"},
        .cwd = other.path(),
    });

    REQUIRE(resumed.exit_code == 1);
    CHECK(resumed.stdout_text.empty());
    CHECK(resumed.stderr_text == missing_cwd_error_text(
        original.path(), session, other.path()));
    CHECK_FALSE(std::filesystem::exists(session.parent_path() / "sessions"));
}

TEST_CASE(
    "CLI --session open of a session with a vanished cwd fails identically",
    "[cli][session-family][issue404]") {
    cch::tests::TempWorkspace original;
    cch::tests::TempWorkspace other;
    cch::tests::TempWorkspace storage;
    auto session = storage.path() / "vanished-open.jsonl";
    auto first = run_in_workspace(
        original, {"--session", session.string(), "first"});
    REQUIRE(first.exit_code == 0);

    std::error_code ec;
    REQUIRE(std::filesystem::remove_all(original.path(), ec) > 0);
    CHECK_FALSE(ec);

    // pi `SessionManager.open` on the existing file resumes (header cwd from
    // the file), so the missing-cwd boot check applies to --session too.
    auto resumed = cch::tests::run_cli(cch::tests::CliRunOptions{
        .args = {"--session", session.string(), "second"},
        .cwd = other.path(),
    });

    REQUIRE(resumed.exit_code == 1);
    CHECK(resumed.stderr_text == missing_cwd_error_text(
        original.path(), session, other.path()));
}

TEST_CASE("CLI --skill explicit paths load and survive --no-skills", "[cli][skill][issue412]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace home;
    workspace.write("explicit-skill.md",
                    "---\n"
                    "name: explicit-skill\n"
                    "description: Explicit skill.\n"
                    "---\n"
                    "# Explicit Skill\n\n"
                    "Do explicit.\n");
    auto session = workspace.path() / "explicit-skill.jsonl";

    auto result = cch::tests::run_cli(cch::tests::CliRunOptions{
        .args = {"--no-skills", "--skill", "explicit-skill.md",
                 "--session", session.string(), "/skill:explicit-skill"},
        .cwd = workspace.path(),
        .env = {{"HOME", home.path().string()}},
    });

    REQUIRE(result.exit_code == 0);
    // --no-skills drops discovery but keeps explicit paths (pi args.ts).
    CHECK(result.stdout_text.find("<skill name=\"explicit-skill\"") != std::string::npos);
    CHECK(result.stdout_text.find("Do explicit.") != std::string::npos);
}

TEST_CASE("CLI --no-skills drops discovered skills but keeps the prompt", "[cli][skill][issue412]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace home;
    workspace.write(".pi/skills/demo/SKILL.md",
                    "---\n"
                    "name: demo\n"
                    "description: Demo skill.\n"
                    "---\n"
                    "# Demo Skill\n\n"
                    "Do demo.\n");
    auto session = workspace.path() / "no-skills.jsonl";

    auto result = cch::tests::run_cli(cch::tests::CliRunOptions{
        .args = {"--approve", "--no-skills", "--session", session.string(), "/skill:demo"},
        .cwd = workspace.path(),
        .env = {{"HOME", home.path().string()}},
    });

    REQUIRE(result.exit_code == 0);
    // The unknown skill passes through as an ordinary prompt (no expansion).
    CHECK(result.stdout_text.find("Do demo.") == std::string::npos);
}

TEST_CASE("CLI loads user skills from ~/.pi/agent/skills with root-level .md inclusion", "[cli][skill][issue412]") {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace home;
    home.write(".pi/agent/skills/user-skill/SKILL.md",
               "---\n"
               "name: user-skill\n"
               "description: User skill.\n"
               "---\n"
               "# User Skill\n\n"
               "Do user.\n");
    auto session = workspace.path() / "user-skill.jsonl";

    auto result = cch::tests::run_cli(cch::tests::CliRunOptions{
        .args = {"--session", session.string(), "/skill:user-skill"},
        .cwd = workspace.path(),
        .env = {{"HOME", home.path().string()},
                {"PI_CODING_AGENT_DIR", std::nullopt}},
    });

    REQUIRE(result.exit_code == 0);
    CHECK(result.stdout_text.find("<skill name=\"user-skill\"") != std::string::npos);
    CHECK(result.stdout_text.find("Do user.") != std::string::npos);
}
