#include <cch/chord/ReplicatedState.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

using namespace cch;
using namespace cch::chord;

namespace {

struct DeliveryEvent {
    ReplicatedStateDeliveryKind kind;
    std::int64_t sequence;
    double value;
};

[[nodiscard]] Json revision(double value) { return Json{Json::Object{{"value", Json{value}}}}; }

[[nodiscard]] double number_of(const Json& value) { return *value.as_object()->at("value").as_number(); }

[[nodiscard]] support::AsyncResult<void> ready() {
    return support::AsyncResult<void>{std::expected<void, support::Error>{}};
}

[[nodiscard]] support::AsyncResult<void> ready_failure(std::string message) {
    return support::AsyncResult<void>{std::expected<void, support::Error>{
            std::unexpected(support::make_error(support::ErrorCode::Unknown, std::move(message)))}};
}

using Completion = support::AsyncCompletion<void, support::Error>;

/// A listener that stays pending until the test settles it (pi's awaited
/// callback): the producer stores the terminal completion in `slot`.
[[nodiscard]] support::AsyncResult<void> hold(std::optional<Completion>& slot) {
    return support::AsyncResult<void>{support::AsyncProducer<void, support::Error>{
            [&slot](Completion completion) { slot = std::move(completion); }}};
}

void settle(std::optional<Completion>& slot) {
    Completion completion = std::move(*slot);
    slot.reset();
    completion(std::expected<void, support::Error>{});
}

void settle_failure(std::optional<Completion>& slot, std::string message) {
    Completion completion = std::move(*slot);
    slot.reset();
    completion(std::expected<void, support::Error>{
            std::unexpected(support::make_error(support::ErrorCode::Unknown, std::move(message)))});
}

class TestSource;

/// Frozen test attachment (`packages/chord/test/replicated-state.test.ts`
/// `TestSourceAttachment`): buffers before activation, publishes to the one
/// listener, and counts idempotent disposals.
class TestAttachment final : public ReplicatedStateSourceAttachment {
public:
    TestAttachment(ReplicatedStateSnapshot snapshot, TestSource& source)
        : snapshot_(std::move(snapshot)), source_(source) {}

    [[nodiscard]] const ReplicatedStateSnapshot& snapshot() const noexcept override { return snapshot_; }

    void activate(ReplicatedStateFrameSink listener) override {
        activated_ = true;
        listener_ = std::move(listener);
        std::deque<ReplicatedStateSourceFrame> buffered = std::move(buffered_);
        buffered_.clear();
        for (const ReplicatedStateSourceFrame& frame : buffered)
            (*listener_)(frame);
    }

    void dispose() noexcept override;

    void publish(const ReplicatedStateSourceFrame& frame) {
        if (disposed_) return;
        if (listener_) {
            (*listener_)(frame);
            return;
        }
        buffered_.push_back(frame);
    }

    [[nodiscard]] bool activated() const noexcept { return activated_; }
    [[nodiscard]] int dispose_calls() const noexcept { return dispose_calls_; }

private:
    ReplicatedStateSnapshot snapshot_;
    TestSource& source_;
    std::deque<ReplicatedStateSourceFrame> buffered_;
    std::optional<ReplicatedStateFrameSink> listener_;
    bool activated_{false};
    bool disposed_{false};
    int dispose_calls_{0};
};

/// Frozen test source (`packages/chord/test/replicated-state.test.ts`
/// `TestSource`): advances its own cursor even with no attachment and
/// publishes every later commit to every attachment in registration order.
class TestSource final : public ReplicatedStateSource {
public:
    explicit TestSource(Json value, std::int64_t cursor = 0) : value_(std::move(value)), cursor_(cursor) {}

    [[nodiscard]] support::Expected<std::unique_ptr<ReplicatedStateSourceAttachment>> attach() override {
        auto attachment =
                std::make_unique<TestAttachment>(ReplicatedStateSnapshot{.value = value_, .cursor = cursor_}, *this);
        attachments_.push_back(attachment.get());
        if (on_attach) on_attach();
        return std::unique_ptr<ReplicatedStateSourceAttachment>{std::move(attachment)};
    }

    void commit(Json value, Context context = background_context()) {
        commit_at(std::move(value), cursor_ + 1, std::move(context));
    }

    void commit_at(Json value, std::int64_t cursor, Context context = background_context(), OpBatch ops = {}) {
        value_ = value;
        cursor_ = cursor;
        const ReplicatedStateSourceFrame frame{
                .cursor = cursor, .value = value_, .ops = std::move(ops), .context = std::move(context)};
        const std::vector<TestAttachment*> attachments = attachments_;
        for (TestAttachment* attachment : attachments)
            attachment->publish(frame);
    }

    void detach(TestAttachment& attachment) {
        std::erase(attachments_, &attachment);
        ++dispose_calls_;
    }

    [[nodiscard]] std::size_t attachment_count() const noexcept { return attachments_.size(); }

    [[nodiscard]] bool attachment_activated() const noexcept {
        return !attachments_.empty() && attachments_.front()->activated();
    }

    [[nodiscard]] int dispose_calls() const noexcept { return dispose_calls_; }

    std::move_only_function<void()> on_attach;
    std::vector<TestAttachment*> attachments_;

private:
    Json value_;
    std::int64_t cursor_{0};
    int dispose_calls_{0};
};

void TestAttachment::dispose() noexcept {
    if (disposed_) return;
    disposed_ = true;
    ++dispose_calls_;
    buffered_.clear();
    listener_.reset();
    source_.detach(*this);
}

void record_delivery(
        std::vector<DeliveryEvent>& into, ReplicatedStateDeliveryKind kind, std::int64_t sequence, double value) {
    into.push_back(DeliveryEvent{.kind = kind, .sequence = sequence, .value = value});
}

} // namespace

// Frozen evidence: fixtures/pi-v1.3.0/audit-1062/ordered-frames.json
// "an attached state restores the latest committed revision when subscribing after existing commits".
TEST_CASE("attached state restores the latest committed revision when subscribing after existing commits",
        "[chord][replicated_state][issue1062][spec]") {
    TestSource source{revision(0), 40};
    source.commit_at(revision(1), 41);
    auto attached = AttachedReplicatedState::attach(source);
    REQUIRE(attached.has_value());

    std::vector<DeliveryEvent> deliveries;
    auto unsubscribe = attached->subscribe([&deliveries](Json value, Context, ReplicatedStateDelivery delivery) {
        record_delivery(deliveries, delivery.kind, delivery.sequence, number_of(value));
        return ready();
    });

    CHECK(source.attachment_count() == 1);
    CHECK(source.attachment_activated());
    REQUIRE(deliveries.size() == 1);
    CHECK(deliveries[0].kind == ReplicatedStateDeliveryKind::Hydrate);
    CHECK(deliveries[0].sequence == 0);
    CHECK(deliveries[0].value == 1.0);
    CHECK(number_of(attached->value()) == 1.0);
    unsubscribe();
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1062/ordered-frames.json
// "attachment captures the snapshot before activation and drains buffered frames in commit order".
TEST_CASE("attachment captures the snapshot before activation and drains buffered frames in commit order",
        "[chord][replicated_state][issue1062][spec]") {
    TestSource source{revision(0), 10};
    source.on_attach = [&source] {
        source.commit(revision(1));
        source.commit(revision(2));
    };
    auto attached = AttachedReplicatedState::attach(source);
    REQUIRE(attached.has_value());

    std::vector<DeliveryEvent> deliveries;
    auto unsubscribe = attached->subscribe([&deliveries](Json value, Context, ReplicatedStateDelivery delivery) {
        record_delivery(deliveries, delivery.kind, delivery.sequence, number_of(value));
        return ready();
    });

    REQUIRE(deliveries.size() == 1);
    CHECK(deliveries[0].kind == ReplicatedStateDeliveryKind::Hydrate);
    CHECK(deliveries[0].sequence == 2);
    CHECK(deliveries[0].value == 2.0);
    CHECK(number_of(attached->value()) == 2.0);
    unsubscribe();
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1062/ordered-frames.json
// "exact source values are published without applying or re-diffing the frame operations".
TEST_CASE("exact source values are published without applying or re-diffing frame operations",
        "[chord][replicated_state][issue1062][spec]") {
    TestSource source{revision(0)};
    auto attached = AttachedReplicatedState::attach(source);
    REQUIRE(attached.has_value());

    std::vector<double> published;
    auto unsubscribe = attached->subscribe([&published](Json value, Context, ReplicatedStateDelivery delivery) {
        if (delivery.kind == ReplicatedStateDeliveryKind::Update) published.push_back(number_of(value));
        return ready();
    });
    // The committed value deliberately disagrees with the frame's operation:
    // re-applying or re-diffing `ops` would publish 7 instead of 1.
    source.commit_at(
            revision(1), 1, background_context(), OpBatch{SetOp{.path = {path_key("value")}, .value = Json{7}}});

    REQUIRE(published.size() == 1);
    CHECK(published[0] == 1.0);
    CHECK(number_of(attached->value()) == 1.0);
    unsubscribe();
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1062/ordered-frames.json
// "reentrant commits buffer in order and a late subscriber hydrates past them at the latest sequence".
TEST_CASE("reentrant commits buffer in order and a late subscriber hydrates at the latest sequence",
        "[chord][replicated_state][issue1062][spec]") {
    TestSource source{revision(0)};
    auto attached = AttachedReplicatedState::attach(source);
    REQUIRE(attached.has_value());

    std::vector<double> early_updates;
    std::vector<DeliveryEvent> late_deliveries;
    bool nested = false;
    ReplicatedStateUnsubscribeHook late_unsubscribe;
    auto early_unsubscribe = attached->subscribe([&](Json value, Context, ReplicatedStateDelivery delivery) {
        if (delivery.kind != ReplicatedStateDeliveryKind::Update || delivery.sequence != 1 || nested) return ready();
        nested = true;
        source.commit(revision(2));
        late_unsubscribe = attached->subscribe(
                [&late_deliveries](Json late_value, Context, ReplicatedStateDelivery late_delivery) {
                    record_delivery(late_deliveries, late_delivery.kind, late_delivery.sequence, number_of(late_value));
                    return ready();
                });
        return ready();
    });
    auto update_unsubscribe =
            attached->subscribe([&early_updates](Json value, Context, ReplicatedStateDelivery delivery) {
                if (delivery.kind == ReplicatedStateDeliveryKind::Update) early_updates.push_back(number_of(value));
                return ready();
            });

    source.commit(revision(1));

    CHECK((early_updates == std::vector<double>{1.0, 2.0}));
    REQUIRE(late_deliveries.size() == 1);
    CHECK(late_deliveries[0].kind == ReplicatedStateDeliveryKind::Hydrate);
    CHECK(late_deliveries[0].sequence == 2);
    CHECK(late_deliveries[0].value == 2.0);
    CHECK(number_of(attached->value()) == 2.0);
    early_unsubscribe();
    update_unsubscribe();
    late_unsubscribe();
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1062/ordered-frames.json
// "one subscription serializes reentrant hydration and update callbacks in publication order".
TEST_CASE("one subscription serializes reentrant hydration and update callbacks in publication order",
        "[chord][replicated_state][issue1062][spec]") {
    TestSource source{revision(0)};
    auto attached = AttachedReplicatedState::attach(source);
    REQUIRE(attached.has_value());

    std::vector<std::string> events;
    auto unsubscribe = attached->subscribe([&](Json value, Context, ReplicatedStateDelivery) {
        const double current = number_of(value);
        events.push_back("start:" + std::to_string(static_cast<int>(current)));
        if (current < 2) source.commit(revision(current + 1));
        events.push_back("end:" + std::to_string(static_cast<int>(current)));
        return ready();
    });

    CHECK((events == std::vector<std::string>{"start:0", "end:0", "start:1", "end:1", "start:2", "end:2"}));
    unsubscribe();
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1062/ordered-frames.json
// "independent attachments converge separately and dispose idempotently without affecting each other".
TEST_CASE("independent attachments converge separately and dispose idempotently",
        "[chord][replicated_state][issue1062][spec]") {
    TestSource source{revision(0)};
    auto first = AttachedReplicatedState::attach(source);
    auto second = AttachedReplicatedState::attach(source);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK(source.attachment_count() == 2);

    std::vector<DeliveryEvent> second_deliveries;
    auto second_unsubscribe =
            second->subscribe([&second_deliveries](Json value, Context, ReplicatedStateDelivery delivery) {
                record_delivery(second_deliveries, delivery.kind, delivery.sequence, number_of(value));
                return ready();
            });

    source.commit(revision(1));
    CHECK(number_of(first->value()) == 1.0);
    CHECK(number_of(second->value()) == 1.0);

    first->dispose();
    first->dispose();
    CHECK(source.attachment_count() == 1);

    source.commit(revision(2));
    CHECK(number_of(first->value()) == 1.0);
    CHECK(number_of(second->value()) == 2.0);
    REQUIRE(second_deliveries.size() == 3);
    CHECK(second_deliveries[0].kind == ReplicatedStateDeliveryKind::Hydrate);
    CHECK(second_deliveries[0].sequence == 0);
    CHECK(second_deliveries[1].kind == ReplicatedStateDeliveryKind::Update);
    CHECK(second_deliveries[1].sequence == 1);
    CHECK(second_deliveries[2].kind == ReplicatedStateDeliveryKind::Update);
    CHECK(second_deliveries[2].sequence == 2);

    second->dispose();
    CHECK(source.attachment_count() == 0);
    CHECK(number_of(first->value()) == 1.0);
    second_unsubscribe();
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1057/replicated-state.json
// "a cursor gap is reported once and leaves the last good replicated value".
// Negative control: an implementation that reordered or skipped the cursor
// check would deliver the gap frame and converge on 9 instead of retaining 1.
TEST_CASE("a cursor gap is reported once and leaves the last good replicated value",
        "[chord][replicated_state][issue1062][spec]") {
    TestSource source{revision(1), 0};
    std::vector<std::string> errors;
    auto attached = AttachedReplicatedState::attach(source,
            ReplicatedStateSourceOptions{
                    .onError = [&errors](const support::Error& error) { errors.push_back(error.message); }});
    REQUIRE(attached.has_value());

    std::vector<DeliveryEvent> deliveries;
    auto unsubscribe = attached->subscribe([&deliveries](Json value, Context, ReplicatedStateDelivery delivery) {
        record_delivery(deliveries, delivery.kind, delivery.sequence, number_of(value));
        return ready();
    });
    REQUIRE(deliveries.size() == 1);

    source.commit_at(revision(9), 2);
    CHECK((errors == std::vector<std::string>{"Replicated state source cursor has a gap: expected 1, received 2"}));
    CHECK(number_of(attached->value()) == 1.0);
    CHECK(source.dispose_calls() == 1);

    source.commit_at(revision(10), 3);
    CHECK(errors.size() == 1);
    CHECK(number_of(attached->value()) == 1.0);
    REQUIRE(deliveries.size() == 1);
    CHECK(deliveries[0].value == 1.0);
    unsubscribe();
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1062/ordered-frames.json
// "a repeated cursor is reported once as a gap and keeps the last good value".
// Negative control for out-of-order delivery: the backward cursor is rejected by
// the same cursor check that rejects a skipped one.
TEST_CASE("a repeated cursor is reported once as a gap and keeps the last good value",
        "[chord][replicated_state][issue1062][spec]") {
    TestSource source{revision(1), 0};
    std::vector<std::string> errors;
    auto attached = AttachedReplicatedState::attach(source,
            ReplicatedStateSourceOptions{
                    .onError = [&errors](const support::Error& error) { errors.push_back(error.message); }});
    REQUIRE(attached.has_value());

    std::vector<DeliveryEvent> deliveries;
    auto unsubscribe = attached->subscribe([&deliveries](Json value, Context, ReplicatedStateDelivery delivery) {
        record_delivery(deliveries, delivery.kind, delivery.sequence, number_of(value));
        return ready();
    });
    source.commit_at(revision(2), 1);
    source.commit_at(revision(3), 1);

    CHECK((errors == std::vector<std::string>{"Replicated state source cursor has a gap: expected 2, received 1"}));
    REQUIRE(deliveries.size() == 2);
    CHECK(deliveries[1].kind == ReplicatedStateDeliveryKind::Update);
    CHECK(deliveries[1].sequence == 1);
    CHECK(deliveries[1].value == 2.0);
    CHECK(number_of(attached->value()) == 2.0);
    CHECK(source.attachment_count() == 0);

    source.commit_at(revision(4), 2);
    CHECK(errors.size() == 1);
    CHECK(number_of(attached->value()) == 2.0);
    unsubscribe();
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1062/observer-delivery.json
// "at most 100 deliveries pend behind one held listener and overflow keeps the newest (100 commits)".
TEST_CASE("at most 100 deliveries pend behind one held listener while a commit fits the bound",
        "[chord][replicated_state][issue1062][spec]") {
    TestSource source{revision(0)};
    auto attached = AttachedReplicatedState::attach(source);
    REQUIRE(attached.has_value());

    std::optional<Completion> hydration;
    std::vector<double> received;
    auto unsubscribe = attached->subscribe([&](Json value, Context, ReplicatedStateDelivery) {
        received.push_back(number_of(value));
        if (number_of(value) == 0) return hold(hydration);
        return ready();
    });
    for (int value = 1; value <= 100; value += 1)
        source.commit(revision(value));

    CHECK(received == std::vector<double>{0.0});
    settle(hydration);
    REQUIRE(received.size() == 101);
    for (int value = 1; value <= 100; value += 1)
        CHECK(received[static_cast<std::size_t>(value)] == value);
    unsubscribe();
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1062/observer-delivery.json
// "at most 100 deliveries pend behind one held listener and overflow keeps the newest (102 commits)".
// Negative control: an unbounded queue would also deliver 1 and 100; the
// coalesced-sequence assertion distinguishes the 100-pending bound.
TEST_CASE("overflow at the 100-pending bound keeps the newest pending delivery",
        "[chord][replicated_state][issue1062][spec]") {
    TestSource source{revision(0)};
    auto attached = AttachedReplicatedState::attach(source);
    REQUIRE(attached.has_value());

    std::optional<Completion> hydration;
    std::vector<double> received;
    auto unsubscribe = attached->subscribe([&](Json value, Context, ReplicatedStateDelivery) {
        received.push_back(number_of(value));
        if (number_of(value) == 0) return hold(hydration);
        return ready();
    });
    for (int value = 1; value <= 102; value += 1)
        source.commit(revision(value));

    CHECK(received == std::vector<double>{0.0});
    settle(hydration);
    CHECK((received == std::vector<double>{0.0, 101.0, 102.0}));
    unsubscribe();
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1062/observer-delivery.json
// "at most 100 deliveries pend behind one held listener and overflow keeps the newest (202 commits)".
TEST_CASE("repeated overflow at the 100-pending bound keeps the newest pending delivery",
        "[chord][replicated_state][issue1062][spec]") {
    TestSource source{revision(0)};
    auto attached = AttachedReplicatedState::attach(source);
    REQUIRE(attached.has_value());

    std::optional<Completion> hydration;
    std::vector<double> received;
    auto unsubscribe = attached->subscribe([&](Json value, Context, ReplicatedStateDelivery) {
        received.push_back(number_of(value));
        if (number_of(value) == 0) return hold(hydration);
        return ready();
    });
    for (int value = 1; value <= 202; value += 1)
        source.commit(revision(value));

    CHECK(received == std::vector<double>{0.0});
    settle(hydration);
    CHECK((received == std::vector<double>{0.0, 201.0, 202.0}));
    unsubscribe();
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1062/observer-delivery.json
// "overflow keeps only the newest pending delivery with its exact value context and delivery".
TEST_CASE("overflow keeps only the newest pending delivery with its exact value context and delivery",
        "[chord][replicated_state][issue1062][spec]") {
    TestSource source{revision(0)};
    auto attached = AttachedReplicatedState::attach(source);
    REQUIRE(attached.has_value());

    const ContextKey<int> marker_key = create_context_key<int>("1062.delivery-marker");
    const Context marked = with_context_value(marker_key, 102, background_context());
    std::optional<Completion> held;
    std::vector<double> received;
    std::vector<ReplicatedStateDelivery> deliveries;
    std::vector<std::optional<int>> markers;
    auto unsubscribe = attached->subscribe([&](Json value, Context context, ReplicatedStateDelivery delivery) {
        received.push_back(number_of(value));
        deliveries.push_back(delivery);
        markers.push_back(context.value(marker_key));
        if (number_of(value) == 1) return hold(held);
        return ready();
    });
    for (int value = 1; value < 102; value += 1)
        source.commit(revision(value));
    source.commit(revision(102), marked);
    CHECK(number_of(attached->value()) == 102.0);
    source.commit(revision(103));

    CHECK((received == std::vector<double>{0.0, 1.0}));
    settle(held);
    CHECK((received == std::vector<double>{0.0, 1.0, 102.0, 103.0}));
    REQUIRE(deliveries.size() == 4);
    CHECK(deliveries[2].kind == ReplicatedStateDeliveryKind::Update);
    CHECK(deliveries[2].sequence == 102);
    CHECK(markers[2] == std::optional<int>{102});
    unsubscribe();
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1062/observer-delivery.json
// "a failing listener is reported through onError in isolation and keeps receiving later deliveries".
TEST_CASE("a failing listener is reported in isolation and keeps receiving later deliveries",
        "[chord][replicated_state][issue1062][spec]") {
    TestSource source{revision(0)};
    std::vector<std::string> errors;
    auto attached = AttachedReplicatedState::attach(source,
            ReplicatedStateSourceOptions{
                    .onError = [&errors](const support::Error& error) { errors.push_back(error.message); }});
    REQUIRE(attached.has_value());

    std::optional<Completion> hydration;
    std::optional<Completion> update;
    std::vector<double> failing_received;
    std::vector<double> fast_received;
    auto failing_unsubscribe = attached->subscribe([&](Json value, Context, ReplicatedStateDelivery) {
        const double current = number_of(value);
        failing_received.push_back(current);
        if (current == 0) return hold(hydration);
        if (current == 1) return ready_failure("sync update");
        if (current == 2) return hold(update);
        return ready();
    });
    auto fast_unsubscribe = attached->subscribe([&fast_received](Json value, Context, ReplicatedStateDelivery) {
        fast_received.push_back(number_of(value));
        return ready();
    });
    for (int value = 1; value <= 3; value += 1)
        source.commit(revision(value));
    // The failing subscription is still held on its hydration: the other
    // subscriber has already converged in publication order.
    CHECK((fast_received == std::vector<double>{0.0, 1.0, 2.0, 3.0}));
    settle_failure(hydration, "async hydration");
    REQUIRE((failing_received == std::vector<double>{0.0, 1.0, 2.0}));
    settle_failure(update, "async update");

    CHECK((failing_received == std::vector<double>{0.0, 1.0, 2.0, 3.0}));
    CHECK((errors == std::vector<std::string>{"async hydration", "sync update", "async update"}));
    CHECK((fast_received == std::vector<double>{0.0, 1.0, 2.0, 3.0}));
    failing_unsubscribe();
    fast_unsubscribe();
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1062/observer-delivery.json
// "a synchronous hydration failure is reported without removing the subscription".
TEST_CASE("a synchronous hydration failure is reported without removing the subscription",
        "[chord][replicated_state][issue1062][spec]") {
    TestSource source{revision(0)};
    std::vector<std::string> errors;
    auto attached = AttachedReplicatedState::attach(source,
            ReplicatedStateSourceOptions{
                    .onError = [&errors](const support::Error& error) { errors.push_back(error.message); }});
    REQUIRE(attached.has_value());

    std::vector<double> received;
    auto unsubscribe = attached->subscribe([&received](Json value, Context, ReplicatedStateDelivery) {
        received.push_back(number_of(value));
        if (number_of(value) == 0) return ready_failure("sync hydration");
        return ready();
    });
    source.commit(revision(1));

    CHECK((errors == std::vector<std::string>{"sync hydration"}));
    CHECK((received == std::vector<double>{0.0, 1.0}));
    unsubscribe();
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1062/observer-delivery.json
// "unsubscribe discards queued deliveries without aborting or joining the running callback".
// The C++ mapping settles the pending listener through its AsyncResult
// completion; the callback retains no post-await body to join.
TEST_CASE("unsubscribe discards queued deliveries without aborting the running callback",
        "[chord][replicated_state][issue1062][spec]") {
    TestSource source{revision(0)};
    auto attached = AttachedReplicatedState::attach(source);
    REQUIRE(attached.has_value());

    std::stop_source cancellation;
    const Context cancellable = with_abort_signal(cancellation.get_token(), "1062", background_context());
    std::optional<Completion> held;
    std::vector<double> received;
    auto unsubscribe = attached->subscribe([&](Json value, Context, ReplicatedStateDelivery) {
        received.push_back(number_of(value));
        if (number_of(value) == 1) return hold(held);
        return ready();
    });
    source.commit(revision(1), cancellable);
    source.commit(revision(2), cancellable);
    unsubscribe();
    source.commit(revision(3), cancellable);

    CHECK_FALSE(cancellable.aborted());
    CHECK((received == std::vector<double>{0.0, 1.0}));
    settle(held);
    CHECK((received == std::vector<double>{0.0, 1.0}));
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1062/observer-delivery.json
// "a callback rejection that settles after unsubscribe is still reported".
TEST_CASE("a callback rejection that settles after unsubscribe is still reported",
        "[chord][replicated_state][issue1062][spec]") {
    TestSource source{revision(0)};
    std::vector<std::string> errors;
    auto attached = AttachedReplicatedState::attach(source,
            ReplicatedStateSourceOptions{
                    .onError = [&errors](const support::Error& error) { errors.push_back(error.message); }});
    REQUIRE(attached.has_value());

    std::optional<Completion> hydration;
    std::vector<double> received;
    auto unsubscribe = attached->subscribe([&](Json value, Context, ReplicatedStateDelivery) {
        received.push_back(number_of(value));
        return hold(hydration);
    });
    source.commit(revision(1));
    unsubscribe();
    settle_failure(hydration, "stopped callback");

    CHECK((errors == std::vector<std::string>{"stopped callback"}));
    CHECK(received == std::vector<double>{0.0});
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1062/observer-delivery.json
// "a held listener serializes its own updates while another subscription converges independently".
// Negative control for ordering/isolation: a Chord implementation that let the
// held subscription gate the publisher would not show the other subscriber at
// 1 and 2 before the held deliveries settle.
TEST_CASE("a held listener serializes its own updates while another subscription converges independently",
        "[chord][replicated_state][issue1062][spec]") {
    TestSource source{revision(0)};
    auto attached = AttachedReplicatedState::attach(source);
    REQUIRE(attached.has_value());

    // The C++ listener has no post-await body: a pi listener's continuation
    // maps to settling its AsyncResult completion, so the test records `end`
    // immediately before the settlement that lets the subscription advance.
    std::optional<Completion> hydration;
    std::optional<Completion> update;
    std::vector<std::string> events;
    std::vector<double> fast;
    const auto record_end = [&events](int value) { events.push_back("end:" + std::to_string(value)); };
    auto held_unsubscribe = attached->subscribe([&](Json value, Context, ReplicatedStateDelivery) {
        const double current = number_of(value);
        events.push_back("start:" + std::to_string(static_cast<int>(current)));
        if (current == 0) return hold(hydration);
        if (current == 1) return hold(update);
        record_end(static_cast<int>(current));
        return ready();
    });
    auto fast_unsubscribe = attached->subscribe([&fast](Json value, Context, ReplicatedStateDelivery) {
        fast.push_back(number_of(value));
        return ready();
    });
    source.commit(revision(1));
    source.commit(revision(2));

    CHECK((events == std::vector<std::string>{"start:0"}));
    CHECK((fast == std::vector<double>{0.0, 1.0, 2.0}));
    CHECK(number_of(attached->value()) == 2.0);
    record_end(0);
    settle(hydration);
    CHECK((events == std::vector<std::string>{"start:0", "end:0", "start:1"}));
    record_end(1);
    settle(update);
    CHECK((events == std::vector<std::string>{"start:0", "end:0", "start:1", "end:1", "start:2", "end:2"}));
    held_unsubscribe();
    fast_unsubscribe();
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1062/observer-delivery.json
// "two subscriptions of the same listener are delivered independently".
TEST_CASE("two subscriptions of the same listener behavior are delivered independently",
        "[chord][replicated_state][issue1062][spec]") {
    TestSource source{revision(0)};
    auto attached = AttachedReplicatedState::attach(source);
    REQUIRE(attached.has_value());

    std::optional<Completion> first_hydration;
    std::optional<Completion> second_hydration;
    std::vector<double> received;
    auto first_subscribe = attached->subscribe([&](Json value, Context, ReplicatedStateDelivery) {
        received.push_back(number_of(value));
        if (number_of(value) == 0) return hold(first_hydration);
        return ready();
    });
    auto second_subscribe = attached->subscribe([&](Json value, Context, ReplicatedStateDelivery) {
        received.push_back(number_of(value));
        if (number_of(value) == 0) return hold(second_hydration);
        return ready();
    });
    source.commit(revision(1));
    first_subscribe();
    first_subscribe();
    settle(first_hydration);
    settle(second_hydration);

    CHECK((received == std::vector<double>{0.0, 0.0, 1.0}));
    second_subscribe();
}
