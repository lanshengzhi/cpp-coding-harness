#include <cch/chord/ReplicatedState.hpp>

#include "support/UniqueFd.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <expected>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <unistd.h>

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
            [&slot](Completion completion) noexcept { slot = std::move(completion); }}};
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

/// One attachment's dispatch state, owned by the source and by the attachment
/// (the frozen source's strong Set reference and attachment fields). The
/// source holds it strongly until dispose, and keeps the dispatch state alive
/// through a frame callback that disposes synchronously, so a callback can
/// never observe a destroyed callable (frozen JS keeps the attachment until
/// its dispatch returns).
struct TestAttachmentState {
    ReplicatedStateSnapshot snapshot;
    std::deque<ReplicatedStateSourceFrame> buffered;
    std::optional<ReplicatedStateFrameSink> listener;
    bool activated{false};
    bool disposed{false};
    int dispose_calls{0};
};

/// Frozen test attachment (`packages/chord/test/replicated-state.test.ts`
/// `TestSourceAttachment`): buffers before activation, publishes to the one
/// listener, and counts idempotent disposals. Dispose keeps the executing
/// callable alive: a source may be disposed synchronously from its callback.
class TestAttachment final : public ReplicatedStateSourceAttachment {
public:
    TestAttachment(std::shared_ptr<TestAttachmentState> state, TestSource& source)
        : state_(std::move(state)), source_(source) {}

    [[nodiscard]] const ReplicatedStateSnapshot& snapshot() const noexcept override { return state_->snapshot; }

    void activate(ReplicatedStateFrameSink listener) override {
        state_->activated = true;
        state_->listener = std::move(listener);
        std::deque<ReplicatedStateSourceFrame> buffered = std::move(state_->buffered);
        state_->buffered.clear();
        for (const ReplicatedStateSourceFrame& frame : buffered) {
            if (state_->disposed) return; // a callback disposed the source
            (*state_->listener)(frame);
        }
    }

    void dispose() noexcept override;

    [[nodiscard]] bool activated() const noexcept { return state_->activated; }
    [[nodiscard]] int dispose_calls() const noexcept { return state_->dispose_calls; }

private:
    std::shared_ptr<TestAttachmentState> state_;
    TestSource& source_;
};

/// Frozen test source (`packages/chord/test/replicated-state.test.ts`
/// `TestSource`): advances its own cursor even with no attachment and
/// publishes every later commit to every attachment in registration order.
class TestSource final : public ReplicatedStateSource {
public:
    explicit TestSource(Json value, std::int64_t cursor = 0) : value_(std::move(value)), cursor_(cursor) {}

    [[nodiscard]] support::Expected<std::unique_ptr<ReplicatedStateSourceAttachment>> attach() override {
        auto state = std::make_shared<TestAttachmentState>();
        state->snapshot = ReplicatedStateSnapshot{.value = value_, .cursor = cursor_};
        dispatches_.push_back(state);
        auto attachment = std::make_unique<TestAttachment>(state, *this);
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
        const std::vector<std::shared_ptr<TestAttachmentState>> dispatches = dispatches_;
        for (const std::shared_ptr<TestAttachmentState>& state : dispatches)
            publish(state, frame);
    }

    static void publish(const std::shared_ptr<TestAttachmentState>& state, const ReplicatedStateSourceFrame& frame) {
        if (state->disposed) return;
        if (state->listener) {
            (*state->listener)(frame);
            return;
        }
        state->buffered.push_back(frame);
    }

    void detach(const std::shared_ptr<TestAttachmentState>& state) {
        std::erase(dispatches_, state);
        ++dispose_calls_;
    }

    [[nodiscard]] std::size_t attachment_count() const noexcept { return dispatches_.size(); }

    [[nodiscard]] bool attachment_activated() const noexcept {
        return !dispatches_.empty() && dispatches_.front()->activated;
    }

    [[nodiscard]] int dispose_calls() const noexcept { return dispose_calls_; }

    std::move_only_function<void()> on_attach;
    std::vector<std::shared_ptr<TestAttachmentState>> dispatches_;

private:
    Json value_;
    std::int64_t cursor_{0};
    int dispose_calls_{0};
};

void TestAttachment::dispose() noexcept {
    if (state_->disposed) return;
    state_->disposed = true;
    ++state_->dispose_calls;
    state_->buffered.clear();
    source_.detach(state_);
}

/// Captures the process's stderr into a pipe so a default diagnostic can be
/// compared exactly (the strict-no-exception mapping of pi's async rethrow).
/// The descriptors are owned by `support::UniqueFd` (CODING_STANDARDS §7.8).
class StderrCapture {
public:
    StderrCapture() {
        int fds[2];
        if (::pipe(fds) != 0) return;
        read_fd_.reset(fds[0]);
        support::UniqueFd write_end(fds[1]);
        saved_fd_.reset(::dup(STDERR_FILENO));
        if (saved_fd_) (void)::dup2(write_end.get(), STDERR_FILENO);
    }

    StderrCapture(const StderrCapture&) = delete;
    StderrCapture& operator=(const StderrCapture&) = delete;

    ~StderrCapture() { (void)finish(); }

    [[nodiscard]] std::string finish() {
        if (!read_fd_) return {};
        std::fflush(stderr);
        if (saved_fd_) {
            (void)::dup2(saved_fd_.get(), STDERR_FILENO);
            (void)saved_fd_.close();
        }
        std::string text;
        char buffer[256];
        ssize_t count = 0;
        while ((count = ::read(read_fd_.get(), buffer, sizeof buffer)) > 0)
            text.append(buffer, static_cast<std::size_t>(count));
        (void)read_fd_.close();
        return text;
    }

private:
    support::UniqueFd read_fd_;
    support::UniqueFd saved_fd_;
};

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
    auto early_unsubscribe = attached->subscribe([&](Json, Context, ReplicatedStateDelivery delivery) {
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
    CHECK(deliveries.size() == 1);

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
    CHECK((failing_received == std::vector<double>{0.0, 1.0, 2.0}));
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

// Review edge: fixtures/pi-v1.3.0/audit-1062/review-edges.json
// "a snapshot cursor outside the safe integer range is rejected and the attachment is disposed once".
TEST_CASE("a snapshot cursor outside the safe integer range fails attach and disposes the attachment once",
        "[chord][replicated_state][issue1062][review]") {
    TestSource source{revision(0), 9007199254740992LL};
    auto attached = AttachedReplicatedState::attach(source);
    REQUIRE_FALSE(attached.has_value());
    CHECK(attached.error().message == "Replicated state source snapshot cursor must be a safe integer");
    CHECK(source.dispose_calls() == 1);
    CHECK(source.attachment_count() == 0);
}

// "a frame cursor outside the safe integer range fails the attachment and keeps the last good value".
TEST_CASE("a frame cursor outside the safe integer range fails the attachment and keeps the last good value",
        "[chord][replicated_state][issue1062][review]") {
    TestSource source{revision(1), 9007199254740991LL};
    std::vector<std::string> errors;
    auto attached = AttachedReplicatedState::attach(source,
            ReplicatedStateSourceOptions{
                    .onError = [&errors](const support::Error& error) { errors.push_back(error.message); }});
    REQUIRE(attached.has_value());

    source.commit_at(revision(2), 9007199254740992LL);
    CHECK((errors == std::vector<std::string>{"Replicated state source frame cursor must be a safe integer"}));
    CHECK(number_of(attached->value()) == 1.0);
    CHECK(source.dispose_calls() == 1);
    CHECK(source.attachment_count() == 0);

    source.commit_at(revision(3), 9007199254740993LL);
    CHECK(errors.size() == 1);
    CHECK(number_of(attached->value()) == 1.0);
    CHECK(source.dispose_calls() == 1);
}

// "negative safe integer cursors are accepted for the snapshot and later frames".
TEST_CASE("negative safe integer cursors are accepted for the snapshot and later frames",
        "[chord][replicated_state][issue1062][review]") {
    TestSource source{revision(0), -5};
    auto attached = AttachedReplicatedState::attach(source);
    REQUIRE(attached.has_value());
    std::vector<double> received;
    auto unsubscribe = attached->subscribe([&received](Json value, Context, ReplicatedStateDelivery) {
        received.push_back(number_of(value));
        return ready();
    });

    source.commit_at(revision(1), -4);
    CHECK((received == std::vector<double>{0.0, 1.0}));
    CHECK(number_of(attached->value()) == 1.0);
    CHECK(source.attachment_count() == 1);
    unsubscribe();
}

// "a source contract failure without an onError hook is observable asynchronously and keeps the last good value".
TEST_CASE("a source gap without an onError hook writes the exact default diagnostic once and keeps the last good value",
        "[chord][replicated_state][issue1062][review]") {
    TestSource source{revision(1), 0};
    std::string diagnostics;
    {
        StderrCapture capture;
        auto attached = AttachedReplicatedState::attach(source);
        REQUIRE(attached.has_value());
        source.commit_at(revision(2), 1);
        source.commit_at(revision(3), 3);
        source.commit_at(revision(4), 4);
        CHECK(number_of(attached->value()) == 2.0);
        diagnostics = capture.finish();
    }
    CHECK(diagnostics == "Replicated state source cursor has a gap: expected 2, received 3\n");
    CHECK(source.dispose_calls() == 1);
    CHECK(source.attachment_count() == 0);
}

TEST_CASE("a listener failure without an onError hook writes the exact default diagnostic and delivery continues",
        "[chord][replicated_state][issue1062][review]") {
    TestSource source{revision(0)};
    std::string diagnostics;
    std::vector<double> received;
    {
        StderrCapture capture;
        auto attached = AttachedReplicatedState::attach(source);
        REQUIRE(attached.has_value());
        auto unsubscribe = attached->subscribe([&received](Json value, Context, ReplicatedStateDelivery) {
            const double current = number_of(value);
            received.push_back(current);
            if (current == 0) return ready_failure("sync hydration");
            return ready();
        });
        source.commit(revision(1));
        unsubscribe();
        diagnostics = capture.finish();
    }
    CHECK((received == std::vector<double>{0.0, 1.0}));
    CHECK(diagnostics == "sync hydration\n");
}

TEST_CASE(
        "an explicit onError hook suppresses the default diagnostic", "[chord][replicated_state][issue1062][review]") {
    TestSource source{revision(1), 0};
    std::vector<std::string> errors;
    std::string diagnostics;
    {
        StderrCapture capture;
        auto attached = AttachedReplicatedState::attach(source,
                ReplicatedStateSourceOptions{
                        .onError = [&errors](const support::Error& error) { errors.push_back(error.message); }});
        CHECK(attached.has_value());
        source.commit_at(revision(2), 3);
        diagnostics = capture.finish();
    }
    CHECK((errors == std::vector<std::string>{"Replicated state source cursor has a gap: expected 1, received 3"}));
    CHECK(diagnostics.empty());
}

// "a gap while draining a buffered activation ignores later buffered frames and disposes once".
TEST_CASE("a gap while draining buffered activation stops dispatch without touching a disposed listener",
        "[chord][replicated_state][issue1062][review]") {
    TestSource source{revision(0), 0};
    std::vector<std::string> errors;
    source.on_attach = [&source] {
        source.commit_at(revision(1), 1);
        source.commit_at(revision(2), 3);
        source.commit_at(revision(3), 4);
    };
    auto attached = AttachedReplicatedState::attach(source,
            ReplicatedStateSourceOptions{
                    .onError = [&errors](const support::Error& error) { errors.push_back(error.message); }});
    REQUIRE(attached.has_value());
    CHECK((errors == std::vector<std::string>{"Replicated state source cursor has a gap: expected 2, received 3"}));
    CHECK(number_of(attached->value()) == 1.0);
    CHECK(source.dispose_calls() == 1);
    CHECK(source.attachment_count() == 0);
}

TEST_CASE("a subscriber destroying its own attached state inside a callback does not disturb other subscribers",
        "[chord][replicated_state][issue1062][review]") {
    TestSource source{revision(0)};
    auto created = AttachedReplicatedState::attach(source);
    REQUIRE(created.has_value());
    auto state = std::move(*created);

    auto owner = std::make_optional(std::move(state));
    std::vector<double> other_received;
    auto destroyer = owner->subscribe([&owner, &source](Json, Context, ReplicatedStateDelivery delivery) {
        if (delivery.sequence == 1) {
            source.commit(revision(2)); // queues a publication behind the running dispatch
            owner.reset();
        }
        return ready();
    });
    auto other = owner->subscribe([&other_received](Json value, Context, ReplicatedStateDelivery) {
        other_received.push_back(number_of(value));
        return ready();
    });
    CHECK(owner.has_value());

    source.commit(revision(1));
    CHECK_FALSE(owner.has_value());
    CHECK((other_received == std::vector<double>{0.0, 1.0, 2.0}));
    CHECK(source.dispose_calls() == 1);
    destroyer();
    other();
}

// "subscribers are delivered in registration order and a reentrant subscriber hydrates before the next publication".
TEST_CASE("subscribers are delivered in registration order with reentrant unsubscribe and subscribe",
        "[chord][replicated_state][issue1062][review]") {
    TestSource source{revision(0)};
    auto attached = AttachedReplicatedState::attach(source);
    REQUIRE(attached.has_value());

    std::vector<std::string> events;
    std::optional<ReplicatedStateUnsubscribeHook> b;
    std::optional<ReplicatedStateUnsubscribeHook> d;
    const auto record = [&events](const char* name, ReplicatedStateDelivery delivery) {
        events.push_back(std::format("{}:{}:{}",
                name,
                delivery.kind == ReplicatedStateDeliveryKind::Hydrate ? "hydrate" : "update",
                delivery.sequence));
    };
    auto a = attached->subscribe([&](Json, Context, ReplicatedStateDelivery delivery) {
        record("a", delivery);
        if (delivery.kind == ReplicatedStateDeliveryKind::Update && delivery.sequence == 2) {
            if (b) (*b)();
            d = attached->subscribe([&events](Json, Context, ReplicatedStateDelivery inner) {
                events.push_back(std::format("d:{}:{}",
                        inner.kind == ReplicatedStateDeliveryKind::Hydrate ? "hydrate" : "update",
                        inner.sequence));
                return ready();
            });
        }
        return ready();
    });
    b = attached->subscribe([&](Json, Context, ReplicatedStateDelivery delivery) {
        record("b", delivery);
        return ready();
    });
    auto c = attached->subscribe([&](Json, Context, ReplicatedStateDelivery delivery) {
        record("c", delivery);
        return ready();
    });

    source.commit(revision(1));
    source.commit(revision(2));
    source.commit(revision(3));

    const std::vector<std::string> expected{"a:hydrate:0",
            "b:hydrate:0",
            "c:hydrate:0",
            "a:update:1",
            "b:update:1",
            "c:update:1",
            "a:update:2",
            "d:hydrate:2",
            "c:update:2",
            "a:update:3",
            "c:update:3",
            "d:update:3"};
    CHECK(events == expected);
    a();
    if (d) (*d)();
    c();
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1062/review-edges.json
// "a retained value reference keeps the revision it was read at across a later commit".
TEST_CASE("a retained value reference keeps the revision it was read at",
        "[chord][replicated_state][issue1062][review]") {
    TestSource source{revision(0)};
    auto attached = AttachedReplicatedState::attach(source);
    REQUIRE(attached.has_value());

    // The frozen observation: `retainedIsTheOldObject: true` (the retained
    // revision IS the object committed as revision 1) and
    // `currentIsTheRetainedObject: false` (the current revision is not that
    // object), with retainedValue 1 and currentValue 2.
    source.commit(revision(1));
    // The by-value result is a snapshot of one revision. Holding it as a
    // const reference extends the returned revision's lifetime, so this
    // observation must still read revision 1 after a later commit. A
    // re-aliased `const Json&` result would instead read the publisher's live
    // revision 2 here and fail this assertion.
    const Json& retained = attached->value();
    source.commit(revision(2));
    const Json current = attached->value();

    CHECK(number_of(retained) == 1.0);
    CHECK(number_of(current) == 2.0);
    CHECK_FALSE(retained.shares_container_with(current));
    CHECK((std::is_same_v<decltype(attached->value()), Json>));
}

TEST_CASE("a moved-from attached state can only be disposed or destroyed",
        "[chord][replicated_state][issue1062][review]") {
    TestSource source{revision(0)};
    auto created = AttachedReplicatedState::attach(source);
    REQUIRE(created.has_value());

    AttachedReplicatedState first = std::move(*created);
    first.dispose();
    CHECK(source.dispose_calls() == 1);
    AttachedReplicatedState moved = std::move(first);
    moved.dispose();
    CHECK(source.dispose_calls() == 1);
    first.dispose();
    CHECK(source.dispose_calls() == 1);
}

// Registration order is insertion order, not the subscriber object's address:
// freed registrations are reused by the allocator in LIFO order, so an
// address-keyed container reverses these deliveries.
TEST_CASE("subscribers registered after released registrations keep insertion order",
        "[chord][replicated_state][issue1062][review]") {
    TestSource source{revision(0)};
    auto attached = AttachedReplicatedState::attach(source);
    REQUIRE(attached.has_value());
    std::vector<std::string> order;
    const auto record = [&order](const char* name, ReplicatedStateDelivery delivery) {
        if (delivery.kind == ReplicatedStateDeliveryKind::Update) order.push_back(name);
    };
    std::vector<ReplicatedStateUnsubscribeHook> released;
    for (int index = 0; index < 4; index += 1) {
        released.push_back(
                attached->subscribe([&record, name = "released"](Json, Context, ReplicatedStateDelivery delivery) {
                    record(name, delivery);
                    return ready();
                }));
    }
    for (auto& hook : released)
        hook();
    released.clear(); // release the subscriber objects themselves

    std::vector<ReplicatedStateUnsubscribeHook> subscribers;
    for (int index = 0; index < 4; index += 1) {
        subscribers.push_back(attached->subscribe(
                [&record, name = std::format("s{}", index)](Json, Context, ReplicatedStateDelivery delivery) {
                    record(name.c_str(), delivery);
                    return ready();
                }));
    }
    source.commit(revision(1));
    CHECK((order == std::vector<std::string>{"s0", "s1", "s2", "s3"}));
    for (auto& hook : subscribers)
        hook();
}
