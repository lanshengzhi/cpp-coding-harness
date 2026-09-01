#include "support/FakeAsyncFileSystem.hpp"
#include "support/TempWorkspace.hpp"

#include <cch/agent/harness/LocalFileSystem.hpp>
#include <cch/agent/harness/Shell.hpp>
#include "agent/harness/AsyncFileSystemOperations.hpp"
#include "agent/harness/RuntimeRoot.hpp"
#include "agent/harness/WorkspaceFileSystem.hpp"
#include "support/AsyncResultBridge.hpp"

#include <catch2/catch_test_macros.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
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
    TestRuntime() : loop_(std::make_shared<boost::asio::io_context>()), root_(loop_, harness::RuntimeLimits{}) {}

    [[nodiscard]] std::shared_ptr<harness::RuntimeTarget> make_target() { return root_.make_target(); }

    template <typename T, typename E> std::expected<T, E> run(support::AsyncResult<T, E> operation) {
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

template <typename T> void check_file_operation_aborted(const std::expected<T, harness::FileError>& result) {
    CHECK_FALSE(result);
    if (!result) {
        CHECK(result.error().code == harness::FileErrorCode::Aborted);
    }
}

bool path_exists(const std::filesystem::path& path) {
    std::error_code error;
    const bool exists = std::filesystem::exists(path, error);
    CHECK_FALSE(error);
    return exists;
}

template <typename T, typename E> std::expected<T, E> run_to_completion(support::AsyncResult<T, E> operation) {
    boost::asio::io_context io;
    std::optional<std::expected<T, E>> result;
    boost::asio::co_spawn(
            io,
            [operation = std::move(operation), &result]() mutable -> boost::asio::awaitable<void> {
                result = co_await support::detail::await_async_result(std::move(operation));
                co_return;
            },
            boost::asio::detached);
    io.run();
    REQUIRE(result.has_value());
    return std::move(*result);
}

} // namespace

TEST_CASE("filesystem capability is complete and keeps Shell as a separate capability",
        "[harness][filesystem][u1][issue558]") {
    static_assert(std::is_abstract_v<harness::AsyncFileSystem>);
    static_assert(std::is_abstract_v<harness::AsyncShell>);
    static_assert(!std::is_base_of_v<harness::AsyncShell, harness::AsyncFileSystem>);
    static_assert(std::is_final_v<harness::AsyncLocalFileSystem>);

    CHECK(harness::kFileSystemCapacity.max_file_bytes > 0);
    CHECK(harness::kFileSystemCapacity.max_text_lines > 0);
    CHECK(harness::kFileSystemCapacity.max_directory_entries > 0);
}

TEST_CASE("complete filesystem fake models success cancellation and typed failures",
        "[harness][filesystem][fake][issue558]") {
    tests::FakeAsyncFileSystem fake("/workspace");

    auto path = fake.readTextFile("note.txt", {});
    std::optional<std::expected<std::string, harness::FileError>> path_result;
    std::move(path).start([&path_result](std::expected<std::string, harness::FileError> result) noexcept {
        path_result.emplace(std::move(result));
    });
    REQUIRE(path_result.has_value());
    REQUIRE(*path_result);
    CHECK(**path_result == "note.txt");

    std::stop_source stop_source;
    stop_source.request_stop();
    auto cancelled = fake.exists("note.txt", stop_source.get_token());
    std::optional<std::expected<bool, harness::FileError>> cancelled_result;
    std::move(cancelled).start([&cancelled_result](std::expected<bool, harness::FileError> result) noexcept {
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
    std::move(busy).start([&busy_result](std::expected<harness::FileInfo, harness::FileError> result) noexcept {
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
    std::move(cleanup).start([&cleanup_result](std::expected<void, harness::FileError> result) noexcept {
        cleanup_result.emplace(std::move(result));
    });
    REQUIRE(cleanup_result.has_value());
    REQUIRE_FALSE(*cleanup_result);
    CHECK(cleanup_result->error().code == harness::FileErrorCode::ResourceLimit);
}

TEST_CASE("Local filesystem adapter preserves containment and cancellation", "[harness][filesystem][local][issue558]") {
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
    auto cancelled =
            runtime.run(filesystem.writeFile("nested/note.txt", std::string{"changed"}, stop_source.get_token()));
    REQUIRE_FALSE(cancelled);
    CHECK(cancelled.error().code == harness::FileErrorCode::Aborted);
    CHECK(workspace.read("nested/note.txt") == "hello");

    runtime.close();
}

TEST_CASE("started filesystem work returns its committed outcome after cancellation",
        "[harness][filesystem][async][issue558]") {
    tests::TempWorkspace workspace;
    TestRuntime runtime;
    auto filesystem = std::make_shared<harness::WorkspaceFileSystem>(workspace.path());
    std::stop_source stop_source;

    auto result = runtime.run(harness::filesystem_detail::submit_filesystem_operation<void>(runtime.make_target(),
            filesystem,
            0,
            stop_source.get_token(),
            std::string{"committed.txt"},
            [&stop_source](const harness::WorkspaceFileSystem& fs) {
                auto written = fs.writeFile("committed.txt", std::string{"committed"});
                stop_source.request_stop();
                return written;
            }));

    REQUIRE(result);
    CHECK(workspace.read("committed.txt") == "committed");
    runtime.close();
}

TEST_CASE("filesystem reads fail with ResourceLimit before returning partial results",
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

TEST_CASE("directory listings reject over-capacity results instead of returning a partial list",
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

TEST_CASE("filesystem cleanup removes only each adapter's tracked temporary resources",
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

TEST_CASE("every async filesystem operation observes a pre-requested cancellation", "[harness][async][issue40]") {
    tests::TempWorkspace workspace;
    workspace.write("note.txt", "unchanged");
    TestRuntime runtime;
    harness::AsyncLocalFileSystem filesystem(runtime.make_target(), workspace.path());
    std::stop_source stop_source;
    stop_source.request_stop();
    const auto stop_token = stop_source.get_token();

    check_file_operation_aborted(runtime.run(filesystem.absolutePath("note.txt", stop_token)));
    check_file_operation_aborted(runtime.run(filesystem.joinPath({"nested", "note.txt"}, stop_token)));
    check_file_operation_aborted(runtime.run(filesystem.readTextFile("note.txt", stop_token)));
    check_file_operation_aborted(runtime.run(filesystem.readTextLines("note.txt", std::nullopt, stop_token)));
    check_file_operation_aborted(runtime.run(filesystem.readBinaryFile("note.txt", stop_token)));
    check_file_operation_aborted(runtime.run(filesystem.writeFile("note.txt", std::string{"changed"}, stop_token)));
    check_file_operation_aborted(runtime.run(filesystem.appendFile("note.txt", std::string{"changed"}, stop_token)));
    check_file_operation_aborted(runtime.run(filesystem.fileInfo("note.txt", stop_token)));
    check_file_operation_aborted(runtime.run(filesystem.listDir(".", stop_token)));
    check_file_operation_aborted(runtime.run(filesystem.canonicalPath("note.txt", stop_token)));
    check_file_operation_aborted(runtime.run(filesystem.exists("note.txt", stop_token)));
    check_file_operation_aborted(runtime.run(filesystem.createDir("new-dir", true, stop_token)));
    check_file_operation_aborted(runtime.run(filesystem.remove("note.txt", false, stop_token)));
    check_file_operation_aborted(runtime.run(filesystem.createTempDir("tmp-", stop_token)));
    check_file_operation_aborted(runtime.run(filesystem.createTempFile("tmp-", ".txt", stop_token)));

    CHECK(workspace.read("note.txt") == "unchanged");
    CHECK_FALSE(path_exists(workspace.path() / "new-dir"));
    runtime.close();
}

TEST_CASE("async local filesystem adapter preserves file read and write safety", "[harness][async][u6]") {
    tests::TempWorkspace workspace;
    TestRuntime runtime;
    harness::AsyncLocalFileSystem filesystem(runtime.make_target(), workspace.path());

    auto written = runtime.run(filesystem.writeFile("nested/note.txt", std::string{"hello"}, std::stop_token{}));
    REQUIRE(written);

    auto read = runtime.run(filesystem.readTextFile("nested/note.txt", std::stop_token{}));
    REQUIRE(read);
    CHECK(*read == "hello");

    auto escaped = runtime.run(filesystem.readTextFile("../outside.txt", std::stop_token{}));
    REQUIRE_FALSE(escaped);
    CHECK(escaped.error().code == harness::FileErrorCode::PermissionDenied);
    runtime.close();
}

TEST_CASE(
        "async filesystem submission returns typed Busy when the runtime is saturated", "[harness][async][issue459]") {
    tests::TempWorkspace workspace;
    workspace.write("note.txt", "original");
    auto io = std::make_shared<boost::asio::io_context>();
    harness::RuntimeRoot root(io,
            harness::RuntimeLimits{
                    .worker_count = 1,
                    .max_admitted_operations = 1,
                    .max_admitted_bytes = 1024 * 1024,
            });
    auto target = root.make_target();
    harness::AsyncLocalFileSystem filesystem(target, workspace.path());

    std::atomic<bool> release_worker{false};
    auto gate = target->try_admit(4);
    REQUIRE(gate.has_value());
    REQUIRE(gate->post_worker([&]() noexcept {
        while (!release_worker.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::microseconds{100});
        }
    }));

    // The single admission is occupied; the read is rejected as typed Busy
    // before any worker runs it, and the file is untouched (no inline
    // fallback, no silent drop).
    auto result = run_to_completion(filesystem.readTextFile("note.txt", std::stop_token{}));
    REQUIRE_FALSE(result);
    CHECK(result.error().code == harness::FileErrorCode::Busy);
    CHECK(workspace.read("note.txt") == "original");

    release_worker.store(true, std::memory_order_release);
    root.close();
}

TEST_CASE("queued filesystem work cancelled before start performs no side effects", "[harness][async][issue459]") {
    tests::TempWorkspace workspace;
    auto io = std::make_shared<boost::asio::io_context>();
    harness::RuntimeRoot root(io,
            harness::RuntimeLimits{
                    .worker_count = 1,
                    .max_admitted_operations = 8,
                    .max_admitted_bytes = 1024 * 1024,
            });
    auto target = root.make_target();
    harness::AsyncLocalFileSystem filesystem(target, workspace.path());

    // Occupy the single worker so the write below stays queued (not started).
    std::atomic<bool> release_gate{false};
    auto gate = target->try_admit(4);
    REQUIRE(gate.has_value());
    REQUIRE(gate->post_worker([&]() noexcept {
        while (!release_gate.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::microseconds{100});
        }
    }));
    // The gate holds mailbox sequence 0; complete it with a no-op terminal so
    // the admission-order mailbox does not hold back the write's outcome.
    std::move(*gate).complete([]() noexcept {});

    std::stop_source stop_source;
    std::optional<std::expected<void, harness::FileError>> outcome;
    auto pending = filesystem.writeFile("note.txt", std::string{"changed"}, stop_source.get_token());
    std::move(pending).start(
            [&](std::expected<void, harness::FileError> result) noexcept { outcome.emplace(std::move(result)); });
    // Cancel before the queued write reaches a worker, then release the gate.
    stop_source.request_stop();
    release_gate.store(true, std::memory_order_release);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (!outcome.has_value() && std::chrono::steady_clock::now() < deadline) {
        if (io->stopped()) {
            io->restart();
        }
        (void)io->poll();
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    REQUIRE(outcome.has_value());
    REQUIRE_FALSE(*outcome);
    CHECK(outcome->error().code == harness::FileErrorCode::Aborted);
    CHECK_FALSE(path_exists(workspace.path() / "note.txt"));
    root.close();
}

TEST_CASE("filesystem cleanup preserves replacement temporary resources", "[harness][filesystem][cleanup][issue558]") {
    tests::TempWorkspace workspace;
    TestRuntime runtime;
    harness::AsyncLocalFileSystem filesystem(runtime.make_target(), workspace.path());

    auto file = runtime.run(filesystem.createTempFile("owned-", std::nullopt, {}));
    auto directory = runtime.run(filesystem.createTempDir("owned-dir-", {}));
    REQUIRE(file);
    REQUIRE(directory);

    std::filesystem::remove(*file);
    std::ofstream replacement(*file, std::ios::binary | std::ios::trunc);
    replacement << "replacement";
    replacement.close();

    std::filesystem::remove_all(*directory);
    std::filesystem::create_directories(*directory);
    std::ofstream marker(std::filesystem::path{*directory} / "marker", std::ios::binary);
    marker << "replacement-dir";
    marker.close();

    auto cleanup = runtime.run(filesystem.cleanup());
    REQUIRE(cleanup);
    CHECK(std::filesystem::exists(*file));
    CHECK(workspace.read(".cch-tmp/" + std::filesystem::path{*file}.filename().string()) == "replacement");
    CHECK(std::filesystem::exists(std::filesystem::path{*directory} / "marker"));
    runtime.close();
}

TEST_CASE("filesystem ResourceLimit remains distinct from support Busy", "[harness][filesystem][capacity][issue558]") {
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
                                 })
                    .code == support::ErrorCode::Busy);
}
