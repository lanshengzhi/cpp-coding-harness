#include <cch/chord/ReplicatedState.hpp>

#include <cch/support/AsyncResult.hpp>
#include <cch/support/Error.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cch::chord {

namespace {

/// One undelivered revision plus the exact context and delivery it was
/// published with (pi `StateDelivery`).
struct StateDelivery {
    Json value;
    Context context;
    ReplicatedStateDelivery delivery;
};

/// One public subscription, independent of producer and other subscriber
/// progress (pi `StateSubscriber`).
///
/// Deliveries run in publication order. A listener whose `AsyncResult` is not
/// settled inline keeps the subscription serializing later deliveries behind
/// it: at most `kReplicatedStatePendingLimit` wait, and overflow keeps only the
/// newest pending revision. Listener failures are reported once through the
/// shared error hook and delivery continues. Unsubscribe drops pending work
/// without aborting the callback the producer already owns.
class StateSubscriber : public std::enable_shared_from_this<StateSubscriber> {
public:
    StateSubscriber(ReplicatedStateSink listener, std::shared_ptr<ReplicatedStateErrorHook> report_error)
        : listener_(std::move(listener)), report_error_(std::move(report_error)) {}

    void push(StateDelivery frame) {
        if (closed_) return;
        if (pending_.size() == kReplicatedStatePendingLimit) {
            // Keep the cold hydration only while this subscription has not
            // started (pi's cold-replica guard); otherwise the user latest
            // revision replaces the whole pending queue.
            std::optional<StateDelivery> hydration;
            if (!started_) hydration = std::move(pending_.front());
            pending_.clear();
            if (hydration) pending_.push_back(std::move(*hydration));
        }
        pending_.push_back(std::move(frame));
    }

    void drain() noexcept {
        if (running_ || closed_) return;
        running_ = true;
        for (;;) {
            if (pending_.empty()) {
                running_ = false;
                return;
            }
            StateDelivery frame = std::move(pending_.front());
            pending_.pop_front();
            started_ = true;
            // Handshake between the listener's completion and this loop:
            // 0 = the listener is running, 1 = the loop will return and the
            // completion owns resuming, 2 = the completion settled inline so
            // this loop continues synchronously (pi's synchronous listener).
            auto handshake = std::make_shared<std::atomic<int>>(0);
            listener_(std::move(frame.value), std::move(frame.context), frame.delivery)
                    .start([self = shared_from_this(), handshake](
                                   std::expected<void, support::Error> outcome) mutable noexcept {
                        if (!outcome) self->report(outcome.error());
                        int state = 0;
                        if (handshake->compare_exchange_strong(state, 2, std::memory_order_acq_rel)) return;
                        self->resume(); // deferred settlement owns the next drain
                    });
            int state = 0;
            if (handshake->compare_exchange_strong(state, 1, std::memory_order_acq_rel)) return;
        }
    }

    void close() noexcept {
        closed_ = true;
        pending_.clear();
    }

private:
    void resume() noexcept {
        running_ = false;
        drain();
    }

    void report(const support::Error& error) noexcept {
        // Keep the hook alive if a reentrant report destroys the owner.
        const std::shared_ptr<ReplicatedStateErrorHook> hook = report_error_;
        if (*hook) (*hook)(error);
    }

    ReplicatedStateSink listener_;
    std::shared_ptr<ReplicatedStateErrorHook> report_error_;
    std::deque<StateDelivery> pending_;
    bool running_{false};
    bool started_{false};
    bool closed_{false};
};

/// Maintains local publication order independently of how revisions are
/// produced (pi `ReplicatedStatePublisher`). Every subscriber sees the
/// publisher's sequence order, including frames committed reentrantly from
/// another subscriber's callback.
class ReplicatedStatePublisher : public std::enable_shared_from_this<ReplicatedStatePublisher> {
public:
    struct Snapshot {
        Json value;
        std::int64_t sequence{0};
    };

    ReplicatedStatePublisher(Json initial, std::shared_ptr<ReplicatedStateErrorHook> report_error)
        : report_error_(std::move(report_error)), value_(std::move(initial)) {}

    [[nodiscard]] Json value() const noexcept { return value_; }

    [[nodiscard]] ReplicatedStateUnsubscribeHook subscribe(ReplicatedStateSink listener) {
        // The hydration callback may destroy the attached state; keep the
        // publisher and its registration storage alive until it returns.
        const std::shared_ptr<ReplicatedStatePublisher> self = shared_from_this();
        const Snapshot current{.value = value_, .sequence = sequence_};
        auto subscriber = std::make_shared<StateSubscriber>(std::move(listener), report_error_);
        listeners_.push_back(Listener{.subscriber = subscriber, .hydrated_sequence = current.sequence});
        subscriber->push(StateDelivery{
                .value = current.value,
                .context = background_context(),
                .delivery = ReplicatedStateDelivery{.kind = ReplicatedStateDeliveryKind::Hydrate,
                        .sequence = current.sequence},
        });
        subscriber->drain();
        std::weak_ptr<ReplicatedStatePublisher> publisher = weak_from_this();
        return ReplicatedStateUnsubscribeHook{[publisher, subscriber = std::move(subscriber)]() noexcept {
            subscriber->close();
            const std::shared_ptr<ReplicatedStatePublisher> owner = publisher.lock();
            if (!owner) return;
            std::erase_if(owner->listeners_,
                    [&subscriber](const Listener& listener) { return listener.subscriber == subscriber; });
        }};
    }

    /// Publish an already-prepared immutable revision. Subscriber failures are
    /// reported, never thrown.
    void publish(Json value, const Context& context) {
        // A listener may destroy the attached state inside this dispatch;
        // keep the publisher and its queued publications alive until the
        // accepted publication order drains (frozen GC retains the publisher).
        const std::shared_ptr<ReplicatedStatePublisher> self = shared_from_this();
        value_ = std::move(value);
        sequence_ += 1;
        publications_.push_back(Publication{.value = value_, .sequence = sequence_, .context = context});
        if (delivering_) return;

        delivering_ = true;
        while (!publications_.empty()) {
            Publication publication = std::move(publications_.front());
            publications_.pop_front();
            // Copy the registrations so reentrant subscribe/unsubscribe during
            // delivery cannot invalidate this iteration (pi `[...listeners]`).
            // The vector keeps registration order, like pi's insertion-ordered
            // Map; a reentrant unsubscribe closes the subscriber, so its copy
            // still in this iteration delivers nothing.
            const std::vector<Listener> listeners = listeners_;
            for (const Listener& listener : listeners) {
                if (publication.sequence <= listener.hydrated_sequence) continue;
                listener.subscriber->push(StateDelivery{
                        .value = publication.value,
                        .context = publication.context,
                        .delivery = ReplicatedStateDelivery{.kind = ReplicatedStateDeliveryKind::Update,
                                .sequence = publication.sequence},
                });
                listener.subscriber->drain();
            }
        }
        delivering_ = false;
    }

private:
    struct Publication {
        Json value;
        std::int64_t sequence{0};
        Context context;
    };

    struct Listener {
        std::shared_ptr<StateSubscriber> subscriber;
        std::int64_t hydrated_sequence{0};
    };

    std::vector<Listener> listeners_;
    std::shared_ptr<ReplicatedStateErrorHook> report_error_;
    std::deque<Publication> publications_;
    Json value_;
    std::int64_t sequence_{0};
    bool delivering_{false};
};

/// pi `Number.isSafeInteger` domain: \u00b1(2^53 - 1). Negative cursors are valid.
inline constexpr std::int64_t kMaxSafeCursor{9007199254740991};
inline constexpr std::int64_t kMinSafeCursor{-9007199254740991};

[[nodiscard]] bool is_safe_cursor(std::int64_t cursor) noexcept {
    return cursor >= kMinSafeCursor && cursor <= kMaxSafeCursor;
}

} // namespace

struct AttachedReplicatedState::Impl : std::enable_shared_from_this<AttachedReplicatedState::Impl> {
    Impl(std::unique_ptr<ReplicatedStateSourceAttachment> attachment, ReplicatedStateSourceOptions options)
        : attachment_(std::move(attachment)),
          report_error_(std::make_shared<ReplicatedStateErrorHook>(std::move(options.onError))),
          publisher_(std::make_shared<ReplicatedStatePublisher>(attachment_->snapshot().value, report_error_)),
          cursor_(attachment_->snapshot().cursor) {}

    ~Impl() { dispose(); }

    void activate() {
        attachment_->activate([this](const ReplicatedStateSourceFrame& frame) { receive(frame); });
    }

    [[nodiscard]] Json value() const noexcept { return publisher_->value(); }

    [[nodiscard]] ReplicatedStateUnsubscribeHook subscribe(ReplicatedStateSink listener) {
        // The hydration callback may destroy the owning state; keep this
        // implementation alive until subscription registration returns.
        const std::shared_ptr<Impl> self = shared_from_this();
        return publisher_->subscribe(std::move(listener));
    }

    void dispose() noexcept {
        if (disposed_) return;
        disposed_ = true;
        attachment_->dispose();
    }

private:
    void receive(const ReplicatedStateSourceFrame& frame) {
        // A listener or hook may destroy the owning state during this frame;
        // the frozen state is retained until the in-flight dispatch returns.
        const std::shared_ptr<Impl> self = shared_from_this();
        if (disposed_) return;
        if (!is_safe_cursor(frame.cursor)) {
            fail(support::make_error(
                    support::ErrorCode::Validation, "Replicated state source frame cursor must be a safe integer"));
            return;
        }
        // The snapshot cursor is a safe integer, so `expected` cannot
        // overflow and the next frame at the boundary is rejected above.
        const std::int64_t expected = cursor_ + 1;
        if (frame.cursor != expected) {
            fail(support::make_error(support::ErrorCode::Validation,
                    std::format("Replicated state source cursor has a gap: expected {}, received {}",
                            expected,
                            frame.cursor)));
            return;
        }
        cursor_ = frame.cursor;
        publisher_->publish(frame.value, frame.context);
    }

    /// Report the source contract failure once, dispose the broken attachment,
    /// and ignore every later frame; the last good value stays published.
    void fail(support::Error error) noexcept {
        if (disposed_) return;
        disposed_ = true;
        attachment_->dispose();
        const std::shared_ptr<ReplicatedStateErrorHook> hook = report_error_;
        if (*hook) (*hook)(error);
    }

    std::unique_ptr<ReplicatedStateSourceAttachment> attachment_;
    std::shared_ptr<ReplicatedStateErrorHook> report_error_;
    std::shared_ptr<ReplicatedStatePublisher> publisher_;
    std::int64_t cursor_{0};
    bool disposed_{false};
};

AttachedReplicatedState::AttachedReplicatedState(std::shared_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

support::Expected<AttachedReplicatedState> AttachedReplicatedState::attach(
        ReplicatedStateSource& source, ReplicatedStateSourceOptions options) {
    auto attachment = source.attach();
    if (!attachment) return std::unexpected(std::move(attachment.error()));
    if (!is_safe_cursor((*attachment)->snapshot().cursor)) {
        (*attachment)->dispose();
        return std::unexpected(support::make_error(
                support::ErrorCode::Validation, "Replicated state source snapshot cursor must be a safe integer"));
    }
    if (!options.onError) {
        // Strict-no-exception mapping of pi's reportErrorAsync: an ordinary
        // source or listener failure stays observable, once, on stderr.
        options.onError = [](const support::Error& error) {
            std::fputs(error.message.c_str(), stderr);
            std::fputc('\n', stderr);
            std::fflush(stderr);
        };
    }
    auto impl = std::make_shared<Impl>(std::move(*attachment), std::move(options));
    impl->activate();
    return AttachedReplicatedState{std::move(impl)};
}

AttachedReplicatedState::AttachedReplicatedState(AttachedReplicatedState&&) noexcept = default;

AttachedReplicatedState& AttachedReplicatedState::operator=(AttachedReplicatedState&&) noexcept = default;

AttachedReplicatedState::~AttachedReplicatedState() = default;

Json AttachedReplicatedState::value() const noexcept { return impl_->value(); }

ReplicatedStateUnsubscribeHook AttachedReplicatedState::subscribe(ReplicatedStateSink listener) {
    // A hydration listener may destroy this state; the local owner keeps the
    // implementation alive until registration returns.
    const std::shared_ptr<Impl> impl = impl_;
    return impl->subscribe(std::move(listener));
}

void AttachedReplicatedState::dispose() noexcept {
    if (impl_) impl_->dispose();
}

} // namespace cch::chord
