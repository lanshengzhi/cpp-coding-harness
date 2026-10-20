#pragma once

#include <cch/chord/ChordContext.hpp>
#include <cch/chord/Delta.hpp>
#include <cch/chord/Json.hpp>
#include <cch/support/AsyncResult.hpp>
#include <cch/support/Error.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>

namespace cch::chord {

/// Whether one delivery is the subscription's cold hydration or a later
/// published revision (pi `ReplicatedStateDelivery.kind`).
enum class ReplicatedStateDeliveryKind {
    Hydrate,
    Update,
};

/// One ordered delivery to a subscription (pi `ReplicatedStateDelivery`).
struct ReplicatedStateDelivery {
    ReplicatedStateDeliveryKind kind{ReplicatedStateDeliveryKind::Hydrate};
    /// Monotonic publication sequence, independent of the source cursor.
    std::int64_t sequence{0};
};

/// One immutable authoritative revision committed after an attachment
/// snapshot (pi `ReplicatedStateSourceFrame`). The value and operation batch
/// stay valid after delivery; Chord publishes them and never applies or
/// re-diffs them.
struct ReplicatedStateSourceFrame {
    /// Monotonic source cursor. The first frame after a snapshot must be
    /// `snapshot.cursor + 1`; any other cursor is a reported gap.
    std::int64_t cursor{0};
    Json value;
    OpBatch ops;
    Context context;
};

/// The fixed snapshot captured at the atomic attachment boundary (pi
/// `ReplicatedStateSourceAttachment.snapshot`).
struct ReplicatedStateSnapshot {
    Json value;
    std::int64_t cursor{0};
};

/// One public subscription listener (pi `StateListener`), expressed through
/// `support::AsyncResult`: a ready result is the synchronous callback path and
/// a pending producer is the awaited path whose completion serializes the next
/// delivery of this subscription. Parameters are owned so a pending listener
/// may retain them after the call returns.
using ReplicatedStateSink = std::move_only_function<support::AsyncResult<void>(
        Json value, Context context, ReplicatedStateDelivery delivery)>;

/// Receives source-contract and subscription-listener failures without
/// throwing them into the source (pi `ReplicatedStateSourceOptions.onError`).
/// Without a hook the failure is dropped; the state and other subscriptions
/// keep working either way.
using ReplicatedStateErrorHook = std::move_only_function<void(const support::Error&)>;

/// The closure a subscription returns (pi `subscribe`'s returned function).
/// Idempotent, discards pending deliveries, and never aborts or joins a
/// callback already running.
using ReplicatedStateUnsubscribeHook = std::move_only_function<void() noexcept>;

/// Options of one attached replicated state (pi `ReplicatedStateSourceOptions`).
struct ReplicatedStateSourceOptions {
    ReplicatedStateErrorHook onError;
};

/// Receives every committed source frame in commit order (pi attachment
/// `activate` listener).
using ReplicatedStateFrameSink = std::move_only_function<void(const ReplicatedStateSourceFrame&)>;

/// The atomic attachment to one authoritative revision source (pi
/// `ReplicatedStateSourceAttachment`). The source owns the concrete
/// implementation; Chord only drives this contract.
class ReplicatedStateSourceAttachment {
public:
    ReplicatedStateSourceAttachment() = default;
    ReplicatedStateSourceAttachment(const ReplicatedStateSourceAttachment&) = delete;
    ReplicatedStateSourceAttachment& operator=(const ReplicatedStateSourceAttachment&) = delete;
    virtual ~ReplicatedStateSourceAttachment() = default;

    /// The fixed snapshot captured when `attach()` ran.
    [[nodiscard]] virtual const ReplicatedStateSnapshot& snapshot() const noexcept = 0;

    /// Install the sole listener and synchronously drain every buffered frame
    /// in source commit order. Single-use: an attachment that is already
    /// active or disposed is an invariant violation.
    virtual void activate(ReplicatedStateFrameSink listener) = 0;

    /// Stop delivery and release source resources. Idempotent.
    virtual void dispose() noexcept = 0;
};

/// An authoritative immutable revision source (pi `ReplicatedStateSource`).
/// `attach()` must synchronously and atomically capture one snapshot and
/// register the returned attachment to buffer every later committed frame:
/// the snapshot includes every commit before that boundary and buffered
/// frames include every commit after it, with no overlap or gap.
///
/// The source owns revision history and the cursor policy: a source that
/// retains older revisions can snapshot at the cursor it can replay from, and
/// it replays its own buffered frames through the attachment. Chord never
/// applies, re-diffs or reorders frame operations, and a subscription is only
/// told `value`/`context`/`delivery` — the operation batch is not part of the
/// public delivery. The ordered-history seam a Session document store supplies
/// belongs to that source (durable Session watch, #1063); this package
/// guarantees the attachment, ordering and retention contract around it.
class ReplicatedStateSource {
public:
    ReplicatedStateSource() = default;
    ReplicatedStateSource(const ReplicatedStateSource&) = delete;
    ReplicatedStateSource& operator=(const ReplicatedStateSource&) = delete;
    virtual ~ReplicatedStateSource() = default;

    /// Attach one publication view. A source that refuses attachment reports
    /// the ordinary failure through the expected error channel.
    [[nodiscard]] virtual support::Expected<std::unique_ptr<ReplicatedStateSourceAttachment>> attach() = 0;
};

/// At most this many deliveries wait behind one subscription's running
/// callback. Overflow keeps only the newest pending `value`/`context`/
/// `delivery`, so update sequences may skip (pi `StateSubscriber.push`).
inline constexpr std::size_t kReplicatedStatePendingLimit{100};

/// A synchronously hydrated publication-only view of one authoritative source
/// (pi `AttachedReplicatedState`): it publishes the exact immutable revisions
/// the source committed, keeps the last good value across a source contract
/// failure, and isolates every subscription's progress and failures.
class AttachedReplicatedState final {
public:
    /// Attach to `source` and synchronously hydrate from its snapshot. `source`
    /// must outlive the returned state (CODING_STANDARDS.md §7.5).
    [[nodiscard]] static support::Expected<AttachedReplicatedState> attach(
            ReplicatedStateSource& source, ReplicatedStateSourceOptions options = {});

    // Owning pimpl handle: moving preserves the subscriptions; the destructor
    // releases the source attachment.
    AttachedReplicatedState(AttachedReplicatedState&&) noexcept;
    AttachedReplicatedState& operator=(AttachedReplicatedState&&) noexcept;
    ~AttachedReplicatedState();

    AttachedReplicatedState(const AttachedReplicatedState&) = delete;
    AttachedReplicatedState& operator=(const AttachedReplicatedState&) = delete;

    /// The current immutable revision. After a cursor gap this is the last
    /// good value; after `dispose()` it is the last published value.
    [[nodiscard]] const Json& value() const noexcept;

    /// Add one independent subscription. The cold hydration is delivered
    /// before this returns; a later subscriber receives only the current
    /// revision, never revisions already covered by that hydration. Listener
    /// failures are reported through the error hook and never stop delivery;
    /// a slow listener retains at most `kReplicatedStatePendingLimit`
    /// deliveries without delaying the producer or other subscriptions.
    [[nodiscard]] ReplicatedStateUnsubscribeHook subscribe(ReplicatedStateSink listener);

    /// Idempotently release the source attachment. Subscriptions keep the last
    /// published value but receive no later frames.
    void dispose() noexcept;

private:
    struct Impl;

    explicit AttachedReplicatedState(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

} // namespace cch::chord
