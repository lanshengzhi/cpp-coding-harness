#pragma once

#include <cch/durable/Records.hpp>
#include <cch/durable/Session.hpp>
#include <cch/durable/Storage.hpp>
#include <cch/durable/Task.hpp>
#include <cch/support/AsyncResult.hpp>
#include <cch/support/Error.hpp>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace cch::durable {

/// Options for opening a Harness (frozen pi `HarnessOptions`).
struct HarnessOptions {
    std::vector<TaskDefinition> tasks{};
    std::move_only_function<std::int64_t()> now{};
};

/// Top-level durable runtime harness (frozen pi `Harness`).
class Harness {
public:
    Harness(Harness&&) noexcept;
    Harness& operator=(Harness&&) noexcept;
    ~Harness();

    Harness(const Harness&) = delete;
    Harness& operator=(const Harness&) = delete;

    /// Open a Harness over storage. The caller keeps `storage` alive until `close()` completes.
    [[nodiscard]] static support::AsyncResult<std::unique_ptr<Harness>> open(
            Storage& storage, HarnessOptions options = {});

    /// Access the underlying durable Session.
    [[nodiscard]] Session& session() noexcept;

    /// Look up a registered task definition by kind.
    [[nodiscard]] const TaskDefinition* task_definition(std::string_view kind) const noexcept;

    /// Run one atomic transaction on the Session.
    [[nodiscard]] support::AsyncResult<void> commit(
            std::move_only_function<support::Expected<void>(Transaction&)> change);

    /// Resume scheduling tasks.
    [[nodiscard]] support::ExpectedVoid resume();

    /// Reconcile surviving running tasks back to pending (call upon reopen).
    [[nodiscard]] support::AsyncResult<void> open_tasks();

    /// Wait for a task to reach terminal status and return its terminal record receipt.
    [[nodiscard]] support::AsyncResult<TaskRecord> wait_for_task(TaskId id);

    /// Abort a task. Returns "marked" or "terminal". The Harness must outlive the asynchronous operation.
    [[nodiscard]] support::AsyncResult<std::string> abort_task(TaskId id);

    /// Read the latest complete record for one task.
    [[nodiscard]] support::AsyncResult<std::optional<TaskRecord>> get_task(TaskId id);

    /// Close the harness.
    [[nodiscard]] support::AsyncResult<void> close();

private:
    struct Impl;
    explicit Harness(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace cch::durable
