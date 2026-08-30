#pragma once

#include <cch/support/AsyncResult.hpp>
#include "agent/harness/WorkspaceFileSystem.hpp"

#include <coroutine>
#include <expected>
#include <exception>
#include <memory>
#include <optional>
#include <utility>

namespace cch::coding_agent::detail {

/// Small coroutine implementation detail for composing Owner Interface
/// AsyncResults. The public loader contracts expose only support::AsyncResult;
/// no coroutine, executor, or scheduler type crosses the Owner boundary.
template <typename T, typename E>
class AsyncTask {
public:
    using completion_type = support::AsyncCompletion<T, E>;

    struct State {
        std::coroutine_handle<> handle;
        completion_type completion;

        ~State() {
            if (handle) {
                handle.destroy();
            }
        }
    };

    struct promise_type {
        std::shared_ptr<State> state;
        std::optional<std::expected<T, E>> outcome;

        [[nodiscard]] AsyncTask get_return_object() {
            auto state = std::make_shared<State>();
            const auto handle = std::coroutine_handle<promise_type>::from_promise(*this);
            state->handle = handle;
            this->state = state;
            return AsyncTask{std::move(state)};
        }

        [[nodiscard]] static std::suspend_always initial_suspend() noexcept {
            return {};
        }

        struct FinalAwaiter {
            [[nodiscard]] static bool await_ready() noexcept { return false; }
            void await_suspend(std::coroutine_handle<promise_type> handle) noexcept {
                auto state = handle.promise().state;
                auto completion = std::move(state->completion);
                auto outcome = std::move(*handle.promise().outcome);
                state->handle = {};
                handle.promise().state.reset();
                if (completion) {
                    completion(std::move(outcome));
                }
                handle.destroy();
            }
            static void await_resume() noexcept {}
        };

        [[nodiscard]] static FinalAwaiter final_suspend() noexcept { return {}; }

        static void unhandled_exception() noexcept { std::terminate(); }

        void return_value(std::expected<T, E> value) noexcept {
            outcome.emplace(std::move(value));
        }

        void return_value(std::unexpected<E> value) noexcept {
            outcome.emplace(std::move(value));
        }

        void return_value(T value) noexcept {
            outcome.emplace(std::move(value));
        }
    };

    AsyncTask(AsyncTask&&) noexcept = default;
    AsyncTask& operator=(AsyncTask&&) noexcept = default;
    AsyncTask(const AsyncTask&) = delete;
    AsyncTask& operator=(const AsyncTask&) = delete;

    void start(completion_type completion) noexcept {
        auto state = std::move(state_);
        state->completion = std::move(completion);
        state->handle.resume();
    }

private:
    explicit AsyncTask(std::shared_ptr<State> state) noexcept
        : state_(std::move(state)) {}

    std::shared_ptr<State> state_;
};

template <typename T, typename E>
[[nodiscard]] support::AsyncResult<T, E> to_async_result(
    AsyncTask<T, E> task) {
    return support::AsyncResult<T, E>{
        support::AsyncProducer<T, E>{
            [task = std::move(task)](
                typename AsyncTask<T, E>::completion_type completion) mutable noexcept {
                task.start(std::move(completion));
            }}};
}

/// Temporary synchronous expand-contract adapter for legacy callers. It owns
/// a copy of the established WorkspaceFileSystem and exposes only the
/// canonical asynchronous filesystem Interface to the loader implementations.
class SyncAsyncFileSystemAdapter final : public harness::AsyncFileSystem {
public:
    explicit SyncAsyncFileSystemAdapter(harness::WorkspaceFileSystem filesystem)
        : filesystem_(std::move(filesystem)) {}

    [[nodiscard]] const std::filesystem::path& workspace() const override {
        return filesystem_.root();
    }

    [[nodiscard]] support::AsyncResult<std::string, harness::FileError> absolutePath(
        std::string path,
        std::stop_token stop_token) override {
        return immediate<std::string>(path, stop_token, [&] {
            return filesystem_.absolutePath(path);
        });
    }

    [[nodiscard]] support::AsyncResult<std::string, harness::FileError> joinPath(
        std::vector<std::string> parts,
        std::stop_token stop_token) override {
        return immediate<std::string>(std::nullopt, stop_token, [&] {
            return filesystem_.joinPath(parts);
        });
    }

    [[nodiscard]] support::AsyncResult<std::string, harness::FileError> readTextFile(
        std::string path,
        std::stop_token stop_token) override {
        return immediate<std::string>(path, stop_token, [&] {
            return filesystem_.readTextFile(path);
        });
    }

    [[nodiscard]] support::AsyncResult<std::vector<std::string>, harness::FileError> readTextLines(
        std::string path,
        std::optional<int> max_lines,
        std::stop_token stop_token) override {
        return immediate<std::vector<std::string>>(path, stop_token, [&] {
            return filesystem_.readTextLines(path, max_lines);
        });
    }

    [[nodiscard]] support::AsyncResult<harness::BinaryData, harness::FileError> readBinaryFile(
        std::string path,
        std::stop_token stop_token) override {
        return immediate<harness::BinaryData>(path, stop_token, [&] {
            return filesystem_.readBinaryFile(path);
        });
    }

    [[nodiscard]] support::AsyncResult<void, harness::FileError> writeFile(
        std::string path,
        harness::WriteContent content,
        std::stop_token stop_token) override {
        return immediate<void>(path, stop_token, [&] {
            return filesystem_.writeFile(path, content);
        });
    }

    [[nodiscard]] support::AsyncResult<void, harness::FileError> appendFile(
        std::string path,
        harness::WriteContent content,
        std::stop_token stop_token) override {
        return immediate<void>(path, stop_token, [&] {
            return filesystem_.appendFile(path, content);
        });
    }

    [[nodiscard]] support::AsyncResult<harness::FileInfo, harness::FileError> fileInfo(
        std::string path,
        std::stop_token stop_token) override {
        return immediate<harness::FileInfo>(path, stop_token, [&] {
            return filesystem_.fileInfo(path);
        });
    }

    [[nodiscard]] support::AsyncResult<std::vector<harness::FileInfo>, harness::FileError> listDir(
        std::string path,
        std::stop_token stop_token) override {
        return immediate<std::vector<harness::FileInfo>>(path, stop_token, [&] {
            return filesystem_.listDir(path);
        });
    }

    [[nodiscard]] support::AsyncResult<std::string, harness::FileError> canonicalPath(
        std::string path,
        std::stop_token stop_token) override {
        return immediate<std::string>(path, stop_token, [&] {
            return filesystem_.canonicalPath(path);
        });
    }

    [[nodiscard]] support::AsyncResult<bool, harness::FileError> exists(
        std::string path,
        std::stop_token stop_token) override {
        return immediate<bool>(path, stop_token, [&] {
            return filesystem_.exists(path);
        });
    }

    [[nodiscard]] support::AsyncResult<void, harness::FileError> createDir(
        std::string path,
        bool recursive,
        std::stop_token stop_token) override {
        return immediate<void>(path, stop_token, [&] {
            return filesystem_.createDir(path, recursive);
        });
    }

    [[nodiscard]] support::AsyncResult<void, harness::FileError> remove(
        std::string path,
        bool recursive,
        std::stop_token stop_token) override {
        return immediate<void>(path, stop_token, [&] {
            return filesystem_.remove(path, recursive);
        });
    }

    [[nodiscard]] support::AsyncResult<std::string, harness::FileError> createTempDir(
        std::optional<std::string> prefix,
        std::stop_token stop_token) override {
        return immediate<std::string>(std::nullopt, stop_token, [&] {
            return filesystem_.createTempDir(prefix);
        });
    }

    [[nodiscard]] support::AsyncResult<std::string, harness::FileError> createTempFile(
        std::optional<std::string> prefix,
        std::optional<std::string> suffix,
        std::stop_token stop_token) override {
        return immediate<std::string>(std::nullopt, stop_token, [&] {
            return filesystem_.createTempFile(prefix, suffix);
        });
    }

    [[nodiscard]] support::AsyncResult<void, harness::FileError> cleanup() override {
        return support::AsyncResult<void, harness::FileError>{
            std::expected<void, harness::FileError>{}};
    }

private:
    template <typename Value, typename Operation>
    [[nodiscard]] static support::AsyncResult<Value, harness::FileError> immediate(
        std::optional<std::string> path,
        std::stop_token stop_token,
        Operation operation) {
        if (stop_token.stop_requested()) {
            return support::AsyncResult<Value, harness::FileError>{std::unexpected(
                harness::FileError{
                    .code = harness::FileErrorCode::Aborted,
                    .message = "Operation aborted",
                    .path = std::move(path),
                })};
        }
        return support::AsyncResult<Value, harness::FileError>{operation()};
    }

    harness::WorkspaceFileSystem filesystem_;
};

[[nodiscard]] inline std::shared_ptr<harness::AsyncFileSystem>
make_sync_async_filesystem(const harness::WorkspaceFileSystem& filesystem) {
    return std::make_shared<SyncAsyncFileSystemAdapter>(filesystem);
}

template <typename T, typename E>
[[nodiscard]] std::optional<std::expected<T, E>> run_sync_bridge(
    support::AsyncResult<T, E> operation) {
    std::optional<std::expected<T, E>> outcome;
    std::move(operation).start(
        [&outcome](std::expected<T, E> value) noexcept {
            outcome.emplace(std::move(value));
        });
    return outcome;
}

} // namespace cch::coding_agent::detail
