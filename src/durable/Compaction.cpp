#include "durable/Compaction.hpp"

#include "durable/CommittedContext.hpp"
#include "WorkflowRuntime.hpp"

#include <cch/ai/Content.hpp>
#include <cch/ai/InferenceFailure.hpp>
#include <cch/ai/Message.hpp>
#include <cch/ai/Model.hpp>
#include <cch/ai/ModelStream.hpp>
#include <cch/ai/Usage.hpp>
#include <cch/chord/Json.hpp>
#include <cch/durable/Session.hpp>
#include <cch/durable/Storage.hpp>

#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace cch::durable {
namespace {

using Json = chord::Json;
using Object = Json::Object;
using Array = Json::Array;

support::Error validation(std::string message) {
    return support::make_error(support::ErrorCode::Validation, std::move(message));
}

Json numeric(std::int64_t value) { return Json{static_cast<double>(value)}; }

support::Expected<std::int64_t> as_integer(const Json& json) {
    const auto* number = json.as_number();
    if (number == nullptr || !(*number >= -0x1p63 && *number < 0x1p63) || std::trunc(*number) != *number) {
        return std::unexpected(validation("Expected an integer field"));
    }
    return static_cast<std::int64_t>(*number);
}

const Json* member(const Json& value, std::string_view key) {
    const auto* object = value.as_object();
    if (object == nullptr) return nullptr;
    const auto found = object->find(std::string{key});
    return found == object->end() ? nullptr : &found->second;
}

std::string_view json_string(const Json& value) {
    const std::string* string = value.as_string();
    return string == nullptr ? std::string_view{} : std::string_view{*string};
}

/// Wait for one asynchronous value on this thread. Every Harness task phase
/// runs on its own execution thread, so blocking until completion is the
/// frozen `await`-shaped contract; `runtime.commit` publishes progress.
template <typename T> support::Expected<T> await_result(support::AsyncResult<T> operation) {
    std::mutex mutex;
    std::condition_variable ready;
    std::optional<support::Expected<T>> outcome;
    std::move(operation).start([&mutex, &ready, &outcome](support::Expected<T> result) noexcept {
        std::lock_guard lock(mutex);
        outcome.emplace(std::move(result));
        ready.notify_one();
    });
    std::unique_lock lock(mutex);
    ready.wait(lock, [&outcome] { return outcome.has_value(); });
    return std::move(*outcome);
}

/// Sleep until `until` milliseconds on the Harness clock, waking early on
/// cancellation (frozen pi `TaskRuntime.sleep`).
support::ExpectedVoid sleep_until(const std::stop_token& token, const std::int64_t until, const std::int64_t now) {
    if (now >= until || token.stop_requested()) return {};
    std::mutex mutex;
    std::condition_variable ready;
    // The stop callback only wakes the wait; the stop check below is the
    // decision, so a lost notification cannot hang the retry phase.
    const std::stop_callback wake(token, [&ready] { ready.notify_all(); });
    std::unique_lock lock(mutex);
    ready.wait_for(lock, std::chrono::milliseconds(until - now), [&token] { return token.stop_requested(); });
    return {};
}

/// One entry record as the beforeCompact hook sees it (frozen pi
/// `CompactionInput.entries`).
Json entry_record_json(const EntryRecord& entry) {
    Object value{{"id", numeric(entry.id.value)},
            {"conversationId", numeric(entry.conversation_id.value)},
            {"kind", entry.kind},
            {"data", entry.data}};
    if (entry.head.has_value()) value.emplace("head", numeric(entry.head->value));
    Array model;
    for (const Json& message : entry.model)
        model.push_back(message);
    value.emplace("model", Json{std::move(model)});
    if (entry.by_task_id.has_value()) value.emplace("byTaskId", numeric(entry.by_task_id->value));
    return Json{std::move(value)};
}

constexpr std::string_view kSummaryPrefix =
        "The conversation history before this point was compacted into the following summary:\n\n<summary>\n";
constexpr std::string_view kSummarySuffix = "\n</summary>";

constexpr std::string_view kSummarizationSystemPrompt =
        "You are a context summarization assistant. Your task is to read a conversation between a user and an AI "
        "assistant, then produce a structured summary following the exact format specified.\n"
        "\n"
        "Do NOT continue the conversation. Do NOT respond to any questions in the conversation. ONLY output the "
        "structured summary.";

constexpr std::string_view kSummarizationPrompt =
        "The messages above are a conversation to summarize. Create a structured context checkpoint summary that "
        "another LLM will use to continue the work. If the conversation starts with an earlier summary, preserve its "
        "information and fold the newer messages into it.\n"
        "\n"
        "Use this EXACT format:\n"
        "\n"
        "## Goal\n"
        "[What is the user trying to accomplish? Can be multiple items if the session covers different tasks.]\n"
        "\n"
        "## Constraints & Preferences\n"
        "- [Any constraints, preferences, or requirements mentioned by user]\n"
        "- [Or \"(none)\" if none were mentioned]\n"
        "\n"
        "## Progress\n"
        "### Done\n"
        "- [x] [Completed tasks/changes]\n"
        "\n"
        "### In Progress\n"
        "- [ ] [Current work]\n"
        "\n"
        "### Blocked\n"
        "- [Issues preventing progress, if any]\n"
        "\n"
        "## Key Decisions\n"
        "- **[Decision]**: [Brief rationale]\n"
        "\n"
        "## Next Steps\n"
        "1. [Ordered list of what should happen next]\n"
        "\n"
        "## Critical Context\n"
        "- [Any data, examples, or references needed to continue]\n"
        "- [Or \"(none)\" if none were mentioned]\n"
        "\n"
        "Keep each section concise. Preserve exact file paths, function names, and error messages.";

/// The summarizer's user message: the serialized conversation, the prompt,
/// and any instructions (frozen pi `summaryPrompt`).
std::string summary_prompt(const std::vector<Json>& messages, const std::optional<std::string>& instructions) {
    const std::string focus = instructions.has_value() ? std::string{"\n\nAdditional focus: "} + *instructions : "";
    return std::string{"<conversation>\n"} + serialize_conversation(messages) + "\n</conversation>\n\n" +
           std::string{kSummarizationPrompt} + focus;
}

/// The summary of a clean `stop` with text and no tool call; anything else is
/// not a summary (frozen pi `summaryText`).
std::optional<std::string> summary_text(const ai::AssistantMessage& message) {
    if (message.stop_reason != ai::AssistantStopReason::Stop) return std::nullopt;
    std::string text;
    for (const ai::AssistantContent& content : message.content) {
        const auto* block = std::get_if<ai::TextContent>(&content);
        if (block == nullptr) continue;
        if (!text.empty()) text += "\n";
        text += block->text;
    }
    const std::size_t first = text.find_first_not_of(" \t\n\r");
    if (first == std::string::npos) return std::nullopt;
    const std::size_t last = text.find_last_not_of(" \t\n\r");
    return text.substr(first, last - first + 1);
}

/// The failure detail of one summarization attempt (frozen pi `summaryFailure`).
std::string summarization_failure(const ai::AssistantMessage& message) {
    if (message.stop_reason == ai::AssistantStopReason::Error ||
            message.stop_reason == ai::AssistantStopReason::Aborted) {
        return "Summarization failed: " +
               message.error_message.value_or(ai::stop_reason_to_string(message.stop_reason));
    }
    if (message.stop_reason == ai::AssistantStopReason::Length) {
        return "Summarization hit the token limit; the summary is incomplete";
    }
    const bool tool_call = std::ranges::any_of(message.content,
            [](const ai::AssistantContent& content) { return std::holds_alternative<ai::ToolCallContent>(content); });
    if (tool_call) return "Summarization attempted to call a tool";
    return "Summarization produced no text";
}

/// The user message a summary marker carries (frozen pi `placeSummary`).
Json summary_message_json(const std::string& summary, const std::int64_t now) {
    return Json{Object{{"role", "user"},
            {"content",
                    Json{Array{Json{Object{{"type", "text"},
                            {"text", Json{std::string{kSummaryPrefix} + summary + std::string{kSummarySuffix}}}}}}}},
            {"timestamp", numeric(now)}}};
}

/// The per-attempt delay of the durable retry policy: `baseDelayMs *
/// 2^(attempt-1)` capped by `maxAgentDelayMs` (frozen pi `retryDelayMs`).
std::int64_t retry_delay_ms(const RetryPolicy& policy, const std::int64_t attempt) {
    const std::int64_t exponent = std::max<std::int64_t>(0, attempt - 1);
    if (policy.base_delay_ms <= 0) return 0;
    if (exponent > 62) return policy.max_agent_delay_ms;
    const double delay = static_cast<double>(policy.base_delay_ms) * std::pow(2.0, static_cast<double>(exponent));
    return static_cast<std::int64_t>(std::min<double>(delay, static_cast<double>(policy.max_agent_delay_ms)));
}

/// The model identity and render options pinned at `select` and replayed by
/// every summarization attempt (frozen pi `SummaryRequest`).
struct SummaryRequest {
    std::int64_t attempt{1};
    std::string provider{};
    std::string model_id{};
    std::optional<std::string> thinking_level{};
    std::int64_t max_tokens{0};
    EntryId tail{};
    EntryId first_kept{};
};

Json summary_request_json(const SummaryRequest& request) {
    Object value{{"attempt", numeric(request.attempt)},
            {"model", Json{Object{{"provider", request.provider}, {"modelId", request.model_id}}}},
            {"maxTokens", numeric(request.max_tokens)},
            {"tail", numeric(request.tail.value)},
            {"firstKept", numeric(request.first_kept.value)}};
    if (request.thinking_level.has_value()) value.emplace("thinkingLevel", *request.thinking_level);
    return Json{std::move(value)};
}

support::Expected<SummaryRequest> parse_summary_request(const Json& checkpoint) {
    SummaryRequest request;
    auto attempt = as_integer(*member(checkpoint, "attempt"));
    if (!attempt) return std::unexpected(attempt.error());
    request.attempt = *attempt;
    const Json* model = member(checkpoint, "model");
    const Json* provider = model == nullptr ? nullptr : member(*model, "provider");
    const Json* model_id = model == nullptr ? nullptr : member(*model, "modelId");
    if (provider == nullptr || model_id == nullptr) {
        return std::unexpected(validation("A summary request needs a model"));
    }
    request.provider = std::string{json_string(*provider)};
    request.model_id = std::string{json_string(*model_id)};
    if (const Json* thinking = member(checkpoint, "thinkingLevel")) {
        const std::string_view level = json_string(*thinking);
        if (!level.empty()) request.thinking_level.emplace(level);
    }
    if (const Json* max_tokens = member(checkpoint, "maxTokens")) {
        auto parsed = as_integer(*max_tokens);
        if (!parsed) return std::unexpected(parsed.error());
        request.max_tokens = *parsed;
    }
    for (const auto& [key, target] :
            {std::pair{"tail", &request.tail.value}, std::pair{"firstKept", &request.first_kept.value}}) {
        const Json* value = member(checkpoint, key);
        if (value == nullptr) {
            return std::unexpected(validation("A summary request needs a tail and a first kept entry"));
        }
        auto parsed = as_integer(*value);
        if (!parsed) return std::unexpected(parsed.error());
        *target = *parsed;
    }
    return request;
}

/// The structured classification of the terminal stream error, captured from
/// the error event: the diagnostic `error_message` is never consulted for
/// the retry decision (frozen pi `isRetryableAssistantError`; the C++
/// contract is the Magpie structured `InferenceFailure`).
struct FailureCapture {
    std::optional<ai::InferenceFailure> inference_failure{};
};

ai::AssistantEventSink failure_capture_sink(std::shared_ptr<FailureCapture> captured) {
    return [captured](const ai::AssistantStreamEvent& event) -> support::ExpectedVoid {
        const auto* error = std::get_if<ai::AssistantErrorEvent>(&event);
        if (error != nullptr) captured->inference_failure = error->inference_failure;
        return {};
    };
}

chord::Path live_compactions_path() { return chord::Path{chord::path_key("compactions")}; }

chord::Path live_compaction_failure_path() { return chord::Path{chord::path_key("compactionFailure")}; }

} // namespace

support::Expected<std::optional<CompactionStatusRecord>> live_compaction_status(chord::Draft& live, TaskId task) {
    auto statuses = live.get(live_compactions_path());
    if (!statuses) return std::unexpected(statuses.error());
    if (!statuses->has_value()) return std::optional<CompactionStatusRecord>{};
    const auto* array = (**statuses).as_array();
    if (array == nullptr) return std::unexpected(validation("Live compaction statuses must be an array"));
    for (const Json& item : *array) {
        const Json* id = member(item, "taskId");
        if (id == nullptr) return std::unexpected(validation("A compaction status must name its task"));
        auto parsed = as_integer(*id);
        if (!parsed) return std::unexpected(parsed.error());
        if (*parsed != task.value) continue;
        CompactionStatusRecord record{.task = task};
        const Json* reason = member(item, "reason");
        if (reason == nullptr) return std::unexpected(validation("A compaction status must name its reason"));
        record.reason = std::string{json_string(*reason)};
        const Json* blocking = member(item, "blocking");
        record.blocking = blocking != nullptr && blocking->as_boolean() != nullptr && *blocking->as_boolean();
        if (const Json* attempt = member(item, "attempt")) {
            auto parsed_attempt = as_integer(*attempt);
            if (!parsed_attempt) return std::unexpected(parsed_attempt.error());
            record.attempt = *parsed_attempt;
        }
        if (const Json* retry = member(item, "retry")) {
            if (const Json* at = member(*retry, "at")) {
                auto parsed_at = as_integer(*at);
                if (!parsed_at) return std::unexpected(parsed_at.error());
                record.retry_at = *parsed_at;
            }
            if (const Json* error = member(*retry, "error")) record.retry_error = std::string{json_string(*error)};
        }
        return std::optional<CompactionStatusRecord>{std::move(record)};
    }
    return std::optional<CompactionStatusRecord>{};
}

support::ExpectedVoid add_live_compaction_status(chord::Draft& live, CompactionStatusRecord status) {
    auto statuses = live.get(live_compactions_path());
    if (!statuses) return std::unexpected(statuses.error());
    Array array;
    if (statuses->has_value()) {
        const auto* existing = (**statuses).as_array();
        if (existing == nullptr) return std::unexpected(validation("Live compaction statuses must be an array"));
        array = *existing;
    }
    array.push_back(Json{Object{{"taskId", numeric(status.task.value)},
            {"reason", status.reason},
            {"blocking", status.blocking},
            {"attempt", numeric(status.attempt)}}});
    return live.set(live_compactions_path(), Json{std::move(array)});
}

namespace {

/// Replace the status of one compaction task in place, or append it when not
/// listed (frozen pi `compactionStatus` updates).
support::ExpectedVoid set_live_compaction_status(chord::Draft& live, const CompactionStatusRecord& status) {
    auto current = live_compaction_status(live, status.task);
    if (!current) return std::unexpected(current.error());
    if (!current->has_value()) return add_live_compaction_status(live, status);
    auto statuses = live.get(live_compactions_path());
    if (!statuses) return std::unexpected(statuses.error());
    Array array;
    const auto* existing = (**statuses).as_array();
    if (existing == nullptr) return std::unexpected(validation("Live compaction statuses must be an array"));
    for (const Json& item : *existing) {
        const Json* id = member(item, "taskId");
        const auto* number = id == nullptr ? nullptr : id->as_number();
        if (number != nullptr && static_cast<std::int64_t>(*number) == status.task.value) {
            Object updated{{"taskId", numeric(status.task.value)},
                    {"reason", status.reason},
                    {"blocking", status.blocking},
                    {"attempt", numeric(status.attempt)}};
            if (status.retry_at)
                updated.emplace(
                        "retry", Json{Object{{"at", numeric(*status.retry_at)}, {"error", status.retry_error}}});
            array.push_back(Json{std::move(updated)});
        } else {
            array.push_back(item);
        }
    }
    return live.set(live_compactions_path(), Json{std::move(array)});
}

} // namespace

support::ExpectedVoid remove_live_compaction_status(chord::Draft& live, TaskId task) {
    auto statuses = live.get(live_compactions_path());
    if (!statuses) return std::unexpected(statuses.error());
    if (!statuses->has_value()) return {};
    const auto* array = (**statuses).as_array();
    if (array == nullptr) return std::unexpected(validation("Live compaction statuses must be an array"));
    Array kept;
    for (const Json& item : *array) {
        const Json* id = member(item, "taskId");
        const auto* number = id == nullptr ? nullptr : id->as_number();
        if (number != nullptr && static_cast<std::int64_t>(*number) == task.value) continue;
        kept.push_back(item);
    }
    if (kept.size() == array->size()) return {};
    if (kept.empty()) return live.erase(live_compactions_path());
    return live.set(live_compactions_path(), Json{std::move(kept)});
}

support::ExpectedVoid fail_live_compaction_status(chord::Draft& live, TaskId task, std::string_view error) {
    auto status = live_compaction_status(live, task);
    if (!status) return std::unexpected(status.error());
    if (status->has_value()) {
        auto failed = live.set(live_compaction_failure_path(),
                Json{Object{{"taskId", numeric(task.value)},
                        {"reason", (*status)->reason},
                        {"error", std::string{error}}}});
        if (!failed) return std::unexpected(failed.error());
    }
    return remove_live_compaction_status(live, task);
}

support::Expected<bool> live_has_compaction(chord::Draft& live) {
    auto statuses = live.get(live_compactions_path());
    if (!statuses) return std::unexpected(statuses.error());
    if (!statuses->has_value()) return false;
    const auto* array = (**statuses).as_array();
    if (array == nullptr) return std::unexpected(validation("Live compaction statuses must be an array"));
    return !array->empty();
}

support::Expected<TaskId> create_compaction_task(Transaction& tx,
        const TaskDefinition& definition,
        const ConversationId conversation_id,
        const Json& input,
        const std::optional<TaskId> owner,
        const bool background) {
    TaskOptions options{.conversation_id = owner.has_value() ? std::optional<ConversationId>{}
                                                             : std::optional<ConversationId>{conversation_id},
            .owner = owner,
            .background = background};
    auto task = tx.create_task(definition, input, std::move(options));
    if (!task) return std::unexpected(task.error());
    auto live = tx.doc(live_document(), DocumentAddress{.conversation_id = conversation_id});
    if (!live) return std::unexpected(live.error());
    const Json* reason = member(input, "reason");
    auto added = add_live_compaction_status(*live,
            CompactionStatusRecord{.task = *task,
                    .reason = std::string{reason == nullptr ? "manual" : json_string(*reason)},
                    .blocking = owner.has_value(),
                    .attempt = 1});
    if (!added) return std::unexpected(added.error());
    return *task;
}

struct CompactionRuntime::Impl {
    Session& session;
    Storage& storage;
    std::shared_ptr<std::move_only_function<std::int64_t()>> clock;
    WorkflowRuntime& workflow;
    std::shared_ptr<const TaskDefinition> definition_;

    [[nodiscard]] std::int64_t now() const { return (*clock)(); }
    [[nodiscard]] const CompactionPolicy& policy() const { return workflow.settings().compaction; }
    [[nodiscard]] const RetryPolicy& retry_policy() const { return workflow.settings().retry; }

    /// The configured model identity of the conversation's agent document
    /// (frozen pi `runtime.agent(context)).model`), with the Harness model
    /// as the C++ slice's catalog stand-in: the stream factory receives the
    /// Harness `ai::Model` value, and an agent naming no model is only an
    /// error when the Harness has none either.
    struct AgentModel {
        std::string provider{};
        std::string model_id{};
        std::optional<std::string> thinking_level{};
        bool harness_model_available{false};
    };

    [[nodiscard]] support::Expected<AgentModel> agent_model(ConversationId conversation_id) {
        AgentModel agent;
        agent.harness_model_available = !workflow.model().id.empty();
        auto found = storage.find_document(
                "pi.agent:v1", DocumentAddress{.conversation_id = conversation_id}, DocumentPoint::current());
        if (!found) return std::unexpected(found.error());
        if (found->has_value()) {
            auto doc = storage.document(found->value().id, DocumentPoint::current());
            if (!doc) return std::unexpected(doc.error());
            if (doc->has_value()) {
                if (const Json* model = member((**doc).value, "model")) {
                    if (const Json* provider = member(*model, "provider")) {
                        agent.provider = std::string{json_string(*provider)};
                    }
                    if (const Json* model_id = member(*model, "modelId")) {
                        agent.model_id = std::string{json_string(*model_id)};
                    }
                }
                if (const Json* thinking = member((**doc).value, "thinkingLevel")) {
                    const std::string_view level = json_string(*thinking);
                    if (!level.empty()) agent.thinking_level.emplace(level);
                }
            }
        }
        if (agent.provider.empty() && agent.model_id.empty()) {
            agent.provider = workflow.model().provider;
            agent.model_id = workflow.model().id;
        }
        return agent;
    }

    /// The provider session id of one conversation, created on first use
    /// (frozen pi `ensureProviderSessionId`).
    [[nodiscard]] support::Expected<std::string> provider_session_id(ConversationId conversation_id) {
        auto found = storage.find_document(
                "pi.provider:v1", DocumentAddress{.conversation_id = conversation_id}, DocumentPoint::current());
        if (!found) return std::unexpected(found.error());
        if (found->has_value()) {
            auto doc = storage.document(found->value().id, DocumentPoint::current());
            if (!doc) return std::unexpected(doc.error());
            if (doc->has_value()) {
                if (const Json* session_id = member((**doc).value, "sessionId")) {
                    return std::string{json_string(*session_id)};
                }
            }
        }
        const std::string created = boost::uuids::to_string(boost::uuids::random_generator{}());
        auto committed =
                await_result(session.commit([conversation_id, created](Transaction& tx) -> support::ExpectedVoid {
                    auto document = tx.doc(
                            detail::builtin_document(
                                    "pi.provider:v1",
                                    DocumentHistory::Latest,
                                    DocumentFork::Initial,
                                    [] { return Json{Object{}}; },
                                    [](const chord::Json&, const chord::OpBatch&, std::int64_t) { return true; }),
                            DocumentAddress{.conversation_id = conversation_id});
                    if (!document) return std::unexpected(document.error());
                    auto set = document->set(chord::Path{chord::path_key("sessionId")}, Json{created});
                    if (!set) return std::unexpected(set.error());
                    return {};
                }));
        if (!committed) return std::unexpected(committed.error());
        return created;
    }

    [[nodiscard]] support::Expected<TaskDefinition> build_definition();

    // ── Phase handlers (frozen pi `CompactionTask.phases`) ──────────────

    [[nodiscard]] support::AsyncResult<void> select(const TaskRecord& task, TaskRuntime& runtime);
    [[nodiscard]] support::AsyncResult<void> summarize(const TaskRecord& task, TaskRuntime& runtime);
    [[nodiscard]] support::AsyncResult<void> retry(const TaskRecord& task, TaskRuntime& runtime);
    [[nodiscard]] support::AsyncResult<void> abort(const TaskRecord& task, TaskRuntime& runtime);

    /// Remove the status and complete without a summary (frozen pi
    /// `complete`).
    [[nodiscard]] support::AsyncResult<void> complete(const TaskRecord& task, TaskRuntime& runtime);
    /// Fail with `no_model` (frozen pi `failNoModel`).
    [[nodiscard]] support::AsyncResult<void> fail_no_model(
            const TaskRecord& task, TaskRuntime& runtime, const AgentModel& agent);
    /// Place a summary and complete in one commit (frozen pi `placeSummary`).
    [[nodiscard]] support::Expected<std::optional<NextTaskState>> place_summary(Transaction& tx,
            const TaskRecord& task,
            chord::Draft& live,
            EntryId first_kept,
            const std::string& summary,
            std::int64_t now) const;
};

support::Expected<TaskDefinition> CompactionRuntime::Impl::build_definition() {
    TaskDefinitionBuilder builder{"pi.compaction", 1};
    builder.initial([](const Json&) { return Json{Object{{"phase", "select"}}}; });
    builder.hook("beforeCompact");
    // This runtime is owned by Harness::Impl, which joins every execution
    // before releasing the definition or these callbacks' borrowed pointer.
    builder.phase("select", [this](const TaskRecord& task, TaskRuntime& runtime) { return select(task, runtime); });
    builder.phase(
            "summarize", [this](const TaskRecord& task, TaskRuntime& runtime) { return summarize(task, runtime); });
    builder.phase("retry", [this](const TaskRecord& task, TaskRuntime& runtime) { return retry(task, runtime); });
    builder.abort([this](const TaskRecord& task, TaskRuntime& runtime) { return abort(task, runtime); });
    return builder.build();
}

support::AsyncResult<void> CompactionRuntime::Impl::complete(const TaskRecord& task, TaskRuntime& runtime) {
    return runtime.commit(
            [task](Transaction& tx, const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
                auto live = tx.doc(live_document(), DocumentAddress{.conversation_id = task.conversation_id});
                if (!live) return std::unexpected(live.error());
                auto removed = remove_live_compaction_status(*live, task.id);
                if (!removed) return std::unexpected(removed.error());
                return std::optional<NextTaskState>{
                        NextTaskState::terminal(TaskOutcome{.status = "completed", .result = Json{Object{}}})};
            });
}

support::AsyncResult<void> CompactionRuntime::Impl::fail_no_model(
        const TaskRecord& task, TaskRuntime& runtime, const AgentModel& agent) {
    const std::string message = agent.provider.empty() && agent.model_id.empty()
                                        ? "No model is configured"
                                        : "Model " + agent.provider + "/" + agent.model_id + " is not available";
    return runtime.commit([task, message](Transaction& tx,
                                  const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
        auto live = tx.doc(live_document(), DocumentAddress{.conversation_id = task.conversation_id});
        if (!live) return std::unexpected(live.error());
        auto failed = fail_live_compaction_status(*live, task.id, message);
        if (!failed) return std::unexpected(failed.error());
        return std::optional<NextTaskState>{NextTaskState::terminal(TaskOutcome{.status = "failed",
                .error = TaskOutcomeError{.message = message, .detail = Json{Object{{"reason", "no_model"}}}}})};
    });
}

support::Expected<std::optional<NextTaskState>> CompactionRuntime::Impl::place_summary(Transaction& tx,
        const TaskRecord& task,
        chord::Draft& live,
        const EntryId first_kept,
        const std::string& summary,
        const std::int64_t now) const {
    auto removed = remove_live_compaction_status(live, task.id);
    if (!removed) return std::unexpected(removed.error());
    auto cleared = live.erase(live_compaction_failure_path());
    if (!cleared) return std::unexpected(cleared.error());
    const Json* reason_value = member(task.input, "reason");
    const std::string reason = reason_value == nullptr ? "manual" : std::string{json_string(*reason_value)};
    const std::vector<chord::Json> model{
            chord::Json::from_support(summary_message_json(summary, now).to_support()).value()};
    if (task.owner.has_value()) {
        // A blocking compaction, owned by its generation, appends its
        // summary directly: the generation holds the run and waits.
        auto entry = tx.append_entry(task.conversation_id,
                "pi.compaction",
                Json{Object{{"reason", reason}}},
                EntryHeadVariant{first_kept},
                model);
        if (!entry) return std::unexpected(entry.error());
        return std::optional<NextTaskState>{NextTaskState::terminal(
                TaskOutcome{.status = "completed", .result = Json{Object{{"entryId", numeric(entry->id.value)}}}})};
    }
    // A conversation-owned one admits its summary as a write submission,
    // placed at once when idle, otherwise at the next boundary, or settled
    // `stale`; nothing else may append to a busy conversation (frozen pi
    /// `placeSummary`).
    WriteEntry write{.kind = "pi.compaction",
            .data = Json{Object{{"reason", reason}}},
            .model = model,
            .head = EntryHeadVariant{first_kept}};
    SubmissionDraft draft{.request_id = "compaction:" + std::to_string(task.id.value),
            .body = WriteSubmissionDraft{std::move(write)}};
    auto admitted = workflow.admit_compaction_summary(tx, task.conversation_id, std::move(draft), now);
    if (!admitted) return std::unexpected(admitted.error());
    return std::optional<NextTaskState>{NextTaskState::terminal(
            TaskOutcome{.status = "completed", .result = Json{Object{{"submissionId", numeric(admitted->value)}}}})};
}

support::AsyncResult<void> CompactionRuntime::Impl::select(const TaskRecord& task, TaskRuntime& runtime) {
    if (runtime.stop_token().stop_requested()) {
        return support::AsyncResult<void>{support::ExpectedVoid{}};
    }
    auto agent = agent_model(task.conversation_id);
    if (!agent) {
        return support::AsyncResult<void>{std::unexpected(agent.error())};
    }
    if (!agent->harness_model_available) {
        return fail_no_model(task, runtime, *agent);
    }
    auto view = derive_committed_context(session, task.conversation_id);
    if (!view) {
        return support::AsyncResult<void>{std::unexpected(view.error())};
    }
    const auto cut = select_cut(*view, policy().keep_recent_tokens);
    if (!cut.has_value()) {
        return complete(task, runtime);
    }
    const EntryId first_kept = view->entries[*cut].id;
    const std::optional<std::string> instructions = [&]() -> std::optional<std::string> {
        const Json* value = member(task.input, "instructions");
        if (value == nullptr) return std::nullopt;
        const std::string_view text = json_string(*value);
        return text.empty() ? std::optional<std::string>{} : std::optional<std::string>{std::string{text}};
    }();
    const std::string reason = [](const Json& input) {
        const Json* value = member(input, "reason");
        return value == nullptr ? "manual" : std::string{json_string(*value)};
    }(task.input);

    Object hook_input{{"reason", reason},
            {"firstKept", numeric(first_kept.value)},
            {"messages", Json{summarized_messages(*view, *cut)}}};
    Array entries;
    for (std::size_t index = 0; index < *cut; ++index) {
        entries.push_back(entry_record_json(view->entries[index]));
    }
    hook_input.emplace("entries", Json{std::move(entries)});
    if (instructions.has_value()) hook_input.emplace("instructions", *instructions);

    // The first non-absent hook decision wins; hook errors are isolated by
    // the runtime's hook machinery (frozen pi `HookRunner.each`).
    struct Decision {
        bool decline{false};
        std::optional<std::string> summary{};
    };
    auto decision = std::make_shared<Decision>();
    // `runtime` outlives every invoke: `each_hook` runs handlers to
    // completion before the phase continues. The first decision wins;
    // handler errors stay isolated (reported, no decision), so a throwing
    // hook neither declines nor vetoes progress.
    auto hooked = await_result(runtime.each_hook("beforeCompact",
            [input = Json{std::move(hook_input)}, decision, &runtime](
                    TaskHook& handler) mutable -> support::AsyncResult<void> {
                if (decision->decline || decision->summary.has_value()) {
                    return support::AsyncResult<void>{support::ExpectedVoid{}};
                }
                auto result = handler(input, runtime);
                return support::AsyncResult<void>{support::AsyncProducer<void, support::Error>{
                        [result = std::move(result), decision](
                                support::AsyncCompletion<void, support::Error> done) mutable noexcept {
                            std::move(result).start([decision, done = std::move(done)](
                                                            support::Expected<chord::Json> value) mutable noexcept {
                                if (!value) {
                                    done(std::unexpected(value.error()));
                                    return;
                                }
                                if (const Json* declined = member(*value, "decline");
                                        declined && declined->as_boolean() && *declined->as_boolean()) {
                                    decision->decline = true;
                                } else if (const Json* text = member(*value, "summary"); text && text->as_string()) {
                                    decision->summary.emplace(*text->as_string());
                                }
                                done(support::ExpectedVoid{});
                            });
                        }}};
            }));
    if (!hooked) {
        return support::AsyncResult<void>{std::unexpected(hooked.error())};
    }
    if (decision->decline) {
        return complete(task, runtime);
    }
    if (decision->summary.has_value()) {
        const std::string summary = std::move(*decision->summary);
        auto placed = await_result(
                runtime.commit([this, task, first_kept, summary, now = now()](Transaction& tx,
                                       const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
                    auto live = tx.doc(live_document(), DocumentAddress{.conversation_id = task.conversation_id});
                    if (!live) return std::unexpected(live.error());
                    return place_summary(tx, task, *live, first_kept, summary, now);
                }));
        if (!placed) {
            return support::AsyncResult<void>{std::unexpected(placed.error())};
        }
        return support::AsyncResult<void>{support::ExpectedVoid{}};
    }

    // Pin the request across attempts; the committed range at `tail` is
    // immutable, so every attempt summarizes the same entries.
    SummaryRequest request{.attempt = 1,
            .provider = agent->provider,
            .model_id = agent->model_id,
            .thinking_level = agent->thinking_level,
            .max_tokens = static_cast<std::int64_t>(std::floor(0.8 * static_cast<double>(policy().reserve_tokens))),
            .tail = view->tail.value_or(first_kept),
            .first_kept = first_kept};
    for (const auto& entry : view->entries)
        if (entry.id.value > request.tail.value) request.tail = entry.id;
    if (workflow.model().max_tokens > 0) {
        request.max_tokens = std::min<std::int64_t>(request.max_tokens, workflow.model().max_tokens);
    }
    Object checkpoint_object = *summary_request_json(request).as_object();
    checkpoint_object["phase"] = "summarize";
    Json checkpoint{std::move(checkpoint_object)};
    auto committed = await_result(runtime.commit(
            [checkpoint](Transaction&, const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
                return std::optional<NextTaskState>{NextTaskState::running(checkpoint)};
            }));
    if (!committed) {
        return support::AsyncResult<void>{std::unexpected(committed.error())};
    }
    return support::AsyncResult<void>{support::ExpectedVoid{}};
}

support::ExpectedVoid record_model_usage(
        Transaction& tx, ConversationId conversation, const ai::AssistantMessage& message) {
    auto definition = detail::builtin_document(
            "pi.usage:v1",
            DocumentHistory::Latest,
            DocumentFork::Initial,
            [] { return Json{Object{{"models", Json{Object{}}}, {"tools", Json{Object{}}}}}; },
            [](const chord::Json&, const chord::OpBatch&, std::int64_t) { return true; });
    auto doc = tx.doc(definition, DocumentAddress{.conversation_id = conversation});
    if (!doc) return std::unexpected(doc.error());
    const auto key = message.provider + "/" + message.model;
    const chord::Path path{chord::path_key("models"), chord::path_key(key)};
    auto existing = doc->get(path);
    if (!existing) return std::unexpected(existing.error());
    Object total;
    if (existing->has_value()) {
        const auto* object = (**existing).as_object();
        if (!object) return std::unexpected(validation("Model usage must be an object"));
        total = *object;
    }
    const auto add = [](Object& object, std::string name, double value) {
        auto found = object.find(name);
        if (found != object.end() && found->second.as_number()) value += *found->second.as_number();
        object.insert_or_assign(std::move(name), Json{value});
    };
    const auto& usage = message.usage;
    add(total, "input", usage.input);
    add(total, "output", usage.output);
    add(total, "cacheRead", usage.cache_read);
    add(total, "cacheWrite", usage.cache_write);
    add(total, "totalTokens", usage.total_tokens);
    if (usage.cache_write_1h) add(total, "cacheWrite1h", *usage.cache_write_1h);
    if (usage.reasoning) add(total, "reasoning", *usage.reasoning);
    Object cost;
    if (auto found = total.find("cost"); found != total.end() && found->second.as_object())
        cost = *found->second.as_object();
    add(cost, "input", usage.cost.input);
    add(cost, "output", usage.cost.output);
    add(cost, "cacheRead", usage.cost.cache_read);
    add(cost, "cacheWrite", usage.cost.cache_write);
    add(cost, "total", usage.cost.total);
    total.insert_or_assign("cost", Json{std::move(cost)});
    return doc->set(path, Json{std::move(total)});
}

support::AsyncResult<void> CompactionRuntime::Impl::summarize(const TaskRecord& task, TaskRuntime& runtime) {
    if (runtime.stop_token().stop_requested()) {
        return support::AsyncResult<void>{support::ExpectedVoid{}};
    }
    if (!task.state.checkpoint.has_value()) {
        return support::AsyncResult<void>{std::unexpected(validation("Summarize phase needs a checkpoint"))};
    }
    auto request = parse_summary_request(*task.state.checkpoint);
    if (!request) {
        return support::AsyncResult<void>{std::unexpected(request.error())};
    }
    if (!workflow.model_available(request->provider, request->model_id)) {
        auto agent = agent_model(task.conversation_id);
        if (!agent) return support::AsyncResult<void>{std::unexpected(agent.error())};
        agent->provider = request->provider;
        agent->model_id = request->model_id;
        return fail_no_model(task, runtime, *agent);
    }
    // The context at `tail` is immutable, so this is the range `select` chose.
    auto view = derive_committed_context(session, task.conversation_id, request->tail);
    if (!view) {
        return support::AsyncResult<void>{std::unexpected(view.error())};
    }
    std::size_t cut = view->entries.size();
    for (std::size_t index = 0; index < view->entries.size(); ++index) {
        if (view->entries[index].id == request->first_kept) {
            cut = index;
            break;
        }
    }
    const std::vector<Json> summarized = summarized_messages(*view, cut);
    const std::optional<std::string> instructions = [&]() -> std::optional<std::string> {
        const Json* value = member(task.input, "instructions");
        if (value == nullptr) return std::nullopt;
        const std::string_view text = json_string(*value);
        return text.empty() ? std::optional<std::string>{} : std::optional<std::string>{std::string{text}};
    }();
    const std::int64_t now_ms = now();
    ai::AiContext context;
    context.messages.push_back(
            ai::SystemMessage{.content = std::string{kSummarizationSystemPrompt}, .timestamp = now_ms});
    context.messages.push_back(ai::UserMessage{
            .content = std::vector<ai::Content>{ai::text_content(summary_prompt(summarized, instructions))},
            .timestamp = now_ms});
    auto stream_options = workflow.settings().stream;
    stream_options.cache_retention = ai::CacheRetention::None;
    if (request->max_tokens > 0) {
        stream_options.max_tokens = static_cast<std::uint64_t>(request->max_tokens);
    }
    stream_options.stop_token = runtime.stop_token();
    auto session_id = provider_session_id(task.conversation_id);
    if (!session_id) {
        return support::AsyncResult<void>{std::unexpected(session_id.error())};
    }
    stream_options.session_id = *session_id;
    if (request->thinking_level.has_value()) {
        if (const auto level = ai::parse_stream_thinking_level(*request->thinking_level)) {
            stream_options.reasoning = *level;
        }
    }
    const auto captured = std::make_shared<FailureCapture>();
    auto response =
            await_result(workflow.stream(std::move(context), stream_options).run(failure_capture_sink(captured)));
    if (!response) {
        // A transport failure settles the compaction like a terminal
        // summarization error (frozen pi `classify` of an error message).
        const std::string message = "Summarization failed: " + response.error().message;
        auto failed = await_result(runtime.commit(
                [task, message](Transaction& tx, const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
                    auto live = tx.doc(live_document(), DocumentAddress{.conversation_id = task.conversation_id});
                    if (!live) return std::unexpected(live.error());
                    auto failed_status = fail_live_compaction_status(*live, task.id, message);
                    if (!failed_status) return std::unexpected(failed_status.error());
                    return std::optional<NextTaskState>{NextTaskState::terminal(TaskOutcome{.status = "failed",
                            .error = TaskOutcomeError{.message = message, .detail = Json{Object{}}}})};
                }));
        if (!failed) {
            return support::AsyncResult<void>{std::unexpected(failed.error())};
        }
        return support::AsyncResult<void>{support::ExpectedVoid{}};
    }
    if (runtime.stop_token().stop_requested()) {
        // An abort mark or close: the abort invocation or the reopened task
        // handles the committed state.
        return support::AsyncResult<void>{support::ExpectedVoid{}};
    }
    const std::optional<std::string> summary = summary_text(*response);
    const bool retryable = response->stop_reason == ai::AssistantStopReason::Error &&
                           captured->inference_failure.has_value() &&
                           ai::is_retryable_inference_failure(*captured->inference_failure);
    const bool retry = retryable && retry_policy().enabled && request->attempt <= retry_policy().max_retries;
    const std::int64_t until = retry ? now() + retry_delay_ms(retry_policy(), request->attempt) : 0;
    const std::string failure = summarization_failure(*response);
    auto committed =
            await_result(runtime.commit([this,
                                                task,
                                                request = *request,
                                                summary,
                                                retry,
                                                until,
                                                failure,
                                                retry_error = response->error_message.value_or(""),
                                                response = *response,
                                                now = now()](Transaction& tx,
                                                const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
                auto usage = record_model_usage(tx, task.conversation_id, response);
                if (!usage) return std::unexpected(usage.error());
                auto live = tx.doc(live_document(), DocumentAddress{.conversation_id = task.conversation_id});
                if (!live) return std::unexpected(live.error());
                if (summary.has_value()) {
                    auto placed = place_summary(tx, task, *live, request.first_kept, *summary, now);
                    if (!placed) return std::unexpected(placed.error());
                    return *placed;
                }
                if (retry) {
                    auto status = live_compaction_status(*live, task.id);
                    if (!status) return std::unexpected(status.error());
                    if (status->has_value()) {
                        (*status)->retry_at = until;
                        (*status)->retry_error = retry_error;
                        auto updated = set_live_compaction_status(*live, **status);
                        if (!updated) return std::unexpected(updated.error());
                    }
                    // The live status stays listed through the backoff;
                    // `retry` bumps its attempt when the next try starts.
                    Object retry_checkpoint = *summary_request_json(request).as_object();
                    retry_checkpoint["phase"] = "retry";
                    retry_checkpoint["until"] = numeric(until);
                    return std::optional<NextTaskState>{NextTaskState::running(Json{std::move(retry_checkpoint)})};
                }
                auto failed_status = fail_live_compaction_status(*live, task.id, failure);
                if (!failed_status) return std::unexpected(failed_status.error());
                return std::optional<NextTaskState>{NextTaskState::terminal(TaskOutcome{
                        .status = "failed", .error = TaskOutcomeError{.message = failure, .detail = Json{Object{}}}})};
            }));
    if (!committed) {
        return support::AsyncResult<void>{std::unexpected(committed.error())};
    }
    return support::AsyncResult<void>{support::ExpectedVoid{}};
}

support::AsyncResult<void> CompactionRuntime::Impl::retry(const TaskRecord& task, TaskRuntime& runtime) {
    if (!task.state.checkpoint.has_value()) {
        return support::AsyncResult<void>{std::unexpected(validation("Retry phase needs a checkpoint"))};
    }
    const Json* until_value = member(*task.state.checkpoint, "until");
    std::int64_t until = 0;
    if (until_value != nullptr) {
        auto parsed = as_integer(*until_value);
        if (!parsed) {
            return support::AsyncResult<void>{std::unexpected(parsed.error())};
        }
        until = *parsed;
    }
    const auto slept = sleep_until(runtime.stop_token(), until, now());
    if (!slept) {
        return support::AsyncResult<void>{std::unexpected(slept.error())};
    }
    if (runtime.stop_token().stop_requested()) {
        return support::AsyncResult<void>{support::ExpectedVoid{}};
    }
    auto request = parse_summary_request(*task.state.checkpoint);
    if (!request) {
        return support::AsyncResult<void>{std::unexpected(request.error())};
    }
    const std::int64_t attempt = request->attempt + 1;
    auto committed =
            await_result(runtime.commit([task, request = *request, attempt](Transaction& tx,
                                                const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
                auto live = tx.doc(live_document(), DocumentAddress{.conversation_id = task.conversation_id});
                if (!live) return std::unexpected(live.error());
                auto status = live_compaction_status(*live, task.id);
                if (!status) return std::unexpected(status.error());
                if (status->has_value()) {
                    auto updated = set_live_compaction_status(*live,
                            CompactionStatusRecord{.task = task.id,
                                    .reason = (*status)->reason,
                                    .blocking = (*status)->blocking,
                                    .attempt = attempt});
                    if (!updated) return std::unexpected(updated.error());
                }
                Object checkpoint_object = *summary_request_json(request).as_object();
                checkpoint_object["attempt"] = numeric(attempt);
                checkpoint_object["phase"] = "summarize";
                Json checkpoint{std::move(checkpoint_object)};
                return std::optional<NextTaskState>{NextTaskState::running(std::move(checkpoint))};
            }));
    if (!committed) {
        return support::AsyncResult<void>{std::unexpected(committed.error())};
    }
    return support::AsyncResult<void>{support::ExpectedVoid{}};
}

support::AsyncResult<void> CompactionRuntime::Impl::abort(const TaskRecord& task, TaskRuntime& runtime) {
    return runtime.commit(
            [task](Transaction& tx, const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
                auto live = tx.doc(live_document(), DocumentAddress{.conversation_id = task.conversation_id});
                if (!live) return std::unexpected(live.error());
                auto removed = remove_live_compaction_status(*live, task.id);
                if (!removed) return std::unexpected(removed.error());
                return std::optional<NextTaskState>{NextTaskState::terminal(TaskOutcome{.status = "aborted"})};
            });
}

CompactionRuntime::~CompactionRuntime() = default;

CompactionRuntime::CompactionRuntime(Session& session,
        Storage& storage,
        std::shared_ptr<std::move_only_function<std::int64_t()>> clock,
        WorkflowRuntime& workflow)
    : impl_(std::make_unique<Impl>(Impl{
              .session = session,
              .storage = storage,
              .clock = std::move(clock),
              .workflow = workflow,
              .definition_ = nullptr,
      })) {
    auto definition = impl_->build_definition();
    if (!definition) std::terminate(); // fixed, valid built-in definition
    impl_->definition_ = std::make_shared<const TaskDefinition>(std::move(*definition));
}

std::shared_ptr<const TaskDefinition> CompactionRuntime::definition() const noexcept { return impl_->definition_; }

} // namespace cch::durable
