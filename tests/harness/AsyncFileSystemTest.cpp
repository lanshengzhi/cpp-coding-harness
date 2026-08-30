#include "support/FakeAsyncFileSystem.hpp"
#include "support/TempWorkspace.hpp"

#include <cch/agent/harness/ExecutionEnv.hpp>
#include <cch/agent/harness/LocalFileSystem.hpp>
#include "agent/harness/AsyncFileSystemOperations.hpp"
#include "agent/harness/RuntimeRoot.hpp"
#include "support/AsyncResultBridge.hpp"

#include <catch2/catch_test_macros.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

using namespace cch;

namespace {

class TestRuntime final {
public:
    TestRuntime()
        : loop_(std::make_shared<boost::asio::io_context>()),
          root_(loop_, harness::RuntimeLimits{}) {}

    [[nodiscard]] std::shared_ptr<harness::RuntimeTarget> make_target() {
        return root_.make_target();
    }

    template <typename T, typename E>
    std::expected<T, E> run(support::AsyncResult<T, E> operation) {
        loop_->restart();
        std::optional<std::expected<T, E>> result;
        boost::asio::co_spawn(
            *loop_,
            [operation = std::move(operation), &result]() mutable -> boost::asio::awaitable<void> {
                result = co_await support::detail::await_async_result(std::move(operation));
                co_return;
            },
            boost::asio::detached);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (!result && std::chrono::steady_clock::now() < deadline) {
            if (loop_->stopped()) {
                loop_->restart();
            }
            (void)loop_->poll();
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        REQUIRE(result.has_value());
        while (loop_->poll() != 0) {
        }
        return std::move(*result);
    }

    void close() noexcept {
        root_.close();
        while (loop_->poll() != 0) {
        }
    }

private:
    std::shared_ptr<boost::asio::io_context> loop_;
    harness::RuntimeRoot root_;
};

} // namespace

TEST_CASE(
    "filesystem capability is complete and keeps Shell as a separate capability",
    "[harness][filesystem][u1][issue558]") {
    static_assert(std::is_abstract_v<harness::AsyncFileSystem>);
    static_assert(std::is_abstract_v<harness::AsyncShell>);
    static_assert(std::is_abstract_v<harness::AsyncExecutionEnv>);
    static_assert(std::is_base_of_v<harness::AsyncFileSystem, harness::AsyncExecutionEnv>);
    static_assert(std::is_base_of_v<harness::AsyncShell, harness::AsyncExecutionEnv>);
    static_assert(!std::is_base_of_v<harness::AsyncShell, harness::AsyncFileSystem>);
    static_assert(std::is_final_v<harness::AsyncLocalFileSystem>);

    CHECK(harness::kFileSystemCapacity.max_file_bytes > 0);
    CHECK(harness::kFileSystemCapacity.max_text_lines > 0);
    CHECK(harness::kFileSystemCapacity.max_directory_entries > 0);
}

TEST_CASE(
    "complete filesystem fake models success cancellation and typed failures",
    "[harness][filesystem][fake][issue558]") {
    tests::FakeAsyncFileSystem fake("/workspace");

    auto path = fake.readTextFile("note.txt", {});
    std::optional<std::expected<std::string, harness::FileError>> path_result;
    std::move(path).start(
        [&path_result](std::expected<std::string, harness::FileError> result) noexcept {
            path_result.emplace(std::move(result));
        });
    REQUIRE(path_result.has_value());
    REQUIRE(*path_result);
    CHECK(**path_result == "note.txt");

    std::stop_source stop_source;
    stop_source.request_stop();
    auto cancelled = fake.exists("note.txt", stop_source.get_token());
    std::optional<std::expected<bool, harness::FileError>> cancelled_result;
    std::move(cancelled).start(
        [&cancelled_result](std::expected<bool, harness::FileError> result) noexcept {
            cancelled_result.emplace(std::move(result));
        });
    REQUIRE(cancelled_result.has_value());
    REQUIRE_FALSE(*cancelled_result);
    CHECK(cancelled_result->error().code == harness::FileErrorCode::Aborted);

    fake.next_error = harness::FileError{
        .code = harness::FileErrorCode::Busy,
        .message = "fake admission overload",
        .path = std::nullopt,
    };
    auto busy = fake.fileInfo("note.txt", {});
    std::optional<std::expected<harness::FileInfo, harness::FileError>> busy_result;
    std::move(busy).start(
        [&busy_result](std::expected<harness::FileInfo, harness::FileError> result) noexcept {
            busy_result.emplace(std::move(result));
        });
    REQUIRE(busy_result.has_value());
    REQUIRE_FALSE(*busy_result);
    CHECK(busy_result->error().code == harness::FileErrorCode::Busy);

    fake.next_error = harness::FileError{
        .code = harness::FileErrorCode::ResourceLimit,
        .message = "fake result limit",
        .path = std::nullopt,
    };
    auto limited = fake.listDir(".", {});
    std::optional<std::expected<std::vector<harness::FileInfo>, harness::FileError>> limited_result;
    std::move(limited).start(
        [&limited_result](std::expected<std::vector<harness::FileInfo>, harness::FileError> result) noexcept {
            limited_result.emplace(std::move(result));
        });
    REQUIRE(limited_result.has_value());
    REQUIRE_FALSE(*limited_result);
    CHECK(limited_result->error().code == harness::FileErrorCode::ResourceLimit);

    fake.next_error.reset();
    fake.cleanup_error = harness::FileError{
        .code = harness::FileErrorCode::ResourceLimit,
        .message = "fake cleanup outcome",
        .path = std::nullopt,
    };
    auto cleanup = fake.cleanup();
    std::optional<std::expected<void, harness::FileError>> cleanup_result;
    std::move(cleanup).start(
        [&cleanup_result](std::expected<void, harness::FileError> result) noexcept {
            cleanup_result.emplace(std::move(result));
        });
    REQUIRE(cleanup_result.has_value());
    REQUIRE_FALSE(*cleanup_result);
    CHECK(cleanup_result->error().code == harness::FileErrorCode::ResourceLimit);
}

TEST_CASE(
    "Local filesystem adapter preserves containment and cancellation",
    "[harness][filesystem][local][issue558]") {
    tests::TempWorkspace workspace;
    workspace.write("nested/note.txt", "hello");
    TestRuntime runtime;
    harness::AsyncLocalFileSystem filesystem(runtime.make_target(), workspace.path());

    auto read = runtime.run(filesystem.readTextFile("nested/note.txt", {}));
    REQUIRE(read);
    CHECK(*read == "hello");

    auto escaped = runtime.run(filesystem.readTextFile("../outside.txt", {}));
    REQUIRE_FALSE(escaped);
    CHECK(escaped.error().code == harness::FileErrorCode::PermissionDenied);

    std::stop_source stop_source;
    stop_source.request_stop();
    auto cancelled = runtime.run(filesystem.writeFile(
        "nested/note.txt", std::string{"changed"}, stop_source.get_token()));
    REQUIRE_FALSE(cancelled);
    CHECK(cancelled.error().code == harness::FileErrorCode::Aborted);
    CHECK(workspace.read("nested/note.txt") == "hello");

    runtime.close();
}

TEST_CASE("started filesystem work returns its committed outcome after cancellation",
        "[harness][filesystem][async][issue558]") {
    tests::TempWorkspace workspace;
    TestRuntime runtime;
    auto sync = std::make_shared<harness::SyncLocalExecutionEnv>(workspace.path());
    std::stop_source stop_source;

    auto result = runtime.run(harness::filesystem_detail::submit_filesystem_operation<void>(runtime.make_target(),
            sync,
            0,
            stop_source.get_token(),
            std::string{"committed.txt"},
            [&stop_source](const harness::SyncLocalExecutionEnv& local) {
                auto written = local.writeFile("committed.txt", std::string{"committed"});
                stop_source.request_stop();
                return written;
            }));

    REQUIRE(result);
    CHECK(workspace.read("committed.txt") == "committed");
    runtime.close();
}

TEST_CASE(
    "filesystem reads fail with ResourceLimit before returning partial results",
    "[harness][filesystem][capacity][issue558]") {
    tests::TempWorkspace workspace;
    const auto oversized_bytes = harness::kFileSystemCapacity.max_file_bytes + 1;
    workspace.write("oversized.txt", std::string(oversized_bytes, 'x'));

    std::string many_lines;
    for (std::size_t index = 0; index <= harness::kFileSystemCapacity.max_text_lines; ++index) {
        many_lines += "line\n";
    }
    workspace.write("many-lines.txt", many_lines);

    TestRuntime runtime;
    harness::AsyncLocalFileSystem filesystem(runtime.make_target(), workspace.path());

    auto text = runtime.run(filesystem.readTextFile("oversized.txt", {}));
    REQUIRE_FALSE(text);
    CHECK(text.error().code == harness::FileErrorCode::ResourceLimit);

    auto binary = runtime.run(filesystem.readBinaryFile("oversized.txt", {}));
    REQUIRE_FALSE(binary);
    CHECK(binary.error().code == harness::FileErrorCode::ResourceLimit);

    // maxLines reduces the requested result but cannot bypass the fixed file
    // byte limit.
    auto reduced = runtime.run(filesystem.readTextLines("oversized.txt", 1, {}));
    REQUIRE_FALSE(reduced);
    CHECK(reduced.error().code == harness::FileErrorCode::ResourceLimit);

    auto lines = runtime.run(filesystem.readTextLines("many-lines.txt", std::nullopt, {}));
    REQUIRE_FALSE(lines);
    CHECK(lines.error().code == harness::FileErrorCode::ResourceLimit);

    runtime.close();
}

TEST_CASE(
    "directory listings reject over-capacity results instead of returning a partial list",
    "[harness][filesystem][capacity][issue558]") {
    tests::TempWorkspace workspace;
    for (std::size_t index = 0; index <= harness::kFileSystemCapacity.max_directory_entries; ++index) {
        workspace.write("entry-" + std::to_string(index), "x");
    }

    TestRuntime runtime;
    harness::AsyncLocalFileSystem filesystem(runtime.make_target(), workspace.path());
    auto listing = runtime.run(filesystem.listDir(".", {}));

    REQUIRE_FALSE(listing);
    CHECK(listing.error().code == harness::FileErrorCode::ResourceLimit);
    runtime.close();
}

TEST_CASE(
    "filesystem cleanup removes only each adapter's tracked temporary resources",
    "[harness][filesystem][cleanup][issue558]") {
    tests::TempWorkspace workspace;
    std::filesystem::create_directories(workspace.path() / ".cch-tmp");
    workspace.write(".cch-tmp/foreign", "keep");

    TestRuntime runtime;
    harness::AsyncLocalFileSystem first(runtime.make_target(), workspace.path());
    harness::AsyncLocalFileSystem second(runtime.make_target(), workspace.path());

    auto first_temp = runtime.run(first.createTempFile("owned-", ".tmp", {}));
    auto second_temp = runtime.run(second.createTempDir("owned-dir-", {}));
    REQUIRE(first_temp);
    REQUIRE(second_temp);
    REQUIRE(std::filesystem::exists(*first_temp));
    REQUIRE(std::filesystem::exists(*second_temp));

    auto first_cleanup = runtime.run(first.cleanup());
    REQUIRE(first_cleanup);
    CHECK_FALSE(std::filesystem::exists(*first_temp));
    CHECK(std::filesystem::exists(*second_temp));
    CHECK(std::filesystem::exists(workspace.path() / ".cch-tmp" / "foreign"));

    // Repeated cleanup is a successful no-op, not a shared-directory sweep.
    auto repeated_cleanup = runtime.run(first.cleanup());
    REQUIRE(repeated_cleanup);
    CHECK(std::filesystem::exists(workspace.path() / ".cch-tmp" / "foreign"));

    auto second_cleanup = runtime.run(second.cleanup());
    REQUIRE(second_cleanup);
    CHECK_FALSE(std::filesystem::exists(*second_temp));
    CHECK(std::filesystem::exists(workspace.path() / ".cch-tmp" / "foreign"));

    runtime.close();
}

TEST_CASE(
    "filesystem ResourceLimit remains distinct from support Busy",
    "[harness][filesystem][capacity][issue558]") {
    const auto file_error = harness::to_util_error(harness::FileError{
        .code = harness::FileErrorCode::ResourceLimit,
        .message = "too large",
        .path = std::nullopt,
    });
    CHECK(file_error.code == support::ErrorCode::ResourceLimit);
    CHECK(support::to_string(support::ErrorCode::ResourceLimit) == "resource_limit");
    CHECK(harness::to_util_error(harness::FileError{
              .code = harness::FileErrorCode::Busy,
              .message = "busy",
              .path = std::nullopt,
          }).code == support::ErrorCode::Busy);
}
