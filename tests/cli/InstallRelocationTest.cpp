#include <catch2/catch_test_macros.hpp>

#include "support/CliRunFixture.hpp"
#include "support/ShellQuoting.hpp"
#include "support/TempWorkspace.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include <sys/wait.h>

#ifndef CCH_BINARY
#define CCH_BINARY "./cpp_harness"
#endif
#ifndef CCH_BUILD_DIR
#define CCH_BUILD_DIR "./build"
#endif
#ifndef CCH_CMAKE_COMMAND
#define CCH_CMAKE_COMMAND "cmake"
#endif
#ifndef CCH_PYTHON3
#define CCH_PYTHON3 "python3"
#endif
#ifndef CCH_SOURCE_DIR
#define CCH_SOURCE_DIR "."
#endif

namespace {

namespace fs = std::filesystem;

using cch::tests::shell_quote;

struct CommandResult {
    int exit_code{0};
    std::string stdout_text;
    std::string stderr_text;
};

std::string read_file(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

/// Run a shell command with stdout/stderr captured into separate files under
/// `capture_dir` (each invocation owns its capture pair, so a reused
/// directory never mixes output across commands).
CommandResult run_command(const std::string& command, const fs::path& capture_dir) {
    static int capture_counter = 0;
    const auto stamp = capture_dir / ("capture-" + std::to_string(capture_counter++));
    std::error_code dir_error;
    fs::create_directories(stamp, dir_error);
    const auto stdout_path = stamp / "stdout.txt";
    const auto stderr_path = stamp / "stderr.txt";
    const int status = std::system(
        (command + " > '" + stdout_path.string() + "' 2> '" + stderr_path.string() + "'").c_str());
    const int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : status;
    return {exit_code, read_file(stdout_path), read_file(stderr_path)};
}

/// Every file expected in the staged Runtime-only install: the Runtime itself
/// and the required third-party license/notice texts. Anything else is a
/// development-surface leak.
[[nodiscard]] std::vector<std::string> expected_staged_files() {
    return {
        "bin/cpp_harness",
        "share/cpp_harness/licenses/boost.txt",
        "share/cpp_harness/licenses/cli11.txt",
        "share/cpp_harness/licenses/glaze.txt",
        "share/cpp_harness/licenses/libwebp.txt",
        "share/cpp_harness/licenses/md4c.txt",
        "share/cpp_harness/licenses/openssl.txt",
        "share/cpp_harness/licenses/stb.txt",
        "share/cpp_harness/licenses/utf8proc.txt",
    };
}

[[nodiscard]] bool is_development_artifact(const std::string& relative) {
    const std::vector<std::string> forbidden_prefixes{"include/", "lib/", "lib64/", "lib/cmake/"};
    for (const auto& prefix : forbidden_prefixes) {
        if (relative.starts_with(prefix)) {
            return true;
        }
    }
    const std::vector<std::string> forbidden_suffixes{
        ".cmake", ".h", ".hpp", ".a", ".so", "Config.cmake", "Targets.cmake"};
    for (const auto& suffix : forbidden_suffixes) {
        if (relative.ends_with(suffix)) {
            return true;
        }
    }
    return relative.find("CMake") != std::string::npos;
}

} // namespace

// Issue #472: a clean-prefix staging install produces only the relocatable
// Runtime and its required notices, passes the dependency-closure audit, and
// keeps working after relocation without network credentials.
TEST_CASE(
    "staged install contains only the relocatable Runtime and behaves after relocation",
    "[cli][install][issue472]") {
#ifdef CCH_SANITIZER_BUILD
    // A sanitizer build links libasan/libubsan into every binary, so the
    // dependency-closure audit correctly refuses it: sanitizers gate the
    // test suite, they never produce a shippable install (issue #473).
    SKIP("staged install is a shippable-configuration contract; sanitizer builds are not installable");
#else
    cch::tests::TempWorkspace root;
    const auto stage = root.path() / "stage";
    const auto captures = root.path() / "captures";

    // ── Stage into a clean prefix (the install path runs the Gate first) ──
    const auto install = run_command(
        shell_quote(CCH_CMAKE_COMMAND) + " --install " + shell_quote(CCH_BUILD_DIR) +
            " --prefix " + shell_quote(stage),
        captures);
    INFO(install.stdout_text + install.stderr_text);
    REQUIRE(install.exit_code == 0);

    // ── Layout: exactly the Runtime and the required notices ──
    std::vector<std::string> files;
    std::error_code walk_error;
    for (const auto& entry : fs::recursive_directory_iterator(stage, walk_error)) {
        CHECK_FALSE(entry.is_symlink(walk_error));
        if (entry.is_regular_file(walk_error)) {
            files.push_back(fs::relative(entry.path(), stage, walk_error).generic_string());
        }
    }
    std::sort(files.begin(), files.end());
    CHECK(files == expected_staged_files());
    for (const auto& file : files) {
        INFO(file);
        CHECK_FALSE(is_development_artifact(file));
    }
    std::error_code probe_error;
    CHECK_FALSE(fs::exists(stage / "include", probe_error));
    CHECK_FALSE(fs::exists(stage / "lib", probe_error));

    // Licenses and notices are required runtime material, not empty placeholders.
    for (const auto& relative : files) {
        if (relative.starts_with("share/")) {
            INFO(relative);
            std::error_code size_error;
            const auto size = fs::file_size(stage / relative, size_error);
            CHECK_FALSE(size_error);
            CHECK(size > 0);
        }
    }

    // ── Dependency-closure audit on the staged Runtime ──
    const auto staged_binary = stage / "bin" / "cpp_harness";
    const auto source_dir = fs::path(CCH_SOURCE_DIR);
    const auto audit = run_command(
        shell_quote(CCH_PYTHON3) + " " +
            shell_quote(source_dir / "cmake" / "install" / "audit_runtime_deps.py") +
            " --binary " + shell_quote(staged_binary) + " --allowlist " +
            shell_quote(source_dir / "cmake" / "install" / "runtime-deps.json") +
            " --forbid-root " + shell_quote(source_dir) + " --forbid-root " +
            shell_quote(CCH_BUILD_DIR),
        captures);
    INFO(audit.stdout_text + audit.stderr_text);
    CHECK(audit.exit_code == 0);

    // ── Relocate the whole prefix outside the build-tree spelling ──
    const auto relocated = root.path() / "relocated";
    std::error_code rename_error;
    fs::rename(stage, relocated, rename_error);
    REQUIRE_FALSE(rename_error);
    const auto relocated_binary = relocated / "bin" / "cpp_harness";

    // Scrubbed environment: no inherited credentials, proxies, or config.
    const auto home = root.path() / "home";
    const auto agent_dir = root.path() / "agent";
    std::error_code home_error;
    fs::create_directories(home, home_error);
    const std::string clean_env = "env -i PATH=/usr/bin:/bin HOME=" + shell_quote(home) +
        " PI_CODING_AGENT_DIR=" + shell_quote(agent_dir) + " ";
    const std::string relocated_run =
        "cd " + shell_quote(root.path()) + " && " + clean_env + shell_quote(relocated_binary);

    // ── --version matches the build-tree Runtime ──
    const auto build_version = run_command(shell_quote(CCH_BINARY) + " --version", captures);
    REQUIRE(build_version.exit_code == 0);
    const auto staged_version = run_command(relocated_run + " --version", captures);
    INFO(staged_version.stdout_text + staged_version.stderr_text);
    REQUIRE(staged_version.exit_code == 0);
    CHECK(staged_version.stdout_text == build_version.stdout_text);
    CHECK_FALSE(staged_version.stdout_text.empty());

    // ── --help prints the pi-aligned flag surface ──
    const auto staged_help = run_command(relocated_run + " --help", captures);
    INFO(staged_help.stdout_text + staged_help.stderr_text);
    REQUIRE(staged_help.exit_code == 0);
    CHECK(staged_help.stdout_text.find("Usage:") != std::string::npos);
    CHECK(staged_help.stdout_text.find("--print") != std::string::npos);

    // ── Offline staged run without credentials fails deterministically ──
    const auto offline =
        run_command(relocated_run + " --print ping < /dev/null", captures);
    INFO(offline.stdout_text + offline.stderr_text);
    CHECK(offline.exit_code == 1);
    CHECK(offline.stderr_text.find("Unknown provider") != std::string::npos);

    // ── Offline fake-provider smoke outside the build tree (in-process seam) ──
    auto smoke = cch::tests::run_cli(cch::tests::CliRunOptions{
        .args = {"--print", "hello"},
        .cwd = root.path(),
        .env = {{"HOME", home.string()}, {"PI_CODING_AGENT_DIR", agent_dir.string()}},
        .stdin_text = "",
        .models = nullptr,
    });
    INFO(smoke.stdout_text + smoke.stderr_text);
    REQUIRE(smoke.exit_code == 0);
    CHECK(smoke.stdout_text == "fake: hello\n");
    CHECK(smoke.stderr_text.empty());
#endif
}
