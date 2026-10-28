#pragma once

#include <cch/chord/Delta.hpp>
#include <cch/chord/Json.hpp>
#include <cch/durable/Records.hpp>
#include <cch/durable/Session.hpp>
#include <cch/support/AsyncResult.hpp>
#include <cch/support/Error.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cch::durable {

class Harness;
struct ViewMount;

/// One committed revision of a structural view and the exact operations that
/// produced it (frozen pi `CommittedWatch.advance(value, ops)`): applying the
/// operations to the previously delivered revision reproduces `value`
/// exactly, so a serialized consumer never needs a fresh hydration.
struct ViewFrame {
    chord::Json value{};
    chord::OpBatch ops{};
};

/// A live task's durable status in the task graph without its checkpoint and
/// outcome payloads (frozen pi `TaskGraphState`).
struct TaskGraphNodeState {
    /// "pending", "running", "waiting", or "completing".
    std::string status{};
    /// The checkpoint phase of a pending/running/waiting task.
    std::string phase{};
    /// The tasks a waiting task waits for (frozen pi `TaskGraphState.on`).
    std::vector<TaskId> on{};
    /// The join policy of a waiting task (frozen pi `TaskGraphState.policy`).
    std::optional<JoinPolicy> policy{std::nullopt};
    /// The held outcome status of a completing task.
    std::optional<std::string> outcome{std::nullopt};

    [[nodiscard]] bool operator==(const TaskGraphNodeState&) const = default;
};

/// One live task node: its record, owner edge, and owned conversations
/// (frozen pi `TaskGraphNode`).
struct TaskGraphNode {
    TaskId id{};
    std::string kind{};
    ConversationId conversation_id{};
    /// Owner task; absent for a conversation-owned task.
    std::optional<TaskId> owner{std::nullopt};
    bool background{false};
    bool abort_requested{false};
    TaskGraphNodeState state{};
    /// Conversations this task owns, in ID order.
    std::vector<ConversationId> conversations{};

    [[nodiscard]] bool operator==(const TaskGraphNode&) const = default;
};

/// Every live task of the Session (frozen pi `TaskGraph`): a terminal task
/// leaves the graph, so an idle Session reports an empty task list.
struct TaskGraph {
    /// Every live task, in ID order.
    std::vector<TaskGraphNode> tasks{};

    [[nodiscard]] bool operator==(const TaskGraph&) const = default;
};

/// Receives one committed view frame (frozen pi `CommittedWatch` callback).
///
/// Serialized domain: every frame delivery of one watch runs in the one
/// serialized execution domain the mount publishes from (the Session's
/// commit-publication line for `Harness` watches), in commit order. The
/// callback must return promptly: it runs on the Session's publication line.
using ViewFrameListener = std::move_only_function<void(ViewFrame)>;

/// One committed structural watch over a conversation view or the task graph
/// (frozen pi `WatchHandle` over a `ConversationViews`/`TaskGraphView`
/// mount). The watch is acquired at one committed revision and delivers every
/// later committed frame in commit order.
///
/// Stopping a watch ends its deliveries only: it changes no committed or
/// runtime state, and other watches of the same mount keep receiving. On
/// Session end every live watch terminates with `SessionClosed` or
/// `SessionFailed`.
class ViewWatch {
public:
    ViewWatch(ViewWatch&&) noexcept;
    ViewWatch& operator=(ViewWatch&&) noexcept;
    ~ViewWatch();

    ViewWatch(const ViewWatch&) = delete;
    ViewWatch& operator=(const ViewWatch&) = delete;

    /// A copy of the acquisition revision, or of the latest delivered
    /// revision once frames have been delivered (frozen pi
    /// `CommittedWatch.value`).
    [[nodiscard]] chord::Json value() const;

    /// Idempotently stop future deliveries and return this watch's terminal
    /// result (frozen pi `WatchHandle.stop`).
    [[nodiscard]] WatchEndReason stop();

    /// Opaque state; defined in the module's private sources.
    struct Impl;

private:
    friend class Harness;
    friend struct ViewMount;
    friend class ViewMountRegistry;

    /// Attach a watcher at the mount's current revision. Runs on the Session
    /// mutation line so the acquisition revision and the registration admit
    /// no interleaved publication.
    [[nodiscard]] static ViewWatch attach(std::shared_ptr<ViewMount> mount, ViewFrameListener listener);

    explicit ViewWatch(std::shared_ptr<Impl> impl);
    std::shared_ptr<Impl> impl_;
};

} // namespace cch::durable
