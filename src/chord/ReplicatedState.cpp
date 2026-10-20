#include <cch/chord/ReplicatedState.hpp>

#include <cch/support/AsyncResult.hpp>
#include <cch/support/Error.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <expected>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>

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
        if (*report_error_) (*report_error_)(error);
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

    [[nodiscard]] const Json& value() const noexcept { return value_; }

    [[nodiscard]] ReplicatedStateUnsubscribeHook subscribe(ReplicatedStateSink listener) {
        const Snapshot current{.value = value_, .sequence = sequence_};
        auto subscriber = std::make_shared<StateSubscriber>(std::move(listener), report_error_);
        listeners_.emplace(subscriber, current.sequence);
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
            if (const std::shared_ptr<ReplicatedStatePublisher> owner = publisher.lock())
                owner->listeners_.erase(subscriber);
        }};
    }

    /// Publish an already-prepared immutable revision. Subscriber failures are
    /// reported, never thrown.
    void publish(Json value, const Context& context) {
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
            const std::map<std::shared_ptr<StateSubscriber>, std::int64_t> listeners = listeners_;
            for (const auto& [subscriber, hydrated_sequence] : listeners) {
                if (publication.sequence <= hydrated_sequence) continue;
                subscriber->push(StateDelivery{
                        .value = publication.value,
                        .context = publication.context,
                        .delivery = ReplicatedStateDelivery{.kind = ReplicatedStateDeliveryKind::Update,
                                .sequence = publication.sequence},
                });
                subscriber->drain();
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

    std::map<std::shared_ptr<StateSubscriber>, std::int64_t> listeners_;
    std::shared_ptr<ReplicatedStateErrorHook> report_error_;
    std::deque<Publication> publications_;
    Json value_;
    std::int64_t sequence_{0};
    bool delivering_{false};
};

} // namespace

struct AttachedReplicatedState::Impl {
    Impl(std::unique_ptr<ReplicatedStateSourceAttachment> attachment, ReplicatedStateSourceOptions options)
        : attachment_(std::move(attachment)),
          report_error_(std::make_shared<ReplicatedStateErrorHook>(std::move(options.onError))),
          publisher_(std::make_shared<ReplicatedStatePublisher>(attachment_->snapshot().value, report_error_)),
          cursor_(attachment_->snapshot().cursor) {}

    ~Impl() { dispose(); }

    void activate() {
        attachment_->activate([this](const ReplicatedStateSourceFrame& frame) { receive(frame); });
    }

    [[nodiscard]] const Json& value() const noexcept { return publisher_->value(); }

    [[nodiscard]] ReplicatedStateUnsubscribeHook subscribe(ReplicatedStateSink listener) {
        return publisher_->subscribe(std::move(listener));
    }

    void dispose() noexcept {
        if (disposed_) return;
        disposed_ = true;
        attachment_->dispose();
    }

private:
    void receive(const ReplicatedStateSourceFrame& frame) {
        if (disposed_) return;
        if (cursor_ == std::numeric_limits<std::int64_t>::max()) std::terminate(); // cursor domain exhausted
        const std::int64_t expected = cursor_ + 1;
        if (frame.cursor != expected) {
            fail(support::make_error(support::ErrorCode::Validation,
                    "Replicated state source cursor has a gap: expected " + std::to_string(expected) + ", received " +
                            std::to_string(frame.cursor)));
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
        if (*report_error_) (*report_error_)(error);
    }

    std::unique_ptr<ReplicatedStateSourceAttachment> attachment_;
    std::shared_ptr<ReplicatedStateErrorHook> report_error_;
    std::shared_ptr<ReplicatedStatePublisher> publisher_;
    std::int64_t cursor_{0};
    bool disposed_{false};
};

AttachedReplicatedState::AttachedReplicatedState(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

support::Expected<AttachedReplicatedState> AttachedReplicatedState::attach(
        ReplicatedStateSource& source, ReplicatedStateSourceOptions options) {
    auto attachment = source.attach();
    if (!attachment) return std::unexpected(std::move(attachment.error()));
    auto impl = std::make_unique<Impl>(std::move(*attachment), std::move(options));
    impl->activate();
    return AttachedReplicatedState{std::move(impl)};
}

AttachedReplicatedState::AttachedReplicatedState(AttachedReplicatedState&&) noexcept = default;

AttachedReplicatedState& AttachedReplicatedState::operator=(AttachedReplicatedState&&) noexcept = default;

AttachedReplicatedState::~AttachedReplicatedState() = default;

const Json& AttachedReplicatedState::value() const noexcept { return impl_->value(); }

ReplicatedStateUnsubscribeHook AttachedReplicatedState::subscribe(ReplicatedStateSink listener) {
    return impl_->subscribe(std::move(listener));
}

void AttachedReplicatedState::dispose() noexcept {
    if (impl_) impl_->dispose();
}

} // namespace cch::chord
