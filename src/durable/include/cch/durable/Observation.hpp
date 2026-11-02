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
#include <stop_token>
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

/// One committed agent event (frozen pi `AgentEvent`).
///
/// The value is the frozen event object verbatim, including its `type`
/// discriminator, so a consumer reads the same members the frozen caller reads
/// (`toolCallId`, `taskId`, `entry`, `record`, `inputs`, `usage`, `message`).
struct AgentEvent {
    chord::Json value{};

    /// The frozen `AgentEvent.type` of this event; empty when the value carries
    /// none.
    [[nodiscard]] std::string type() const;
};

/// Every event one commit produced, in order (frozen pi `listener(events)`).
using AgentEventBatch = std::vector<AgentEvent>;

/// Receives one committed event batch (frozen pi `AgentEventStream.start`).
///
/// Serialized domain: every batch delivery of one stream runs in the one
/// serialized execution domain the stream publishes from (the Session's
/// commit-publication line for `Harness` streams), in commit order, and one
/// batch at a time. The callback must return promptly: it runs on the
/// Session's publication line. A reported failure ends the stream with
/// `ListenerError` and never aborts work, changes committed state, or becomes
/// scheduling authority.
using AgentEventListener = std::move_only_function<support::ExpectedVoid(const AgentEventBatch&)>;

/// Opaque report bridge for observation failures; defined in the module's
/// private sources.
struct ObservationReportSink;

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
    /// `CommittedWatch.value`). A moved-from watch has no mount and returns
    /// an empty JSON value.
    [[nodiscard]] chord::Json value() const;

    /// Idempotently stop future deliveries and return this watch's terminal
    /// result (frozen pi `WatchHandle.stop`). A stop that arrives before a
    /// callback is admitted cancels that delivery and returns without waiting
    /// for the cancellation to be observed; a callback already entered may
    /// finish after this returns, because stopping never waits for or joins a
    /// running callback. Cross-watch stop cycles are not supported.
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

/// The durable public agent event stream of one conversation (frozen pi
/// `AgentEventStream` over `watchEvents`): the `snapshot` at attachment and
/// every later committed batch of `AgentEvent`s, in commit order.
///
/// The stream is a weak observation: it reads committed records and commits
/// nothing. Stopping it, failing it, or dropping it changes no committed or
/// runtime state and cannot become a second scheduler or abort persistent
/// work; only a cancellation the frozen observation surface observes (the
/// `stop_token` given at attachment) ends the stream with `Cancelled`.
///
/// Lifetime: the stream owns its mount independently of the Harness
/// afterwards, but the Harness and its Session must be closed or destroyed
/// before the storage they own; on Session end every live stream terminates
/// with `SessionClosed` or `SessionFailed`.
class AgentEventStream {
public:
    AgentEventStream(AgentEventStream&&) noexcept;
    AgentEventStream& operator=(AgentEventStream&&) noexcept;
    ~AgentEventStream();

    AgentEventStream(const AgentEventStream&) = delete;
    AgentEventStream& operator=(const AgentEventStream&) = delete;

    /// The `snapshot` event at attachment (frozen pi
    /// `AgentEventStream.snapshot`): the committed view, live run and
    /// generation state, tool slots, inbox, agent and usage at that revision.
    /// It is constant for the stream's lifetime.
    [[nodiscard]] const chord::Json& snapshot() const noexcept;

    /// Install the sole listener and deliver every buffered batch in commit
    /// order, one at a time (frozen pi `CommittedWatch.start`). Single-use: an
    /// already-started or stopped stream is rejected. Batches committed before
    /// this call were buffered; more than 100 undelivered batches are replaced
    /// by one snapshot batch (frozen pi `MAX_PENDING_WATCH_FRAMES`).
    [[nodiscard]] support::ExpectedVoid start(AgentEventListener listener);

    /// Idempotently stop future deliveries and return this stream's terminal
    /// result (frozen pi `WatchHandle.stop`). Stopping drops buffered batches
    /// and ends this observer only: other streams of the same conversation keep
    /// receiving, and a callback already entered may finish after this
    /// returns, because stopping never waits for or joins a running callback.
    [[nodiscard]] WatchEndReason stop();

    /// The listener failure that ended this stream, when one did (frozen pi
    /// `WatchEnd` `listener_error`).
    [[nodiscard]] std::optional<std::string> listener_error() const;

    /// Opaque state; defined in the module's private sources.
    struct Impl;

private:
    friend class Harness;
    friend struct ViewMount;
    friend class ViewMountRegistry;

    /// Attach a stream at the mount's current revision with its acquisition
    /// snapshot and the generation tasks already held at `completing`. Runs on
    /// the Session mutation line so the snapshot and the registration admit no
    /// interleaved publication.
    [[nodiscard]] static AgentEventStream attach(std::shared_ptr<ViewMount> mount,
            ConversationId conversation_id,
            chord::Json snapshot,
            std::vector<TaskId> held,
            std::shared_ptr<ObservationReportSink> reporter,
            std::stop_token stop);

    explicit AgentEventStream(std::shared_ptr<Impl> impl);
    std::shared_ptr<Impl> impl_;
};

} // namespace cch::durable
