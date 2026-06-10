#include "../../third_party/catch2/catch_test_macros.hpp"

#include <cch/harness/LocalExecutionEnv.hpp>
#include "../support/TempWorkspace.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>

#include <optional>

using namespace cch;

namespace {

template <typename T, typename Start>
util::Expected<T> run_awaitable(Start start) {
    boost::asio::io_context io;
    std::optional<util::Expected<T>> result;
    boost::asio::co_spawn(
        io,
        [&]() -> boost::asio::awaitable<void> {
            result = co_await start();
            co_return;
        },
        boost::asio::detached);
    io.run();
    REQUIRE(result.has_value());
    return std::move(*result);
}

} // namespace

TEST_CASE("async local execution env preserves file read and write safety", "[harness][async][u6]") {
    tests::TempWorkspace workspace;
    harness::AsyncLocalExecutionEnv env(workspace.path());

    auto written = run_awaitable<harness::AsyncFileWriteResult>([&]() {
        return env.write_file("nested/note.txt", "hello", true);
    });
    REQUIRE(written);
    CHECK(written->bytes_written == 5);

    auto read = run_awaitable<harness::AsyncFileReadResult>([&]() {
        return env.read_file("nested/note.txt", 1, 0);
    });
    REQUIRE(read);
    CHECK(read->content == "hello");

    auto escaped = run_awaitable<harness::AsyncFileReadResult>([&]() {
        return env.read_file("../outside.txt", 1, 0);
    });
    REQUIRE_FALSE(escaped);
    CHECK(escaped.error().code == util::ErrorCode::Workspace);
}

TEST_CASE("async local execution env preserves exact edit ambiguity errors", "[harness][async][u6][ae4]") {
    tests::TempWorkspace workspace;
    workspace.write("note.txt", "same\nsame\n");
    harness::AsyncLocalExecutionEnv env(workspace.path());

    auto edited = run_awaitable<harness::AsyncFileEditResult>([&]() {
        return env.edit_file("note.txt", "same", "new");
    });

    REQUIRE_FALSE(edited);
    CHECK(edited.error().detail.find("multiple") != std::string::npos);
    CHECK(workspace.read("note.txt") == "same\nsame\n");
}

TEST_CASE("async local execution env returns typed shell unavailable errors", "[harness][async][u6]") {
    tests::TempWorkspace workspace;
    harness::AsyncLocalExecutionEnv env(workspace.path(), false);

    auto shell = run_awaitable<harness::AsyncShellResult>([&]() {
        return env.run_shell("echo blocked", std::chrono::milliseconds(1000));
    });

    REQUIRE_FALSE(shell);
    CHECK(shell.error().code == util::ErrorCode::Process);
    CHECK(shell.error().detail.find("disabled") != std::string::npos);
}
