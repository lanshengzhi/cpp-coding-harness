#include <catch2/catch_test_macros.hpp>

#include "support/CliRunFixture.hpp"
#include "support/ShellQuoting.hpp"
#include "support/TempWorkspace.hpp"
#include "support/AgentRootFixture.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include <sys/wait.h>

#ifndef PIKE_EXECUTABLE
#define PIKE_EXECUTABLE "./pike"
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
    bool capture_failed{false};
    std::string capture_error;
    int exit_code{-1};
    int signal{0};
    std::string stdout_text;
    std::string stderr_text;

    [[nodiscard]] bool success() const { return !capture_failed && signal == 0 && exit_code == 0; }
    [[nodiscard]] bool exited_with(int code) const { return !capture_failed && signal == 0 && exit_code == code; }

    /// Name the mechanism, not only a wait status: an exit code and a signal
    /// death are different observations and a raw `WIFEXITED ? ... : status`
    /// folds them into one number.
    [[nodiscard]] std::string status_text() const {
        if (capture_failed) {
            return "capture setup failed: " + capture_error;
        }
        if (signal != 0) {
            const char* name = ::strsignal(signal);
            return "signal " + std::to_string(signal) + " (" +
                   (name != nullptr ? std::string{name} : std::string{"unknown"}) + "), no exit code";
        }
        return "exit code " + std::to_string(exit_code) + ", no terminating signal";
    }

    [[nodiscard]] std::string diagnostic() const {
        return "command " + status_text() + "\n--- stdout ---\n" + stdout_text + "\n--- stderr ---\n" + stderr_text;
    }
};

/// Headroom beyond the staged binary copy. The stage also carries notices and
/// filesystem bookkeeping; `cmake --install` needs the source binary readable
/// while it writes the destination.
constexpr std::uintmax_t kInstallSpaceHeadroom = 64ULL * 1024ULL * 1024ULL;

struct SpaceCheck {
    fs::path path;
    std::uintmax_t available{0};
    std::uintmax_t required{0};

    [[nodiscard]] bool sufficient() const { return available >= required; }

    [[nodiscard]] std::string message() const {
        return "install space pre-flight: path '" + path.string() + "', available " + std::to_string(available) +
               " bytes, required " + std::to_string(required) + " bytes";
    }
};

using CommandExecutor = std::function<CommandResult(const std::string&, const fs::path&)>;

struct InstallAttempt {
    std::optional<CommandResult> result;
    SpaceCheck space;
};

/// Test-only runtime override for the build-configured CMake command. The
/// default remains the compile-time `CCH_CMAKE_COMMAND`; setting the same
/// environment name lets a diagnostic run prove a command was not executed by
/// pointing it at a path that cannot exist.
[[nodiscard]] std::string cmake_command() {
    const char* override = std::getenv("CCH_CMAKE_COMMAND");
    if (override != nullptr && *override != '\0') {
        return override;
    }
    return CCH_CMAKE_COMMAND;
}

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
    if (dir_error) {
        CommandResult result;
        result.capture_failed = true;
        result.capture_error =
                "could not create command capture directory '" + stamp.string() + "': " + dir_error.message();
        return result;
    }
    const auto stdout_path = stamp / "stdout.txt";
    const auto stderr_path = stamp / "stderr.txt";
    const auto redirected = command + " > " + shell_quote(stdout_path) + " 2> " + shell_quote(stderr_path);
    const int status = std::system(redirected.c_str());
    CommandResult result;
    if (status == -1) {
        const int system_errno = errno;
        result.capture_failed = true;
        result.capture_error = "could not start a shell for a command in '" + capture_dir.string() +
                               "': " + std::error_code{system_errno, std::generic_category()}.message();
        return result;
    }
    if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.signal = WTERMSIG(status);
    } else {
        result.capture_failed = true;
        result.capture_error = "unexpected wait status " + std::to_string(status);
        return result;
    }
    result.stdout_text = read_file(stdout_path);
    result.stderr_text = read_file(stderr_path);
    return result;
}

[[nodiscard]] std::uintmax_t required_install_bytes(std::uintmax_t binary_size, std::uintmax_t floor) {
    const auto maximum = std::numeric_limits<std::uintmax_t>::max();
    const auto doubled =
            binary_size > (maximum - kInstallSpaceHeadroom) / 2 ? maximum : binary_size * 2 + kInstallSpaceHeadroom;
    return std::max(floor, doubled);
}

[[nodiscard]] std::expected<std::uintmax_t, std::string> configured_floor_bytes() {
    const char* value = std::getenv("CCH_TEST_MIN_TMP_BYTES");
    if (value == nullptr || *value == '\0') {
        return 0;
    }
    std::uintmax_t parsed = 0;
    const auto* end = value + std::strlen(value);
    const auto [position, error] = std::from_chars(value, end, parsed);
    if (error != std::errc{} || position != end) {
        return std::unexpected(
                "invalid CCH_TEST_MIN_TMP_BYTES '" + std::string{value} + "': expected a non-negative byte count");
    }
    return parsed;
}

[[nodiscard]] std::expected<SpaceCheck, std::string> check_install_space(
        const fs::path& path, const fs::path& binary, std::uintmax_t floor) {
    std::error_code error;
    const auto binary_size = fs::file_size(binary, error);
    if (error) {
        return std::unexpected("could not size the Runtime binary '" + binary.string() + "': " + error.message());
    }
    const auto info = fs::space(path, error);
    if (error) {
        return std::unexpected("could not read free space for '" + path.string() + "': " + error.message());
    }
    return SpaceCheck{path, info.available, required_install_bytes(binary_size, floor)};
}

/// Run the install only after the space gate admits it. The executor seam keeps
/// the "not invoked when insufficient" property observable in a test.
[[nodiscard]] InstallAttempt install_if_sufficient(const SpaceCheck& space,
        const std::string& command,
        const fs::path& capture_dir,
        const CommandExecutor& execute) {
    if (!space.sufficient()) {
        return {.result = std::nullopt, .space = space};
    }
    return {.result = execute(command, capture_dir), .space = space};
}

/// Every file expected in the staged Runtime-only install: the Runtime itself
/// and the required third-party license/notice texts. Anything else is a
/// development-surface leak.
[[nodiscard]] std::vector<std::string> expected_staged_files() {
    return {
            "bin/pike",
            "share/pike/codemode/quickjs.wasm",
            "share/pike/export-html/template.css",
            "share/pike/export-html/template.html",
            "share/pike/export-html/template.js",
            "share/pike/export-html/vendor/highlight.min.js",
            "share/pike/export-html/vendor/marked.min.js",
            "share/pike/licenses/boost.txt",
            "share/pike/licenses/fmt.txt",
            "share/pike/licenses/glaze.txt",
            "share/pike/licenses/icu.txt",
            "share/pike/licenses/libwebp.txt",
            "share/pike/licenses/md4c.txt",
            "share/pike/licenses/openssl.txt",
            "share/pike/licenses/pi.txt",
            "share/pike/licenses/spdlog.txt",
            "share/pike/licenses/stb.txt",
            "share/pike/licenses/utf8proc.txt",
            "share/pike/licenses/wasmedge.txt",
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

// The staged-install case must keep its first cause visible. On this
// -fno-exceptions test TU a scoped INFO is never popped when a REQUIRE fails,
// and Catch2 aborts on the leftover message; the command result must therefore
// report an exit code and a signal death as different observations.
TEST_CASE(
        "command capture separates exit status from signals and names capture failures", "[cli][install][diagnostic]") {
    cch::tests::TempWorkspace root;
    const auto captures = root.path() / "captures";

    const auto exited = run_command("sh -c 'printf out; printf err >&2; exit 7'", captures);
    CHECK_FALSE(exited.capture_failed);
    CHECK(exited.signal == 0);
    CHECK(exited.exit_code == 7);
    CHECK_FALSE(exited.success());
    CHECK(exited.diagnostic().find("exit code 7, no terminating signal") != std::string::npos);
    CHECK(exited.diagnostic().find("--- stderr ---\nerr") != std::string::npos);

    const auto signaled = run_command("kill -TERM $$", captures);
    CHECK_FALSE(signaled.capture_failed);
    CHECK(signaled.signal == SIGTERM);
    CHECK(signaled.diagnostic().find("signal " + std::to_string(SIGTERM)) != std::string::npos);
    CHECK(signaled.diagnostic().find("no exit code") != std::string::npos);

    root.write("not-a-directory", "block");
    const auto blocked = run_command("true", root.path() / "not-a-directory");
    CHECK(blocked.capture_failed);
    CHECK(blocked.capture_error.find("not-a-directory") != std::string::npos);
    CHECK(blocked.capture_error.find("capture-") != std::string::npos);
}

// The space gate protects the staged install from runner resourcing failures.
// Its message must name the filesystem and both byte figures, and a rejected
// pre-flight must not execute the install command at all.
TEST_CASE("staged-install space pre-flight names path, available and required before executing the install",
        "[cli][install][diagnostic]") {
    cch::tests::TempWorkspace root;
    const auto floor = std::numeric_limits<std::uintmax_t>::max();
    const auto space = check_install_space(root.path(), PIKE_EXECUTABLE, floor);
    REQUIRE(space.has_value());
    CHECK_FALSE(space->sufficient());

    const auto message = space->message();
    CHECK(message.find(root.path().string()) != std::string::npos);
    CHECK(message.find(std::to_string(space->available)) != std::string::npos);
    CHECK(message.find(std::to_string(space->required)) != std::string::npos);

    bool executed = false;
    const auto attempt = install_if_sufficient(*space, "false", root.path(), [&](const std::string&, const fs::path&) {
        executed = true;
        return CommandResult{};
    });
    CHECK_FALSE(attempt.result.has_value());
    CHECK_FALSE(executed);
    CHECK(attempt.space.message() == message);
}

// Issue #472: a clean-prefix staging install produces only the relocatable
// Runtime and its required notices, passes the dependency-closure audit, and
// keeps working after relocation without network credentials.
// #734 (fixed): the abort was a torn read, not runner resourcing — under the parallel
// ctest default the cch_parity_gate_production_build test rewrites this build tree's
// parity build-gate depfile evidence while this case's `cmake --install` reads it, and
// an invalid-JSON evidence file aborts the install Gate. Both tests now hold the
// `cch-parity-build-gate-evidence` RESOURCE_LOCK (cmake/tests/CliTests.cmake,
// cmake/tests/ArchitectureTests.cmake), so ctest serializes the writer against this
// reader; the [quarantine] tag is dropped per its re-enable condition (root cause fixed).
TEST_CASE("staged install contains only the relocatable Runtime and behaves after relocation",
        "[cli][install][issue472][spec]") {
#ifdef CCH_SANITIZER_BUILD
    // A sanitizer build links libasan/libubsan into every binary, so the
    // dependency-closure audit correctly refuses it: sanitizers gate the
    // test suite, they never produce a shippable install (issue #473).
    SKIP("staged install is a shippable-configuration contract; sanitizer builds are not installable");
#else
    const auto floor = configured_floor_bytes();
    if (!floor) {
        UNSCOPED_INFO(floor.error());
        FAIL("staged install cannot resolve the configured space floor");
    }
    std::error_code staging_error;
    const auto staging = std::filesystem::temp_directory_path(staging_error);
    if (staging_error) {
        UNSCOPED_INFO("could not resolve the staging filesystem: " + staging_error.message());
        FAIL("staged install cannot resolve the staging filesystem");
    }
    const auto space = check_install_space(staging, PIKE_EXECUTABLE, *floor);
    if (!space) {
        UNSCOPED_INFO(space.error());
        FAIL("staged install space pre-flight could not run");
    }
    if (!space->sufficient()) {
        // Insufficient staging space is a runner precondition, not a product
        // defect. Skip with the path and both byte figures instead of turning a
        // full-suite resource shortage into a red install test.
        SKIP(space->message());
    }

    cch::tests::TempWorkspace root;
    const auto stage = root.path() / "stage";
    const auto captures = root.path() / "captures";

    // ── Stage into a clean prefix (the install path runs the Gate first) ──
    const auto install_command = shell_quote(cmake_command()) + " --install " + shell_quote(CCH_BUILD_DIR) +
                                 " --prefix " + shell_quote(stage);
    const auto attempt = install_if_sufficient(*space, install_command, captures, run_command);
    if (!attempt.result) {
        UNSCOPED_INFO(attempt.space.message());
        FAIL("staged install executed without a sufficient space pre-flight");
    }
    const auto& install = *attempt.result;
    // Keep the installer's full status and streams visible: a scoped INFO
    // cannot be popped on a failing path in this -fno-exceptions TU, and
    // Catch2 aborts on the leftover message before later diagnostics print.
    if (!install.success()) {
        UNSCOPED_INFO(install.diagnostic());
    }
    REQUIRE(install.success());

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
        UNSCOPED_INFO(file);
        CHECK_FALSE(is_development_artifact(file));
    }
    std::error_code probe_error;
    CHECK_FALSE(fs::exists(stage / "include", probe_error));
    CHECK_FALSE(fs::exists(stage / "lib", probe_error));

    // Licenses and notices are required runtime material, not empty placeholders.
    for (const auto& relative : files) {
        if (relative.starts_with("share/")) {
            UNSCOPED_INFO(relative);
            std::error_code size_error;
            const auto size = fs::file_size(stage / relative, size_error);
            CHECK_FALSE(size_error);
            CHECK(size > 0);
        }
    }

    // ── Dependency-closure audit on the staged Runtime ──
    const auto staged_binary = stage / "bin" / "pike";
    const auto source_dir = fs::path(CCH_SOURCE_DIR);
    const auto audit = run_command(
        shell_quote(CCH_PYTHON3) + " " +
            shell_quote(source_dir / "cmake" / "install" / "audit_runtime_deps.py") +
            " --binary " + shell_quote(staged_binary) + " --allowlist " +
            shell_quote(source_dir / "cmake" / "install" / "runtime-deps.json") +
            " --forbid-root " + shell_quote(source_dir) + " --forbid-root " +
            shell_quote(CCH_BUILD_DIR),
        captures);
    UNSCOPED_INFO(audit.stdout_text + audit.stderr_text);
    CHECK(audit.exit_code == 0);

    // ── Relocate the whole prefix outside the build-tree spelling ──
    const auto relocated = root.path() / "relocated";
    std::error_code rename_error;
    fs::rename(stage, relocated, rename_error);
    REQUIRE_FALSE(rename_error);
    const auto relocated_binary = relocated / "bin" / "pike";

    // Scrubbed environment: no inherited credentials, proxies, or config.
    const auto home = root.path() / "home";
    std::error_code home_error;
    fs::create_directories(home, home_error);
    const std::string clean_env = "env -i PATH=/usr/bin:/bin HOME=" + shell_quote(home) + " ";
    const std::string relocated_run =
        "cd " + shell_quote(root.path()) + " && " + clean_env + shell_quote(relocated_binary);

    // ── --version matches the build-tree Runtime ──
    const auto build_version = run_command(shell_quote(PIKE_EXECUTABLE) + " --version", captures);
    REQUIRE(build_version.success());
    const auto staged_version = run_command(relocated_run + " --version", captures);
    UNSCOPED_INFO(staged_version.stdout_text + staged_version.stderr_text);
    REQUIRE(staged_version.success());
    CHECK(staged_version.stdout_text == build_version.stdout_text);
    CHECK_FALSE(staged_version.stdout_text.empty());

    // ── --help prints the pi-aligned flag surface ──
    const auto staged_help = run_command(relocated_run + " --help", captures);
    UNSCOPED_INFO(staged_help.stdout_text + staged_help.stderr_text);
    REQUIRE(staged_help.success());
    CHECK(staged_help.stdout_text.find("Usage:") != std::string::npos);
    CHECK(staged_help.stdout_text.find("--print") != std::string::npos);

    // ── Offline staged run without a reachable gateway fails deterministically ──
    // The product default is the local Magpie gateway; with no catalog the
    // placeholder model fails through the one AI stream contract. The check
    // must not observe a gateway that happens to be listening on the developer
    // host: run it in an isolated network namespace when the host permits one,
    // and otherwise skip only when something already owns the loopback
    // endpoint (a live service makes the offline premise untestable).
    const auto namespace_probe = run_command("unshare -rn true", captures);
    const bool network_isolation_available = namespace_probe.exit_code == 0;
    if (!network_isolation_available) {
        const auto gateway_probe =
                run_command(shell_quote(CCH_PYTHON3) + " -c \"import socket,sys;s=socket.socket();s.settimeout(0.25);"
                                                       "sys.exit(0 if s.connect_ex(('127.0.0.1',3425))==0 else 1)\"",
                        captures);
        if (gateway_probe.exit_code == 0) {
            SKIP("a service is listening on the default Magpie endpoint 127.0.0.1:3425 and this host "
                 "cannot create an isolated network namespace");
        }
    }
    const std::string offline_run = "cd " + shell_quote(root.path()) + " && " +
                                    (network_isolation_available ? std::string{"unshare -rn "} : std::string{}) +
                                    clean_env + shell_quote(relocated_binary) + " --print ping < /dev/null";
    const auto offline = run_command(offline_run, captures);
    UNSCOPED_INFO(offline.stdout_text + offline.stderr_text);
    CHECK(offline.exited_with(1));
    CHECK(offline.stderr_text.find("has no API implementation") != std::string::npos);

    // ── Offline fake-provider smoke outside the build tree (in-process seam) ──
    auto smoke = cch::tests::run_cli(cch::tests::CliRunOptions{
            .args = {"--print", "hello"},
            .cwd = root.path(),
            .env = {{"HOME", home.string()}},
            .stdin_text = "",
    });
    UNSCOPED_INFO(smoke.stdout_text + smoke.stderr_text);
    REQUIRE(smoke.exit_code == 0);
    CHECK(smoke.stdout_text == "fake: hello\n");
    UNSCOPED_INFO("stderr: " + smoke.stderr_text);
    CHECK(smoke.stderr_text.empty());
#endif
}
