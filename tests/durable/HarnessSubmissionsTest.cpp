#include <cch/ai/Content.hpp>
#include <cch/ai/Models.hpp>
#include <cch/durable/Harness.hpp>
#include <cch/durable/Storage.hpp>
#include "durable/FrozenExpectation.hpp"
#include "durable/Outcome.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <semaphore>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace cch::tests::durable {
using namespace cch::durable;

namespace {

class RecordingStorage final : public Storage {
public:
    RecordingStorage() : inner_(std::make_unique<MemoryStorage>()) {}
    ~RecordingStorage() override = default;

    std::vector<std::vector<StorageWrite>> batches;
    std::mutex mutex;

    [[nodiscard]] support::AsyncResult<Seq> commit(std::span<const StorageWrite> writes) override {
        {
            std::lock_guard lock(mutex);
            batches.emplace_back(writes.begin(), writes.end());
        }
        return inner_->commit(writes);
    }

    [[nodiscard]] support::Expected<std::int64_t> mint_id() override { return inner_->mint_id(); }
    [[nodiscard]] support::Expected<std::optional<ConversationRecord>> conversation(ConversationId id) override {
        return inner_->conversation(id);
    }
    [[nodiscard]] support::Expected<std::optional<StoredEntry>> entry(EntryId id) override { return inner_->entry(id); }
    [[nodiscard]] support::Expected<std::vector<ConversationRecord>> scan_conversations(std::size_t limit) override {
        return inner_->scan_conversations(limit);
    }
    [[nodiscard]] support::Expected<std::vector<EntryRecord>> scan_entries(
            ConversationId conversation_id, std::size_t limit) override {
        return inner_->scan_entries(conversation_id, limit);
    }
    [[nodiscard]] support::Expected<std::optional<DocumentRecord>> find_document(
            std::string_view kind, std::optional<ConversationId> conversation_id) override {
        return inner_->find_document(kind, conversation_id);
    }
    [[nodiscard]] support::Expected<std::optional<chord::Json>> document(DocumentId id) override {
        return inner_->document(id);
    }
    [[nodiscard]] support::Expected<std::optional<TaskRecord>> task(TaskId id) override { return inner_->task(id); }
    [[nodiscard]] support::Expected<std::vector<TaskRecord>> scan_tasks(std::size_t limit) override {
        return inner_->scan_tasks(limit);
    }
    [[nodiscard]] support::Expected<std::optional<SubmissionRecord>> submission(SubmissionId id) override {
        return inner_->submission(id);
    }
    [[nodiscard]] support::Expected<std::optional<SubmissionRecord>> submission_by_request(
            ConversationId conversation_id, std::string_view request_id) override {
        return inner_->submission_by_request(conversation_id, request_id);
    }
    [[nodiscard]] support::Expected<std::vector<SubmissionRecord>> scan_submissions(std::size_t limit) override {
        return inner_->scan_submissions(limit);
    }
    [[nodiscard]] support::ExpectedVoid close() override { return inner_->close(); }

private:
    std::unique_ptr<MemoryStorage> inner_;
};

class HeldStep {
public:
    std::binary_semaphore reached{0};
    std::atomic<bool> released{false};
    std::mutex mutex;
    std::condition_variable cv;
    std::string response_text{"first"};

    void release() {
        {
            std::lock_guard lock(mutex);
            released = true;
        }
        cv.notify_all();
    }

    ai::ModelStream step(std::stop_token stop) {
        return ai::ModelStream{ai::ModelStreamProducer{
                [this, stop](ai::AssistantEventSink sink, ai::ModelStreamCompletion completion) mutable noexcept {
                    reached.release();
                    std::stop_callback cb(stop, [this]() {
                        {
                            std::lock_guard lock(mutex);
                            released = true;
                        }
                        cv.notify_all();
                    });
                    std::unique_lock lock(mutex);
                    cv.wait(lock, [this, &stop]() { return released.load() || stop.stop_requested(); });
                    if (stop.stop_requested()) {
                        completion(std::unexpected(support::make_error(support::ErrorCode::Cancelled, "aborted")));
                        return;
                    }
                    auto message = ai::assistant_text_message(response_text);
                    auto delivered = sink(ai::AssistantDoneEvent{.reason = message.stop_reason, .message = message});
                    if (!delivered)
                        completion(std::unexpected(delivered.error()));
                    else
                        completion(std::move(message));
                }}};
    }
};

SubmissionDraft text_input(std::string text,
        std::optional<WhenBusy> when_busy = std::nullopt,
        std::optional<std::string> request_id = std::nullopt) {
    return SubmissionDraft{.request_id = std::move(request_id),
            .body = InputSubmissionDraft{.content = chord::Json{std::move(text)}, .when_busy = when_busy}};
}

SubmissionDraft write_submission(std::string kind,
        std::optional<chord::Json> data = std::nullopt,
        std::optional<std::vector<chord::Json>> model = std::nullopt,
        std::optional<std::string> request_id = std::nullopt,
        std::optional<EntryHeadVariant> head = std::nullopt) {
    return SubmissionDraft{.request_id = std::move(request_id),
            .body = WriteSubmissionDraft{.entry = WriteEntry{.kind = std::move(kind),
                                                 .data = std::move(data),
                                                 .model = std::move(model),
                                                 .head = std::move(head)}}};
}

std::optional<std::string> text_of(const EntryRecord& entry) {
    const auto* envelope = entry.data.as_object();
    if (envelope == nullptr) return std::nullopt;
    const auto found = envelope->find("model");
    if (found == envelope->end()) return std::nullopt;
    const auto* messages = found->second.as_array();
    if (messages == nullptr || messages->empty()) return std::nullopt;
    const auto* message = messages->front().as_object();
    if (message == nullptr) return std::nullopt;
    const auto role = message->find("role");
    if (role != message->end() && role->second.as_string()) {
        const auto& r = *role->second.as_string();
        if (r == "system" || r == "toolResult") return std::nullopt;
    }
    const auto content = message->find("content");
    if (content == message->end()) return std::nullopt;
    if (const auto* s = content->second.as_string()) return *s;
    if (const auto* arr = content->second.as_array()) {
        for (const auto& item : *arr) {
            if (const auto* obj = item.as_object()) {
                const auto t = obj->find("type");
                if (t != obj->end() && t->second.as_string() && *t->second.as_string() == "text") {
                    const auto txt = obj->find("text");
                    if (txt != obj->end() && txt->second.as_string()) return *txt->second.as_string();
                }
            }
        }
    }
    return std::nullopt;
}

chord::Json transcript(const std::vector<EntryRecord>& entries) {
    chord::Json::Array result;
    for (const auto& entry : entries) {
        if (entry.kind == "pi.system") continue;
        auto text = text_of(entry);
        if (!text.has_value()) {
            result.emplace_back(entry.kind);
        } else {
            result.emplace_back(entry.kind + ":" + *text);
        }
    }
    return chord::Json{std::move(result)};
}

chord::Json::Array inbox_items(Storage& storage, ConversationId root) {
    auto doc_rec = storage.find_document("pi.inbox:v1", root);
    if (!doc_rec || !doc_rec->has_value()) return {};
    auto content = storage.document((*doc_rec)->id);
    if (!content || !content->has_value()) return {};
    const auto* obj = (*content)->as_object();
    if (obj == nullptr) return {};
    const auto it = obj->find("items");
    if (it == obj->end() || it->second.as_array() == nullptr) return {};
    return *it->second.as_array();
}

FrozenScenario frozen_1080(std::string_view name) {
    auto scenario = FrozenScenario::load("fixtures/pi-v1.3.0/audit-1080/submissions-inbox.json", name);
    REQUIRE(scenario.has_value());
    return *scenario;
}

FrozenScenario frozen_1055(std::string_view name) {
    auto scenario = FrozenScenario::load("fixtures/pi-v1.3.0/audit-1055/submissions-inbox.json", name);
    REQUIRE(scenario.has_value());
    return *scenario;
}

} // namespace

// ===========================================================================
// audit-1080 scenarios
// ===========================================================================

TEST_CASE("a busy steer input is placed at the postTools boundary and a busy follow-up is held to the final boundary",
        "[durable][workflow][issue1080]") {
    const auto frozen = frozen_1080("a busy steer input is placed at the postTools boundary and a busy follow-up is "
                                    "held to the final boundary");

    std::binary_semaphore entered{0};
    std::binary_semaphore release_gate{0};
    Extension hold_ext{.name = "held-tool"};
    hold_ext.tools.push_back(
            ToolRegistration{.declaration = ai::Tool{.name = "hold", .description = "Waits for the test"},
                    .execute = [&entered, &release_gate](support::JsonValue, std::stop_token) {
                        entered.release();
                        release_gate.acquire();
                        return support::AsyncResult<std::vector<ai::Content>>{
                                std::vector<ai::Content>{ai::text_content("held")}};
                    }});

    Registry registry;
    REQUIRE(registry.install(std::move(hold_ext)).has_value());

    std::atomic<int> turn{0};
    HarnessOptions options{.registry = &registry};
    options.stream = [&turn](ai::Model, ai::AiContext, ai::SimpleStreamOptions) {
        const int current = turn.fetch_add(1);
        if (current == 0) {
            return ai::ModelStream{ai::ModelStreamProducer{[](ai::AssistantEventSink sink,
                                                                   ai::ModelStreamCompletion completion) noexcept {
                auto message = ai::assistant_text_message("");
                message.content = {ai::ToolCallContent{
                        .id = "c1", .name = "hold", .arguments = support::JsonValue{support::JsonValue::object_t{}}}};
                message.stop_reason = ai::AssistantStopReason::ToolUse;
                auto delivered = sink(ai::AssistantDoneEvent{.reason = message.stop_reason, .message = message});
                if (!delivered)
                    completion(std::unexpected(delivered.error()));
                else
                    completion(std::move(message));
            }}};
        }
        if (current == 1) {
            return ai::ModelStream{ai::ModelStreamProducer{[](ai::AssistantEventSink sink,
                                                                   ai::ModelStreamCompletion completion) noexcept {
                auto message = ai::assistant_text_message("after tools");
                auto delivered = sink(ai::AssistantDoneEvent{.reason = message.stop_reason, .message = message});
                if (!delivered)
                    completion(std::unexpected(delivered.error()));
                else
                    completion(std::move(message));
            }}};
        }
        return ai::ModelStream{
                ai::ModelStreamProducer{[](ai::AssistantEventSink sink, ai::ModelStreamCompletion completion) noexcept {
                    auto message = ai::assistant_text_message("follow-up");
                    auto delivered = sink(ai::AssistantDoneEvent{.reason = message.stop_reason, .message = message});
                    if (!delivered)
                        completion(std::unexpected(delivered.error()));
                    else
                        completion(std::move(message));
                }}};
    };

    MemoryStorage storage;
    auto opened = consume(Harness::open(storage, std::move(options)));
    REQUIRE(opened.has_value());
    auto harness = std::move(opened.value());
    auto root = consume(harness->root());
    REQUIRE(root.has_value());

    auto first = consume(harness->submit(root.value(), text_input("a")));
    REQUIRE(first.has_value());
    REQUIRE(entered.try_acquire_for(std::chrono::seconds(5)));

    auto steer = consume(harness->submit(root.value(), text_input("s", WhenBusy::Steer)));
    REQUIRE(steer.has_value());
    auto followUp = consume(harness->submit(root.value(), text_input("f")));
    REQUIRE(followUp.has_value());

    auto first_status = consume(first.value().status());
    REQUIRE(first_status.has_value());
    CHECK(first_status.value().status ==
            frozen.expected().get_object().at("before").get_object().at("first").get_string());

    auto steer_status = consume(steer.value().status());
    REQUIRE(steer_status.has_value());
    CHECK(steer_status.value().status ==
            frozen.expected().get_object().at("before").get_object().at("steer").get_string());

    auto follow_status = consume(followUp.value().status());
    REQUIRE(follow_status.has_value());
    CHECK(follow_status.value().status ==
            frozen.expected().get_object().at("before").get_object().at("followUp").get_string());

    auto items_before = inbox_items(storage, root.value());
    REQUIRE(items_before.size() == 2);
    CHECK(items_before[0].as_object()->at("mode") == chord::Json{"steer"});
    CHECK(items_before[0].as_object()->at("id") == chord::Json{static_cast<double>(steer.value().id.value)});
    CHECK(items_before[1].as_object()->at("mode") == chord::Json{"followUp"});
    CHECK(items_before[1].as_object()->at("id") == chord::Json{static_cast<double>(followUp.value().id.value)});

    auto entries_before = storage.scan_entries(root.value(), 100);
    REQUIRE(entries_before.has_value());
    std::ranges::reverse(*entries_before);
    CHECK(json_equals(transcript(*entries_before), *frozen.member("busyTranscript")));

    release_gate.release();

    auto settled_follow_up = consume(followUp.value().wait());
    REQUIRE(settled_follow_up.has_value());

    auto entries = storage.scan_entries(root.value(), 100);
    REQUIRE(entries.has_value());
    std::ranges::reverse(*entries);
    CHECK(json_equals(transcript(*entries), *frozen.member("transcript")));

    std::vector<EntryRecord> assistants;
    for (const auto& e : *entries) {
        if (e.kind == "pi.assistant") assistants.push_back(e);
    }
    REQUIRE(assistants.size() == 3);

    auto first_settled = consume(first.value().status());
    REQUIRE(first_settled.has_value());
    CHECK(first_settled.value().status == "done");
    CHECK(first_settled.value().answer.has_value());
    CHECK(first_settled.value().answer->value == assistants[1].id.value);

    auto steer_settled = consume(steer.value().status());
    REQUIRE(steer_settled.has_value());
    CHECK(steer_settled.value().status == "done");
    CHECK(steer_settled.value().answer.has_value());
    CHECK(steer_settled.value().answer->value == assistants[1].id.value);

    CHECK(settled_follow_up.value().status == "done");
    CHECK(settled_follow_up.value().answer.has_value());
    CHECK(settled_follow_up.value().answer->value == assistants[2].id.value);

    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE(
        "a duplicate request ID with a different payload returns the existing submission with zero new storage batches",
        "[durable][workflow][issue1080]") {
    const auto frozen = frozen_1080("a duplicate request ID with a different payload returns the existing submission "
                                    "with zero new storage batches");

    RecordingStorage storage;
    HeldStep held;
    HarnessOptions options;
    options.stream = [&held](ai::Model, ai::AiContext, ai::SimpleStreamOptions s) { return held.step(s.stop_token); };

    auto opened = consume(Harness::open(storage, std::move(options)));
    REQUIRE(opened.has_value());
    auto harness = std::move(opened.value());
    auto root = consume(harness->root());
    REQUIRE(root.has_value());

    auto first = consume(harness->submit(root.value(), text_input("a", std::nullopt, "req-1080")));
    REQUIRE(first.has_value());
    REQUIRE(held.reached.try_acquire_for(std::chrono::seconds(5)));

    std::size_t batches_before = 0;
    {
        std::lock_guard lock(storage.mutex);
        batches_before = storage.batches.size();
    }
    const auto inbox_before = inbox_items(storage, root.value()).size();

    auto duplicate =
            consume(harness->submit(root.value(), text_input("a different payload", std::nullopt, "req-1080")));
    REQUIRE(duplicate.has_value());

    std::size_t duplicate_batches = 0;
    {
        std::lock_guard lock(storage.mutex);
        duplicate_batches = storage.batches.size() - batches_before;
    }
    const auto inbox_after = inbox_items(storage, root.value()).size();

    const auto& expected = frozen.expected().get_object();
    CHECK(duplicate.value().id.value == first.value().id.value);
    CHECK(duplicate_batches == static_cast<std::size_t>(expected.at("duplicateBatches").get_number()));

    auto first_status = consume(first.value().status());
    REQUIRE(first_status.has_value());
    CHECK(first_status.value().status == expected.at("originalStatus").get_string());

    auto duplicate_status = consume(duplicate.value().status());
    REQUIRE(duplicate_status.has_value());
    CHECK(duplicate_status.value().status == expected.at("duplicateStatus").get_string());

    CHECK(inbox_before == static_cast<std::size_t>(expected.at("inboxBefore").get_number()));
    CHECK(inbox_after == static_cast<std::size_t>(expected.at("inboxAfter").get_number()));

    held.release();
    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("a conversation abort settles the queued input unanswered and aborted, keeps the queued write queued and "
          "settles the placed input unanswered and aborted",
        "[durable][workflow][issue1080]") {
    const auto frozen = frozen_1080("a conversation abort settles the queued input unanswered and aborted, keeps the "
                                    "queued write queued and settles the placed input unanswered and aborted");

    MemoryStorage storage;
    HeldStep held;
    HarnessOptions options;
    options.stream = [&held](ai::Model, ai::AiContext, ai::SimpleStreamOptions s) { return held.step(s.stop_token); };

    auto opened = consume(Harness::open(storage, std::move(options)));
    REQUIRE(opened.has_value());
    auto harness = std::move(opened.value());
    auto root = consume(harness->root());
    REQUIRE(root.has_value());

    auto placed = consume(harness->submit(root.value(), text_input("a")));
    REQUIRE(placed.has_value());
    REQUIRE(held.reached.try_acquire_for(std::chrono::seconds(5)));

    auto queued_input = consume(harness->submit(root.value(), text_input("b")));
    REQUIRE(queued_input.has_value());

    std::vector<chord::Json> write_model{chord::Json{chord::Json::Object{
            {"role", chord::Json{"user"}}, {"content", chord::Json{"n"}}, {"timestamp", chord::Json{0.0}}}}};
    auto queued_write =
            consume(harness->submit(root.value(), write_submission("note", std::nullopt, std::move(write_model))));
    REQUIRE(queued_write.has_value());

    auto placed_before = consume(placed.value().status());
    auto input_before = consume(queued_input.value().status());
    auto write_before = consume(queued_write.value().status());
    REQUIRE(placed_before.has_value());
    REQUIRE(input_before.has_value());
    REQUIRE(write_before.has_value());
    CHECK(placed_before.value().status == "placed");
    CHECK(input_before.value().status == "queued");
    CHECK(write_before.value().status == "queued");

    REQUIRE(consume(harness->abort_conversation(root.value())).has_value());

    auto placed_after = consume(placed.value().status());
    auto input_after = consume(queued_input.value().status());
    auto write_after = consume(queued_write.value().status());
    REQUIRE(placed_after.has_value());
    REQUIRE(input_after.has_value());
    REQUIRE(write_after.has_value());

    CHECK(placed_after.value().status == "unanswered");
    CHECK(placed_after.value().reason == "aborted");
    CHECK(input_after.value().status == "unanswered");
    CHECK(input_after.value().reason == "aborted");
    CHECK(write_after.value().status == "queued");
    CHECK_FALSE(write_after.value().reason.has_value());

    auto items = inbox_items(storage, root.value());
    REQUIRE(items.size() == 1);
    CHECK(items[0].as_object()->at("mode") == chord::Json{"write"});
    CHECK(items[0].as_object()->at("id") == chord::Json{static_cast<double>(queued_write.value().id.value)});

    auto live_doc = storage.find_document("pi.live:v1", root.value());
    REQUIRE(live_doc.has_value());
    REQUIRE(live_doc->has_value());
    auto live_val = storage.document((*live_doc)->id);
    REQUIRE(live_val.has_value());
    REQUIRE(live_val->has_value());
    CHECK((*live_val)->as_object()->find("run") == (*live_val)->as_object()->end());

    held.release();
    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("a busy follow-up is placed at the final boundary and answered by a second run",
        "[durable][workflow][issue1080]") {
    const auto frozen = frozen_1080("a busy follow-up is placed at the final boundary and answered by a second run");

    MemoryStorage storage;
    HeldStep held;
    std::atomic<int> turn{0};
    HarnessOptions options;
    options.stream = [&held, &turn](ai::Model, ai::AiContext, ai::SimpleStreamOptions s) {
        if (turn.fetch_add(1) == 0) {
            return held.step(s.stop_token);
        }
        return ai::ModelStream{
                ai::ModelStreamProducer{[](ai::AssistantEventSink sink, ai::ModelStreamCompletion completion) noexcept {
                    auto message = ai::assistant_text_message("second");
                    auto delivered = sink(ai::AssistantDoneEvent{.reason = message.stop_reason, .message = message});
                    if (!delivered)
                        completion(std::unexpected(delivered.error()));
                    else
                        completion(std::move(message));
                }}};
    };

    auto opened = consume(Harness::open(storage, std::move(options)));
    REQUIRE(opened.has_value());
    auto harness = std::move(opened.value());
    auto root = consume(harness->root());
    REQUIRE(root.has_value());

    auto first = consume(harness->submit(root.value(), text_input("a")));
    REQUIRE(first.has_value());
    REQUIRE(held.reached.try_acquire_for(std::chrono::seconds(5)));

    auto follow_up = consume(harness->submit(root.value(), text_input("f")));
    REQUIRE(follow_up.has_value());

    auto first_before = consume(first.value().status());
    auto follow_before = consume(follow_up.value().status());
    REQUIRE(first_before.has_value());
    REQUIRE(follow_before.has_value());
    CHECK(first_before.value().status == "placed");
    CHECK(follow_before.value().status == "queued");

    auto items_before = inbox_items(storage, root.value());
    REQUIRE(items_before.size() == 1);
    CHECK(items_before[0].as_object()->at("mode") == chord::Json{"followUp"});

    held.release();

    auto settled_follow_up = consume(follow_up.value().wait());
    REQUIRE(settled_follow_up.has_value());

    auto entries = storage.scan_entries(root.value(), 100);
    REQUIRE(entries.has_value());
    std::ranges::reverse(*entries);
    CHECK(json_equals(transcript(*entries), *frozen.member("transcript")));

    std::vector<EntryRecord> assistants;
    for (const auto& e : *entries) {
        if (e.kind == "pi.assistant") assistants.push_back(e);
    }
    REQUIRE(assistants.size() == 2);

    auto first_settled = consume(first.value().status());
    REQUIRE(first_settled.has_value());
    CHECK(first_settled.value().status == "done");
    CHECK(first_settled.value().answer.has_value());
    CHECK(first_settled.value().answer->value == assistants[0].id.value);

    CHECK(settled_follow_up.value().status == "done");
    CHECK(settled_follow_up.value().answer.has_value());
    CHECK(settled_follow_up.value().answer->value == assistants[1].id.value);

    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("a busy queued admission commits one batch with the submission and the pi.inbox items change",
        "[durable][workflow][issue1080]") {
    const auto frozen =
            frozen_1080("a busy queued admission commits one batch with the submission and the pi.inbox items change");

    RecordingStorage storage;
    HeldStep held;
    HarnessOptions options;
    options.stream = [&held](ai::Model, ai::AiContext, ai::SimpleStreamOptions s) { return held.step(s.stop_token); };

    auto opened = consume(Harness::open(storage, std::move(options)));
    REQUIRE(opened.has_value());
    auto harness = std::move(opened.value());
    auto root = consume(harness->root());
    REQUIRE(root.has_value());

    auto first = consume(harness->submit(root.value(), text_input("a")));
    REQUIRE(first.has_value());
    REQUIRE(held.reached.try_acquire_for(std::chrono::seconds(5)));

    std::size_t before = 0;
    {
        std::lock_guard lock(storage.mutex);
        before = storage.batches.size();
    }

    auto queued = consume(harness->submit(root.value(), text_input("queued")));
    REQUIRE(queued.has_value());

    std::vector<StorageWrite> batch;
    {
        std::lock_guard lock(storage.mutex);
        REQUIRE(storage.batches.size() > before);
        batch = storage.batches[before];
    }
    REQUIRE(batch.size() == 2);

    const auto* sub_write =
            batch[0].type == "submission" ? &batch[0] : (batch[1].type == "submission" ? &batch[1] : nullptr);
    const auto* doc_write =
            batch[0].type == "document.change" ? &batch[0] : (batch[1].type == "document.change" ? &batch[1] : nullptr);
    REQUIRE(sub_write != nullptr);
    REQUIRE(doc_write != nullptr);

    const auto& expected = frozen.expected().get_object();
    REQUIRE(sub_write->submission.has_value());
    CHECK(sub_write->submission->status == expected.at("submissionStatus").get_string());
    CHECK(sub_write->submission->type == expected.at("submissionType").get_string());

    bool found_items_op = false;
    for (const auto& op : doc_write->document_ops) {
        if (const auto* set_op = std::get_if<chord::SetOp>(&op)) {
            if (!set_op->path.empty()) {
                if (const auto* key = std::get_if<chord::PathKey>(&set_op->path[0])) {
                    if (key->key == "items") found_items_op = true;
                }
            }
        }
    }
    CHECK(found_items_op);

    held.release();
    REQUIRE(consume(harness->close()).has_value());
}

// ===========================================================================
// audit-1055 scenarios
// ===========================================================================

TEST_CASE("an idle input is placed and answered by a run", "[durable][workflow][issue1080]") {
    const auto frozen = frozen_1055("an idle input is placed and answered by a run");

    MemoryStorage storage;
    HarnessOptions options;
    options.stream = [](ai::Model, ai::AiContext, ai::SimpleStreamOptions) {
        return ai::ModelStream{
                ai::ModelStreamProducer{[](ai::AssistantEventSink sink, ai::ModelStreamCompletion completion) noexcept {
                    auto message = ai::assistant_text_message("hello");
                    auto delivered = sink(ai::AssistantDoneEvent{.reason = message.stop_reason, .message = message});
                    if (!delivered)
                        completion(std::unexpected(delivered.error()));
                    else
                        completion(std::move(message));
                }}};
    };

    auto opened = consume(Harness::open(storage, std::move(options)));
    REQUIRE(opened.has_value());
    auto harness = std::move(opened.value());
    auto root = consume(harness->root());
    REQUIRE(root.has_value());

    auto sub = consume(harness->submit(root.value(), text_input("hi")));
    REQUIRE(sub.has_value());
    auto settled = consume(sub.value().wait());
    REQUIRE(settled.has_value());
    const auto& expected = frozen.expected().get_object();
    CHECK(settled.value().status == expected.at("status").get_string());
    CHECK(settled.value().type == expected.at("type").get_string());

    auto entries = storage.scan_entries(root.value(), 100);
    REQUIRE(entries.has_value());
    std::ranges::reverse(*entries);
    REQUIRE(!entries->empty());
    CHECK((*entries)[0].kind == expected.at("entryKind").get_string());
    CHECK(json_equals(transcript(*entries), *frozen.member("transcript")));

    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE(
        "an idle passive write is appended and settled done without starting a run", "[durable][workflow][issue1080]") {
    const auto frozen = frozen_1055("an idle passive write is appended and settled done without starting a run");

    MemoryStorage storage;
    HarnessOptions options;
    options.stream = [](ai::Model, ai::AiContext, ai::SimpleStreamOptions) {
        FAIL("stream must not be called for passive write");
        return ai::ModelStream{ai::ModelStreamProducer{[](auto, auto) noexcept {}}};
    };

    auto opened = consume(Harness::open(storage, std::move(options)));
    REQUIRE(opened.has_value());
    auto harness = std::move(opened.value());
    auto root = consume(harness->root());
    REQUIRE(root.has_value());

    auto write = consume(harness->submit(root.value(),
            write_submission("audit.note", chord::Json{chord::Json::Object{{"text", chord::Json{"passive"}}}})));
    REQUIRE(write.has_value());
    auto settled = consume(write.value().wait());
    REQUIRE(settled.has_value());
    const auto& expected = frozen.expected().get_object();
    CHECK(settled.value().status == expected.at("status").get_string());

    auto entries = storage.scan_entries(root.value(), 100);
    REQUIRE(entries.has_value());
    std::ranges::reverse(*entries);
    REQUIRE(!entries->empty());
    CHECK((*entries)[0].kind == expected.at("entryKind").get_string());
    CHECK(json_equals((*entries)[0].data, expected.at("entryData").get_object()));

    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("a busy input queues as a follow-up and is placed at the final boundary", "[durable][workflow][issue1080]") {
    const auto frozen = frozen_1055("a busy input queues as a follow-up and is placed at the final boundary");

    MemoryStorage storage;
    HeldStep held;
    std::atomic<int> turn{0};
    HarnessOptions options;
    options.stream = [&held, &turn](ai::Model, ai::AiContext, ai::SimpleStreamOptions s) {
        if (turn.fetch_add(1) == 0) {
            return held.step(s.stop_token);
        }
        return ai::ModelStream{
                ai::ModelStreamProducer{[](ai::AssistantEventSink sink, ai::ModelStreamCompletion completion) noexcept {
                    auto message = ai::assistant_text_message("second");
                    auto delivered = sink(ai::AssistantDoneEvent{.reason = message.stop_reason, .message = message});
                    if (!delivered)
                        completion(std::unexpected(delivered.error()));
                    else
                        completion(std::move(message));
                }}};
    };

    auto opened = consume(Harness::open(storage, std::move(options)));
    REQUIRE(opened.has_value());
    auto harness = std::move(opened.value());
    auto root = consume(harness->root());
    REQUIRE(root.has_value());

    auto first = consume(harness->submit(root.value(), text_input("a")));
    REQUIRE(first.has_value());
    REQUIRE(held.reached.try_acquire_for(std::chrono::seconds(5)));

    auto follow_up = consume(harness->submit(root.value(), text_input("f1")));
    REQUIRE(follow_up.has_value());

    auto first_status = consume(first.value().status());
    auto follow_status = consume(follow_up.value().status());
    REQUIRE(first_status.has_value());
    REQUIRE(follow_status.has_value());
    const auto& expected = frozen.expected().get_object();
    CHECK(first_status.value().status == expected.at("before").get_object().at("first").get_string());
    CHECK(follow_status.value().status == expected.at("before").get_object().at("followUp").get_string());

    auto items = inbox_items(storage, root.value());
    REQUIRE(items.size() == 1);
    CHECK(items[0].as_object()->at("mode") == chord::Json{"followUp"});

    held.release();

    auto settled = consume(follow_up.value().wait());
    REQUIRE(settled.has_value());
    CHECK(settled.value().status == expected.at("followUpStatus").get_string());

    auto entries = storage.scan_entries(root.value(), 100);
    REQUIRE(entries.has_value());
    std::ranges::reverse(*entries);
    CHECK(json_equals(transcript(*entries), *frozen.member("transcript")));

    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("a busy steer input queues in steer mode and a conversation abort withdraws it",
        "[durable][workflow][issue1080]") {
    const auto frozen = frozen_1055("a busy steer input queues in steer mode and a conversation abort withdraws it");

    MemoryStorage storage;
    HeldStep held;
    HarnessOptions options;
    options.stream = [&held](ai::Model, ai::AiContext, ai::SimpleStreamOptions s) { return held.step(s.stop_token); };

    auto opened = consume(Harness::open(storage, std::move(options)));
    REQUIRE(opened.has_value());
    auto harness = std::move(opened.value());
    auto root = consume(harness->root());
    REQUIRE(root.has_value());

    auto first = consume(harness->submit(root.value(), text_input("a")));
    REQUIRE(first.has_value());
    REQUIRE(held.reached.try_acquire_for(std::chrono::seconds(5)));

    auto steer = consume(harness->submit(root.value(), text_input("s", WhenBusy::Steer)));
    REQUIRE(steer.has_value());

    auto first_status = consume(first.value().status());
    auto steer_status = consume(steer.value().status());
    REQUIRE(first_status.has_value());
    REQUIRE(steer_status.has_value());
    const auto& expected = frozen.expected().get_object();
    CHECK(first_status.value().status == expected.at("before").get_object().at("first").get_string());
    CHECK(steer_status.value().status == expected.at("before").get_object().at("steer").get_string());

    auto items = inbox_items(storage, root.value());
    REQUIRE(items.size() == 1);
    CHECK(items[0].as_object()->at("mode") == chord::Json{"steer"});

    REQUIRE(consume(harness->abort_conversation(root.value())).has_value());

    auto after = consume(steer.value().status());
    REQUIRE(after.has_value());
    CHECK(after.value().status == expected.at("after").get_object().at("status").get_string());
    CHECK(after.value().reason == expected.at("after").get_object().at("reason").get_string());

    auto live_doc = storage.find_document("pi.live:v1", root.value());
    REQUIRE(live_doc.has_value());
    REQUIRE(live_doc->has_value());
    auto live_val = storage.document((*live_doc)->id);
    REQUIRE(live_val.has_value());
    REQUIRE(live_val->has_value());
    CHECK((*live_val)->as_object()->find("run") == (*live_val)->as_object()->end());

    held.release();
    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("a display-only write is appended and settled while the conversation is busy",
        "[durable][workflow][issue1080]") {
    const auto frozen = frozen_1055("a display-only write is appended and settled while the conversation is busy");

    MemoryStorage storage;
    HeldStep held;
    HarnessOptions options;
    options.stream = [&held](ai::Model, ai::AiContext, ai::SimpleStreamOptions s) { return held.step(s.stop_token); };

    auto opened = consume(Harness::open(storage, std::move(options)));
    REQUIRE(opened.has_value());
    auto harness = std::move(opened.value());
    auto root = consume(harness->root());
    REQUIRE(root.has_value());

    auto first = consume(harness->submit(root.value(), text_input("a")));
    REQUIRE(first.has_value());
    REQUIRE(held.reached.try_acquire_for(std::chrono::seconds(5)));

    auto write = consume(harness->submit(root.value(),
            write_submission("audit.display", chord::Json{chord::Json::Object{{"label", chord::Json{"visible"}}}})));
    REQUIRE(write.has_value());

    auto settled = consume(write.value().status());
    REQUIRE(settled.has_value());
    const auto& expected = frozen.expected().get_object();
    CHECK(settled.value().status == expected.at("status").get_string());

    auto items = inbox_items(storage, root.value());
    CHECK(items.size() == static_cast<std::size_t>(expected.at("inboxLength").get_number()));

    held.release();
    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("a duplicate request ID returns the existing submission without a storage write",
        "[durable][workflow][issue1080]") {
    const auto frozen = frozen_1055("a duplicate request ID returns the existing submission without a storage write");

    RecordingStorage storage;
    HeldStep held;
    HarnessOptions options;
    options.stream = [&held](ai::Model, ai::AiContext, ai::SimpleStreamOptions s) { return held.step(s.stop_token); };

    auto opened = consume(Harness::open(storage, std::move(options)));
    REQUIRE(opened.has_value());
    auto harness = std::move(opened.value());
    auto root = consume(harness->root());
    REQUIRE(root.has_value());

    auto first = consume(harness->submit(root.value(), text_input("a", std::nullopt, "req-9")));
    REQUIRE(first.has_value());
    REQUIRE(held.reached.try_acquire_for(std::chrono::seconds(5)));

    std::size_t before = 0;
    {
        std::lock_guard lock(storage.mutex);
        before = storage.batches.size();
    }

    auto duplicate = consume(harness->submit(root.value(), text_input("different", std::nullopt, "req-9")));
    REQUIRE(duplicate.has_value());

    std::size_t duplicate_batches = 0;
    {
        std::lock_guard lock(storage.mutex);
        duplicate_batches = storage.batches.size() - before;
    }

    const auto& expected = frozen.expected().get_object();
    CHECK(duplicate.value().id.value == first.value().id.value);
    CHECK(duplicate_batches == static_cast<std::size_t>(expected.at("duplicateBatches").get_number()));

    auto first_status = consume(first.value().status());
    REQUIRE(first_status.has_value());
    CHECK(first_status.value().status == expected.at("originalStatus").get_string());

    auto wrong = consume(harness->submit(root.value(), write_submission("note", std::nullopt, std::nullopt, "req-9")));
    CHECK_FALSE(wrong.has_value());
    CHECK(wrong.error().message == expected.at("wrongType").get_object().at("message").get_string());

    held.release();
    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("a busy input with whenBusy reject throws ConversationBusy and writes no submission",
        "[durable][workflow][issue1080]") {
    const auto frozen =
            frozen_1055("a busy input with whenBusy reject throws ConversationBusy and writes no submission");

    MemoryStorage storage;
    HeldStep held;
    HarnessOptions options;
    options.stream = [&held](ai::Model, ai::AiContext, ai::SimpleStreamOptions s) { return held.step(s.stop_token); };

    auto opened = consume(Harness::open(storage, std::move(options)));
    REQUIRE(opened.has_value());
    auto harness = std::move(opened.value());
    auto root = consume(harness->root());
    REQUIRE(root.has_value());

    auto first = consume(harness->submit(root.value(), text_input("a")));
    REQUIRE(first.has_value());
    REQUIRE(held.reached.try_acquire_for(std::chrono::seconds(5)));

    auto subs_before = storage.scan_submissions(100);
    REQUIRE(subs_before.has_value());

    auto rejected = consume(harness->submit(root.value(), text_input("rejected", WhenBusy::Reject)));
    CHECK_FALSE(rejected.has_value());
    CHECK(rejected.error().code == support::ErrorCode::Busy);
    const auto& expected = frozen.expected().get_object();
    CHECK(rejected.error().message == expected.at("error").get_object().at("message").get_string());

    auto subs_after = storage.scan_submissions(100);
    REQUIRE(subs_after.has_value());
    CHECK(subs_before->size() == static_cast<std::size_t>(expected.at("submissionsBefore").get_number()));
    CHECK(subs_after->size() == static_cast<std::size_t>(expected.at("submissionsAfter").get_number()));

    auto items = inbox_items(storage, root.value());
    CHECK(items.size() == static_cast<std::size_t>(expected.at("inboxLength").get_number()));

    held.release();
    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("admission commits the queued submission and its inbox change in one storage batch",
        "[durable][workflow][issue1080]") {
    const auto frozen =
            frozen_1055("admission commits the queued submission and its inbox change in one storage batch");

    RecordingStorage storage;
    HeldStep held;
    HarnessOptions options;
    options.stream = [&held](ai::Model, ai::AiContext, ai::SimpleStreamOptions s) { return held.step(s.stop_token); };

    auto opened = consume(Harness::open(storage, std::move(options)));
    REQUIRE(opened.has_value());
    auto harness = std::move(opened.value());
    auto root = consume(harness->root());
    REQUIRE(root.has_value());

    auto first = consume(harness->submit(root.value(), text_input("a")));
    REQUIRE(first.has_value());
    REQUIRE(held.reached.try_acquire_for(std::chrono::seconds(5)));

    std::size_t before = 0;
    {
        std::lock_guard lock(storage.mutex);
        before = storage.batches.size();
    }

    auto queued = consume(harness->submit(root.value(), text_input("queued")));
    REQUIRE(queued.has_value());

    std::vector<StorageWrite> batch;
    {
        std::lock_guard lock(storage.mutex);
        REQUIRE(storage.batches.size() > before);
        batch = storage.batches[before];
    }
    REQUIRE(batch.size() == 2);

    const auto* sub_write =
            batch[0].type == "submission" ? &batch[0] : (batch[1].type == "submission" ? &batch[1] : nullptr);
    const auto* doc_write =
            batch[0].type == "document.change" ? &batch[0] : (batch[1].type == "document.change" ? &batch[1] : nullptr);
    REQUIRE(sub_write != nullptr);
    REQUIRE(doc_write != nullptr);

    const auto& expected = frozen.expected().get_object();
    REQUIRE(sub_write->submission.has_value());
    CHECK(sub_write->submission->status == expected.at("submissionStatus").get_string());
    CHECK(sub_write->submission->type == expected.at("submissionType").get_string());

    bool found_items_op = false;
    for (const auto& op : doc_write->document_ops) {
        if (const auto* set_op = std::get_if<chord::SetOp>(&op)) {
            if (!set_op->path.empty()) {
                if (const auto* key = std::get_if<chord::PathKey>(&set_op->path[0])) {
                    if (key->key == "items") found_items_op = true;
                }
            }
        }
    }
    CHECK(found_items_op);

    held.release();
    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("queue modes decide whether all follow-ups or one are placed at the final boundary",
        "[durable][workflow][issue1080]") {
    const auto frozen =
            frozen_1055("queue modes decide whether all follow-ups or one are placed at the final boundary");

    auto run_mode = [](QueueMode mode) {
        struct ModeResult {
            std::string two_after_first;
            chord::Json transcript;
        };

        MemoryStorage storage;
        HeldStep held;
        std::atomic<int> turn{0};
        HarnessOptions options;
        options.settings.follow_up_mode = mode;
        options.stream = [&held, &turn](ai::Model, ai::AiContext, ai::SimpleStreamOptions s) {
            const int current = turn.fetch_add(1);
            if (current == 0) {
                return held.step(s.stop_token);
            }
            if (current == 1) {
                return ai::ModelStream{ai::ModelStreamProducer{[](ai::AssistantEventSink sink,
                                                                       ai::ModelStreamCompletion completion) noexcept {
                    auto message = ai::assistant_text_message("a");
                    auto delivered = sink(ai::AssistantDoneEvent{.reason = message.stop_reason, .message = message});
                    if (!delivered)
                        completion(std::unexpected(delivered.error()));
                    else
                        completion(std::move(message));
                }}};
            }
            return ai::ModelStream{ai::ModelStreamProducer{[](ai::AssistantEventSink sink,
                                                                   ai::ModelStreamCompletion completion) noexcept {
                auto message = ai::assistant_text_message("b");
                auto delivered = sink(ai::AssistantDoneEvent{.reason = message.stop_reason, .message = message});
                if (!delivered)
                    completion(std::unexpected(delivered.error()));
                else
                    completion(std::move(message));
            }}};
        };

        auto opened = consume(Harness::open(storage, std::move(options)));
        REQUIRE(opened.has_value());
        auto harness = std::move(opened.value());
        auto root = consume(harness->root());
        REQUIRE(root.has_value());

        auto first = consume(harness->submit(root.value(), text_input("a")));
        REQUIRE(first.has_value());
        REQUIRE(held.reached.try_acquire_for(std::chrono::seconds(5)));

        auto one = consume(harness->submit(root.value(), text_input("f1")));
        REQUIRE(one.has_value());
        auto two = consume(harness->submit(root.value(), text_input("f2")));
        REQUIRE(two.has_value());

        held.release();

        REQUIRE(consume(first.value().wait()).has_value());
        REQUIRE(consume(one.value().wait()).has_value());

        auto two_status = consume(two.value().status());
        REQUIRE(two_status.has_value());
        std::string two_after_first = two_status.value().status;

        REQUIRE(consume(two.value().wait()).has_value());

        auto entries = storage.scan_entries(root.value(), 100);
        REQUIRE(entries.has_value());
        std::ranges::reverse(*entries);

        REQUIRE(consume(harness->close()).has_value());
        return ModeResult{.two_after_first = std::move(two_after_first), .transcript = transcript(*entries)};
    };

    const auto& expected = frozen.expected().get_object();
    auto all_res = run_mode(QueueMode::All);
    CHECK(all_res.two_after_first == expected.at("all").get_object().at("twoAfterFirst").get_string());
    CHECK(json_equals(all_res.transcript, expected.at("all").get_object().at("transcript").get_array()));

    auto one_res = run_mode(QueueMode::OneAtATime);
    CHECK(one_res.two_after_first == expected.at("oneAtATime").get_object().at("twoAfterFirst").get_string());
    CHECK(json_equals(one_res.transcript, expected.at("oneAtATime").get_object().at("transcript").get_array()));
}

TEST_CASE("abort reports queued, placed and settled outcomes and removes the queued item",
        "[durable][workflow][issue1080]") {
    const auto frozen = frozen_1055("abort reports queued, placed and settled outcomes and removes the queued item");

    MemoryStorage storage;
    HeldStep held;
    HarnessOptions options;
    options.stream = [&held](ai::Model, ai::AiContext, ai::SimpleStreamOptions s) { return held.step(s.stop_token); };

    auto opened = consume(Harness::open(storage, std::move(options)));
    REQUIRE(opened.has_value());
    auto harness = std::move(opened.value());
    auto root = consume(harness->root());
    REQUIRE(root.has_value());

    auto placed = consume(harness->submit(root.value(), text_input("a")));
    REQUIRE(placed.has_value());
    REQUIRE(held.reached.try_acquire_for(std::chrono::seconds(5)));

    auto queued = consume(harness->submit(root.value(), text_input("b")));
    REQUIRE(queued.has_value());

    ConversationId other{999999};

    const auto& expected = frozen.expected().get_object();
    const auto& results_expected = expected.at("results").get_object();

    auto placed_abort = consume(placed.value().abort());
    REQUIRE(placed_abort.has_value());
    CHECK(placed_abort.value() == results_expected.at("placed").get_string());

    auto wrong_conv = consume(harness->abort_submission(queued.value().id, other));
    REQUIRE(wrong_conv.has_value());
    CHECK(wrong_conv.value() == results_expected.at("wrongConversation").get_string());

    auto queued_abort = consume(harness->abort_submission(queued.value().id, root.value()));
    REQUIRE(queued_abort.has_value());
    CHECK(queued_abort.value() == results_expected.at("queued").get_string());

    auto again_abort = consume(harness->abort_submission(queued.value().id, root.value()));
    REQUIRE(again_abort.has_value());
    CHECK(again_abort.value() == results_expected.at("again").get_string());

    auto unknown_abort = consume(harness->abort_submission(SubmissionId{999999}));
    REQUIRE(unknown_abort.has_value());
    CHECK(unknown_abort.value() == results_expected.at("unknown").get_string());

    auto queued_status = consume(queued.value().status());
    REQUIRE(queued_status.has_value());
    const auto& queued_expected = expected.at("queuedRecord").get_object();
    CHECK(queued_status.value().status == queued_expected.at("status").get_string());
    CHECK(queued_status.value().reason == queued_expected.at("reason").get_string());

    auto items = inbox_items(storage, root.value());
    CHECK(items.size() == static_cast<std::size_t>(expected.at("inboxLength").get_number()));

    held.release();
    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("conversation abort withdraws queued inputs, keeps queued writes and settles the placed input unanswered",
        "[durable][workflow][issue1080]") {
    const auto frozen = frozen_1055(
            "conversation abort withdraws queued inputs, keeps queued writes and settles the placed input unanswered");

    MemoryStorage storage;
    HeldStep held;
    HarnessOptions options;
    options.stream = [&held](ai::Model, ai::AiContext, ai::SimpleStreamOptions s) { return held.step(s.stop_token); };

    auto opened = consume(Harness::open(storage, std::move(options)));
    REQUIRE(opened.has_value());
    auto harness = std::move(opened.value());
    auto root = consume(harness->root());
    REQUIRE(root.has_value());

    auto placed = consume(harness->submit(root.value(), text_input("a")));
    REQUIRE(placed.has_value());
    REQUIRE(held.reached.try_acquire_for(std::chrono::seconds(5)));

    auto queued_input = consume(harness->submit(root.value(), text_input("b")));
    REQUIRE(queued_input.has_value());

    std::vector<chord::Json> write_model{chord::Json{chord::Json::Object{
            {"role", chord::Json{"user"}}, {"content", chord::Json{"n"}}, {"timestamp", chord::Json{0.0}}}}};
    auto queued_write =
            consume(harness->submit(root.value(), write_submission("note", std::nullopt, std::move(write_model))));
    REQUIRE(queued_write.has_value());

    auto placed_before = consume(placed.value().status());
    auto input_before = consume(queued_input.value().status());
    auto write_before = consume(queued_write.value().status());
    REQUIRE(placed_before.has_value());
    REQUIRE(input_before.has_value());
    REQUIRE(write_before.has_value());
    const auto& expected = frozen.expected().get_object();
    const auto& before_expected = expected.at("before").get_array();
    CHECK(placed_before.value().status == before_expected.at(0).get_string());
    CHECK(input_before.value().status == before_expected.at(1).get_string());
    CHECK(write_before.value().status == before_expected.at(2).get_string());

    REQUIRE(consume(harness->abort_conversation(root.value())).has_value());

    auto placed_after = consume(placed.value().status());
    auto input_after = consume(queued_input.value().status());
    auto write_after = consume(queued_write.value().status());
    REQUIRE(placed_after.has_value());
    REQUIRE(input_after.has_value());
    REQUIRE(write_after.has_value());

    const auto& after_expected = expected.at("after").get_array();
    CHECK(placed_after.value().status == after_expected.at(0).get_object().at("status").get_string());
    CHECK(placed_after.value().reason == after_expected.at(0).get_object().at("reason").get_string());
    CHECK(input_after.value().status == after_expected.at(1).get_object().at("status").get_string());
    CHECK(input_after.value().reason == after_expected.at(1).get_object().at("reason").get_string());
    CHECK(write_after.value().status == after_expected.at(2).get_object().at("status").get_string());
    CHECK_FALSE(write_after.value().reason.has_value());

    auto items = inbox_items(storage, root.value());
    REQUIRE(items.size() == 1);
    CHECK(items[0].as_object()->at("mode") == chord::Json{"write"});

    auto live_doc = storage.find_document("pi.live:v1", root.value());
    REQUIRE(live_doc.has_value());
    REQUIRE(live_doc->has_value());
    auto live_val = storage.document((*live_doc)->id);
    REQUIRE(live_val.has_value());
    REQUIRE(live_val->has_value());
    CHECK((*live_val)->as_object()->find("run") == (*live_val)->as_object()->end());

    held.release();
    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("cancelling a submission wait leaves the submission placed until it settles",
        "[durable][workflow][issue1080]") {
    const auto frozen = frozen_1055("cancelling a submission wait leaves the submission placed until it settles");

    MemoryStorage storage;
    HeldStep held;
    HarnessOptions options;
    options.stream = [&held](ai::Model, ai::AiContext, ai::SimpleStreamOptions s) { return held.step(s.stop_token); };

    auto opened = consume(Harness::open(storage, std::move(options)));
    REQUIRE(opened.has_value());
    auto harness = std::move(opened.value());
    auto root = consume(harness->root());
    REQUIRE(root.has_value());

    auto first = consume(harness->submit(root.value(), text_input("a")));
    REQUIRE(first.has_value());
    REQUIRE(held.reached.try_acquire_for(std::chrono::seconds(5)));

    std::stop_source stop_source;
    auto waiting = first.value().wait(stop_source.get_token());
    stop_source.request_stop();

    auto cancelled = consume(std::move(waiting));
    CHECK_FALSE(cancelled.has_value());
    const auto& expected = frozen.expected().get_object();
    CHECK(cancelled.error().message == expected.at("error").get_object().at("message").get_string());

    auto status_after_cancel = consume(first.value().status());
    REQUIRE(status_after_cancel.has_value());
    CHECK(status_after_cancel.value().status == expected.at("afterCancel").get_string());

    held.release();

    auto settled = consume(first.value().wait());
    REQUIRE(settled.has_value());
    CHECK(settled.value().status == expected.at("settled").get_string());

    REQUIRE(consume(harness->close()).has_value());
}

} // namespace cch::tests::durable
