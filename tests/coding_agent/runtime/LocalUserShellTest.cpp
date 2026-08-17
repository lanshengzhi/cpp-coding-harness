#include "coding_agent/runtime/LocalUserShell.hpp"

#include "ai/AsyncResultBridge.hpp"
#include "support/EnvVarGuard.hpp"
#include "support/TempWorkspace.hpp"

#include <catch2/catch_test_macros.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

using namespace cch;
namespace runtime = cch::coding_agent::runtime;

namespace {

struct ShellRun {
    support::Expected<runtime::UserShellResult> result;
    std::string updates;
    std::chrono::milliseconds elapsed{0};
};

/// Drives one User Shell execution on a private io_context, collecting every
/// streamed update in delivery order. When a stop delay is given, a timer on
/// the same executor requests cancellation while the process runs.
[[nodiscard]] ShellRun run_user_shell(
    runtime::AsyncUserShell& shell,
    std::string command,
    std::optional<std::chrono::milliseconds> stop_after = std::nullopt,
    bool fail_updates = false,
    bool throw_updates = false) {
    boost::asio::io_context io;
    std::stop_source stop_source;
    ShellRun run;
    std::optional<boost::asio::steady_timer> stop_timer;
    if (stop_after) {
        stop_timer.emplace(io, *stop_after);
        stop_timer->async_wait([&](const boost::system::error_code& error) {
            if (!error) {
                stop_source.request_stop();
            }
        });
    }
    boost::asio::co_spawn(
        io,
        [&]() -> boost::asio::awaitable<void> {
            // Captures remain alive until io.run() drains this operation to completion.
            run.result = co_await ai::detail::await_async_result(shell.execute(
                std::move(command),
                // The capture targets remain alive until io.run() drains this operation.
                [&](std::string_view update) -> support::ExpectedVoid {
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
                    if (throw_updates) {
                        throw std::runtime_error{"update sink threw"};
                    }
#else
                    (void)throw_updates;
#endif
                    if (fail_updates) {
                        return std::unexpected(support::make_error(
                            support::ErrorCode::Unknown,
                            "update sink rejected output"));
                    }
                    run.updates.append(update);
                    return {};
                },
                stop_source.get_token()));
            co_return;
        },
        boost::asio::detached);
    const auto started = std::chrono::steady_clock::now();
    io.run();
    run.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    return run;
}

} // namespace

TEST_CASE("Local User Shell is an independently owned final capability", "[coding_agent][runtime][shell][issue90]") {
    static_assert(std::is_final_v<runtime::LocalUserShell>);
    static_assert(std::is_base_of_v<runtime::AsyncUserShell, runtime::LocalUserShell>);
}


TEST_CASE("Local User Shell streams output and reports exit zero", "[coding_agent][runtime][shell][issue90]") {
    tests::TempWorkspace workspace;
    runtime::LocalUserShell shell{workspace.path(), {}, {}};

    const auto run = run_user_shell(shell, "printf 'user-bash-output\\n'");
    REQUIRE(run.result);
    CHECK(run.result->exit_code == 0);
    CHECK_FALSE(run.result->cancelled);
    CHECK(run.updates.find("user-bash-output") != std::string::npos);
}

TEST_CASE("Local User Shell merges stdout and stderr in emission order", "[coding_agent][runtime][shell][issue90]") {
    tests::TempWorkspace workspace;
    runtime::LocalUserShell shell{workspace.path(), {}, {}};

    const auto run = run_user_shell(
        shell,
        "printf 'out-one\\n'; printf 'err-one\\n' >&2; printf 'out-two\\n'");
    REQUIRE(run.result);
    CHECK(run.result->exit_code == 0);
    CHECK(run.updates == "out-one\nerr-one\nout-two\n");
}

TEST_CASE(
    "Local User Shell records a non-zero exit as a completed outcome",
    "[coding_agent][runtime][shell][issue90]") {
    tests::TempWorkspace workspace;
    runtime::LocalUserShell shell{workspace.path(), {}, {}};

    const auto run = run_user_shell(shell, "printf 'before-exit\\n'; exit 7");
    REQUIRE(run.result);
    CHECK(run.result->exit_code == 7);
    CHECK_FALSE(run.result->cancelled);
    CHECK(run.updates.find("before-exit") != std::string::npos);
}

TEST_CASE(
    "Local User Shell cancellation stops the process and retains streamed output",
    "[coding_agent][runtime][shell][issue90]") {
    tests::TempWorkspace workspace;
    runtime::LocalUserShell shell{workspace.path(), {}, {}};

    const auto run = run_user_shell(
        shell,
        "printf 'partial\\n'; sleep 30",
        std::chrono::milliseconds{300});
    REQUIRE(run.result);
    CHECK(run.result->cancelled);
    CHECK_FALSE(run.result->exit_code.has_value());
    CHECK(run.updates.find("partial") != std::string::npos);
}

TEST_CASE("Local User Shell filters secret environment variables", "[coding_agent][runtime][shell][issue90]") {
    tests::TempWorkspace workspace;
    tests::EnvVarGuard heuristic_secret{"CCH_LOCAL_USER_SHELL_TEST_API_KEY"};
    tests::EnvVarGuard explicit_secret{"CCH_LOCAL_USER_SHELL_PLAIN"};
    heuristic_secret.set("heuristic-value");
    explicit_secret.set("explicit-value");

    runtime::LocalUserShell shell{
        workspace.path(),
        {"CCH_LOCAL_USER_SHELL_PLAIN"},
        {}};

    const auto run = run_user_shell(
        shell,
        "printenv CCH_LOCAL_USER_SHELL_TEST_API_KEY || echo heuristic-filtered; "
        "printenv CCH_LOCAL_USER_SHELL_PLAIN || echo explicit-filtered; "
        "test -n \"$PATH\" && echo path-present");
    REQUIRE(run.result);
    CHECK(run.result->exit_code == 0);
    CHECK(run.updates.find("heuristic-filtered") != std::string::npos);
    CHECK(run.updates.find("explicit-filtered") != std::string::npos);
    CHECK(run.updates.find("path-present") != std::string::npos);
    CHECK(run.updates.find("heuristic-value") == std::string::npos);
    CHECK(run.updates.find("explicit-value") == std::string::npos);
}

TEST_CASE(
    "Local User Shell starts every command in the Session workspace",
    "[coding_agent][runtime][shell][issue90]") {
    tests::TempWorkspace workspace;
    runtime::LocalUserShell shell{workspace.path(), {}, {}};

    const auto first = run_user_shell(shell, "cd / && pwd");
    REQUIRE(first.result);
    CHECK(first.updates.find("/\n") != std::string::npos);

    const auto second = run_user_shell(shell, "pwd");
    REQUIRE(second.result);
    CHECK(second.result->exit_code == 0);
    CHECK(second.updates.find(workspace.path().string()) != std::string::npos);
    CHECK(second.updates.find("/\n") == std::string::npos);
}

TEST_CASE("Local User Shell applies the configured command prefix as setup only", "[coding_agent][runtime][shell][issue90]") {
    tests::TempWorkspace workspace;
    runtime::LocalUserShell shell{
        workspace.path(),
        {},
        harness::ShellConfig{.command_prefix = std::string{"cch_prefix_value=from-prefix"}}};

    const auto run = run_user_shell(shell, "printf '%s\\n' \"$cch_prefix_value\"");
    REQUIRE(run.result);
    CHECK(run.result->exit_code == 0);
    CHECK(run.updates.find("from-prefix") != std::string::npos);
}

TEST_CASE(
    "Local User Shell honors a configured shell path and defers stale paths to execution",
    "[coding_agent][runtime][shell][issue90]") {
    tests::TempWorkspace workspace;

    runtime::LocalUserShell stale_shell{
        workspace.path(),
        {},
        harness::ShellConfig{.shell_path = std::string{"/nonexistent/cch-user-shell"}}};
    const auto failed = run_user_shell(stale_shell, "printf 'never\\n'");
    REQUIRE_FALSE(failed.result);
    CHECK(failed.result.error().code == support::ErrorCode::Process);
    CHECK(failed.updates.empty());

    runtime::LocalUserShell sh_shell{
        workspace.path(),
        {},
        harness::ShellConfig{.shell_path = std::string{"/bin/sh"}}};
    const auto run = run_user_shell(sh_shell, "printf 'via-sh\\n'");
    REQUIRE(run.result);
    CHECK(run.result->exit_code == 0);
    CHECK(run.updates.find("via-sh") != std::string::npos);
}

TEST_CASE(
    "Local User Shell reports update-sink failure as an infrastructure error",
    "[coding_agent][runtime][shell][issue90]") {
    tests::TempWorkspace workspace;
    runtime::LocalUserShell shell{workspace.path(), {}, {}};

    const auto run = run_user_shell(shell, "printf 'rejected\\n'", std::nullopt, true);
    REQUIRE_FALSE(run.result);
}

#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
TEST_CASE(
    "Local User Shell contains a throwing update sink and stops the process",
    "[coding_agent][runtime][shell][issue484]") {
    tests::TempWorkspace workspace;
    runtime::LocalUserShell shell{workspace.path(), {}, {}};

    const auto run = run_user_shell(
        shell,
        "printf 'rejected\\n'; sleep 30",
        std::nullopt,
        false,
        true);

    REQUIRE_FALSE(run.result);
    CHECK(run.result.error().message == "user shell update sink threw");
    CHECK(run.elapsed < std::chrono::seconds{3});
}
#endif

