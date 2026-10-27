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
#include <latch>
#include <memory>
#include <mutex>
#include <optional>
#include <semaphore>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <variant>
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
    [[nodiscard]] support::Expected<std::vector<DocumentRecord>> scan_conversation_documents(
            ConversationId conversation_id, DocumentPoint at, std::size_t limit) override {
        return inner_->scan_conversation_documents(conversation_id, at, limit);
    }
    [[nodiscard]] support::Expected<std::optional<DocumentRecord>> find_document(
            std::string_view kind, const DocumentAddress& address, DocumentPoint at) override {
        return inner_->find_document(kind, address, at);
    }
    [[nodiscard]] support::Expected<std::optional<StoredDocument>> document(DocumentId id, DocumentPoint at) override {
        return inner_->document(id, at);
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
    auto doc_rec =
            storage.find_document("pi.inbox:v1", DocumentAddress{.conversation_id = root}, DocumentPoint::current());
    if (!doc_rec || !doc_rec->has_value()) return {};
    auto content = storage.document((*doc_rec)->id, DocumentPoint::current());
    if (!content || !content->has_value()) return {};
    const auto* obj = (*content)->value.as_object();
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

FrozenScenario frozen_reset(std::string_view name) {
    auto scenario = FrozenScenario::load("fixtures/pi-v1.3.0/audit-1080/reset-writes.json", name);
    REQUIRE(scenario.has_value());
    return *scenario;
}

/// The workflow's inbox document definition, used by tests to stage malformed
/// persisted items the production readers must reject with typed errors.
DocumentDefinition test_inbox_document() {
    // The same semantics the root seeds pi.inbox:v1 with (frozen pi
    // `InboxDoc`), or the persisted record fails the scope check.
    return detail::builtin_document("pi.inbox:v1", DocumentHistory::Latest, DocumentFork::Initial, [] {
        return chord::Json{chord::Json::Object{{"items", chord::Json{chord::Json::Array{}}}}};
    });
}

chord::Path inbox_items_member() { return chord::Path{chord::path_key("items")}; }

/// Replace the persisted inbox items with one staged document write.
Outcome<void> set_inbox_items(Harness& harness, ConversationId root, chord::Json::Array items) {
    return consume(harness.commit([root, items = std::move(items)](Transaction& tx) mutable -> support::Expected<void> {
        auto inbox = tx.doc(test_inbox_document(), DocumentAddress{.conversation_id = root});
        if (!inbox) {
            return std::unexpected(std::move(inbox.error()));
        }
        if (auto set = inbox->set(inbox_items_member(), chord::Json{std::move(items)}); !set) {
            return std::unexpected(std::move(set.error()));
        }
        return {};
    }));
}

/// Whether a persisted inbox item is a write whose entry pins `head: "self"`.
bool has_self_head(const chord::Json& item) {
    const auto* object = item.as_object();
    if (object == nullptr) return false;
    const auto entry = object->find("entry");
    if (entry == object->end()) return false;
    const auto* draft = entry->second.as_object();
    if (draft == nullptr) return false;
    const auto head = draft->find("head");
    return head != draft->end() && head->second.as_string() != nullptr && *head->second.as_string() == "self";
}

/// Role:text projection of one request message, matching the frozen
/// `requestMessages` helper (the system baseline text is elided).
std::string message_projection(const ai::MessageVariant& message) {
    return std::visit(
            [](const auto& value) -> std::string {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, ai::SystemMessage>) {
                    return "system:";
                } else if constexpr (std::is_same_v<T, ai::UserMessage>) {
                    std::string text;
                    if (const auto* plain = std::get_if<std::string>(&value.content)) {
                        text = *plain;
                    } else {
                        for (const ai::Content& block : std::get<std::vector<ai::Content>>(value.content)) {
                            if (const auto* content = std::get_if<ai::TextContent>(&block)) text += content->text;
                        }
                    }
                    return "user:" + std::move(text);
                } else if constexpr (std::is_same_v<T, ai::AssistantMessage>) {
                    std::string text;
                    for (const ai::AssistantContent& block : value.content) {
                        if (const auto* content = std::get_if<ai::TextContent>(&block)) text += content->text;
                    }
                    return "assistant:" + std::move(text);
                } else {
                    return std::string{"message"};
                }
            },
            message);
}

/// The frozen-normalized projection of one model request: the elided system
/// marker first, then each carried message as `role:text`.
std::vector<std::string> request_projection(const ai::AiContext& context) {
    std::vector<std::string> projection;
    if (context.system_prompt.has_value()) projection.emplace_back("system:");
    for (const ai::MessageVariant& message : context.messages) {
        projection.push_back(message_projection(message));
    }
    return projection;
}

/// The registered `hold` tool and its section, shared by the reset scenarios.
Extension hold_tool_extension(std::binary_semaphore& entered, std::binary_semaphore& release_gate) {
    Extension extension{.name = "held-tool"};
    extension.tools.push_back(
            ToolRegistration{.declaration = ai::Tool{.name = "hold", .description = "Waits for the test"},
                    .execute = [&entered, &release_gate](support::JsonValue, std::stop_token) {
                        entered.release();
                        release_gate.acquire();
                        return support::AsyncResult<std::vector<ai::Content>>{
                                std::vector<ai::Content>{ai::text_content("held")}};
                    }});
    extension.sections.push_back(PromptSection{.key = "audit-note", .text = "reset context under audit"});
    return extension;
}

/// One model request that only emits a `hold` tool call (frozen faux first response).
ai::ModelStream tool_call_stream() {
    return ai::ModelStream{ai::ModelStreamProducer{
            [](ai::AssistantEventSink sink, ai::ModelStreamCompletion completion) mutable noexcept {
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

/// One model request that answers with `text` (frozen faux answer response).
ai::ModelStream answer_stream(std::string text) {
    return ai::ModelStream{ai::ModelStreamProducer{[text = std::move(text)](ai::AssistantEventSink sink,
                                                           ai::ModelStreamCompletion completion) mutable noexcept {
        auto message = ai::assistant_text_message(std::move(text));
        auto delivered = sink(ai::AssistantDoneEvent{.reason = message.stop_reason, .message = message});
        if (!delivered)
            completion(std::unexpected(delivered.error()));
        else
            completion(std::move(message));
    }}};
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

    auto live_doc = storage.find_document(
            "pi.live:v1", DocumentAddress{.conversation_id = root.value()}, DocumentPoint::current());
    REQUIRE(live_doc.has_value());
    REQUIRE(live_doc->has_value());
    auto live_val = storage.document((*live_doc)->id, DocumentPoint::current());
    REQUIRE(live_val.has_value());
    REQUIRE(live_val->has_value());
    CHECK((*live_val)->value.as_object()->find("run") == (*live_val)->value.as_object()->end());

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

    auto live_doc = storage.find_document(
            "pi.live:v1", DocumentAddress{.conversation_id = root.value()}, DocumentPoint::current());
    REQUIRE(live_doc.has_value());
    REQUIRE(live_doc->has_value());
    auto live_val = storage.document((*live_doc)->id, DocumentPoint::current());
    REQUIRE(live_val.has_value());
    REQUIRE(live_val->has_value());
    CHECK((*live_val)->value.as_object()->find("run") == (*live_val)->value.as_object()->end());

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

    auto live_doc = storage.find_document(
            "pi.live:v1", DocumentAddress{.conversation_id = root.value()}, DocumentPoint::current());
    REQUIRE(live_doc.has_value());
    REQUIRE(live_doc->has_value());
    auto live_val = storage.document((*live_doc)->id, DocumentPoint::current());
    REQUIRE(live_val.has_value());
    REQUIRE(live_val->has_value());
    CHECK((*live_val)->value.as_object()->find("run") == (*live_val)->value.as_object()->end());

    held.release();
    REQUIRE(consume(harness->close()).has_value());
}

// ===========================================================================
// audit-1080 reset-boundary observations and review-fix regressions
// ===========================================================================

TEST_CASE("a pinned-head write at the entry before the reset head settles unanswered and stale while one at the reset "
          "head is done",
        "[durable][workflow][issue1080]") {
    const auto frozen = frozen_reset("a pinned-head write at the entry before the reset head settles unanswered and "
                                     "stale while one at the reset head is done");
    const auto& expected = frozen.expected().get_object();

    MemoryStorage storage;
    HarnessOptions options;
    options.stream = [](ai::Model, ai::AiContext, ai::SimpleStreamOptions) {
        FAIL("stream must not be called for idle writes");
        return ai::ModelStream{ai::ModelStreamProducer{[](auto, auto) noexcept {}}};
    };
    auto opened = consume(Harness::open(storage, std::move(options)));
    REQUIRE(opened.has_value());
    auto harness = std::move(opened.value());
    auto root = consume(harness->root());
    REQUIRE(root.has_value());

    // The entry before the reset head: an idle display-only write.
    auto old = consume(harness->submit(root.value(), write_submission("note")));
    REQUIRE(old.has_value());
    auto old_settled = consume(old.value().wait());
    REQUIRE(old_settled.has_value());
    REQUIRE(old_settled.value().entry.has_value());
    const EntryId old_entry = *old_settled.value().entry;

    // The reset head itself: a head:self write on the idle conversation.
    auto reset = consume(harness->submit(root.value(),
            write_submission("pi.reset", std::nullopt, std::nullopt, std::nullopt, EntryHeadVariant{SelfEntryHead{}})));
    REQUIRE(reset.has_value());
    auto reset_settled = consume(reset.value().wait());
    REQUIRE(reset_settled.has_value());
    REQUIRE(reset_settled.value().entry.has_value());
    const EntryId reset_entry = *reset_settled.value().entry;
    CHECK(old_entry.value < reset_entry.value); // frozen oldBeforeReset

    const auto stored_reset = storage.entry(reset_entry);
    REQUIRE(stored_reset.has_value());
    REQUIRE(stored_reset->has_value());
    CHECK(stored_reset->value().entry.kind == expected.at("resetHead").get_object().at("kind").get_string());
    REQUIRE(stored_reset->value().entry.head.has_value());
    CHECK(stored_reset->value().entry.head->value == reset_entry.value); // frozen headEqualsId

    // A write pinned before the reset head is stale.
    auto stale = consume(harness->submit(root.value(),
            write_submission("summary", std::nullopt, std::nullopt, std::nullopt, EntryHeadVariant{old_entry})));
    REQUIRE(stale.has_value());
    auto stale_settled = consume(stale.value().wait());
    REQUIRE(stale_settled.has_value());
    const auto& stale_expected = expected.at("stale").get_object();
    CHECK(stale_settled.value().status == stale_expected.at("status").get_string());
    REQUIRE(stale_settled.value().reason.has_value());
    CHECK(*stale_settled.value().reason == stale_expected.at("reason").get_string());
    CHECK_FALSE(stale_settled.value().entry.has_value());
    CHECK_FALSE(stale_settled.value().answer.has_value());

    // A write pinned at the reset head lands, carrying the reset head.
    auto fresh = consume(harness->submit(root.value(),
            write_submission("summary", std::nullopt, std::nullopt, std::nullopt, EntryHeadVariant{reset_entry})));
    REQUIRE(fresh.has_value());
    auto fresh_settled = consume(fresh.value().wait());
    REQUIRE(fresh_settled.has_value());
    const auto& fresh_expected = expected.at("fresh").get_object();
    CHECK(fresh_settled.value().status == fresh_expected.at("status").get_string());
    CHECK_FALSE(fresh_settled.value().reason.has_value());
    REQUIRE(fresh_settled.value().entry.has_value());
    CHECK_FALSE(fresh_settled.value().answer.has_value());
    const auto stored_fresh = storage.entry(*fresh_settled.value().entry);
    REQUIRE(stored_fresh.has_value());
    REQUIRE(stored_fresh->has_value());
    REQUIRE(stored_fresh->value().entry.head.has_value());
    CHECK(stored_fresh->value().entry.head->value == reset_entry.value); // frozen freshEntryHead == resetHead.id

    auto entries = storage.scan_entries(root.value(), 100);
    REQUIRE(entries.has_value());
    std::ranges::reverse(*entries);
    CHECK(json_equals(transcript(*entries), *frozen.member("transcript")));

    CHECK(inbox_items(storage, root.value()).empty());
    CHECK(expected.at("inbox").get_number() == 0);

    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("a queued head:self reset at the postTools boundary ends the current run unanswered and reset and starts the "
          "selected queued input in the reset context",
        "[durable][workflow][issue1080]") {
    const auto frozen = frozen_reset("a queued head:self reset at the postTools boundary ends the current run "
                                     "unanswered and reset and starts the selected queued input in the reset context");
    const auto& expected = frozen.expected().get_object();

    std::binary_semaphore entered{0};
    std::binary_semaphore release_gate{0};
    Registry registry;
    REQUIRE(registry.install(hold_tool_extension(entered, release_gate)).has_value());

    std::atomic<int> turn{0};
    std::vector<std::vector<std::string>> requests;
    HarnessOptions options{.registry = &registry};
    options.stream = [&turn, &requests](ai::Model, ai::AiContext context, ai::SimpleStreamOptions) {
        if (turn.fetch_add(1) == 0) {
            return tool_call_stream();
        }
        requests.push_back(request_projection(context));
        return answer_stream("fresh");
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

    auto follow_up = consume(harness->submit(root.value(), text_input("f")));
    REQUIRE(follow_up.has_value());
    auto reset = consume(harness->submit(root.value(),
            write_submission("pi.reset", std::nullopt, std::nullopt, std::nullopt, EntryHeadVariant{SelfEntryHead{}})));
    REQUIRE(reset.has_value());

    const auto& queued_expected = expected.at("queued").get_object();
    auto first_before = consume(first.value().status());
    auto follow_before = consume(follow_up.value().status());
    auto reset_before = consume(reset.value().status());
    REQUIRE(first_before.has_value());
    REQUIRE(follow_before.has_value());
    REQUIRE(reset_before.has_value());
    CHECK(first_before.value().status == queued_expected.at("first").get_string());
    CHECK(follow_before.value().status == queued_expected.at("followUp").get_string());
    CHECK(reset_before.value().status == "queued");

    auto items_before = inbox_items(storage, root.value());
    const auto& inbox_expected = queued_expected.at("inbox").get_array();
    REQUIRE(items_before.size() == inbox_expected.size());
    for (std::size_t i = 0; i < inbox_expected.size(); ++i) {
        const auto* item = items_before[i].as_object();
        REQUIRE(item != nullptr);
        CHECK(item->at("mode") == chord::Json{inbox_expected.at(i).get_object().at("mode").get_string()});
        CHECK(has_self_head(items_before[i]) == inbox_expected.at(i).get_object().at("isReset").get_boolean());
    }
    REQUIRE(inbox_expected.size() == 2);
    CHECK(items_before[0].as_object()->at("id") == chord::Json{static_cast<double>(follow_up.value().id.value)});
    CHECK(items_before[1].as_object()->at("id") == chord::Json{static_cast<double>(reset.value().id.value)});

    release_gate.release();

    auto settled_follow_up = consume(follow_up.value().wait());
    REQUIRE(settled_follow_up.has_value());

    auto entries = storage.scan_entries(root.value(), 100);
    REQUIRE(entries.has_value());
    std::ranges::reverse(*entries);
    CHECK(json_equals(transcript(*entries), *frozen.member("transcript")));

    std::vector<EntryRecord> assistants;
    const EntryRecord* reset_entry = nullptr;
    for (const auto& entry : *entries) {
        if (entry.kind == "pi.assistant") assistants.push_back(entry);
        if (entry.kind == "pi.reset") reset_entry = &entry;
    }
    REQUIRE(assistants.size() == 2);
    const auto& answers_expected = expected.at("assistantAnswers").get_array();
    REQUIRE(answers_expected.size() == assistants.size());
    for (std::size_t i = 0; i < assistants.size(); ++i) {
        const auto text = text_of(assistants[i]);
        if (answers_expected.at(i).holds<std::string>()) {
            REQUIRE(text.has_value());
            CHECK(*text == answers_expected.at(i).get_string());
        } else {
            CHECK_FALSE(text.has_value());
        }
    }
    REQUIRE(reset_entry != nullptr);
    REQUIRE(reset_entry->head.has_value());
    CHECK(reset_entry->head->value == reset_entry->id.value); // frozen reset.headEqualsId

    auto first_settled = consume(first.value().status());
    REQUIRE(first_settled.has_value());
    const auto& first_expected = expected.at("first").get_object();
    CHECK(first_settled.value().status == first_expected.at("status").get_string());
    REQUIRE(first_settled.value().reason.has_value());
    CHECK(*first_settled.value().reason == first_expected.at("reason").get_string());
    REQUIRE(first_settled.value().entry.has_value());
    CHECK_FALSE(first_settled.value().answer.has_value());

    const auto& follow_expected = expected.at("followUp").get_object();
    CHECK(settled_follow_up.value().status == follow_expected.at("status").get_string());
    CHECK_FALSE(settled_follow_up.value().reason.has_value());
    REQUIRE(settled_follow_up.value().answer.has_value());
    CHECK(settled_follow_up.value().answer->value == assistants[1].id.value); // frozen answerIndex 1

    // The reset run's request carries only its own placed input and the
    // elided system marker, never the cut run's transcript.
    REQUIRE(requests.size() == 1);
    const auto& requests_expected = expected.at("followUpRequests").get_array();
    REQUIRE(requests_expected.size() == 1);
    chord::Json::Array observed_request;
    for (const auto& message : requests[0])
        observed_request.emplace_back(chord::Json{message});
    CHECK(json_equals(chord::Json{std::move(observed_request)}, requests_expected.at(0).get_array()));
    CHECK(turn.load() == static_cast<int>(expected.at("requestCount").get_number()));

    CHECK(inbox_items(storage, root.value()).empty());
    CHECK(expected.at("inbox").get_number() == 0);

    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("a display-only write behind a queued reset queues and lands after the reset when the run ends",
        "[durable][workflow][issue1080]") {
    const auto frozen = frozen_reset("a display-only write behind a queued reset queues and lands after the reset when "
                                     "the run ends");
    const auto& expected = frozen.expected().get_object();

    std::binary_semaphore entered{0};
    std::binary_semaphore release_gate{0};
    Registry registry;
    REQUIRE(registry.install(hold_tool_extension(entered, release_gate)).has_value());

    std::atomic<int> turn{0};
    HarnessOptions options{.registry = &registry};
    options.stream = [&turn](ai::Model, ai::AiContext, ai::SimpleStreamOptions) {
        if (turn.fetch_add(1) == 0) {
            return tool_call_stream();
        }
        FAIL("the run must end at the postTools boundary behind a queued reset");
        return ai::ModelStream{ai::ModelStreamProducer{[](auto, auto) noexcept {}}};
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

    auto reset = consume(harness->submit(root.value(),
            write_submission("pi.reset", std::nullopt, std::nullopt, std::nullopt, EntryHeadVariant{SelfEntryHead{}})));
    REQUIRE(reset.has_value());
    auto note = consume(harness->submit(root.value(), write_submission("note")));
    REQUIRE(note.has_value());

    const auto& before_expected = expected.at("before").get_object();
    auto first_before = consume(first.value().status());
    auto note_before = consume(note.value().status());
    REQUIRE(first_before.has_value());
    REQUIRE(note_before.has_value());
    CHECK(first_before.value().status == before_expected.at("first").get_string());
    CHECK(note_before.value().status == before_expected.at("note").get_string());

    auto items_before = inbox_items(storage, root.value());
    const auto& inbox_expected = before_expected.at("inbox").get_array();
    REQUIRE(items_before.size() == inbox_expected.size());
    for (std::size_t i = 0; i < inbox_expected.size(); ++i) {
        const auto* item = items_before[i].as_object();
        REQUIRE(item != nullptr);
        CHECK(item->at("mode") == chord::Json{inbox_expected.at(i).get_object().at("mode").get_string()});
        CHECK(has_self_head(items_before[i]) == inbox_expected.at(i).get_object().at("isReset").get_boolean());
    }
    REQUIRE(inbox_expected.size() == 2);
    CHECK(items_before[0].as_object()->at("id") == chord::Json{static_cast<double>(reset.value().id.value)});
    CHECK(items_before[1].as_object()->at("id") == chord::Json{static_cast<double>(note.value().id.value)});

    release_gate.release();

    auto settled_first = consume(first.value().wait());
    REQUIRE(settled_first.has_value());
    const auto& first_expected = expected.at("first").get_object();
    CHECK(settled_first.value().status == first_expected.at("status").get_string());
    REQUIRE(settled_first.value().reason.has_value());
    CHECK(*settled_first.value().reason == first_expected.at("reason").get_string());
    REQUIRE(settled_first.value().entry.has_value());
    CHECK_FALSE(settled_first.value().answer.has_value());

    auto settled_note = consume(note.value().wait());
    REQUIRE(settled_note.has_value());
    const auto& note_expected = expected.at("note").get_object();
    CHECK(settled_note.value().status == note_expected.at("status").get_string());
    CHECK_FALSE(settled_note.value().reason.has_value());
    REQUIRE(settled_note.value().entry.has_value());
    CHECK_FALSE(settled_note.value().answer.has_value());

    auto entries = storage.scan_entries(root.value(), 100);
    REQUIRE(entries.has_value());
    std::ranges::reverse(*entries);
    CHECK(json_equals(transcript(*entries), *frozen.member("transcript")));

    CHECK(inbox_items(storage, root.value()).empty());
    CHECK(expected.at("inbox").get_number() == 0);

    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("a write pinned before a head marker buried behind more than 4096 newer headless entries settles stale",
        "[durable][workflow][issue1080]") {
    MemoryStorage storage;
    HarnessOptions options;
    options.stream = [](ai::Model, ai::AiContext, ai::SimpleStreamOptions) {
        FAIL("stream must not be called for pinned writes");
        return ai::ModelStream{ai::ModelStreamProducer{[](auto, auto) noexcept {}}};
    };
    auto opened = consume(Harness::open(storage, std::move(options)));
    REQUIRE(opened.has_value());
    auto harness = std::move(opened.value());
    auto root = consume(harness->root());
    REQUIRE(root.has_value());

    auto old = consume(harness->submit(root.value(), write_submission("note")));
    REQUIRE(old.has_value());
    auto old_settled = consume(old.value().wait());
    REQUIRE(old_settled.has_value());
    REQUIRE(old_settled.value().entry.has_value());
    const EntryId old_entry = *old_settled.value().entry;

    auto reset = consume(harness->submit(root.value(),
            write_submission("pi.reset", std::nullopt, std::nullopt, std::nullopt, EntryHeadVariant{SelfEntryHead{}})));
    REQUIRE(reset.has_value());
    auto reset_settled = consume(reset.value().wait());
    REQUIRE(reset_settled.has_value());
    REQUIRE(reset_settled.value().entry.has_value());
    const EntryId reset_entry = *reset_settled.value().entry;
    CHECK(old_entry.value < reset_entry.value);

    // Bury the reset head behind more than one fixed 4096-entry scan page of
    // newer headless entries; the boundary head lookup must grow its page and
    // still find the marker instead of silently dropping the stale pin.
    constexpr int kHeadlessEntries = 4096 + 4;
    auto appended = consume(harness->commit([root_id = root.value()](Transaction& tx) -> support::Expected<void> {
        for (int i = 0; i < kHeadlessEntries; ++i) {
            auto entry = tx.append_entry(root_id,
                    "audit.headless",
                    chord::Json{chord::Json::Object{}},
                    std::nullopt,
                    std::vector<chord::Json>{});
            if (!entry) {
                return std::unexpected(std::move(entry.error()));
            }
        }
        return {};
    }));
    REQUIRE(appended.has_value());

    auto buried = consume(harness->submit(root.value(),
            write_submission("summary", std::nullopt, std::nullopt, std::nullopt, EntryHeadVariant{old_entry})));
    REQUIRE(buried.has_value());
    auto buried_settled = consume(buried.value().wait());
    REQUIRE(buried_settled.has_value());
    CHECK(buried_settled.value().status == "unanswered");
    REQUIRE(buried_settled.value().reason.has_value());
    CHECK(*buried_settled.value().reason == "stale");
    CHECK_FALSE(buried_settled.value().entry.has_value());

    auto at_head = consume(harness->submit(root.value(),
            write_submission("summary", std::nullopt, std::nullopt, std::nullopt, EntryHeadVariant{reset_entry})));
    REQUIRE(at_head.has_value());
    auto at_head_settled = consume(at_head.value().wait());
    REQUIRE(at_head_settled.has_value());
    CHECK(at_head_settled.value().status == "done");
    REQUIRE(at_head_settled.value().entry.has_value());
    const auto stored = storage.entry(*at_head_settled.value().entry);
    REQUIRE(stored.has_value());
    REQUIRE(stored->has_value());
    REQUIRE(stored->value().entry.head.has_value());
    CHECK(stored->value().entry.head->value == reset_entry.value);

    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("a malformed persisted inbox item fails admission with a typed validation error",
        "[durable][workflow][issue1080]") {
    MemoryStorage storage;
    std::atomic<int> turn{0};
    HarnessOptions options;
    options.stream = [&turn](ai::Model, ai::AiContext, ai::SimpleStreamOptions) {
        (void)turn.fetch_add(1);
        return answer_stream("hello");
    };
    auto opened = consume(Harness::open(storage, std::move(options)));
    REQUIRE(opened.has_value());
    auto harness = std::move(opened.value());
    auto root = consume(harness->root());
    REQUIRE(root.has_value());

    const auto mode_item = [](std::string mode) {
        return chord::Json{chord::Json::Object{{"mode", chord::Json{std::move(mode)}}}};
    };
    const std::vector<std::pair<chord::Json, std::string>> malformed{
            {chord::Json{42.0}, "An inbox item must be an object"},
            {mode_item("bogus"), "An inbox item mode must be \"write\", \"steer\" or \"followUp\""},
            {mode_item("write"), "An inbox item must have a entry"},
            {mode_item("followUp"), "An inbox item must have a id"},
    };
    for (const auto& [item, message] : malformed) {
        INFO("expected validation: " << message);
        REQUIRE(set_inbox_items(*harness, root.value(), chord::Json::Array{item}).has_value());
        auto rejected = consume(harness->submit(root.value(), text_input("trigger")));
        REQUIRE_FALSE(rejected.has_value());
        CHECK(rejected.error().code == support::ErrorCode::Validation);
        CHECK(rejected.error().message == message);
        // The rejected commit leaves no trace: no submission is created.
        auto submissions = storage.scan_submissions(100);
        REQUIRE(submissions.has_value());
        CHECK(submissions->empty());
    }

    // The session stays usable once the inbox is repaired.
    REQUIRE(set_inbox_items(*harness, root.value(), chord::Json::Array{}).has_value());
    auto admitted = consume(harness->submit(root.value(), text_input("hello")));
    REQUIRE(admitted.has_value());
    REQUIRE(consume(admitted.value().wait()).has_value());
    CHECK(turn.load() == 1);

    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("a malformed persisted inbox item fails conversation abort with a typed validation error",
        "[durable][workflow][issue1080]") {
    MemoryStorage storage;
    std::atomic<int> turn{0};
    HarnessOptions options;
    options.stream = [&turn](ai::Model, ai::AiContext, ai::SimpleStreamOptions) {
        (void)turn.fetch_add(1);
        return answer_stream("hello");
    };
    auto opened = consume(Harness::open(storage, std::move(options)));
    REQUIRE(opened.has_value());
    auto harness = std::move(opened.value());
    auto root = consume(harness->root());
    REQUIRE(root.has_value());

    REQUIRE(set_inbox_items(*harness,
            root.value(),
            chord::Json::Array{
                    chord::Json{chord::Json::Object{{"mode", chord::Json{"steer"}}}},
            })
                    .has_value());
    auto aborted = consume(harness->abort_conversation(root.value()));
    REQUIRE_FALSE(aborted.has_value());
    CHECK(aborted.error().code == support::ErrorCode::Validation);
    CHECK(aborted.error().message == "An inbox item must have a id");

    SubmissionId queued_id;
    // consume keeps queued_id alive through this commit's completion.
    REQUIRE(consume(
            harness->commit([conversation = root.value(), &queued_id](Transaction& tx) -> support::ExpectedVoid {
                auto queued = tx.create_submission(
                        SubmissionRecord{.conversation_id = conversation, .type = "input", .status = "queued"});
                if (!queued) return std::unexpected(queued.error());
                queued_id = queued->id;
                return {};
            })).has_value());
    REQUIRE(set_inbox_items(*harness, root.value(), chord::Json::Array{chord::Json{42.0}}).has_value());
    auto rejected = consume(harness->abort_submission(queued_id, root.value()));
    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error().code == support::ErrorCode::Validation);
    CHECK(rejected.error().message == "An inbox item must be an object");
    auto unchanged = storage.submission(queued_id);
    REQUIRE(unchanged.has_value());
    REQUIRE(unchanged->has_value());
    CHECK((**unchanged).status == "queued");

    // The session stays usable once the inbox is repaired.
    REQUIRE(set_inbox_items(*harness, root.value(), chord::Json::Array{}).has_value());
    auto admitted = consume(harness->submit(root.value(), text_input("hello")));
    REQUIRE(admitted.has_value());
    REQUIRE(consume(admitted.value().wait()).has_value());
    CHECK(turn.load() == 1);

    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE(
        "a submission abort reports already_placed when a boundary places the submission before the abort commit runs",
        "[durable][workflow][issue1080]") {
    MemoryStorage storage;
    HeldStep answer;
    std::binary_semaphore entered{0};
    std::binary_semaphore release_gate{0};
    Registry registry;
    REQUIRE(registry.install(hold_tool_extension(entered, release_gate)).has_value());

    std::atomic<int> turn{0};
    HarnessOptions options{.registry = &registry};
    options.stream = [&turn, &answer](ai::Model, ai::AiContext, ai::SimpleStreamOptions options) {
        if (turn.fetch_add(1) == 0) {
            return tool_call_stream();
        }
        return answer.step(options.stop_token);
    };
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
    auto steer_before = consume(steer.value().status());
    REQUIRE(steer_before.has_value());
    CHECK(steer_before.value().status == "queued");

    // Construct the operation while queued, then let the real postTools
    // boundary place it before initiating the abort transaction. Admission
    // must read the current record on the Session line, not retain the
    // queued state from operation construction.
    auto pending_abort = harness->abort_submission(steer.value().id, root.value());
    release_gate.release();
    REQUIRE(answer.reached.try_acquire_for(std::chrono::seconds(5)));
    auto placed = consume(steer.value().status());
    REQUIRE(placed.has_value());
    REQUIRE(placed.value().status == "placed");

    auto abort_outcome = consume(std::move(pending_abort));
    REQUIRE(abort_outcome.has_value());
    CHECK(abort_outcome.value() == "already_placed");
    answer.release();

    auto settled_steer = consume(steer.value().wait());
    REQUIRE(settled_steer.has_value());
    CHECK(settled_steer.value().status == "done");
    CHECK(settled_steer.value().answer.has_value());

    auto settled_first = consume(first.value().wait());
    REQUIRE(settled_first.has_value());
    CHECK(settled_first.value().status == "done");

    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("a conversation abort during run rollover settles the follow-up run and leaves the conversation idle",
        "[durable][workflow][issue1080]") {
    MemoryStorage storage;
    std::binary_semaphore entered{0};
    std::binary_semaphore release_gate{0};
    Registry registry;
    REQUIRE(registry.install(hold_tool_extension(entered, release_gate)).has_value());

    std::atomic<int> turn{0};
    HeldStep second;
    HeldStep third;
    HarnessOptions options{.registry = &registry};
    options.stream = [&](ai::Model, ai::AiContext, ai::SimpleStreamOptions s) -> ai::ModelStream {
        switch (turn.fetch_add(1)) {
        case 0:
            return tool_call_stream();
        case 1:
            return second.step(s.stop_token);
        default:
            return third.step(s.stop_token);
        }
    };
    auto opened = consume(Harness::open(storage, std::move(options)));
    REQUIRE(opened.has_value());
    auto harness = std::move(opened.value());
    auto root = consume(harness->root());
    REQUIRE(root.has_value());

    auto first = consume(harness->submit(root.value(), text_input("a")));
    REQUIRE(first.has_value());
    REQUIRE(entered.try_acquire_for(std::chrono::seconds(5)));

    // Run one continues past its postTools boundary into a second model
    // request; the follow-up is queued while that request is in flight, so it
    // is placed only at run one's final boundary.
    release_gate.release();
    REQUIRE(second.reached.try_acquire_for(std::chrono::seconds(5)));

    auto follow_up = consume(harness->submit(root.value(), text_input("f")));
    REQUIRE(follow_up.has_value());
    auto queued = consume(follow_up.value().status());
    REQUIRE(queued.has_value());
    REQUIRE(queued.value().status == "queued");

    // Run one answers. Its final boundary places the follow-up and starts run
    // two, whose model request is in flight when the abort arrives: the abort
    // must catch run two as well, not only the run it read first.
    second.release();
    REQUIRE(third.reached.try_acquire_for(std::chrono::seconds(5)));
    REQUIRE(consume(harness->abort_conversation(root.value())).has_value());

    auto settled_first = consume(first.value().wait());
    REQUIRE(settled_first.has_value());
    CHECK(settled_first.value().status == "done");
    CHECK(settled_first.value().answer.has_value());

    auto settled_follow_up = consume(follow_up.value().wait());
    REQUIRE(settled_follow_up.has_value());
    CHECK(settled_follow_up.value().status == "unanswered");
    REQUIRE(settled_follow_up.value().reason.has_value());
    CHECK(*settled_follow_up.value().reason == "aborted");
    // The follow-up was placed at run one's final boundary, so it carries the
    // entry it was placed at even though run two never answered it.
    REQUIRE(settled_follow_up.value().entry.has_value());

    auto live_doc = storage.find_document(
            "pi.live:v1", DocumentAddress{.conversation_id = root.value()}, DocumentPoint::current());
    REQUIRE(live_doc.has_value());
    REQUIRE(live_doc->has_value());
    auto live_val = storage.document((*live_doc)->id, DocumentPoint::current());
    REQUIRE(live_val.has_value());
    REQUIRE(live_val->has_value());
    CHECK((*live_val)->value.as_object()->find("run") == (*live_val)->value.as_object()->end());

    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("a queued reset placed at admission clears the replay cache of the run it starts",
        "[durable][workflow][issue1080]") {
    std::binary_semaphore entered{0};
    std::binary_semaphore release_gate{0};
    Registry registry;
    REQUIRE(registry.install(hold_tool_extension(entered, release_gate)).has_value());

    std::atomic<int> turn{0};
    std::vector<std::vector<std::string>> requests;
    HeldStep held;
    HarnessOptions options{.registry = &registry};
    options.stream = [&turn, &requests, &held](ai::Model, ai::AiContext context, ai::SimpleStreamOptions s) {
        if (turn.fetch_add(1) == 0) {
            return held.step(s.stop_token);
        }
        requests.push_back(request_projection(context));
        return answer_stream("fresh");
    };
    MemoryStorage storage;
    auto opened = consume(Harness::open(storage, std::move(options)));
    REQUIRE(opened.has_value());
    auto harness = std::move(opened.value());
    auto root = consume(harness->root());
    REQUIRE(root.has_value());

    auto first = consume(harness->submit(root.value(), text_input("a")));
    REQUIRE(first.has_value());
    REQUIRE(held.reached.try_acquire_for(std::chrono::seconds(5)));

    auto reset = consume(harness->submit(root.value(),
            write_submission("pi.reset", std::nullopt, std::nullopt, std::nullopt, EntryHeadVariant{SelfEntryHead{}})));
    REQUIRE(reset.has_value());
    auto reset_before = consume(reset.value().status());
    REQUIRE(reset_before.has_value());
    CHECK(reset_before.value().status == "queued");

    // Aborting the conversation keeps the queued write and ends the run
    // without a boundary; the reset stays queued for the next admission.
    REQUIRE(consume(harness->abort_conversation(root.value())).has_value());
    auto first_settled = consume(first.value().status());
    REQUIRE(first_settled.has_value());
    CHECK(first_settled.value().status == "unanswered");
    REQUIRE(first_settled.value().reason.has_value());
    CHECK(*first_settled.value().reason == "aborted");

    // The new input lands behind the queued reset; admission applies the
    // final boundary and starts one run in the reset context.
    auto second = consume(harness->submit(root.value(), text_input("b")));
    REQUIRE(second.has_value());
    auto settled_second = consume(second.value().wait());
    REQUIRE(settled_second.has_value());
    CHECK(settled_second.value().status == "done");
    CHECK(settled_second.value().answer.has_value());

    // The reset run's request carries only its own placed input and the
    // elided system marker, never the aborted run's transcript.
    REQUIRE(requests.size() == 1);
    CHECK(requests[0] == std::vector<std::string>{"system:", "user:b"});

    // The reset write placed as a pi.reset entry selecting itself, and its
    // submission settled done at that entry.
    auto entries = storage.scan_entries(root.value(), 100);
    REQUIRE(entries.has_value());
    const EntryRecord* reset_entry = nullptr;
    for (const auto& entry : *entries) {
        if (entry.kind == "pi.reset") reset_entry = &entry;
    }
    REQUIRE(reset_entry != nullptr);
    REQUIRE(reset_entry->head.has_value());
    CHECK(reset_entry->head->value == reset_entry->id.value);

    auto reset_settled = consume(reset.value().status());
    REQUIRE(reset_settled.has_value());
    CHECK(reset_settled.value().status == "done");
    REQUIRE(reset_settled.value().entry.has_value());
    CHECK(reset_settled.value().entry->value == reset_entry->id.value);

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

TEST_CASE("concurrent Harness admissions serialize every submission on the Session mutation line",
        "[durable][workflow][issue1080]") {
    MemoryStorage storage;
    auto opened = consume(Harness::open(storage));
    REQUIRE(opened.has_value());
    auto harness = std::move(opened.value());
    auto root = consume(harness->root());
    REQUIRE(root.has_value());

    constexpr int kCallers = 4;
    constexpr int kPerCaller = 32;
    std::latch start{kCallers};
    std::atomic<int> failures{0};
    std::vector<std::jthread> callers;
    for (int caller = 0; caller < kCallers; ++caller) {
        // Joining every caller below keeps the Harness and synchronization
        // objects alive until all admissions and completions have returned.
        callers.emplace_back([&start, &failures, owner = harness.get(), id = root.value()] {
            start.arrive_and_wait();
            for (int index = 0; index < kPerCaller; ++index) {
                auto admitted = consume(owner->submit(id, write_submission("note")));
                if (!admitted.has_value()) {
                    ++failures;
                    continue;
                }
                auto settled = consume(admitted.value().wait());
                if (!settled.has_value() || settled.value().status != "done") ++failures;
            }
        });
    }
    callers.clear();
    CHECK(failures.load() == 0);
    auto submissions = storage.scan_submissions(kCallers * kPerCaller + 1);
    REQUIRE(submissions.has_value());
    REQUIRE(submissions->size() == kCallers * kPerCaller);
    auto entries = storage.scan_entries(root.value(), kCallers * kPerCaller + 1);
    REQUIRE(entries.has_value());
    CHECK(entries->size() == submissions->size());
    REQUIRE(consume(harness->close()).has_value());
}

} // namespace cch::tests::durable
