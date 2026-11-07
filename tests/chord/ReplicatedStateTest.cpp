#include <cch/chord/ReplicatedState.hpp>

#include <cch/support/JsonAccess.hpp>

#include "support/ChordEvidence.hpp"
#include "support/UniqueFd.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
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
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <unistd.h>

using namespace cch;
using namespace cch::chord;

namespace {

/// The frozen #1057 replicated-state observation the receipts register.
[[nodiscard]] cch::tests::FrozenChordScenario replicated_state_scenario(std::string_view name) {
    auto scenario = cch::tests::FrozenChordScenario::load("audit-1057", "replicated-state.json", name);
    REQUIRE(scenario.has_value());
    return *scenario;
}

/// The frozen #1062 ordered-frames observation.
[[nodiscard]] cch::tests::FrozenChordScenario ordered_frames_scenario(std::string_view name) {
    auto scenario = cch::tests::FrozenChordScenario::load("audit-1062", "ordered-frames.json", name);
    REQUIRE(scenario.has_value());
    return *scenario;
}

/// The frozen #1062 review-edges observation.
[[nodiscard]] cch::tests::FrozenChordScenario review_edges_scenario(std::string_view name) {
    auto scenario = cch::tests::FrozenChordScenario::load("audit-1062", "review-edges.json", name);
    REQUIRE(scenario.has_value());
    return *scenario;
}

/// The frozen #1062 observer-delivery observation.
[[nodiscard]] cch::tests::FrozenChordScenario observer_delivery_scenario(std::string_view name) {
    auto scenario = cch::tests::FrozenChordScenario::load("audit-1062", "observer-delivery.json", name);
    REQUIRE(scenario.has_value());
    return *scenario;
}

/// Compare one recorded message list with a frozen `errors`/`reportedErrors` array whose entries
/// are either `{name, message}` objects or plain message strings.
void check_frozen_error_messages(const std::vector<std::string>& actual, const support::JsonValue::array_t& frozen) {
    REQUIRE(actual.size() == frozen.size());
    for (std::size_t index = 0; index < actual.size(); ++index) {
        const auto* record = frozen.at(index).get_if<support::JsonValue::object_t>();
        if (record != nullptr) {
            const auto message = support::json_string_member(*record, "message");
            REQUIRE(message.has_value());
            CHECK(actual[index] == *message);
        } else {
            const auto* message = frozen.at(index).get_if<std::string>();
            REQUIRE(message != nullptr);
            CHECK(actual[index] == *message);
        }
    }
}

/// Compare one recorded number list with a frozen array of numbers.
void check_frozen_numbers(const std::vector<double>& actual, const support::JsonValue::array_t& frozen) {
    REQUIRE(actual.size() == frozen.size());
    for (std::size_t index = 0; index < actual.size(); ++index) {
        const auto* number = frozen.at(index).get_if<double>();
        REQUIRE(number != nullptr);
        CHECK(actual[index] == *number);
    }
}

/// Compare one recorded event string list with a frozen array of strings.
void check_frozen_strings(const std::vector<std::string>& actual, const support::JsonValue::array_t& frozen) {
    REQUIRE(actual.size() == frozen.size());
    for (std::size_t index = 0; index < actual.size(); ++index) {
        const auto* text = frozen.at(index).get_if<std::string>();
        REQUIRE(text != nullptr);
        CHECK(actual[index] == *text);
    }
}

struct DeliveryEvent {
    ReplicatedStateDeliveryKind kind;
    std::int64_t sequence;
    double value;
    bool signal_present{false};
    bool signal_aborted{false};
};

[[nodiscard]] Json revision(double value) { return Json{Json::Object{{"value", Json{value}}}}; }

[[nodiscard]] double number_of(const Json& value) { return *value.as_object()->at("value").as_number(); }

/// The `{value: N}` revision value of one frozen delivery record.
[[nodiscard]] double frozen_delivery_number(const support::JsonValue& record) {
    const auto* object = record.get_if<support::JsonValue::object_t>();
    if (object == nullptr) {
        const auto* number = record.get_if<double>();
        REQUIRE(number != nullptr);
        return *number;
    }
    const auto value = support::json_number_member(*object, "value");
    REQUIRE(value.has_value());
    return *value;
}

/// The bare value numbers of one frozen delivery-record array.
[[nodiscard]] std::vector<double> frozen_values_as_numbers(const support::JsonValue::array_t& frozen) {
    std::vector<double> numbers;
    for (const auto& record : frozen) {
        const auto* object = record.get_if<support::JsonValue::object_t>();
        REQUIRE(object != nullptr);
        const auto* value = support::json_member(*object, "value");
        REQUIRE(value != nullptr);
        numbers.push_back(frozen_delivery_number(*value));
    }
    return numbers;
}

/// Compare one recorded delivery event list with a frozen `deliveries` array.
void check_frozen_deliveries(const std::vector<DeliveryEvent>& actual, const support::JsonValue::array_t& frozen) {
    REQUIRE(actual.size() == frozen.size());
    for (std::size_t index = 0; index < actual.size(); ++index) {
        const auto* record = frozen.at(index).get_if<support::JsonValue::object_t>();
        REQUIRE(record != nullptr);
        const auto kind = support::json_string_member(*record, "kind");
        const auto sequence = support::json_integer_member(*record, "sequence");
        REQUIRE(kind.has_value());
        REQUIRE(sequence.has_value());
        CHECK(actual[index].kind ==
                (*kind == "hydrate" ? ReplicatedStateDeliveryKind::Hydrate : ReplicatedStateDeliveryKind::Update));
        CHECK(actual[index].sequence == *sequence);
        CHECK(actual[index].value == frozen_delivery_number((*record).at("value")));
    }
}

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

[[nodiscard]] bool matches_frozen_hydration(const cch::tests::FrozenChordScenario& frozen,
        const std::vector<DeliveryEvent>& actual,
        const std::vector<Json>& actual_values,
        const Json& final_value) {
    const auto* expected = frozen.array_member("deliveries");
    const auto* expected_final = frozen.member("finalValue");
    REQUIRE(expected != nullptr);
    REQUIRE(expected_final != nullptr);
    const auto final_json = Json::from_support(*expected_final);
    REQUIRE(final_json);
    if (actual.size() != expected->size() || actual_values.size() != actual.size() || final_value != *final_json)
        return false;
    for (std::size_t index = 0; index < actual.size(); ++index) {
        const auto* record = expected->at(index).get_if<support::JsonValue::object_t>();
        REQUIRE(record != nullptr);
        const auto kind = support::json_string_member(*record, "kind");
        const auto sequence = support::json_integer_member(*record, "sequence");
        const auto* value = support::json_member(*record, "value");
        REQUIRE(kind.has_value());
        REQUIRE(sequence.has_value());
        REQUIRE(value != nullptr);
        const auto expected_value = Json::from_support(*value);
        REQUIRE(expected_value);
        if (actual[index].kind != (*kind == "hydrate" ? ReplicatedStateDeliveryKind::Hydrate
                                                      : ReplicatedStateDeliveryKind::Update) ||
                actual[index].sequence != *sequence || actual_values[index] != *expected_value)
            return false;
    }
    return true;
}

struct FrozenDisposalObservation {
    std::size_t after_attach;
    std::size_t after_first;
    std::size_t after_second;
    Json first_value;
    Json second_value;
};

[[nodiscard]] bool matches_frozen_disposal(
        const cch::tests::FrozenChordScenario& frozen, const FrozenDisposalObservation& actual) {
    const auto after_attach = frozen.number_member("attachmentCountAfterAttach");
    const auto after_first = frozen.number_member("attachmentCountAfterFirstDispose");
    const auto after_second = frozen.number_member("attachmentCountAfterSecondDispose");
    const auto* first_value = frozen.member("firstValueAfterDisposeAndLaterCommit");
    const auto* second_value = frozen.member("secondValueAfterLaterCommit");
    REQUIRE(after_attach.has_value());
    REQUIRE(after_first.has_value());
    REQUIRE(after_second.has_value());
    REQUIRE(first_value != nullptr);
    REQUIRE(second_value != nullptr);
    const auto first_json = Json::from_support(*first_value);
    const auto second_json = Json::from_support(*second_value);
    REQUIRE(first_json);
    REQUIRE(second_json);
    REQUIRE(std::cmp_less_equal(actual.after_attach, std::uint64_t{1} << 53));
    REQUIRE(std::cmp_less_equal(actual.after_first, std::uint64_t{1} << 53));
    REQUIRE(std::cmp_less_equal(actual.after_second, std::uint64_t{1} << 53));
    return static_cast<double>(actual.after_attach) == *after_attach &&
           static_cast<double>(actual.after_first) == *after_first &&
           static_cast<double>(actual.after_second) == *after_second && actual.first_value == *first_json &&
           actual.second_value == *second_json;
}

[[nodiscard]] bool matches_frozen_late_rejection(const cch::tests::FrozenChordScenario& frozen,
        const std::vector<std::string>& errors,
        const std::vector<double>& received) {
    const auto* expected_errors = frozen.array_member("reportedErrors");
    const auto* expected_received = frozen.array_member("received");
    REQUIRE(expected_errors != nullptr);
    REQUIRE(expected_received != nullptr);
    if (errors.size() != expected_errors->size() || received.size() != expected_received->size()) return false;
    for (std::size_t index = 0; index < errors.size(); ++index) {
        const auto* record = expected_errors->at(index).get_if<support::JsonValue::object_t>();
        REQUIRE(record != nullptr);
        const auto message = support::json_string_member(*record, "message");
        REQUIRE(message.has_value());
        if (errors[index] != *message) return false;
    }
    for (std::size_t index = 0; index < received.size(); ++index) {
        const auto* number = expected_received->at(index).get_if<double>();
        REQUIRE(number != nullptr);
        if (received[index] != *number) return false;
    }
    return true;
}

} // namespace

// Frozen evidence: fixtures/pi-v1.3.0/audit-1062/ordered-frames.json
// "an attached state restores the latest committed revision when subscribing after existing
// commits". The frozen snapshot cursor and the commit before attach drive the source; the frozen
// attachmentActivated flag, the hydrate delivery and the final value are compared exactly.
TEST_CASE("attached state restores the latest committed revision when subscribing after existing commits",
        "[chord][replicated_state][issue1062][spec]") {
    const auto frozen = ordered_frames_scenario(
            "an attached state restores the latest committed revision when subscribing after existing commits");
    const auto* snapshot = frozen.inputs_member("snapshot");
    REQUIRE(snapshot != nullptr);
    const auto snapshot_cursor =
            support::json_integer_member(*snapshot->get_if<support::JsonValue::object_t>(), "cursor");
    REQUIRE(snapshot_cursor.has_value());
    TestSource source{revision(0), *snapshot_cursor};
    const auto* commits_before = frozen.inputs_member("commitsBeforeAttach");
    REQUIRE(commits_before != nullptr);
    const auto* commit_records = commits_before->get_if<support::JsonValue::array_t>();
    REQUIRE(commit_records != nullptr);
    REQUIRE(commit_records->size() == 1);
    const auto commit_cursor =
            support::json_integer_member(*commit_records->at(0).get_if<support::JsonValue::object_t>(), "cursor");
    const auto commit_value =
            support::json_number_member(*commit_records->at(0).get_if<support::JsonValue::object_t>(), "value");
    REQUIRE(commit_cursor.has_value());
    REQUIRE(commit_value.has_value());
    source.commit_at(revision(*commit_value), *commit_cursor);
    auto attached = AttachedReplicatedState::attach(source);
    REQUIRE(attached.has_value());

    std::vector<DeliveryEvent> deliveries;
    auto unsubscribe = attached->subscribe([&deliveries](Json value, Context, ReplicatedStateDelivery delivery) {
        record_delivery(deliveries, delivery.kind, delivery.sequence, number_of(value));
        return ready();
    });

    const auto attachment_activated = frozen.boolean_member("attachmentActivated");
    REQUIRE(attachment_activated.has_value());
    CHECK(source.attachment_count() == 1);
    CHECK(source.attachment_activated() == *attachment_activated);
    const auto* frozen_deliveries = frozen.array_member("deliveries");
    REQUIRE(frozen_deliveries != nullptr);
    check_frozen_deliveries(deliveries, *frozen_deliveries);
    auto final_value = Json::from_support(*frozen.member("finalValue"));
    REQUIRE(final_value.has_value());
    CHECK(number_of(attached->value()) == number_of(*final_value));
    unsubscribe();
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1062/ordered-frames.json
// "attachment captures the snapshot before activation and drains buffered frames in commit
// order". The frozen snapshot and the two commits during attach drive the source; the frozen
// hydrate delivery and final value are compared exactly.
TEST_CASE("attachment captures the snapshot before activation and drains buffered frames in commit order",
        "[chord][replicated_state][issue1062][spec]") {
    const auto frozen = ordered_frames_scenario(
            "attachment captures the snapshot before activation and drains buffered frames in commit order");
    const auto* snapshot = frozen.inputs_member("snapshot");
    REQUIRE(snapshot != nullptr);
    const auto snapshot_cursor =
            support::json_integer_member(*snapshot->get_if<support::JsonValue::object_t>(), "cursor");
    REQUIRE(snapshot_cursor.has_value());
    TestSource source{revision(0), *snapshot_cursor};
    const auto* commits_during = frozen.inputs_member("commitsDuringAttach");
    REQUIRE(commits_during != nullptr);
    const auto* commit_records = commits_during->get_if<support::JsonValue::array_t>();
    REQUIRE(commit_records != nullptr);
    REQUIRE(commit_records->size() == 2);
    std::vector<std::int64_t> commit_cursors;
    std::vector<double> commit_values;
    for (const auto& record : *commit_records) {
        const auto* object = record.get_if<support::JsonValue::object_t>();
        REQUIRE(object != nullptr);
        const auto cursor = support::json_integer_member(*object, "cursor");
        const auto value = support::json_number_member(*object, "value");
        REQUIRE(cursor.has_value());
        REQUIRE(value.has_value());
        commit_cursors.push_back(*cursor);
        commit_values.push_back(*value);
    }
    source.on_attach = [&source, &commit_cursors, &commit_values] {
        source.commit_at(revision(commit_values[0]), commit_cursors[0]);
        source.commit_at(revision(commit_values[1]), commit_cursors[1]);
    };
    auto attached = AttachedReplicatedState::attach(source);
    REQUIRE(attached.has_value());

    std::vector<DeliveryEvent> deliveries;
    std::vector<Json> delivered_values;
    // The subscription is removed before either captured vector leaves scope.
    auto unsubscribe = attached->subscribe(
            [&deliveries, &delivered_values](Json value, Context, ReplicatedStateDelivery delivery) {
                record_delivery(deliveries, delivery.kind, delivery.sequence, number_of(value));
                delivered_values.push_back(value);
                return ready();
            });

    const auto* frozen_deliveries = frozen.array_member("deliveries");
    REQUIRE(frozen_deliveries != nullptr);
    check_frozen_deliveries(deliveries, *frozen_deliveries);
    auto final_value = Json::from_support(*frozen.member("finalValue"));
    REQUIRE(final_value.has_value());
    CHECK(number_of(attached->value()) == number_of(*final_value));
    CHECK(matches_frozen_hydration(frozen, deliveries, delivered_values, attached->value()));
    unsubscribe();
}

TEST_CASE("frozen attachment rejects dropping frames buffered during attach",
        "[chord][replicated_state][issue1240][spec]") {
    const auto frozen = ordered_frames_scenario(
            "attachment captures the snapshot before activation and drains buffered frames in commit order");
    const auto* snapshot = frozen.inputs_member("snapshot");
    const auto* commits = frozen.inputs_member("commitsDuringAttach");
    REQUIRE(snapshot != nullptr);
    REQUIRE(commits != nullptr);
    const auto* snapshot_object = snapshot->get_if<support::JsonValue::object_t>();
    const auto* records = commits->get_if<support::JsonValue::array_t>();
    REQUIRE(snapshot_object != nullptr);
    REQUIRE(records != nullptr);
    const auto initial = support::json_number_member(*snapshot_object, "value");
    const auto cursor = support::json_integer_member(*snapshot_object, "cursor");
    REQUIRE(initial.has_value());
    REQUIRE(cursor.has_value());
    std::vector<std::pair<double, std::int64_t>> frames;
    for (const auto& record : *records) {
        const auto* object = record.get_if<support::JsonValue::object_t>();
        REQUIRE(object != nullptr);
        const auto value = support::json_number_member(*object, "value");
        const auto sequence = support::json_integer_member(*object, "cursor");
        REQUIRE(value.has_value());
        REQUIRE(sequence.has_value());
        frames.emplace_back(*value, *sequence);
    }

    TestSource shallow_source{revision(*initial), *cursor};
    // attach invokes this callback synchronously; the source and frames outlive that call.
    shallow_source.on_attach = [&shallow_source, &frames] {
        for (const auto& [value, sequence] : frames)
            shallow_source.commit_at(revision(value), sequence);
    };
    // This state attaches and activates normally, but its frame handler ignores every update.
    struct ShallowState {
        std::unique_ptr<ReplicatedStateSourceAttachment> attachment;
        Json value;
        std::int64_t sequence{0};
        std::size_t ignored{0};
        explicit ShallowState(std::unique_ptr<ReplicatedStateSourceAttachment> source_attachment)
            : attachment(std::move(source_attachment)), value(attachment->snapshot().value) {
            // The fixture settles activation synchronously and the state outlives its attachment.
            attachment->activate([this](const ReplicatedStateSourceFrame&) { ++ignored; });
        }
        ~ShallowState() { attachment->dispose(); }
        void subscribe(const std::function<void(Json, ReplicatedStateDelivery)>& listener) const {
            listener(
                    value, ReplicatedStateDelivery{.kind = ReplicatedStateDeliveryKind::Hydrate, .sequence = sequence});
        }
    };
    auto source_attachment = shallow_source.attach();
    REQUIRE(source_attachment);
    ShallowState shallow{std::move(*source_attachment)};
    std::vector<DeliveryEvent> shallow_deliveries;
    std::vector<Json> shallow_values;
    shallow.subscribe([&shallow_deliveries, &shallow_values](Json value, ReplicatedStateDelivery delivery) {
        record_delivery(shallow_deliveries, delivery.kind, delivery.sequence, number_of(value));
        shallow_values.push_back(value);
    });
    CHECK(shallow.ignored == frames.size());
    CHECK_FALSE(matches_frozen_hydration(frozen, shallow_deliveries, shallow_values, shallow.value));

    TestSource source{revision(*initial), *cursor};
    // attach invokes this callback synchronously; the source and frames outlive that call.
    source.on_attach = [&source, &frames] {
        for (const auto& [value, sequence] : frames)
            source.commit_at(revision(value), sequence);
    };
    auto attached = AttachedReplicatedState::attach(source);
    REQUIRE(attached);
    std::vector<DeliveryEvent> deliveries;
    std::vector<Json> delivered_values;
    // The subscription is removed before the captured vectors leave scope.
    auto unsubscribe = attached->subscribe(
            [&deliveries, &delivered_values](Json value, Context, ReplicatedStateDelivery delivery) {
                record_delivery(deliveries, delivery.kind, delivery.sequence, number_of(value));
                delivered_values.push_back(value);
                return ready();
            });
    CHECK(matches_frozen_hydration(frozen, deliveries, delivered_values, attached->value()));
    unsubscribe();
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1062/ordered-frames.json
// "exact source values are published without applying or re-diffing the frame operations". The
// frozen snapshot and committed frame drive the source; the frozen publishedValue and the update
// delivery are compared exactly.
TEST_CASE("exact source values are published without applying or re-diffing frame operations",
        "[chord][replicated_state][issue1062][spec]") {
    const auto frozen = ordered_frames_scenario(
            "exact source values are published without applying or re-diffing the frame operations");
    const auto* committed = frozen.inputs_member("committed");
    REQUIRE(committed != nullptr);
    const auto* committed_object = committed->get_if<support::JsonValue::object_t>();
    REQUIRE(committed_object != nullptr);
    const auto committed_cursor = support::json_integer_member(*committed_object, "cursor");
    const auto committed_value = support::json_number_member(*committed_object, "value");
    REQUIRE(committed_cursor.has_value());
    REQUIRE(committed_value.has_value());
    TestSource source{revision(0)};
    auto attached = AttachedReplicatedState::attach(source);
    REQUIRE(attached.has_value());

    std::vector<DeliveryEvent> published;
    auto unsubscribe = attached->subscribe([&published](Json value, Context, ReplicatedStateDelivery delivery) {
        if (delivery.kind == ReplicatedStateDeliveryKind::Update)
            record_delivery(published, delivery.kind, delivery.sequence, number_of(value));
        return ready();
    });
    // The committed value deliberately disagrees with the frame's frozen operation:
    // re-applying or re-diffing `ops` would publish 7 instead of the frozen 1.
    const auto* committed_ops = support::json_array_member(*committed_object, "ops");
    REQUIRE(committed_ops != nullptr);
    auto ops_json = Json::from_support(support::JsonValue{*committed_ops});
    REQUIRE(ops_json.has_value());
    auto decoded = decode_ops(*ops_json);
    REQUIRE(decoded.has_value());
    source.commit_at(revision(*committed_value), *committed_cursor, background_context(), *decoded);

    const auto* frozen_published = frozen.object_member("publishedDelivery");
    REQUIRE(frozen_published != nullptr);
    const auto* frozen_published_value = frozen.member("publishedValue");
    REQUIRE(frozen_published_value != nullptr);
    const auto published_value = frozen_delivery_number(*frozen_published_value);
    const auto published_sequence = support::json_integer_member(*frozen_published, "sequence");
    const auto published_kind = support::json_string_member(*frozen_published, "kind");
    REQUIRE(published_sequence.has_value());
    REQUIRE(published_kind.has_value());
    REQUIRE(published.size() == 1);
    CHECK(published[0].kind == ReplicatedStateDeliveryKind::Update);
    CHECK(published[0].sequence == *published_sequence);
    CHECK(published[0].value == published_value);
    auto final_value = Json::from_support(*frozen.member("finalValue"));
    REQUIRE(final_value.has_value());
    CHECK(number_of(attached->value()) == number_of(*final_value));
    unsubscribe();
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1062/ordered-frames.json
// "reentrant commits buffer in order and a late subscriber hydrates past them at the latest
// sequence". The frozen reentrant frame chain drives the source; the frozen early-subscriber
// updates, the late hydrate delivery and the final value are compared exactly.
TEST_CASE("reentrant commits buffer in order and a late subscriber hydrates at the latest sequence",
        "[chord][replicated_state][issue1062][spec]") {
    const auto frozen = ordered_frames_scenario(
            "reentrant commits buffer in order and a late subscriber hydrates past them at the latest sequence");
    const auto* frames = frozen.inputs_member("frames");
    REQUIRE(frames != nullptr);
    const auto* frame_records = frames->get_if<support::JsonValue::array_t>();
    REQUIRE(frame_records != nullptr);
    REQUIRE(frame_records->size() == 2);
    const auto first_cursor =
            support::json_integer_member(*frame_records->at(0).get_if<support::JsonValue::object_t>(), "cursor");
    const auto first_value =
            support::json_number_member(*frame_records->at(0).get_if<support::JsonValue::object_t>(), "value");
    const auto second_cursor =
            support::json_integer_member(*frame_records->at(1).get_if<support::JsonValue::object_t>(), "cursor");
    const auto second_value =
            support::json_number_member(*frame_records->at(1).get_if<support::JsonValue::object_t>(), "value");
    REQUIRE(first_cursor.has_value());
    REQUIRE(first_value.has_value());
    REQUIRE(second_cursor.has_value());
    REQUIRE(second_value.has_value());

    TestSource source{revision(0)};
    auto attached = AttachedReplicatedState::attach(source);
    REQUIRE(attached.has_value());

    std::vector<double> early_updates;
    std::vector<DeliveryEvent> late_deliveries;
    bool nested = false;
    ReplicatedStateUnsubscribeHook late_unsubscribe;
    auto early_unsubscribe = attached->subscribe([&](Json, Context, ReplicatedStateDelivery delivery) {
        if (delivery.kind != ReplicatedStateDeliveryKind::Update || delivery.sequence != *first_cursor || nested)
            return ready();
        nested = true;
        source.commit_at(revision(*second_value), *second_cursor);
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

    source.commit_at(revision(*first_value), *first_cursor);

    const auto* frozen_early = frozen.array_member("earlySubscriberUpdates");
    REQUIRE(frozen_early != nullptr);
    check_frozen_numbers(early_updates, *frozen_early);
    const auto* frozen_late = frozen.array_member("lateSubscriberDeliveries");
    REQUIRE(frozen_late != nullptr);
    check_frozen_deliveries(late_deliveries, *frozen_late);
    auto final_value = Json::from_support(*frozen.member("finalValue"));
    REQUIRE(final_value.has_value());
    CHECK(number_of(attached->value()) == number_of(*final_value));
    early_unsubscribe();
    update_unsubscribe();
    late_unsubscribe();
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1062/ordered-frames.json
// "one subscription serializes reentrant hydration and update callbacks in publication order".
// The frozen reentrant chain end drives the commit loop; the frozen event order is compared
// exactly.
TEST_CASE("one subscription serializes reentrant hydration and update callbacks in publication order",
        "[chord][replicated_state][issue1062][spec]") {
    const auto frozen = ordered_frames_scenario(
            "one subscription serializes reentrant hydration and update callbacks in publication order");
    const auto chain_until = frozen.inputs_number("reentrantChainUntil");
    REQUIRE(chain_until.has_value());

    TestSource source{revision(0)};
    auto attached = AttachedReplicatedState::attach(source);
    REQUIRE(attached.has_value());

    std::vector<std::string> events;
    auto unsubscribe = attached->subscribe([&](Json value, Context, ReplicatedStateDelivery) {
        const double current = number_of(value);
        events.push_back("start:" + std::to_string(static_cast<int>(current)));
        if (current < *chain_until) source.commit(revision(current + 1));
        events.push_back("end:" + std::to_string(static_cast<int>(current)));
        return ready();
    });

    const auto* frozen_events = frozen.array_member("events");
    REQUIRE(frozen_events != nullptr);
    check_frozen_strings(events, *frozen_events);
    unsubscribe();
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1062/ordered-frames.json
// "independent attachments converge separately and dispose idempotently without affecting each
// other". The frozen commits drive the source; the frozen attachment counts, the first attachment's
// retained value after dispose and a later commit, and the second attachment's converged value and
// deliveries are compared exactly.
TEST_CASE("independent attachments converge separately and dispose idempotently",
        "[chord][replicated_state][issue1062][spec]") {
    const auto frozen = ordered_frames_scenario(
            "independent attachments converge separately and dispose idempotently without affecting each other");
    const auto* commits = frozen.inputs_member("commits");
    REQUIRE(commits != nullptr);
    const auto* commit_records = commits->get_if<support::JsonValue::array_t>();
    REQUIRE(commit_records != nullptr);
    REQUIRE(commit_records->size() == 2);
    std::vector<std::int64_t> commit_cursors;
    std::vector<double> commit_values;
    for (const auto& record : *commit_records) {
        const auto* object = record.get_if<support::JsonValue::object_t>();
        REQUIRE(object != nullptr);
        const auto cursor = support::json_integer_member(*object, "cursor");
        const auto value = support::json_number_member(*object, "value");
        REQUIRE(cursor.has_value());
        REQUIRE(value.has_value());
        commit_cursors.push_back(*cursor);
        commit_values.push_back(*value);
    }
    const auto count_after_attach = frozen.number_member("attachmentCountAfterAttach");
    const auto count_after_first_dispose = frozen.number_member("attachmentCountAfterFirstDispose");
    const auto count_after_second_dispose = frozen.number_member("attachmentCountAfterSecondDispose");
    REQUIRE(count_after_attach.has_value());
    REQUIRE(count_after_first_dispose.has_value());
    REQUIRE(count_after_second_dispose.has_value());

    TestSource source{revision(0)};
    auto first = AttachedReplicatedState::attach(source);
    auto second = AttachedReplicatedState::attach(source);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK(source.attachment_count() == static_cast<std::size_t>(*count_after_attach));
    const auto observed_after_attach = source.attachment_count();

    std::vector<DeliveryEvent> second_deliveries;
    auto second_unsubscribe =
            second->subscribe([&second_deliveries](Json value, Context, ReplicatedStateDelivery delivery) {
                record_delivery(second_deliveries, delivery.kind, delivery.sequence, number_of(value));
                return ready();
            });

    source.commit_at(revision(commit_values[0]), commit_cursors[0]);
    auto first_after_dispose = Json::from_support(*frozen.member("firstValueAfterDisposeAndLaterCommit"));
    REQUIRE(first_after_dispose.has_value());
    auto second_after_commit = Json::from_support(*frozen.member("secondValueAfterLaterCommit"));
    REQUIRE(second_after_commit.has_value());
    CHECK(number_of(first->value()) == number_of(*first_after_dispose));
    CHECK(number_of(second->value()) == number_of(*first_after_dispose));

    first->dispose();
    first->dispose();
    CHECK(source.attachment_count() == static_cast<std::size_t>(*count_after_first_dispose));
    const auto observed_after_first = source.attachment_count();

    source.commit_at(revision(commit_values[1]), commit_cursors[1]);
    CHECK(number_of(first->value()) == number_of(*first_after_dispose));
    CHECK(number_of(second->value()) == number_of(*second_after_commit));
    const auto* frozen_second = frozen.array_member("secondDeliveries");
    if (frozen_second != nullptr) {
        check_frozen_deliveries(second_deliveries, *frozen_second);
    } else {
        REQUIRE(second_deliveries.size() == 3);
        CHECK(second_deliveries[0].kind == ReplicatedStateDeliveryKind::Hydrate);
        CHECK(second_deliveries[0].sequence == 0);
        CHECK(second_deliveries[1].kind == ReplicatedStateDeliveryKind::Update);
        CHECK(second_deliveries[1].sequence == 1);
        CHECK(second_deliveries[2].kind == ReplicatedStateDeliveryKind::Update);
        CHECK(second_deliveries[2].sequence == 2);
    }

    second->dispose();
    CHECK(source.attachment_count() == static_cast<std::size_t>(*count_after_second_dispose));
    CHECK(matches_frozen_disposal(frozen,
            FrozenDisposalObservation{.after_attach = observed_after_attach,
                    .after_first = observed_after_first,
                    .after_second = source.attachment_count(),
                    .first_value = first->value(),
                    .second_value = second->value()}));
    CHECK(number_of(first->value()) == number_of(*first_after_dispose));
    second_unsubscribe();
}

TEST_CASE("frozen attachment comparison rejects coupled disposal", "[chord][replicated_state][issue1240][spec]") {
    const auto frozen = ordered_frames_scenario(
            "independent attachments converge separately and dispose idempotently without affecting each other");
    const auto* snapshot = frozen.inputs_member("snapshot");
    const auto* commits = frozen.inputs_member("commits");
    REQUIRE(snapshot != nullptr);
    REQUIRE(commits != nullptr);
    const auto* snapshot_object = snapshot->get_if<support::JsonValue::object_t>();
    const auto* commit_array = commits->get_if<support::JsonValue::array_t>();
    REQUIRE(snapshot_object != nullptr);
    REQUIRE(commit_array != nullptr);
    REQUIRE(commit_array->size() == 2);
    const auto* first_commit = commit_array->at(0).get_if<support::JsonValue::object_t>();
    const auto* second_commit = commit_array->at(1).get_if<support::JsonValue::object_t>();
    REQUIRE(first_commit != nullptr);
    REQUIRE(second_commit != nullptr);
    const auto initial = support::json_number_member(*snapshot_object, "value");
    const auto cursor = support::json_integer_member(*snapshot_object, "cursor");
    const auto value = support::json_number_member(*first_commit, "value");
    const auto sequence = support::json_integer_member(*first_commit, "cursor");
    const auto later_value = support::json_number_member(*second_commit, "value");
    const auto later_sequence = support::json_integer_member(*second_commit, "cursor");
    REQUIRE(initial.has_value());
    REQUIRE(cursor.has_value());
    REQUIRE(value.has_value());
    REQUIRE(sequence.has_value());
    REQUIRE(later_value.has_value());
    REQUIRE(later_sequence.has_value());

    // This shallow attached-state implementation couples disposal across its group.
    struct CoupledGroup {
        // The source outlives the group, and the states outlive every group disposal.
        TestSource& source;
        // Each attachment is owned by a state that outlives every group disposal.
        std::vector<ReplicatedStateSourceAttachment*> attachments;
        struct State {
            // The group outlives this state, and the attachment is disposed before destruction.
            CoupledGroup& group;
            std::unique_ptr<ReplicatedStateSourceAttachment> attachment;
            Json value;
            State(CoupledGroup& owner, std::unique_ptr<ReplicatedStateSourceAttachment> acquired)
                : group(owner), attachment(std::move(acquired)), value(attachment->snapshot().value) {
                // The state outlives the callback and disposes its source attachment.
                attachment->activate([this](const ReplicatedStateSourceFrame& frame) { value = frame.value; });
            }
            ~State() { attachment->dispose(); }
            void dispose() {
                for (auto* attachment_pointer : group.attachments)
                    attachment_pointer->dispose();
            }
        };
        std::unique_ptr<State> attach() {
            auto attachment = source.attach();
            REQUIRE(attachment);
            auto state = std::make_unique<State>(*this, std::move(*attachment));
            attachments.push_back(state->attachment.get());
            return state;
        }
    };
    TestSource shallow_source{revision(*initial), *cursor};
    CoupledGroup shallow{.source = shallow_source, .attachments = {}};
    auto shallow_first = shallow.attach();
    auto shallow_second = shallow.attach();
    const auto shallow_after_attach = shallow_source.attachment_count();
    shallow_source.commit_at(revision(*value), *sequence);
    shallow_first->dispose();
    shallow_first->dispose();
    const auto shallow_after_first = shallow_source.attachment_count();
    shallow_source.commit_at(revision(*later_value), *later_sequence);
    shallow_second->dispose();
    CHECK_FALSE(matches_frozen_disposal(frozen,
            FrozenDisposalObservation{.after_attach = shallow_after_attach,
                    .after_first = shallow_after_first,
                    .after_second = shallow_source.attachment_count(),
                    .first_value = shallow_first->value,
                    .second_value = shallow_second->value}));

    TestSource source{revision(*initial), *cursor};
    auto first = AttachedReplicatedState::attach(source);
    auto second = AttachedReplicatedState::attach(source);
    REQUIRE(first);
    REQUIRE(second);
    const auto after_attach = source.attachment_count();
    source.commit_at(revision(*value), *sequence);
    first->dispose();
    first->dispose();
    const auto after_first = source.attachment_count();
    source.commit_at(revision(*later_value), *later_sequence);
    second->dispose();
    CHECK(matches_frozen_disposal(frozen,
            FrozenDisposalObservation{.after_attach = after_attach,
                    .after_first = after_first,
                    .after_second = source.attachment_count(),
                    .first_value = first->value(),
                    .second_value = second->value()}));
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1057/replicated-state.json
// "attached replicated state hydrates at cursor zero and then delivers exact frames in sequence
// order". The frozen snapshot and update drive the source; the frozen deliveries (kind, sequence,
// value, absent abort signal), the empty error list and the one disposal are compared exactly, so
// a view that hydrates only, reorders the two deliveries, or never disposes fails.
TEST_CASE("attached replicated state hydrates at cursor zero and then delivers exact frames in sequence order",
        "[chord][replicated_state][issue1062][spec]") {
    const auto frozen = replicated_state_scenario(
            "attached replicated state hydrates at cursor zero and then delivers exact frames in sequence order");
    const auto* snapshot = frozen.inputs_member("snapshot");
    const auto* update = frozen.inputs_member("update");
    REQUIRE(snapshot != nullptr);
    REQUIRE(update != nullptr);
    const auto snapshot_cursor =
            support::json_integer_member(*snapshot->get_if<support::JsonValue::object_t>(), "cursor");
    const auto snapshot_value = support::json_number_member(*snapshot->get_if<support::JsonValue::object_t>(), "value");
    const auto update_value = support::json_number_member(*update->get_if<support::JsonValue::object_t>(), "value");
    const auto update_sequence =
            support::json_integer_member(*update->get_if<support::JsonValue::object_t>(), "sequence");
    REQUIRE(snapshot_cursor.has_value());
    REQUIRE(snapshot_value.has_value());
    REQUIRE(update_value.has_value());
    REQUIRE(update_sequence.has_value());

    TestSource source{revision(*snapshot_value), *snapshot_cursor};
    std::vector<std::string> errors;
    auto attached = AttachedReplicatedState::attach(source,
            ReplicatedStateSourceOptions{
                    .onError = [&errors](const support::Error& error) { errors.push_back(error.message); }});
    REQUIRE(attached.has_value());

    std::vector<DeliveryEvent> observed;
    auto unsubscribe = attached->subscribe([&observed](Json value, Context context, ReplicatedStateDelivery delivery) {
        observed.push_back(DeliveryEvent{.kind = delivery.kind,
                .sequence = delivery.sequence,
                .value = number_of(value),
                .signal_present = context.stop_token().stop_possible()});
        return ready();
    });
    source.commit_at(revision(*update_value), *update_sequence);

    const auto* frozen_deliveries = frozen.array_member("deliveries");
    REQUIRE(frozen_deliveries != nullptr);
    REQUIRE(observed.size() == frozen_deliveries->size());
    for (std::size_t index = 0; index < frozen_deliveries->size(); ++index) {
        const auto* record = frozen_deliveries->at(index).get_if<support::JsonValue::object_t>();
        REQUIRE(record != nullptr);
        const auto kind = support::json_string_member(*record, "kind");
        const auto sequence = support::json_integer_member(*record, "sequence");
        const auto* frozen_signal = support::json_member(*record, "abortSignal");
        REQUIRE(kind.has_value());
        REQUIRE(sequence.has_value());
        REQUIRE(frozen_signal != nullptr);
        CHECK(observed[index].kind ==
                (*kind == "hydrate" ? ReplicatedStateDeliveryKind::Hydrate : ReplicatedStateDeliveryKind::Update));
        CHECK(observed[index].sequence == *sequence);
        CHECK(observed[index].value == frozen_delivery_number((*record).at("value")));
        // Frozen `abortSignal: null`: the delivery context carries no abort signal.
        REQUIRE(frozen_signal->holds<std::nullptr_t>());
        CHECK_FALSE(observed[index].signal_present);
    }
    const auto* frozen_errors = frozen.array_member("errors");
    REQUIRE(frozen_errors != nullptr);
    check_frozen_error_messages(errors, *frozen_errors);

    unsubscribe();
    attached->dispose();
    const auto dispose_calls = frozen.number_member("disposeCalls");
    REQUIRE(dispose_calls.has_value());
    CHECK(source.dispose_calls() == static_cast<int>(*dispose_calls));
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1057/replicated-state.json
// "a cursor gap is reported once and leaves the last good replicated value". The frozen snapshot
// cursor, skipped sequence and later sequence drive the source; the frozen error message, the
// delivered sequences, the last value and the disposal count are compared exactly. Negative
// control: an implementation that reordered or skipped the cursor check would deliver the gap
// frame and converge on 9 instead of retaining the frozen last value.
TEST_CASE("a cursor gap is reported once and leaves the last good replicated value",
        "[chord][replicated_state][issue1062][spec]") {
    const auto frozen =
            replicated_state_scenario("a cursor gap is reported once and leaves the last good replicated value");
    const auto snapshot_cursor = frozen.inputs_number("snapshotCursor");
    const auto skipped_sequence = frozen.inputs_number("skippedSequence");
    const auto later_sequence = frozen.inputs_number("laterSequence");
    REQUIRE(snapshot_cursor.has_value());
    REQUIRE(skipped_sequence.has_value());
    REQUIRE(later_sequence.has_value());

    TestSource source{revision(1), static_cast<std::int64_t>(*snapshot_cursor)};
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

    source.commit_at(revision(9), static_cast<std::int64_t>(*skipped_sequence));
    const auto* frozen_errors = frozen.array_member("errors");
    REQUIRE(frozen_errors != nullptr);
    check_frozen_error_messages(errors, *frozen_errors);
    auto last_value = Json::from_support(*frozen.member("lastValue"));
    REQUIRE(last_value.has_value());
    CHECK(number_of(attached->value()) == number_of(*last_value));
    const auto dispose_calls = frozen.number_member("disposeCalls");
    REQUIRE(dispose_calls.has_value());
    CHECK(source.dispose_calls() == static_cast<int>(*dispose_calls));

    source.commit_at(revision(10), static_cast<std::int64_t>(*later_sequence));
    const auto* frozen_sequences = frozen.array_member("deliveredSequences");
    REQUIRE(frozen_sequences != nullptr);
    std::vector<double> delivered_sequences;
    for (const auto& delivery : deliveries)
        delivered_sequences.push_back(static_cast<double>(delivery.sequence));
    check_frozen_numbers(delivered_sequences, *frozen_sequences);
    CHECK(errors.size() == frozen_errors->size());
    CHECK(number_of(attached->value()) == number_of(*last_value));
    unsubscribe();
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1062/ordered-frames.json
// "a repeated cursor is reported once as a gap and keeps the last good value". The frozen commits
// drive the source; the frozen error message, delivered sequences, last good value and the
// attachment disposition after the gap are compared exactly. Negative control for out-of-order
// delivery: the backward cursor is rejected by the same cursor check that rejects a skipped one.
TEST_CASE("a repeated cursor is reported once as a gap and keeps the last good value",
        "[chord][replicated_state][issue1062][spec]") {
    const auto frozen =
            ordered_frames_scenario("a repeated cursor is reported once as a gap and keeps the last good value");
    const auto* commits = frozen.inputs_member("commits");
    REQUIRE(commits != nullptr);
    const auto* commit_records = commits->get_if<support::JsonValue::array_t>();
    REQUIRE(commit_records != nullptr);
    REQUIRE(commit_records->size() == 3);
    std::vector<std::int64_t> commit_cursors;
    std::vector<double> commit_values;
    for (const auto& record : *commit_records) {
        const auto* object = record.get_if<support::JsonValue::object_t>();
        REQUIRE(object != nullptr);
        const auto cursor = support::json_integer_member(*object, "cursor");
        const auto value = support::json_number_member(*object, "value");
        REQUIRE(cursor.has_value());
        REQUIRE(value.has_value());
        commit_cursors.push_back(*cursor);
        commit_values.push_back(*value);
    }

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
    source.commit_at(revision(commit_values[0]), commit_cursors[0]);
    source.commit_at(revision(commit_values[1]), commit_cursors[1]);

    const auto* frozen_errors = frozen.array_member("reportedErrors");
    REQUIRE(frozen_errors != nullptr);
    check_frozen_error_messages(errors, *frozen_errors);
    const auto* frozen_sequences = frozen.array_member("deliveredSequences");
    REQUIRE(frozen_sequences != nullptr);
    std::vector<double> delivered_sequences;
    for (const auto& delivery : deliveries)
        delivered_sequences.push_back(static_cast<double>(delivery.sequence));
    check_frozen_numbers(delivered_sequences, *frozen_sequences);
    auto last_good = Json::from_support(*frozen.member("lastGoodValue"));
    REQUIRE(last_good.has_value());
    CHECK(number_of(attached->value()) == number_of(*last_good));
    const auto attachments_after_gap = frozen.number_member("attachmentCountAfterGap");
    REQUIRE(attachments_after_gap.has_value());
    CHECK(source.attachment_count() == static_cast<std::size_t>(*attachments_after_gap));

    source.commit_at(revision(commit_values[2]), commit_cursors[2]);
    CHECK(errors.size() == frozen_errors->size());
    auto last_good_after = Json::from_support(*frozen.member("lastGoodValueAfterLaterFrame"));
    REQUIRE(last_good_after.has_value());
    CHECK(number_of(attached->value()) == number_of(*last_good_after));
    unsubscribe();
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1062/ordered-frames.json
// "a repeated cursor is reported once as a gap and keeps the last good value": a strictly backward
// cursor after a good frame must fail with the same frozen gap message and retain the frozen last
// good value.
TEST_CASE("a backward cursor reports one ordering gap and retains the last good value",
        "[chord][replicated_state][issue1062][spec]") {
    const auto frozen =
            ordered_frames_scenario("a repeated cursor is reported once as a gap and keeps the last good value");
    const auto* frozen_errors = frozen.array_member("reportedErrors");
    auto last_good = Json::from_support(*frozen.member("lastGoodValue"));
    auto last_good_after = Json::from_support(*frozen.member("lastGoodValueAfterLaterFrame"));
    REQUIRE(frozen_errors != nullptr);
    REQUIRE(last_good.has_value());
    REQUIRE(last_good_after.has_value());

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
    source.commit_at(revision(3), 0);
    source.commit_at(revision(4), 2);

    // A strictly backward cursor fails with the same frozen gap message shape as the repeated
    // cursor, naming the received cursor.
    CHECK((errors == std::vector<std::string>{"Replicated state source cursor has a gap: expected 2, received 0"}));
    const auto* frozen_sequences = frozen.array_member("deliveredSequences");
    REQUIRE(frozen_sequences != nullptr);
    std::vector<double> delivered_sequences;
    for (const auto& delivery : deliveries)
        delivered_sequences.push_back(static_cast<double>(delivery.sequence));
    check_frozen_numbers(delivered_sequences, *frozen_sequences);
    CHECK(number_of(attached->value()) == number_of(*last_good_after));
    unsubscribe();
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1062/observer-delivery.json
// "at most 100 deliveries pend behind one held listener and overflow keeps the newest (100
// commits)". The frozen commit count drives the source; the frozen received sequence is compared
// exactly.
TEST_CASE("at most 100 deliveries pend behind one held listener while a commit fits the bound",
        "[chord][replicated_state][issue1062][spec]") {
    const auto frozen = observer_delivery_scenario(
            "at most 100 deliveries pend behind one held listener and overflow keeps the newest (100 commits)");
    const auto commits = frozen.inputs_number("commits");
    REQUIRE(commits.has_value());

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
    for (int value = 1; value <= static_cast<int>(*commits); value += 1)
        source.commit(revision(value));

    const auto* frozen_received = frozen.array_member("received");
    REQUIRE(frozen_received != nullptr);
    check_frozen_numbers(std::vector<double>{received[0]}, std::vector<support::JsonValue>{frozen_received->at(0)});
    settle(hydration);
    check_frozen_numbers(received, *frozen_received);
    unsubscribe();
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1062/observer-delivery.json
// "at most 100 deliveries pend behind one held listener and overflow keeps the newest (102
// commits)". Negative control: an unbounded queue would also deliver 1 and 100; the frozen
// coalesced-sequence assertion distinguishes the 100-pending bound.
TEST_CASE("overflow at the 100-pending bound keeps the newest pending delivery",
        "[chord][replicated_state][issue1062][spec]") {
    const auto frozen = observer_delivery_scenario(
            "at most 100 deliveries pend behind one held listener and overflow keeps the newest (102 commits)");
    const auto commits = frozen.inputs_number("commits");
    REQUIRE(commits.has_value());

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
    for (int value = 1; value <= static_cast<int>(*commits); value += 1)
        source.commit(revision(value));

    const auto* frozen_received = frozen.array_member("received");
    REQUIRE(frozen_received != nullptr);
    check_frozen_numbers(std::vector<double>{received[0]}, std::vector<support::JsonValue>{frozen_received->at(0)});
    settle(hydration);
    check_frozen_numbers(received, *frozen_received);
    unsubscribe();
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1062/observer-delivery.json
// "at most 100 deliveries pend behind one held listener and overflow keeps the newest (202
// commits)". The frozen received sequence distinguishes repeated overflow from a single one.
TEST_CASE("repeated overflow at the 100-pending bound keeps the newest pending delivery",
        "[chord][replicated_state][issue1062][spec]") {
    const auto frozen = observer_delivery_scenario(
            "at most 100 deliveries pend behind one held listener and overflow keeps the newest (202 commits)");
    const auto commits = frozen.inputs_number("commits");
    REQUIRE(commits.has_value());

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
    for (int value = 1; value <= static_cast<int>(*commits); value += 1)
        source.commit(revision(value));

    const auto* frozen_received = frozen.array_member("received");
    REQUIRE(frozen_received != nullptr);
    check_frozen_numbers(std::vector<double>{received[0]}, std::vector<support::JsonValue>{frozen_received->at(0)});
    settle(hydration);
    check_frozen_numbers(received, *frozen_received);
    unsubscribe();
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1062/observer-delivery.json
// "overflow keeps only the newest pending delivery with its exact value context and delivery". The
// frozen newest pending frame carries the exact context published with frame 102 — a `withCancel`
// layer over the background root, so its abort signal is present and un-aborted — and the frozen
// adopted revision at the overflow point is frame 102, before the trailing commit. The frozen
// receivedValues and newestDelivery records are compared exactly.
TEST_CASE("overflow keeps only the newest pending delivery with its exact value context and delivery",
        "[chord][replicated_state][issue1062][spec]") {
    const auto frozen = observer_delivery_scenario(
            "overflow keeps only the newest pending delivery with its exact value context and delivery");
    const auto held_value = frozen.inputs_number("heldValue");
    REQUIRE(held_value.has_value());
    const auto* newest_pending = frozen.inputs_member("newestPending");
    REQUIRE(newest_pending != nullptr);
    const auto* newest_object = newest_pending->get_if<support::JsonValue::object_t>();
    REQUIRE(newest_object != nullptr);
    const auto newest_cursor = support::json_integer_member(*newest_object, "cursor");
    const auto newest_value = support::json_number_member(*newest_object, "value");
    REQUIRE(newest_cursor.has_value());
    REQUIRE(newest_value.has_value());
    const auto* trailing = frozen.inputs_member("trailingCommit");
    REQUIRE(trailing != nullptr);
    const auto trailing_cursor =
            support::json_integer_member(*trailing->get_if<support::JsonValue::object_t>(), "cursor");
    const auto trailing_value = support::json_number_member(*trailing->get_if<support::JsonValue::object_t>(), "value");
    REQUIRE(trailing_cursor.has_value());
    REQUIRE(trailing_value.has_value());

    TestSource source{revision(0)};
    auto attached = AttachedReplicatedState::attach(source);
    REQUIRE(attached.has_value());

    const auto marker_scope = with_cancel(background_context());
    const Context marked = marker_scope.context();
    std::optional<Completion> held;
    std::vector<DeliveryEvent> observed;
    auto unsubscribe = attached->subscribe([&](Json value, Context context, ReplicatedStateDelivery delivery) {
        observed.push_back(DeliveryEvent{.kind = delivery.kind,
                .sequence = delivery.sequence,
                .value = number_of(value),
                .signal_present = context.stop_token().stop_possible(),
                .signal_aborted = context.aborted()});
        if (number_of(value) == *held_value) return hold(held);
        return ready();
    });
    for (int value = 1; value < static_cast<int>(*newest_value); value += 1)
        source.commit(revision(value));
    source.commit_at(revision(*newest_value), *newest_cursor, marked);
    // Frozen adoptedValueAtOverflow: the newest published revision is frame 102 here.
    auto adopted_at_overflow = Json::from_support(*frozen.member("adoptedValueAtOverflow"));
    REQUIRE(adopted_at_overflow.has_value());
    CHECK(number_of(attached->value()) == number_of(*adopted_at_overflow));
    source.commit_at(revision(*trailing_value), *trailing_cursor);

    const auto* frozen_received = frozen.array_member("receivedValues");
    REQUIRE(frozen_received != nullptr);
    std::vector<double> received_values;
    for (const auto& event : observed)
        received_values.push_back(event.value);
    check_frozen_numbers(std::vector<double>{received_values[0], received_values[1]},
            std::vector<support::JsonValue>{frozen_received->at(0), frozen_received->at(1)});
    settle(held);
    received_values.clear();
    for (const auto& event : observed)
        received_values.push_back(event.value);
    check_frozen_numbers(received_values, *frozen_received);

    const auto* frozen_newest = frozen.object_member("newestDelivery");
    REQUIRE(frozen_newest != nullptr);
    REQUIRE(observed.size() == frozen_received->size());
    const auto& newest = observed[2];
    CHECK(newest.kind == ReplicatedStateDeliveryKind::Update);
    CHECK(newest.sequence == *newest_cursor);
    CHECK(newest.value == *newest_value);
    const auto frozen_signal_present = support::json_bool_member(*frozen_newest, "signalPresent");
    const auto frozen_signal_aborted = support::json_bool_member(*frozen_newest, "signalAborted");
    REQUIRE(frozen_signal_present.has_value());
    REQUIRE(frozen_signal_aborted.has_value());
    CHECK(newest.signal_present == *frozen_signal_present);
    CHECK(newest.signal_aborted == *frozen_signal_aborted);
    const auto* frozen_delivery = support::json_object_member(*frozen_newest, "delivery");
    REQUIRE(frozen_delivery != nullptr);
    const auto frozen_kind = support::json_string_member(*frozen_delivery, "kind");
    const auto frozen_sequence = support::json_integer_member(*frozen_delivery, "sequence");
    REQUIRE(frozen_kind.has_value());
    REQUIRE(frozen_sequence.has_value());
    CHECK(newest.kind ==
            (*frozen_kind == "update" ? ReplicatedStateDeliveryKind::Update : ReplicatedStateDeliveryKind::Hydrate));
    CHECK(newest.sequence == *frozen_sequence);
    unsubscribe();
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1062/observer-delivery.json
// "at most 100 deliveries pend behind one held listener and overflow keeps the newest (102
// commits)": the frozen received sequence is the held observer's contract. The fast observer's
// full publication-order sequence is this harness's witness that the held subscription alone
// coalesces.
TEST_CASE("pending overflow keeps the newest frame while fast observers follow every publication",
        "[chord][replicated_state][issue1062][spec]") {
    const auto frozen = observer_delivery_scenario(
            "at most 100 deliveries pend behind one held listener and overflow keeps the newest (102 commits)");
    const auto commits = frozen.inputs_number("commits");
    REQUIRE(commits.has_value());
    const auto* frozen_received = frozen.array_member("received");
    REQUIRE(frozen_received != nullptr);

    TestSource source{revision(0)};
    auto attached = AttachedReplicatedState::attach(source);
    REQUIRE(attached.has_value());

    std::optional<Completion> held_hydration;
    std::vector<double> held_values;
    std::vector<DeliveryEvent> fast_deliveries;
    auto held_unsubscribe = attached->subscribe([&](Json value, Context, ReplicatedStateDelivery) {
        held_values.push_back(number_of(value));
        if (number_of(value) == 0) return hold(held_hydration);
        return ready();
    });
    auto fast_unsubscribe =
            attached->subscribe([&fast_deliveries](Json value, Context, ReplicatedStateDelivery delivery) {
                record_delivery(fast_deliveries, delivery.kind, delivery.sequence, number_of(value));
                return ready();
            });

    for (int value = 1; value <= static_cast<int>(*commits); value += 1)
        source.commit(revision(value));

    // While the held listener is pending, the frozen contract holds: only the hydration arrived.
    check_frozen_numbers(held_values, std::vector<support::JsonValue>{frozen_received->at(0)});
    REQUIRE(fast_deliveries.size() == static_cast<std::size_t>(*commits) + 1);
    for (std::size_t index = 0; index < fast_deliveries.size(); index += 1) {
        CHECK(fast_deliveries[index].sequence == static_cast<std::int64_t>(index));
        CHECK(fast_deliveries[index].value == static_cast<double>(index));
        CHECK(fast_deliveries[index].kind ==
                (index == 0 ? ReplicatedStateDeliveryKind::Hydrate : ReplicatedStateDeliveryKind::Update));
    }
    CHECK(number_of(attached->value()) == *commits);

    settle(held_hydration);
    // The held observer converges to the frozen received sequence: the hydration plus the two
    // newest pending frames only.
    check_frozen_numbers(held_values, *frozen_received);
    CHECK(number_of(attached->value()) == *commits);

    held_unsubscribe();
    fast_unsubscribe();
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1057/replicated-state.json
// "a failing state listener is reported through onError without stopping another subscription".
// The frozen hydration and update values drive the source; the frozen two `listener failed`
// reports and the other subscription's hydrate/update pair are compared exactly, so a state that
// drops a failing subscription or that rethrows into the source fails this case.
TEST_CASE("a failing state listener is reported through onError without stopping another subscription",
        "[chord][replicated_state][issue1062][spec]") {
    const auto frozen = replicated_state_scenario(
            "a failing state listener is reported through onError without stopping another subscription");
    const auto hydration_value = frozen.inputs_number("hydrationValue");
    const auto update_value = frozen.inputs_number("updateValue");
    REQUIRE(hydration_value.has_value());
    REQUIRE(update_value.has_value());

    TestSource source{revision(*hydration_value), 0};
    std::vector<std::string> errors;
    auto attached = AttachedReplicatedState::attach(source,
            ReplicatedStateSourceOptions{
                    .onError = [&errors](const support::Error& error) { errors.push_back(error.message); }});
    REQUIRE(attached.has_value());

    std::vector<double> failing_received;
    auto failing_unsubscribe = attached->subscribe([&failing_received](Json value, Context, ReplicatedStateDelivery) {
        failing_received.push_back(number_of(value));
        return ready_failure("listener failed");
    });
    std::vector<DeliveryEvent> other_deliveries;
    auto other_unsubscribe =
            attached->subscribe([&other_deliveries](Json value, Context, ReplicatedStateDelivery delivery) {
                record_delivery(other_deliveries, delivery.kind, delivery.sequence, number_of(value));
                return ready();
            });
    source.commit_at(revision(*update_value), 1);

    const auto* frozen_errors = frozen.array_member("errors");
    REQUIRE(frozen_errors != nullptr);
    check_frozen_error_messages(errors, *frozen_errors);
    const auto* frozen_received = frozen.array_member("received");
    REQUIRE(frozen_received != nullptr);
    std::vector<double> frozen_values;
    for (const auto& record : *frozen_received) {
        const auto* object = record.get_if<support::JsonValue::object_t>();
        REQUIRE(object != nullptr);
        const auto* value = support::json_member(*object, "value");
        REQUIRE(value != nullptr);
        frozen_values.push_back(frozen_delivery_number(*value));
    }
    CHECK(failing_received == frozen_values_as_numbers(*frozen_received));
    check_frozen_deliveries(other_deliveries, *frozen_received);
    failing_unsubscribe();
    other_unsubscribe();
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1062/observer-delivery.json
// "a failing listener is reported through onError in isolation and keeps receiving later
// deliveries". The frozen per-delivery failure modes and commit chain drive the test; the frozen
// received sequences and the three reported errors are compared exactly.
TEST_CASE("a failing listener is reported in isolation and keeps receiving later deliveries",
        "[chord][replicated_state][issue1062][spec]") {
    const auto frozen = observer_delivery_scenario(
            "a failing listener is reported through onError in isolation and keeps receiving later deliveries");
    const auto* listener_failures = frozen.inputs_member("listenerFailures");
    const auto* commits = frozen.inputs_member("commits");
    REQUIRE(listener_failures != nullptr);
    REQUIRE(commits != nullptr);
    const auto* failure_object = listener_failures->get_if<support::JsonValue::object_t>();
    const auto* commit_records = commits->get_if<support::JsonValue::array_t>();
    REQUIRE(failure_object != nullptr);
    REQUIRE(commit_records != nullptr);
    const auto hydration_failure = support::json_string_member(*failure_object, "0");
    const auto update_failure = support::json_string_member(*failure_object, "1");
    const auto late_failure = support::json_string_member(*failure_object, "2");
    REQUIRE(hydration_failure.has_value());
    REQUIRE(update_failure.has_value());
    REQUIRE(late_failure.has_value());
    REQUIRE(hydration_failure->starts_with("reject:"));
    REQUIRE(update_failure->starts_with("throw:"));
    REQUIRE(late_failure->starts_with("reject:"));

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
        if (current == 1) return ready_failure(std::string{update_failure->substr(6)});
        if (current == 2) return hold(update);
        return ready();
    });
    auto fast_unsubscribe = attached->subscribe([&fast_received](Json value, Context, ReplicatedStateDelivery) {
        fast_received.push_back(number_of(value));
        return ready();
    });
    for (std::size_t index = 0; index < commit_records->size(); ++index) {
        const auto* record = commit_records->at(index).get_if<support::JsonValue::object_t>();
        REQUIRE(record != nullptr);
        const auto cursor = support::json_integer_member(*record, "cursor");
        const auto value = support::json_number_member(*record, "value");
        REQUIRE(cursor.has_value());
        REQUIRE(value.has_value());
        source.commit_at(revision(*value), *cursor);
    }
    // The failing subscription is still held on its hydration: the other
    // subscriber has already converged in publication order.
    const auto* frozen_other = frozen.array_member("otherSubscriberReceived");
    REQUIRE(frozen_other != nullptr);
    check_frozen_numbers(fast_received, *frozen_other);
    settle_failure(hydration, std::string{hydration_failure->substr(7)});
    check_frozen_numbers(std::vector<double>{failing_received[0], failing_received[1], failing_received[2]},
            std::vector<support::JsonValue>{frozen_other->at(0), frozen_other->at(1), frozen_other->at(2)});
    settle_failure(update, std::string{late_failure->substr(7)});

    const auto* frozen_failing = frozen.array_member("failingListenerReceived");
    REQUIRE(frozen_failing != nullptr);
    check_frozen_numbers(failing_received, *frozen_failing);
    const auto* frozen_errors = frozen.array_member("reportedErrors");
    REQUIRE(frozen_errors != nullptr);
    check_frozen_error_messages(errors, *frozen_errors);
    check_frozen_numbers(fast_received, *frozen_other);
    failing_unsubscribe();
    fast_unsubscribe();
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1062/observer-delivery.json
// "a synchronous hydration failure is reported without removing the subscription". The frozen
// hydration failure drives the listener; the frozen received sequence and the single reported
// error are compared exactly.
TEST_CASE("a synchronous hydration failure is reported without removing the subscription",
        "[chord][replicated_state][issue1062][spec]") {
    const auto frozen =
            observer_delivery_scenario("a synchronous hydration failure is reported without removing the subscription");
    const auto hydration_failure = frozen.inputs_string("hydrationFailure");
    REQUIRE(hydration_failure.has_value());
    REQUIRE(hydration_failure->starts_with("throw:"));
    const auto failure_message = hydration_failure->substr(6);

    TestSource source{revision(0)};
    std::vector<std::string> errors;
    auto attached = AttachedReplicatedState::attach(source,
            ReplicatedStateSourceOptions{
                    .onError = [&errors](const support::Error& error) { errors.push_back(error.message); }});
    REQUIRE(attached.has_value());

    std::vector<double> received;
    auto unsubscribe = attached->subscribe([&received, &failure_message](Json value, Context, ReplicatedStateDelivery) {
        received.push_back(number_of(value));
        if (number_of(value) == 0) return ready_failure(failure_message);
        return ready();
    });
    source.commit(revision(1));

    const auto* frozen_errors = frozen.array_member("reportedErrors");
    REQUIRE(frozen_errors != nullptr);
    check_frozen_error_messages(errors, *frozen_errors);
    const auto* frozen_received = frozen.array_member("received");
    REQUIRE(frozen_received != nullptr);
    check_frozen_numbers(received, *frozen_received);
    unsubscribe();
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1062/observer-delivery.json
// "unsubscribe discards queued deliveries without aborting or joining the running callback". The
// frozen held value, the queued frame and the post-unsubscribe commit drive the source; the
// frozen received sequence, the completed-after-release flag and the un-aborted context are
// compared exactly. The C++ mapping settles the pending listener through its AsyncResult
// completion; the callback retains no post-await body to join.
TEST_CASE("unsubscribe discards queued deliveries without aborting the running callback",
        "[chord][replicated_state][issue1062][spec]") {
    const auto frozen = observer_delivery_scenario(
            "unsubscribe discards queued deliveries without aborting or joining the running callback");
    const auto held_value = frozen.inputs_number("heldValue");
    REQUIRE(held_value.has_value());
    const auto* queued = frozen.inputs_member("queuedDuringHold");
    const auto* after = frozen.inputs_member("commitsAfterUnsubscribe");
    REQUIRE(queued != nullptr);
    REQUIRE(after != nullptr);
    const auto* queued_records = queued->get_if<support::JsonValue::array_t>();
    const auto* after_records = after->get_if<support::JsonValue::array_t>();
    REQUIRE(queued_records != nullptr);
    REQUIRE(after_records != nullptr);
    REQUIRE(queued_records->size() == 1);
    REQUIRE(after_records->size() == 1);
    const auto queued_cursor =
            support::json_integer_member(*queued_records->at(0).get_if<support::JsonValue::object_t>(), "cursor");
    const auto queued_value =
            support::json_number_member(*queued_records->at(0).get_if<support::JsonValue::object_t>(), "value");
    const auto after_cursor =
            support::json_integer_member(*after_records->at(0).get_if<support::JsonValue::object_t>(), "cursor");
    const auto after_value =
            support::json_number_member(*after_records->at(0).get_if<support::JsonValue::object_t>(), "value");
    REQUIRE(queued_cursor.has_value());
    REQUIRE(queued_value.has_value());
    REQUIRE(after_cursor.has_value());
    REQUIRE(after_value.has_value());

    TestSource source{revision(0)};
    auto attached = AttachedReplicatedState::attach(source);
    REQUIRE(attached.has_value());

    std::stop_source cancellation;
    const Context cancellable = with_abort_signal(cancellation.get_token(), "1062", background_context());
    std::optional<Completion> held;
    std::vector<double> received;
    auto unsubscribe = attached->subscribe([&](Json value, Context, ReplicatedStateDelivery) {
        received.push_back(number_of(value));
        if (number_of(value) == *held_value) return hold(held);
        return ready();
    });
    source.commit_at(revision(*held_value), *held_value, cancellable);
    source.commit_at(revision(*queued_value), *queued_cursor, cancellable);
    unsubscribe();
    source.commit_at(revision(*after_value), *after_cursor, cancellable);

    const auto frozen_aborted = frozen.boolean_member("aborted");
    const auto completed_after_release = frozen.boolean_member("completedAfterRelease");
    REQUIRE(frozen_aborted.has_value());
    REQUIRE(completed_after_release.has_value());
    CHECK(cancellable.aborted() == *frozen_aborted);
    const auto* frozen_received = frozen.array_member("received");
    REQUIRE(frozen_received != nullptr);
    check_frozen_numbers(received, *frozen_received);
    settle(held);
    check_frozen_numbers(received, *frozen_received);
    CHECK(completed_after_release);
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1062/observer-delivery.json
// "a callback rejection that settles after unsubscribe is still reported". The frozen late
// rejection message drives the settlement; the frozen received sequence and the single reported
// error are compared exactly.
TEST_CASE("a callback rejection that settles after unsubscribe is still reported",
        "[chord][replicated_state][issue1062][spec]") {
    const auto frozen =
            observer_delivery_scenario("a callback rejection that settles after unsubscribe is still reported");
    const auto late_rejection = frozen.inputs_string("lateRejection");
    REQUIRE(late_rejection.has_value());

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
    settle_failure(hydration, *late_rejection);

    const auto* frozen_errors = frozen.array_member("reportedErrors");
    REQUIRE(frozen_errors != nullptr);
    check_frozen_error_messages(errors, *frozen_errors);
    const auto* frozen_received = frozen.array_member("received");
    REQUIRE(frozen_received != nullptr);
    check_frozen_numbers(received, *frozen_received);
    CHECK(matches_frozen_late_rejection(frozen, errors, received));
}

TEST_CASE("frozen late rejection rejects suppressing errors after unsubscribe",
        "[chord][replicated_state][issue1240][spec]") {
    const auto frozen =
            observer_delivery_scenario("a callback rejection that settles after unsubscribe is still reported");
    const auto* snapshot = frozen.inputs_member("snapshot");
    const auto rejection = frozen.inputs_string("lateRejection");
    REQUIRE(snapshot != nullptr);
    REQUIRE(rejection.has_value());
    const auto* snapshot_object = snapshot->get_if<support::JsonValue::object_t>();
    REQUIRE(snapshot_object != nullptr);
    const auto value = support::json_number_member(*snapshot_object, "value");
    const auto cursor = support::json_integer_member(*snapshot_object, "cursor");
    REQUIRE(value.has_value());
    REQUIRE(cursor.has_value());

    // The faulty dispatcher drops errors from pending listeners after unsubscribe.
    struct ShallowDispatcher {
        bool subscribed{false};
        bool pending{false};
        std::move_only_function<void(const support::Error&)> on_error;
        std::move_only_function<support::AsyncResult<void>(Json)> listener;
        std::optional<Json> queued;
        void subscribe(Json initial, std::move_only_function<support::AsyncResult<void>(Json)> callback) {
            subscribed = true;
            listener = std::move(callback);
            pending = true;
            auto result = listener(std::move(initial));
            // The test settles this completion before destroying the dispatcher.
            std::move(result).start([this](std::expected<void, support::Error> outcome) noexcept {
                pending = false;
                if (!outcome && subscribed) on_error(outcome.error());
            });
        }
        void commit(Json next) {
            if (subscribed && pending) queued = std::move(next);
        }
        void unsubscribe() {
            subscribed = false;
            queued.reset();
        }
    };
    std::vector<std::string> shallow_errors;
    std::vector<double> shallow_received;
    std::optional<Completion> shallow_hydration;
    // The recorded vectors and completion outlive the dispatcher and its callbacks.
    ShallowDispatcher shallow{
            .on_error = [&shallow_errors](const support::Error& error) { shallow_errors.push_back(error.message); },
            .listener = {},
            .queued = {}};
    shallow.subscribe(revision(*value), [&shallow_received, &shallow_hydration](Json current) {
        shallow_received.push_back(number_of(current));
        return hold(shallow_hydration);
    });
    shallow.commit(revision(1));
    shallow.unsubscribe();
    REQUIRE(shallow_hydration.has_value());
    settle_failure(shallow_hydration, *rejection);
    const auto* expected_received = frozen.array_member("received");
    REQUIRE(expected_received != nullptr);
    check_frozen_numbers(shallow_received, *expected_received);
    CHECK(shallow_errors.empty());
    CHECK_FALSE(matches_frozen_late_rejection(frozen, shallow_errors, shallow_received));

    TestSource source{revision(*value), *cursor};
    std::vector<std::string> errors;
    std::vector<double> received;
    // Both vectors outlive the attached state; the pending listener is settled below.
    auto attached = AttachedReplicatedState::attach(source,
            ReplicatedStateSourceOptions{
                    .onError = [&errors](const support::Error& error) { errors.push_back(error.message); }});
    REQUIRE(attached);
    std::optional<Completion> hydration;
    auto unsubscribe = attached->subscribe([&received, &hydration](Json current, Context, ReplicatedStateDelivery) {
        received.push_back(number_of(current));
        return hold(hydration);
    });
    source.commit(revision(1));
    unsubscribe();
    settle_failure(hydration, *rejection);
    CHECK(matches_frozen_late_rejection(frozen, errors, received));
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1057/replicated-state.json
// "a watch acquired before start buffers frames and serializes callbacks".
//
// Seam mapping: the frozen durable watch installs its listener at `start`, so its
// delivered sequence begins at the first frame committed after the acquisition
// revision; Chord installs the listener at `subscribe`, so the acquisition
// revision itself arrives as the cold hydration and its update sequence is the
// frozen frame sequence. Every frozen expectation is compared exactly:
// `valuesBeforeRelease` [1], `valuesAfterRelease` [1,2],
// `maxConcurrentCallbacks` 1 and `finalValue` {value: 2}.
//
// Negative controls: a listener invoked reentrantly enters the second delivery
// while the first is in flight (the delivered order and the concurrency counter
// both separate it), and a state that drops the frame committed while a
// callback is running never reaches [1,2].
TEST_CASE("a watch acquired before start buffers frames and serializes callbacks",
        "[chord][replicated_state][issue1188][spec]") {
    const auto frozen =
            replicated_state_scenario("a watch acquired before start buffers frames and serializes callbacks");
    const auto* commits_before = frozen.array_member("valuesBeforeRelease");
    const auto* commits_after = frozen.array_member("valuesAfterRelease");
    const auto max_concurrent = frozen.number_member("maxConcurrentCallbacks");
    REQUIRE(commits_before != nullptr);
    REQUIRE(commits_after != nullptr);
    REQUIRE(max_concurrent.has_value());

    TestSource source{revision(0)};
    auto attached = AttachedReplicatedState::attach(source);
    REQUIRE(attached.has_value());

    std::vector<DeliveryEvent> observed;
    int active = 0;
    int max_active = 0;
    std::optional<Completion> in_flight;
    const auto update_values = [&observed] {
        std::vector<double> values;
        for (const DeliveryEvent& event : observed)
            if (event.kind == ReplicatedStateDeliveryKind::Update) values.push_back(event.value);
        return values;
    };
    auto unsubscribe = attached->subscribe([&observed, &active, &max_active, &in_flight](
                                                   Json value, Context context, ReplicatedStateDelivery delivery) {
        observed.push_back(DeliveryEvent{.kind = delivery.kind,
                .sequence = delivery.sequence,
                .value = number_of(value),
                .signal_present = context.stop_token().stop_possible()});
        if (delivery.kind == ReplicatedStateDeliveryKind::Hydrate) return ready();
        ++active;
        max_active = std::max(max_active, active);
        if (delivery.sequence != 1) {
            --active;
            return ready();
        }
        // The first committed frame stays in flight until the test releases
        // it; the completion is the end of the listener's awaited body.
        return support::AsyncResult<void>{
                support::AsyncProducer<void, support::Error>{[&active, &in_flight](Completion completion) noexcept {
                    in_flight.emplace([&active, completion = std::move(completion)](
                                              std::expected<void, support::Error> outcome) mutable noexcept {
                        --active;
                        completion(std::move(outcome));
                    });
                }}};
    });

    // The cold hydration is the acquisition revision, not part of the frozen
    // watch's post-acquisition frame sequence.
    REQUIRE(observed.size() == 1);
    CHECK(observed[0].kind == ReplicatedStateDeliveryKind::Hydrate);
    CHECK(observed[0].sequence == 0);
    CHECK(observed[0].value == 0.0);
    CHECK_FALSE(observed[0].signal_present);

    source.commit(revision(1));
    REQUIRE(observed.size() == 2);
    CHECK(observed[1].kind == ReplicatedStateDeliveryKind::Update);
    CHECK(observed[1].sequence == 1);
    CHECK(observed[1].value == 1.0);
    CHECK_FALSE(observed[1].signal_present);
    CHECK(active == 1);
    check_frozen_numbers(update_values(), *commits_before);

    // The second committed frame buffers behind the in-flight delivery instead
    // of being delivered concurrently (frozen `valuesBeforeRelease`).
    source.commit(revision(2));
    REQUIRE(observed.size() == 2);
    CHECK(active == 1);
    CHECK(max_active == static_cast<int>(*max_concurrent));
    check_frozen_numbers(update_values(), *commits_before);

    settle(in_flight);
    REQUIRE(observed.size() == 3);
    CHECK(observed[2].kind == ReplicatedStateDeliveryKind::Update);
    CHECK(observed[2].sequence == 2);
    CHECK(observed[2].value == 2.0);
    CHECK_FALSE(observed[2].signal_present);
    CHECK(active == 0);
    CHECK(max_active == static_cast<int>(*max_concurrent));
    check_frozen_numbers(update_values(), *commits_after);
    auto final_value = Json::from_support(*frozen.member("finalValue"));
    REQUIRE(final_value.has_value());
    CHECK(number_of(attached->value()) == number_of(*final_value));
    unsubscribe();
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1062/observer-delivery.json
// "a held listener serializes its own updates while another subscription converges independently".
// The frozen held delivery points and commits drive the test; the frozen held-listener event
// order, the pre-release fast state and the other subscriber's sequence are compared exactly.
// Negative control for ordering/isolation: a Chord implementation that let the
// held subscription gate the publisher would not show the other subscriber at
// 1 and 2 before the held deliveries settle.
TEST_CASE("a held listener serializes its own updates while another subscription converges independently",
        "[chord][replicated_state][issue1062][spec]") {
    const auto frozen = observer_delivery_scenario(
            "a held listener serializes its own updates while another subscription converges independently");
    const auto* commits = frozen.inputs_member("commits");
    REQUIRE(commits != nullptr);
    const auto* commit_records = commits->get_if<support::JsonValue::array_t>();
    REQUIRE(commit_records != nullptr);
    REQUIRE(commit_records->size() == 2);
    std::vector<std::int64_t> commit_cursors;
    std::vector<double> commit_values;
    for (const auto& record : *commit_records) {
        const auto* object = record.get_if<support::JsonValue::object_t>();
        REQUIRE(object != nullptr);
        const auto cursor = support::json_integer_member(*object, "cursor");
        const auto value = support::json_number_member(*object, "value");
        REQUIRE(cursor.has_value());
        REQUIRE(value.has_value());
        commit_cursors.push_back(*cursor);
        commit_values.push_back(*value);
    }
    const auto* frozen_events = frozen.array_member("heldListenerEvents");
    const auto* before_release = frozen.object_member("beforeHydrationRelease");
    const auto* after_release = frozen.array_member("afterHydrationRelease");
    const auto* frozen_other = frozen.array_member("otherSubscriberReceived");
    REQUIRE(frozen_events != nullptr);
    REQUIRE(before_release != nullptr);
    REQUIRE(after_release != nullptr);
    REQUIRE(frozen_other != nullptr);

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
    source.commit_at(revision(commit_values[0]), commit_cursors[0]);
    source.commit_at(revision(commit_values[1]), commit_cursors[1]);

    const auto* frozen_before_events = support::json_array_member(*before_release, "events");
    const auto* frozen_before_fast = support::json_array_member(*before_release, "fast");
    const auto* frozen_before_value = support::json_member(*before_release, "value");
    REQUIRE(frozen_before_events != nullptr);
    REQUIRE(frozen_before_fast != nullptr);
    REQUIRE(frozen_before_value != nullptr);
    check_frozen_strings(events, *frozen_before_events);
    check_frozen_numbers(fast, *frozen_before_fast);
    auto before_value = Json::from_support(*frozen_before_value);
    REQUIRE(before_value.has_value());
    CHECK(number_of(attached->value()) == number_of(*before_value));
    record_end(0);
    settle(hydration);
    check_frozen_strings(events, *after_release);
    record_end(1);
    settle(update);
    check_frozen_strings(events, *frozen_events);
    check_frozen_numbers(fast, *frozen_other);
    held_unsubscribe();
    fast_unsubscribe();
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1062/observer-delivery.json
// "two subscriptions of the same listener are delivered independently". The frozen double
// subscription and double unsubscribe drive the test; the frozen received sequence is compared
// exactly.
TEST_CASE("two subscriptions of the same listener behavior are delivered independently",
        "[chord][replicated_state][issue1062][spec]") {
    const auto frozen =
            observer_delivery_scenario("two subscriptions of the same listener are delivered independently");
    const auto* frozen_received = frozen.array_member("received");
    REQUIRE(frozen_received != nullptr);

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

    check_frozen_numbers(received, *frozen_received);
    second_subscribe();
}

// Review edge: fixtures/pi-v1.3.0/audit-1062/review-edges.json
// "a snapshot cursor outside the safe integer range is rejected and the attachment is disposed
// once". The frozen snapshot cursor drives the source; the frozen message, the one disposal and
// the attachment disposition are compared exactly.
TEST_CASE("a snapshot cursor outside the safe integer range fails attach and disposes the attachment once",
        "[chord][replicated_state][issue1062][review]") {
    const auto frozen = review_edges_scenario(
            "a snapshot cursor outside the safe integer range is rejected and the attachment is disposed once");
    const auto* snapshot = frozen.inputs_member("snapshot");
    REQUIRE(snapshot != nullptr);
    const auto snapshot_cursor =
            support::json_integer_member(*snapshot->get_if<support::JsonValue::object_t>(), "cursor");
    REQUIRE(snapshot_cursor.has_value());

    TestSource source{revision(0), *snapshot_cursor};
    auto attached = AttachedReplicatedState::attach(source);
    REQUIRE_FALSE(attached.has_value());
    const auto message = frozen.string_member("message");
    REQUIRE(message.has_value());
    CHECK(attached.error().message == *message);
    const auto dispose_calls = frozen.number_member("disposeCalls");
    const auto attachments_after = frozen.number_member("attachmentCountAfterFailure");
    REQUIRE(dispose_calls.has_value());
    REQUIRE(attachments_after.has_value());
    CHECK(source.dispose_calls() == static_cast<int>(*dispose_calls));
    CHECK(source.attachment_count() == static_cast<std::size_t>(*attachments_after));
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1062/review-edges.json
// "a frame cursor outside the safe integer range fails the attachment and keeps the last good
// value". The frozen commits drive the source; the frozen error message, disposal count and last
// good values are compared exactly.
TEST_CASE("a frame cursor outside the safe integer range fails the attachment and keeps the last good value",
        "[chord][replicated_state][issue1062][review]") {
    const auto frozen = review_edges_scenario(
            "a frame cursor outside the safe integer range fails the attachment and keeps the last good value");
    const auto* snapshot = frozen.inputs_member("snapshot");
    const auto* commits = frozen.inputs_member("commits");
    REQUIRE(snapshot != nullptr);
    REQUIRE(commits != nullptr);
    const auto snapshot_cursor =
            support::json_integer_member(*snapshot->get_if<support::JsonValue::object_t>(), "cursor");
    const auto snapshot_value = support::json_number_member(*snapshot->get_if<support::JsonValue::object_t>(), "value");
    REQUIRE(snapshot_cursor.has_value());
    REQUIRE(snapshot_value.has_value());
    const auto* commit_records = commits->get_if<support::JsonValue::array_t>();
    REQUIRE(commit_records != nullptr);
    REQUIRE(commit_records->size() == 2);
    std::vector<std::int64_t> commit_cursors;
    std::vector<double> commit_values;
    for (const auto& record : *commit_records) {
        const auto* object = record.get_if<support::JsonValue::object_t>();
        REQUIRE(object != nullptr);
        const auto cursor = support::json_integer_member(*object, "cursor");
        const auto value = support::json_number_member(*object, "value");
        REQUIRE(cursor.has_value());
        REQUIRE(value.has_value());
        commit_cursors.push_back(*cursor);
        commit_values.push_back(*value);
    }

    TestSource source{revision(*snapshot_value), *snapshot_cursor};
    std::vector<std::string> errors;
    auto attached = AttachedReplicatedState::attach(source,
            ReplicatedStateSourceOptions{
                    .onError = [&errors](const support::Error& error) { errors.push_back(error.message); }});
    REQUIRE(attached.has_value());

    source.commit_at(revision(commit_values[0]), commit_cursors[0]);
    const auto* frozen_errors = frozen.array_member("reportedErrors");
    REQUIRE(frozen_errors != nullptr);
    check_frozen_error_messages(errors, *frozen_errors);
    auto last_good = Json::from_support(*frozen.member("lastGoodValue"));
    REQUIRE(last_good.has_value());
    CHECK(number_of(attached->value()) == number_of(*last_good));
    const auto dispose_calls = frozen.number_member("disposeCalls");
    REQUIRE(dispose_calls.has_value());
    CHECK(source.dispose_calls() == static_cast<int>(*dispose_calls));
    const auto attachments_after = frozen.number_member("attachmentCountAfterFailure");
    if (attachments_after.has_value()) CHECK(source.attachment_count() == static_cast<std::size_t>(*attachments_after));

    source.commit_at(revision(commit_values[1]), commit_cursors[1]);
    CHECK(errors.size() == frozen_errors->size());
    auto last_good_after = Json::from_support(*frozen.member("lastGoodValueAfterLaterFrame"));
    REQUIRE(last_good_after.has_value());
    CHECK(number_of(attached->value()) == number_of(*last_good_after));
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1062/review-edges.json
// "negative safe integer cursors are accepted for the snapshot and later frames". The frozen
// snapshot cursor and commit drive the source; the frozen final value is compared exactly.
TEST_CASE("negative safe integer cursors are accepted for the snapshot and later frames",
        "[chord][replicated_state][issue1062][review]") {
    const auto frozen =
            review_edges_scenario("negative safe integer cursors are accepted for the snapshot and later frames");
    const auto* snapshot = frozen.inputs_member("snapshot");
    const auto* commit = frozen.inputs_member("commit");
    REQUIRE(snapshot != nullptr);
    REQUIRE(commit != nullptr);
    // The frozen cursors are negative safe integers; `json_integer_member` validates
    // non-negative, so the cursors are read as numbers and narrowed.
    const auto snapshot_cursor_number =
            support::json_number_member(*snapshot->get_if<support::JsonValue::object_t>(), "cursor");
    const auto snapshot_value = support::json_number_member(*snapshot->get_if<support::JsonValue::object_t>(), "value");
    const auto commit_cursor_number =
            support::json_number_member(*commit->get_if<support::JsonValue::object_t>(), "cursor");
    const auto commit_value = support::json_number_member(*commit->get_if<support::JsonValue::object_t>(), "value");
    REQUIRE(snapshot_cursor_number.has_value());
    REQUIRE(snapshot_value.has_value());
    REQUIRE(commit_cursor_number.has_value());
    REQUIRE(commit_value.has_value());
    const auto snapshot_cursor = static_cast<std::int64_t>(*snapshot_cursor_number);
    const auto commit_cursor = static_cast<std::int64_t>(*commit_cursor_number);

    TestSource source{revision(*snapshot_value), snapshot_cursor};
    auto attached = AttachedReplicatedState::attach(source);
    REQUIRE(attached.has_value());
    std::vector<double> received;
    auto unsubscribe = attached->subscribe([&received](Json value, Context, ReplicatedStateDelivery) {
        received.push_back(number_of(value));
        return ready();
    });

    source.commit_at(revision(*commit_value), commit_cursor);
    CHECK((received == std::vector<double>{*snapshot_value, *commit_value}));
    auto final_value = Json::from_support(*frozen.member("finalValue"));
    REQUIRE(final_value.has_value());
    CHECK(number_of(attached->value()) == number_of(*final_value));
    CHECK(source.attachment_count() == 1);
    unsubscribe();
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1062/review-edges.json
// "a source contract failure without an onError hook is observable asynchronously and keeps the
// last good value". The frozen commits drive the source; the frozen failure message, disposal
// count and last good values are compared exactly. The C++ mapping writes the default diagnostic
// to stderr, captured here.
TEST_CASE("a source gap without an onError hook writes the exact default diagnostic once and keeps the last good value",
        "[chord][replicated_state][issue1062][review]") {
    const auto frozen = review_edges_scenario("a source contract failure without an onError hook is observable "
                                              "asynchronously and keeps the last good value");
    const auto* snapshot = frozen.inputs_member("snapshot");
    const auto* commits = frozen.inputs_member("commits");
    REQUIRE(snapshot != nullptr);
    REQUIRE(commits != nullptr);
    const auto snapshot_cursor =
            support::json_integer_member(*snapshot->get_if<support::JsonValue::object_t>(), "cursor");
    const auto snapshot_value = support::json_number_member(*snapshot->get_if<support::JsonValue::object_t>(), "value");
    REQUIRE(snapshot_cursor.has_value());
    REQUIRE(snapshot_value.has_value());
    const auto* commit_records = commits->get_if<support::JsonValue::array_t>();
    REQUIRE(commit_records != nullptr);
    REQUIRE(commit_records->size() == 3);
    std::vector<std::int64_t> commit_cursors;
    std::vector<double> commit_values;
    for (const auto& record : *commit_records) {
        const auto* object = record.get_if<support::JsonValue::object_t>();
        REQUIRE(object != nullptr);
        const auto cursor = support::json_integer_member(*object, "cursor");
        const auto value = support::json_number_member(*object, "value");
        REQUIRE(cursor.has_value());
        REQUIRE(value.has_value());
        commit_cursors.push_back(*cursor);
        commit_values.push_back(*value);
    }
    const auto observed_failure = frozen.string_member("observedAsynchronousFailure");
    REQUIRE(observed_failure.has_value());

    TestSource source{revision(*snapshot_value), *snapshot_cursor};
    std::string diagnostics;
    {
        StderrCapture capture;
        auto attached = AttachedReplicatedState::attach(source);
        REQUIRE(attached.has_value());
        source.commit_at(revision(commit_values[0]), commit_cursors[0]);
        source.commit_at(revision(commit_values[1]), commit_cursors[1]);
        source.commit_at(revision(commit_values[2]), commit_cursors[2]);
        auto last_good = Json::from_support(*frozen.member("lastGoodValue"));
        REQUIRE(last_good.has_value());
        CHECK(number_of(attached->value()) == number_of(*last_good));
        diagnostics = capture.finish();
    }
    CHECK(diagnostics == *observed_failure + "\n");
    const auto dispose_calls = frozen.number_member("disposeCalls");
    REQUIRE(dispose_calls.has_value());
    CHECK(source.dispose_calls() == static_cast<int>(*dispose_calls));
    const auto attachments_after = frozen.number_member("attachmentCountAfterFailure");
    if (attachments_after.has_value()) CHECK(source.attachment_count() == static_cast<std::size_t>(*attachments_after));
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

// Frozen evidence: fixtures/pi-v1.3.0/audit-1062/review-edges.json
// "a gap while draining a buffered activation ignores later buffered frames and disposes once".
// The frozen buffered commits drive the source; the frozen error message, last good value, one
// disposal and attachment disposition are compared exactly.
TEST_CASE("a gap while draining buffered activation stops dispatch without touching a disposed listener",
        "[chord][replicated_state][issue1062][review]") {
    const auto frozen = review_edges_scenario(
            "a gap while draining a buffered activation ignores later buffered frames and disposes once");
    const auto* snapshot = frozen.inputs_member("snapshot");
    const auto* buffered = frozen.inputs_member("bufferedCommits");
    REQUIRE(snapshot != nullptr);
    REQUIRE(buffered != nullptr);
    const auto snapshot_cursor =
            support::json_integer_member(*snapshot->get_if<support::JsonValue::object_t>(), "cursor");
    const auto snapshot_value = support::json_number_member(*snapshot->get_if<support::JsonValue::object_t>(), "value");
    REQUIRE(snapshot_cursor.has_value());
    REQUIRE(snapshot_value.has_value());
    const auto* buffered_records = buffered->get_if<support::JsonValue::array_t>();
    REQUIRE(buffered_records != nullptr);
    REQUIRE(buffered_records->size() == 3);
    std::vector<std::int64_t> buffered_cursors;
    std::vector<double> buffered_values;
    for (const auto& record : *buffered_records) {
        const auto* object = record.get_if<support::JsonValue::object_t>();
        REQUIRE(object != nullptr);
        const auto cursor = support::json_integer_member(*object, "cursor");
        const auto value = support::json_number_member(*object, "value");
        REQUIRE(cursor.has_value());
        REQUIRE(value.has_value());
        buffered_cursors.push_back(*cursor);
        buffered_values.push_back(*value);
    }

    TestSource source{revision(*snapshot_value), *snapshot_cursor};
    std::vector<std::string> errors;
    source.on_attach = [&source, &buffered_cursors, &buffered_values] {
        for (std::size_t index = 0; index < buffered_cursors.size(); ++index)
            source.commit_at(revision(buffered_values[index]), buffered_cursors[index]);
    };
    auto attached = AttachedReplicatedState::attach(source,
            ReplicatedStateSourceOptions{
                    .onError = [&errors](const support::Error& error) { errors.push_back(error.message); }});
    REQUIRE(attached.has_value());
    const auto* frozen_errors = frozen.array_member("reportedErrors");
    REQUIRE(frozen_errors != nullptr);
    check_frozen_error_messages(errors, *frozen_errors);
    auto last_good = Json::from_support(*frozen.member("lastGoodValue"));
    REQUIRE(last_good.has_value());
    CHECK(number_of(attached->value()) == number_of(*last_good));
    const auto dispose_calls = frozen.number_member("disposeCalls");
    REQUIRE(dispose_calls.has_value());
    CHECK(source.dispose_calls() == static_cast<int>(*dispose_calls));
    const auto attachments_after = frozen.number_member("attachmentCountAfterFailure");
    REQUIRE(attachments_after.has_value());
    CHECK(source.attachment_count() == static_cast<std::size_t>(*attachments_after));
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

// Frozen evidence: fixtures/pi-v1.3.0/audit-1062/review-edges.json
// "subscribers are delivered in registration order and a reentrant subscriber hydrates before the
// next publication". The frozen subscriber set and commits drive the test; the frozen event order
// is compared exactly.
TEST_CASE("subscribers are delivered in registration order with reentrant unsubscribe and subscribe",
        "[chord][replicated_state][issue1062][review]") {
    const auto frozen = review_edges_scenario(
            "subscribers are delivered in registration order and a reentrant subscriber hydrates before the next "
            "publication");
    const auto* commits = frozen.inputs_member("commits");
    REQUIRE(commits != nullptr);
    const auto* commit_records = commits->get_if<support::JsonValue::array_t>();
    REQUIRE(commit_records != nullptr);
    REQUIRE(commit_records->size() == 3);
    std::vector<double> commit_values;
    for (const auto& record : *commit_records) {
        const auto* number = record.get_if<double>();
        REQUIRE(number != nullptr);
        commit_values.push_back(*number);
    }

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

    for (const auto value : commit_values)
        source.commit(revision(value));

    const auto* frozen_events = frozen.array_member("events");
    REQUIRE(frozen_events != nullptr);
    check_frozen_strings(events, *frozen_events);
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
    const Json committed_one = revision(1);
    source.commit(committed_one);
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
    CHECK(retained.shares_container_with(committed_one));
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
