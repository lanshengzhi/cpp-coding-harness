#include "StorageEncoding.hpp"
#include "WorkflowRuntime.hpp"

#include <cch/chord/Json.hpp>
#include <cch/durable/Session.hpp>
#include <cch/durable/Storage.hpp>

#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <format>
#include <limits>
#include <mutex>
#include <type_traits>
#include <utility>

namespace cch::durable {
namespace {

// Only the existing Harness execution thread waits here. Public admission and
// root operations remain callback based; no additional execution domain is created.
template <typename T> support::Expected<T> await_result(support::AsyncResult<T> operation) {
    std::mutex mutex;
    std::condition_variable ready;
    std::optional<support::Expected<T>> outcome;
    // The wait below keeps these locals alive until completion returns.
    std::move(operation).start([&mutex, &ready, &outcome](support::Expected<T> result) noexcept {
        std::lock_guard lock(mutex);
        outcome.emplace(std::move(result));
        ready.notify_one();
    });
    std::unique_lock lock(mutex);
    ready.wait(lock, [&outcome] { return outcome.has_value(); });
    return std::move(*outcome);
}

using Json = chord::Json;
using Object = Json::Object;
using Array = Json::Array;

Json numeric(std::int64_t value) { return Json{static_cast<double>(value)}; }

Json tool_json(const ai::Tool& tool) {
    Object value{{"name", tool.name}, {"description", tool.description},
            {"parameters", *Json::from_support(tool.parameters)}};
    if (tool.output_schema) value.emplace("outputSchema", *Json::from_support(*tool.output_schema));
    return Json{std::move(value)};
}

template <typename Block> Json block_json(const Block& block) {
    return std::visit([](const auto& item) -> Json {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, ai::TextContent>) {
            Object value{{"type", "text"}, {"text", item.text}};
            if (item.text_signature) value.emplace("textSignature", *item.text_signature);
            return Json{std::move(value)};
        } else if constexpr (std::is_same_v<T, ai::ThinkingContent>) {
            Object value{{"type", "thinking"}, {"thinking", item.thinking}};
            if (item.thinking_signature) value.emplace("thinkingSignature", *item.thinking_signature);
            if (item.redacted) value.emplace("redacted", true);
            return Json{std::move(value)};
        } else if constexpr (std::is_same_v<T, ai::ImageContent>) {
            return Json{Object{{"type", "image"}, {"data", item.data}, {"mimeType", item.mime_type}}};
        } else {
            Object value{{"type", "toolCall"}, {"id", item.id}, {"name", item.name},
                    {"arguments", item.arguments ? *Json::from_support(*item.arguments) : Json{Object{}}}};
            if (item.thought_signature) value.emplace("thoughtSignature", *item.thought_signature);
            return Json{std::move(value)};
        }
    }, block);
}

template <typename Blocks> Json blocks_json(const Blocks& blocks) {
    Array result;
    for (const auto& block : blocks) result.push_back(block_json(block));
    return Json{std::move(result)};
}

Json system_json(const ai::SystemMessage& message) {
    Object result{{"role", "system"}, {"content", message.content}, {"timestamp", numeric(message.timestamp)}};
    Object sections;
    for (const auto& section : message.sections) {
        sections.emplace(section.name, section.text ? Json{*section.text} : Json{});
    }
    if (!sections.empty()) result.emplace("sections", Json{std::move(sections)});
    Array tools;
    for (const auto& tool : message.tools_added) tools.push_back(tool_json(tool));
    if (!tools.empty()) result.emplace("toolsAdded", Json{std::move(tools)});
    return Json{std::move(result)};
}

Json assistant_json(const ai::AssistantMessage& message) {
    Object result{{"role", "assistant"}, {"content", blocks_json(message.content)},
            {"api", message.api}, {"provider", message.provider}, {"model", message.model},
            {"stopReason", ai::stop_reason_to_string(message.stop_reason)}, {"timestamp", numeric(message.timestamp)}};
    if (message.error_message) result.emplace("errorMessage", *message.error_message);
    return Json{std::move(result)};
}

Json tool_result_json(const ai::ToolResultMessage& message) {
    Object result{{"role", "toolResult"}, {"toolCallId", message.tool_call_id}, {"toolName", message.tool_name},
            {"content", blocks_json(message.content)}, {"isError", message.is_error},
            {"timestamp", numeric(message.timestamp)}};
    if (message.duration_ms) result.emplace("durationMs", numeric(*message.duration_ms));
    return Json{std::move(result)};
}

Json entry_data(Json message, std::optional<TaskId> task = std::nullopt) {
    Object value{{"model", Json{Array{std::move(message)}}}};
    if (task) value.emplace("byTaskId", numeric(task->value));
    return Json{std::move(value)};
}

chord::Json as_chord(Json json) {
    return chord::Json::from_support(json.to_support()).value();
}

std::string thinking_name(ai::ThinkingLevel level) {
    for (const auto& [model_level, name] : ai::kModelThinkingLevels) {
        (void)model_level;
        if (ai::parse_stream_thinking_level(name) == level) return std::string{name};
    }
    std::terminate();
}

std::vector<std::pair<std::string, Json>> initial_documents(const RootOptions& options, std::string provider_session) {
    Object agent;
    if (!options.model.id.empty()) {
        agent.emplace("model", Json{Object{{"provider", options.model.provider}, {"modelId", options.model.id}}});
    }
    if (options.thinking_level) agent.emplace("thinkingLevel", thinking_name(*options.thinking_level));
    return {{"pi.agent:v1", Json{std::move(agent)}},
            {"pi.inbox:v1", Json{Object{{"items", Json{Array{}}}}}},
            {"pi.live:v1", Json{Object{}}},
            {"pi.provider:v1", Json{Object{{"sessionId", std::move(provider_session)}}}},
            {"pi.usage:v1", Json{Object{{"models", Json{Object{}}}, {"tools", Json{Object{}}}}}}};
}

support::Error validation(std::string message) {
    return support::make_error(support::ErrorCode::Validation, std::move(message));
}

support::Expected<std::int64_t> as_integer(const Json& json) {
    const auto* number = json.as_number();
    // The strict bounds avoid double rounding INT64_MAX up to 2^63.
    if (number == nullptr || !(*number >= -0x1p63 && *number < 0x1p63) || std::trunc(*number) != *number) {
        return std::unexpected(validation("Expected an integer field"));
    }
    return static_cast<std::int64_t>(*number);
}

support::Expected<std::int64_t> integer_field(const Json& json, std::string_view field) {
    const auto* object = json.as_object();
    if (object == nullptr) return std::unexpected(validation("Expected an object"));
    const auto found = object->find(std::string{field});
    if (found == object->end()) return std::unexpected(validation("Expected an integer field"));
    return as_integer(found->second);
}

support::Expected<ai::UserMessage> input_user(const EntryRecord& entry) {
    const auto* envelope = entry.data.as_object();
    if (envelope == nullptr) return std::unexpected(validation("Invalid input entry"));
    const auto model = envelope->find("model");
    const auto* messages = model == envelope->end() ? nullptr : model->second.as_array();
    if (messages == nullptr || messages->size() != 1 || messages->front().as_object() == nullptr) {
        return std::unexpected(validation("Invalid input message"));
    }
    const auto& message = *messages->front().as_object();
    const auto content = message.find("content");
    const auto* blocks = content == message.end() ? nullptr : content->second.as_array();
    if (blocks == nullptr || blocks->size() != 1 || blocks->front().as_object() == nullptr) {
        return std::unexpected(validation("Invalid input content"));
    }
    const auto& block = *blocks->front().as_object();
    const auto text = block.find("text");
    if (text == block.end() || text->second.as_string() == nullptr) return std::unexpected(validation("Invalid input text"));
    auto timestamp = integer_field(messages->front(), "timestamp");
    if (!timestamp) return std::unexpected(timestamp.error());
    return ai::user_text_message(*text->second.as_string(), *timestamp);
}

// ── Lane C admission helpers (frozen pi inbox.ts, live.ts, submissions.ts) ──

constexpr std::string_view kInboxDocKind{"pi.inbox:v1"};
constexpr std::string_view kLiveDocKind{"pi.live:v1"};

chord::Path inbox_items_path() { return chord::Path{chord::path_key("items")}; }

chord::Path live_run_path() { return chord::Path{chord::path_key("run")}; }

/// The starting inbox of every conversation; `root()` creates the same
/// incarnation lazily (frozen pi `InboxDoc.initial`).
Json initial_inbox_value() { return Json{Object{{"items", Json{Array{}}}}}; }

/// The starting live document of every conversation (frozen pi
/// `LiveDoc.initial`).
Json initial_live_value() { return Json{Object{}}; }

/// Working copy of the staged inbox's item array (frozen `readInboxItems`).
support::Expected<Array> read_inbox_items(chord::Draft& inbox) {
    auto items = inbox.get(inbox_items_path());
    if (!items) return std::unexpected(items.error());
    auto* array = items->has_value() ? (**items).as_array() : nullptr;
    if (array == nullptr) return std::unexpected(validation("Inbox items must be an array"));
    return *array;
}

/// A write is display-only when it pins neither model messages nor a head
/// (presence counts, even when empty; frozen `isDisplayOnly`).
bool is_display_only(const WriteEntry& entry) { return !entry.model.has_value() && !entry.head.has_value(); }

/// The validated mode of one persisted inbox item. The mode string is frozen
/// wire vocabulary; anything else fails the commit with a typed error instead
/// of being skipped or throwing (the document survived persistence, so
/// corruption must surface as a rejected commit, not a crash).
support::Expected<std::string_view> inbox_item_mode(const Json& item) {
    const auto* object = item.as_object();
    if (object == nullptr) return std::unexpected(validation("An inbox item must be an object"));
    const auto found = object->find("mode");
    if (found == object->end() || found->second.as_string() == nullptr) {
        return std::unexpected(validation("An inbox item must have a mode"));
    }
    const std::string_view mode{*found->second.as_string()};
    if (mode != "write" && mode != "steer" && mode != "followUp") {
        return std::unexpected(validation("An inbox item mode must be \"write\", \"steer\" or \"followUp\""));
    }
    return mode;
}

/// One required member of a persisted inbox item, without the throwing
/// `std::map::at` (typed error under `-fno-exceptions` parity builds).
support::Expected<const Json*> inbox_item_member(const Object& item, std::string_view name) {
    const auto found = item.find(std::string{name});
    if (found == item.end()) {
        return std::unexpected(validation("An inbox item must have a " + std::string{name}));
    }
    return &found->second;
}

/// The submission id of one persisted inbox item.
support::Expected<std::int64_t> inbox_item_id(const Object& item) {
    auto id = inbox_item_member(item, "id");
    if (!id) return std::unexpected(id.error());
    return as_integer(*id.value());
}

/// Whether a `head: "self"` write waits in the queue (frozen
/// `hasQueuedReset`).
support::Expected<bool> has_queued_reset(const Array& items) {
    for (const Json& item : items) {
        auto mode = inbox_item_mode(item);
        if (!mode) return std::unexpected(mode.error());
        if (*mode != "write") continue;
        const auto* object = item.as_object();
        auto entry = inbox_item_member(*object, "entry");
        if (!entry) return std::unexpected(entry.error());
        const auto* draft = entry.value()->as_object();
        if (draft == nullptr) return std::unexpected(validation("An inbox write item entry must be an object"));
        const auto head = draft->find("head");
        if (head != draft->end() && head->second.as_string() != nullptr && *head->second.as_string() == "self") {
            return true;
        }
    }
    return false;
}

/// A write whose pinned head targets an entry before the active range is
/// stale (frozen `isStale`: strictly before the newest head marker). The
/// typed draft's pin was already validated by `write_entry_from_json`, so no
/// persisted number is cast here.
bool is_stale(const Boundary& boundary, const WriteEntry& draft) {
    if (!draft.head.has_value() || !boundary.head.has_value()) return false;
    const auto* pinned = std::get_if<EntryId>(&*draft.head);
    return pinned != nullptr && pinned->value < boundary.head->value;
}

Json input_entry_json(const InputEntry& entry) {
    Object value;
    value.emplace("kind", Json{entry.kind});
    if (entry.data.has_value()) {
        value.emplace("data", *entry.data);
    }
    return Json{std::move(value)};
}

support::Expected<std::optional<InputEntry>> input_entry_from_json(const Json& json) {
    const auto* value = json.as_object();
    if (value == nullptr) return std::unexpected(validation("Invalid input entry"));
    InputEntry entry;
    const auto kind = value->find("kind");
    if (kind == value->end() || kind->second.as_string() == nullptr) {
        return std::unexpected(validation("An input entry must have a kind"));
    }
    entry.kind = *kind->second.as_string();
    if (const auto it = value->find("data"); it != value->end()) {
        entry.data = it->second;
    }
    return std::optional<InputEntry>{std::move(entry)};
}

Json write_entry_json(const WriteEntry& entry) {
    Object value;
    value.emplace("kind", Json{entry.kind});
    if (entry.data.has_value()) {
        value.emplace("data", *entry.data);
    }
    if (entry.model.has_value()) {
        Array model;
        model.reserve(entry.model->size());
        for (const Json& message : *entry.model) {
            model.push_back(message);
        }
        value.emplace("model", Json{std::move(model)});
    }
    if (entry.head.has_value()) {
        if (std::holds_alternative<SelfEntryHead>(*entry.head)) {
            value.emplace("head", Json{std::string{"self"}});
        } else {
            value.emplace("head", numeric(std::get<EntryId>(*entry.head).value));
        }
    }
    return Json{std::move(value)};
}

support::Expected<WriteEntry> write_entry_from_json(const Json& json) {
    const auto* value = json.as_object();
    if (value == nullptr) return std::unexpected(validation("Invalid write entry"));
    WriteEntry entry;
    const auto kind = value->find("kind");
    if (kind == value->end() || kind->second.as_string() == nullptr) {
        return std::unexpected(validation("A write entry must have a kind"));
    }
    entry.kind = *kind->second.as_string();
    if (const auto it = value->find("data"); it != value->end()) {
        entry.data = it->second;
    }
    if (const auto it = value->find("model"); it != value->end()) {
        const auto* model = it->second.as_array();
        if (model == nullptr) return std::unexpected(validation("A write entry model must be an array"));
        entry.model = std::vector<Json>{};
        entry.model->reserve(model->size());
        for (const Json& message : *model) {
            entry.model->push_back(message);
        }
    }
    if (const auto it = value->find("head"); it != value->end()) {
        if (const std::string* marker = it->second.as_string()) {
            if (*marker != "self") {
                return std::unexpected(validation("A write entry head must be \"self\" or an entry id"));
            }
            entry.head = EntryHeadVariant{SelfEntryHead{}};
        } else {
            auto pinned = as_integer(it->second);
            if (!pinned) return std::unexpected(pinned.error());
            entry.head = EntryHeadVariant{EntryId{.value = *pinned}};
        }
    }
    return entry;
}

/// The user message JSON of one input's content: text stays a single text
/// block, other content passes through (frozen pi `appendInput`).
Json user_message_json(const Json& content, std::int64_t timestamp) {
    Json body = content;
    if (const std::string* text = content.as_string()) {
        body = Json{Array{Json{Object{{"type", Json{"text"}}, {"text", Json{*text}}}}}};
    }
    return Json{Object{
            {"role", Json{"user"}},
            {"content", std::move(body)},
            {"timestamp", numeric(timestamp)},
    }};
}

/// Append the entry an input is placed as: the input entry's kind and data,
/// or `pi.user`, with the user message of the content (frozen `appendInput`).
support::Expected<EntryRecord> append_input_entry(Transaction& tx,
        ConversationId conversation_id,
        const Json& content,
        const std::optional<InputEntry>& entry,
        std::int64_t now) {
    const Json message = user_message_json(content, now);
    if (!entry.has_value()) {
        return tx.append_entry(conversation_id,
                "pi.user",
                Json{Object{{"model", Json{Array{message}}}}},
                std::nullopt,
                std::vector<chord::Json>{as_chord(message)});
    }
    Object data;
    if (entry->data.has_value()) {
        if (const auto* object = entry->data->as_object()) {
            data = *object;
        }
    }
    data["kind"] = Json{entry->kind};
    data["model"] = Json{Array{message}};
    return tx.append_entry(conversation_id,
            entry->kind,
            Json{std::move(data)},
            std::nullopt,
            std::vector<chord::Json>{as_chord(message)});
}

/// Append the entry a write carries: its kind and data, its model messages
/// and its head selection (frozen `Tx.appendEntry` of an `EntryDraft`).
support::Expected<EntryRecord> append_write_entry(
        Transaction& tx, ConversationId conversation_id, const WriteEntry& entry) {
    Json data = entry.data.has_value() ? *entry.data : Json{Object{}};
    std::optional<EntryHeadVariant> head;
    if (entry.head.has_value()) {
        head = *entry.head;
    }
    return tx.append_entry(
            conversation_id, entry.kind, std::move(data), head, entry.model.value_or(std::vector<Json>{}));
}

/// The run control marker of one live document value (frozen pi `LiveRun`).
support::Expected<std::optional<LiveRunState>> parse_live_run(const Json& run) {
    if (run.is_null()) return std::optional<LiveRunState>{};
    const auto* value = run.as_object();
    if (value == nullptr) return std::unexpected(validation("A live run must be an object"));
    const auto task = value->find("task");
    if (task == value->end()) return std::unexpected(validation("A live run must name its task"));
    auto task_id = as_integer(task->second);
    if (!task_id) return std::unexpected(task_id.error());
    LiveRunState state{.task = TaskId{.value = *task_id}, .inputs = {}};
    const auto inputs = value->find("inputs");
    if (inputs == value->end()) return std::unexpected(validation("A live run must list its inputs"));
    const auto* array = inputs->second.as_array();
    if (array == nullptr) return std::unexpected(validation("A live run's inputs must be an array"));
    for (const Json& item : *array) {
        auto id = as_integer(item);
        if (!id) return std::unexpected(id.error());
        state.inputs.push_back(SubmissionId{.value = *id});
    }
    return std::optional<LiveRunState>{std::move(state)};
}

/// Append the users to the continuing run's input list (frozen pi
/// `live.run.inputs.push(...users)`).
support::ExpectedVoid join_run_inputs(chord::Draft& live, TaskId task, const std::vector<SubmissionId>& users) {
    if (users.empty()) return {};
    auto run = live.get(live_run_path());
    if (!run) return std::unexpected(run.error());
    if (!run->has_value()) return {};
    auto state = parse_live_run(**run);
    if (!state) return std::unexpected(state.error());
    if (!state->has_value() || (*state)->task != task) return {};
    const auto* value = (**run).as_object();
    if (value == nullptr) return {};
    const auto it = value->find("inputs");
    if (it == value->end()) return std::unexpected(validation("A live run must list its inputs"));
    const auto* array = it->second.as_array();
    if (array == nullptr) return std::unexpected(validation("A live run's inputs must be an array"));
    Array inputs = *array;
    for (const SubmissionId user : users) {
        inputs.push_back(numeric(user.value));
    }
    Object updated = *value;
    updated["inputs"] = Json{std::move(inputs)};
    return live.set(live_run_path(), Json{std::move(updated)});
}

/// Mint the generation task and publish the live run for `inputs` (frozen pi
/// `startRun`).
support::ExpectedVoid start_run(Transaction& tx,
        ConversationId conversation_id,
        chord::Draft& live,
        const std::vector<SubmissionId>& inputs,
        const TaskDefinition& definition) {
    auto task = tx.create_task(definition, Json{Object{}}, TaskOptions{.conversation_id = conversation_id});
    if (!task) {
        return std::unexpected(task.error());
    }
    Array input_ids;
    input_ids.reserve(inputs.size());
    for (const SubmissionId input : inputs) {
        input_ids.push_back(numeric(input.value));
    }
    auto set = live.set(
            live_run_path(), Json{Object{{"task", numeric(task->value)}, {"inputs", Json{std::move(input_ids)}}}});
    if (!set) {
        return std::unexpected(set.error());
    }
    return {};
}

enum class BoundaryPoint { PostTools, Final };

/// Place the queued items a boundary selects: every write, the first or all
/// steers, and at `final` the first or all follow-ups (frozen `applyBoundary`).
support::Expected<BoundaryResult> apply_boundary(
        Transaction& tx, Boundary& boundary, BoundaryPoint point, std::int64_t now) {
    const auto reset = has_queued_reset(boundary.items);
    if (!reset) return std::unexpected(reset.error());
    const bool final_point = point == BoundaryPoint::Final || *reset;
    const auto pick = [&boundary](std::string_view mode,
                              QueueMode queue_mode) -> support::Expected<std::vector<std::size_t>> {
        std::vector<std::size_t> indexes;
        for (std::size_t index = 0; index < boundary.items.size(); ++index) {
            auto item_mode = inbox_item_mode(boundary.items[index]);
            if (!item_mode) return std::unexpected(item_mode.error());
            if (*item_mode == mode) {
                indexes.push_back(index);
            }
        }
        if (queue_mode == QueueMode::OneAtATime && indexes.size() > 1) {
            indexes.resize(1);
        }
        return indexes;
    };
    const auto writes = pick("write", QueueMode::All);
    if (!writes) return std::unexpected(writes.error());
    auto users = pick("steer", boundary.steering_mode);
    if (!users) return std::unexpected(users.error());
    if (final_point) {
        auto follow_ups = pick("followUp", boundary.follow_up_mode);
        if (!follow_ups) return std::unexpected(follow_ups.error());
        users->insert(users->end(), follow_ups->begin(), follow_ups->end());
    }
    std::ranges::sort(*users);

    BoundaryResult result;
    for (const std::size_t index : *writes) {
        const auto* item = boundary.items[index].as_object();
        auto id = inbox_item_id(*item);
        if (!id) return std::unexpected(id.error());
        auto entry_json = inbox_item_member(*item, "entry");
        if (!entry_json) return std::unexpected(entry_json.error());
        auto draft = write_entry_from_json(*entry_json.value());
        if (!draft) return std::unexpected(draft.error());
        if (is_stale(boundary, *draft)) {
            auto settled = tx.settle_submission(SubmissionId{.value = *id},
                    SubmissionSettlement{.status = "unanswered", .reason = std::string{"stale"}});
            if (!settled) return std::unexpected(settled.error());
            continue;
        }
        auto appended = append_write_entry(tx, boundary.conversation_id, *draft);
        if (!appended) return std::unexpected(appended.error());
        auto placed = tx.place_submission(SubmissionId{.value = *id}, appended->id);
        if (!placed) return std::unexpected(placed.error());
        if (appended->head.has_value()) {
            boundary.head = appended->head;
        }
    }
    for (const std::size_t index : *users) {
        const auto* item = boundary.items[index].as_object();
        auto id = inbox_item_id(*item);
        if (!id) return std::unexpected(id.error());
        auto content = inbox_item_member(*item, "content");
        if (!content) return std::unexpected(content.error());
        std::optional<InputEntry> entry;
        if (const auto it = item->find("entry"); it != item->end()) {
            auto parsed = input_entry_from_json(it->second);
            if (!parsed) return std::unexpected(parsed.error());
            entry = std::move(*parsed);
        }
        auto appended = append_input_entry(tx, boundary.conversation_id, *content.value(), entry, now);
        if (!appended) return std::unexpected(appended.error());
        auto placed = tx.place_submission(SubmissionId{.value = *id}, appended->id);
        if (!placed) return std::unexpected(placed.error());
        result.users.push_back(SubmissionId{.value = *id});
        result.user_entries.push_back(*appended);
    }
    std::vector<std::size_t> removed = *writes;
    removed.insert(removed.end(), users->begin(), users->end());
    std::ranges::sort(removed, std::greater<>{});
    for (const std::size_t index : removed) {
        boundary.items.erase(boundary.items.begin() + static_cast<std::ptrdiff_t>(index));
    }
    auto written = boundary.inbox.set(inbox_items_path(), Json{boundary.items});
    if (!written) return std::unexpected(written.error());
    // The reset flag reports the reset this boundary placed, not what remains
    // queued: every write is picked at every boundary, so a queued reset was
    // necessarily placed here, and it is this placement that ends the run
    // (frozen pi `BoundaryResult.reset`).
    result.reset = *reset;
    return result;
}

} // namespace

/// The live-run marker staged in a live document draft, if any (frozen pi
/// live-document read inside a transaction).
support::Expected<std::optional<LiveRunState>> staged_live_run(chord::Draft& live) {
    auto run = live.get(live_run_path());
    if (!run) return std::unexpected(run.error());
    if (!run->has_value()) return std::optional<LiveRunState>{};
    return parse_live_run(**run);
}

TaskHookRegistration task_hook(std::string task_kind, std::string hook_name, TaskHook handler) {
    TaskHookRegistration reg{.task = std::move(task_kind)};
    reg.handlers.emplace(std::move(hook_name), std::move(handler));
    return reg;
}

TaskHookRegistration task_hook(const TaskDefinition& task, std::string hook_name, TaskHook handler) {
    return task_hook(task.kind(), std::move(hook_name), std::move(handler));
}

TaskHookRegistration task_hook(
        const TaskDefinition& task, std::string hook_name, std::move_only_function<support::ExpectedVoid()> handler) {
    return task_hook(task.kind(),
            std::move(hook_name),
            [fn = std::move(handler)](const chord::Json&, TaskRuntime&) mutable -> support::AsyncResult<chord::Json> {
                if (!fn) {
                    return support::AsyncResult<chord::Json>{chord::Json{nullptr}};
                }
                auto res = fn();
                if (!res) {
                    return support::AsyncResult<chord::Json>{std::unexpected(res.error())};
                }
                return support::AsyncResult<chord::Json>{chord::Json{nullptr}};
            });
}

struct InstalledExtension {
    std::string name{};
    std::vector<std::shared_ptr<ToolRegistration>> tools{};
    std::vector<PromptSection> sections{};
    std::vector<std::shared_ptr<TaskDefinition>> tasks{};
    std::vector<std::shared_ptr<TaskHookRegistration>> hooks{};
};

struct Registry::Impl {
    mutable std::mutex mutex;
    mutable std::mutex notify_mutex;
    std::vector<InstalledExtension> extensions;
    Snapshot current;
    std::uint64_t next_subscriber_id{1};
    std::map<std::uint64_t, std::shared_ptr<std::move_only_function<void()>>> subscribers;

    void rebuild_snapshot() {
        Snapshot next;
        for (const auto& ext : extensions) {
            for (const auto& tool : ext.tools) {
                auto existing = std::ranges::find_if(next.tools,
                        [&tool](const auto& item) { return item->declaration.name == tool->declaration.name; });
                if (existing != next.tools.end()) {
                    *existing = tool;
                } else {
                    next.tools.push_back(tool);
                }
            }
            for (const auto& section : ext.sections) {
                auto existing = std::ranges::find_if(
                        next.sections, [&section](const auto& item) { return item.key == section.key; });
                if (existing != next.sections.end()) {
                    *existing = section;
                } else {
                    next.sections.push_back(section);
                }
            }
            for (const auto& task : ext.tasks) {
                next.tasks.push_back(task);
            }
            for (const auto& hook : ext.hooks) {
                next.hooks.push_back(hook);
            }
        }
        current = std::move(next);
    }

    [[nodiscard]] std::vector<std::shared_ptr<std::move_only_function<void()>>> copy_subscribers() const {
        std::vector<std::shared_ptr<std::move_only_function<void()>>> out;
        out.reserve(subscribers.size());
        for (const auto& [id, fn] : subscribers) {
            (void)id;
            out.push_back(fn);
        }
        return out;
    }

    void notify_all(const std::vector<std::shared_ptr<std::move_only_function<void()>>>& listeners) {
        std::lock_guard notify_lock(notify_mutex);
        for (const auto& fn : listeners) {
            if (fn && *fn) {
                (*fn)();
            }
        }
    }
};

Registry::Registry() : impl_(std::make_unique<Impl>()) {}
Registry::Registry(Registry&&) noexcept = default;
Registry& Registry::operator=(Registry&&) noexcept = default;
Registry::~Registry() = default;

support::ExpectedVoid Registry::install(Extension extension) {
    if (extension.name.empty()) {
        return std::unexpected(validation("Extension name must be non-empty"));
    }
    {
        std::vector<std::string> tool_names;
        for (const auto& tool : extension.tools) {
            if (tool.declaration.name.empty() || !tool.execute ||
                    std::ranges::find(tool_names, tool.declaration.name) != tool_names.end()) {
                return std::unexpected(validation("Tool name must be non-empty, unique and executable"));
            }
            if (auto parameters = Json::from_support(tool.declaration.parameters); !parameters) {
                return std::unexpected(parameters.error());
            }
            if (tool.declaration.output_schema) {
                if (auto schema = Json::from_support(*tool.declaration.output_schema); !schema) {
                    return std::unexpected(schema.error());
                }
            }
            tool_names.push_back(tool.declaration.name);
        }
    }
    {
        std::vector<std::string> section_keys;
        for (const auto& section : extension.sections) {
            if (section.key.empty() || std::ranges::find(section_keys, section.key) != section_keys.end()) {
                return std::unexpected(validation("Section key must be non-empty and unique"));
            }
            section_keys.push_back(section.key);
        }
    }
    {
        std::vector<std::string> task_kinds;
        for (const auto& task : extension.tasks) {
            if (task.kind().empty() || task.kind() == "pi.generation" ||
                    std::ranges::find(task_kinds, task.kind()) != task_kinds.end()) {
                return std::unexpected(validation(
                        "Task " + task.kind() + " of extension " + extension.name + " is already installed"));
            }
            task_kinds.push_back(task.kind());
        }
    }

    std::vector<std::shared_ptr<std::move_only_function<void()>>> listeners;
    {
        std::lock_guard lock(impl_->mutex);
        for (const auto& task : extension.tasks) {
            for (const auto& existing_ext : impl_->extensions) {
                if (existing_ext.name == extension.name) continue;
                for (const auto& existing_task : existing_ext.tasks) {
                    if (existing_task->kind() == task.kind()) {
                        return std::unexpected(validation(
                                "Task " + task.kind() + " of extension " + extension.name + " is already installed"));
                    }
                }
            }
        }

        InstalledExtension installed_ext{.name = std::move(extension.name), .sections = std::move(extension.sections)};
        for (auto& tool : extension.tools) {
            installed_ext.tools.push_back(std::make_shared<ToolRegistration>(std::move(tool)));
        }
        for (auto& task : extension.tasks) {
            installed_ext.tasks.push_back(std::make_shared<TaskDefinition>(std::move(task)));
        }
        for (auto& hook : extension.hooks) {
            installed_ext.hooks.push_back(std::make_shared<TaskHookRegistration>(std::move(hook)));
        }

        auto existing = std::ranges::find_if(
                impl_->extensions, [&installed_ext](const auto& item) { return item.name == installed_ext.name; });
        if (existing != impl_->extensions.end()) {
            *existing = std::move(installed_ext);
        } else {
            impl_->extensions.push_back(std::move(installed_ext));
        }
        impl_->rebuild_snapshot();
        listeners = impl_->copy_subscribers();
    }
    impl_->notify_all(listeners);
    return {};
}

void Registry::uninstall(std::string_view extension_name) {
    std::vector<std::shared_ptr<std::move_only_function<void()>>> listeners;
    {
        std::lock_guard lock(impl_->mutex);
        auto existing = std::ranges::find_if(
                impl_->extensions, [extension_name](const auto& item) { return item.name == extension_name; });
        if (existing == impl_->extensions.end()) {
            return;
        }
        impl_->extensions.erase(existing);
        impl_->rebuild_snapshot();
        listeners = impl_->copy_subscribers();
    }
    impl_->notify_all(listeners);
}

Registry::Snapshot Registry::snapshot() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->current;
}

std::uint64_t Registry::subscribe(std::move_only_function<void()> listener) {
    std::lock_guard lock(impl_->mutex);
    const std::uint64_t id = impl_->next_subscriber_id++;
    impl_->subscribers.emplace(id, std::make_shared<std::move_only_function<void()>>(std::move(listener)));
    return id;
}

void Registry::unsubscribe(std::uint64_t id) {
    if (id == 0) return;
    std::lock_guard notify_lock(impl_->notify_mutex);
    std::lock_guard lock(impl_->mutex);
    impl_->subscribers.erase(id);
}

WorkflowRuntime::WorkflowRuntime(Session& session,
        Storage& storage,
        HarnessOptions& options,
        std::shared_ptr<std::move_only_function<std::int64_t()>> clock)
    : session_(session), storage_(storage), registry_(options.registry ? *options.registry : empty_registry_),
      stream_(std::move(options.stream)), settings_(std::move(options.settings)), clock_(std::move(clock)) {
    auto definition = build_definition();
    if (!definition) std::terminate(); // fixed, valid built-in definition
    generation_ = std::make_shared<TaskDefinition>(std::move(*definition));
}

std::int64_t WorkflowRuntime::now() const { return (*clock_)(); }

std::shared_ptr<TaskDefinition> WorkflowRuntime::generation() const noexcept { return generation_; }

support::Expected<TaskDefinition> WorkflowRuntime::build_definition() {
    TaskDefinitionBuilder builder{"pi.generation", 1};
    builder.initial([](const Json&) { return Json{Object{{"phase", "generate"}}}; });
    // This runtime is owned by Harness::Impl, which joins every execution
    // before releasing the definition or this callback's borrowed pointer.
    builder.phase("generate", [this](const TaskRecord& task, TaskRuntime& runtime) { return generate(task, runtime); });
    builder.abort([this](const TaskRecord& task, TaskRuntime& runtime) { return abort_run(task, runtime); });
    return builder.build();
}

support::AsyncResult<ConversationId> WorkflowRuntime::root(RootOptions options) {
    auto selected = std::make_shared<RootOptions>(std::move(options));
    auto created = std::make_shared<bool>(false);
    return support::AsyncResult<ConversationId>{support::AsyncProducer<ConversationId, support::Error>{
            [this, selected, created](support::AsyncCompletion<ConversationId, support::Error> done) mutable noexcept {
                session_.commit([selected, created](Transaction& tx) -> support::ExpectedVoid {
                    auto existing = tx.conversation(ConversationId{.value = 1});
                    if (!existing) return std::unexpected(existing.error());
                    if (existing->has_value()) return {};
                    auto record = tx.create_root_conversation();
                    if (!record) return std::unexpected(record.error());
                    const auto documents = initial_documents(*selected, boost::uuids::to_string(boost::uuids::random_generator{}()));
                    for (const auto& [kind, initial] : documents) {
                        auto definition = define_document(kind, [initial] { return initial; });
                        auto doc = tx.doc(definition, DocumentAddress{.conversation_id = record->id});
                        if (!doc) return std::unexpected(doc.error());
                    }
                    *created = true;
                    return {};
                }).start([this, selected, created, done = std::move(done)](support::ExpectedVoid result) mutable noexcept {
                    if (!result) {
                        done(std::unexpected(result.error()));
                        return;
                    }
                    if (*created) {
                        model_ = std::move(selected->model);
                        thinking_level_ = selected->thinking_level;
                    }
                    done(ConversationId{.value = 1});
                });
            }}};
}

support::AsyncResult<SubmissionId> WorkflowRuntime::submit(ConversationId conversation_id, SubmissionDraft draft) {
    const std::int64_t admitted_at = now();
    auto admission = std::make_shared<SubmissionId>();
    return support::AsyncResult<SubmissionId>{support::AsyncProducer<SubmissionId, support::Error>{
            [this, conversation_id, draft = std::move(draft), admitted_at, admission](
                    support::AsyncCompletion<SubmissionId, support::Error> done) mutable noexcept {
                session_.commit([this, conversation_id, draft = std::move(draft), admitted_at, admission](
                                        Transaction& tx) -> support::ExpectedVoid {
                            auto admitted = admit_submission(tx, conversation_id, draft, admitted_at);
                            if (!admitted) return std::unexpected(admitted.error());
                            *admission = *admitted;
                            return {};
                        })
                        .start([done = std::move(done), admission](support::ExpectedVoid result) mutable noexcept {
                            if (!result)
                                done(std::unexpected(result.error()));
                            else
                                done(*admission);
                        });
            }}};
}

support::Expected<SubmissionId> WorkflowRuntime::admit_submission(
        Transaction& tx, ConversationId conversation_id, const SubmissionDraft& draft, std::int64_t now) {
    const auto* input = std::get_if<InputSubmissionDraft>(&draft.body);
    const auto* write = std::get_if<WriteSubmissionDraft>(&draft.body);
    if (input == nullptr && write == nullptr) {
        return std::unexpected(validation("A submission draft must be an input or a write"));
    }
    // The input entry kind is a caller-owned namespace outside the reserved
    // `pi.` prefix (frozen pi `checkInputEntry`).
    if (input != nullptr && input->entry.has_value()) {
        const std::string& kind = input->entry->kind;
        if (kind.empty() || kind.starts_with("pi.")) {
            return std::unexpected(validation(std::format(
                    "An input entry's kind must be a non-empty string not starting with \"pi.\", got {}", kind)));
        }
    }
    // One request id admits one submission of one type per conversation
    // (frozen pi `submissionByRequest`): a duplicate returns the existing
    // handle without a write.
    if (draft.request_id.has_value()) {
        auto existing = tx.submission_by_request(conversation_id, *draft.request_id);
        if (!existing) {
            return std::unexpected(existing.error());
        }
        if (existing->has_value()) {
            const std::string_view wanted = input != nullptr ? "input" : "write";
            if ((**existing).type != wanted) {
                return std::unexpected(validation(std::format("Request {} already identifies a submission of type {}",
                        *draft.request_id,
                        (**existing).type)));
            }
            return (**existing).id;
        }
    }
    auto conversation = tx.conversation(conversation_id);
    if (!conversation) {
        return std::unexpected(conversation.error());
    }
    if (!conversation->has_value() || conversation_id.value != 1) {
        return std::unexpected(validation("The workflow requires the root conversation"));
    }
    const DocumentAddress address{.conversation_id = conversation_id};
    // A display-only write appends and settles at once, busy or not; only
    // behind a queued reset it queues (frozen pi `admitSubmission`).
    if (write != nullptr && is_display_only(write->entry)) {
        auto inbox = tx.doc(inbox_document(), address);
        if (!inbox) {
            return std::unexpected(inbox.error());
        }
        auto items = read_inbox_items(*inbox);
        if (!items) {
            return std::unexpected(items.error());
        }
        auto queued_reset = has_queued_reset(*items);
        if (!queued_reset) return std::unexpected(queued_reset.error());
        if (!*queued_reset) {
            auto appended = append_write_entry(tx, conversation_id, write->entry);
            if (!appended) {
                return std::unexpected(appended.error());
            }
            auto created = tx.create_submission(SubmissionRecord{.id = {},
                    .conversation_id = conversation_id,
                    .request_id = draft.request_id,
                    .type = "write",
                    .status = "done",
                    .entry = appended->id});
            if (!created) {
                return std::unexpected(created.error());
            }
            return created->id;
        }
    }
    auto live = tx.doc(live_document(), address);
    if (!live) {
        return std::unexpected(live.error());
    }
    const bool busy = [&] {
        auto run = staged_live_run(*live);
        return run.has_value() && run->has_value();
    }();
    // A busy conversation admits new work only into the queue, and rejects a
    // `whenBusy: reject` input (frozen pi `ConversationBusy`).
    if (busy && input != nullptr && input->when_busy.has_value() && *input->when_busy == WhenBusy::Reject) {
        return std::unexpected(support::make_error(
                support::ErrorCode::Busy, std::format("Conversation {} is busy", conversation_id.value)));
    }
    // A boundary reads the table, so it is prepared before the first table
    // write; a busy conversation needs none (frozen pi `prepareBoundary`).
    std::optional<Boundary> boundary;
    if (!busy) {
        auto inbox = tx.doc(inbox_document(), address);
        if (!inbox) {
            return std::unexpected(inbox.error());
        }
        auto items = read_inbox_items(*inbox);
        if (!items) {
            return std::unexpected(items.error());
        }
        auto prepared = prepare_boundary(tx, conversation_id, std::move(*inbox), std::move(*items));
        if (!prepared) {
            return std::unexpected(prepared.error());
        }
        boundary = std::move(*prepared);
    }
    if (!boundary.has_value() || !boundary->items.empty()) {
        auto created = tx.create_submission(SubmissionRecord{.id = {},
                .conversation_id = conversation_id,
                .request_id = draft.request_id,
                .type = input != nullptr ? "input" : "write",
                .status = "queued"});
        if (!created) {
            return std::unexpected(created.error());
        }
        std::string mode_str = "write";
        if (input != nullptr) {
            mode_str = (input->when_busy.has_value() && *input->when_busy == WhenBusy::Steer) ? "steer" : "followUp";
        }
        Object item_obj{
                {"id", numeric(created->id.value)},
                {"mode", Json{std::move(mode_str)}},
        };
        if (input != nullptr) {
            item_obj.emplace("content", input->content);
            if (input->entry.has_value()) {
                item_obj.emplace("entry", input_entry_json(*input->entry));
            }
        } else {
            item_obj.emplace("entry", write_entry_json(write->entry));
        }
        Json item{std::move(item_obj)};
        if (!boundary.has_value()) {
            // Busy: the item waits for the running run's next boundary.
            auto inbox = tx.doc(inbox_document(), address);
            if (!inbox) {
                return std::unexpected(inbox.error());
            }
            auto items = read_inbox_items(*inbox);
            if (!items) {
                return std::unexpected(items.error());
            }
            items->push_back(item);
            auto set = inbox->set(inbox_items_path(), Json{*items});
            if (!set) {
                return std::unexpected(set.error());
            }
            return created->id;
        }
        // Idle behind queued items: queue behind them and run a final
        // boundary now.
        boundary->items.push_back(item);
        auto applied = apply_boundary(tx, *boundary, BoundaryPoint::Final, now);
        if (!applied) {
            return std::unexpected(applied.error());
        }
        if (applied->reset) {
            // The queued reset reached the final boundary through admission
            // (the previous run ended without a boundary): the next run's
            // replay starts from the reset head, not the stale transcript.
            messages_.clear();
        }
        if (!applied->users.empty()) {
            auto started = start_run(tx, conversation_id, *live, applied->users, *generation_);
            if (!started) {
                return std::unexpected(started.error());
            }
        }
        return created->id;
    }
    if (write != nullptr) {
        if (is_stale(*boundary, write->entry)) {
            auto created = tx.create_submission(SubmissionRecord{.id = {},
                    .conversation_id = conversation_id,
                    .request_id = draft.request_id,
                    .type = "write",
                    .status = "unanswered",
                    .reason = "stale"});
            if (!created) {
                return std::unexpected(created.error());
            }
            return created->id;
        }
        auto appended = append_write_entry(tx, conversation_id, write->entry);
        if (!appended) {
            return std::unexpected(appended.error());
        }
        auto created = tx.create_submission(SubmissionRecord{.id = {},
                .conversation_id = conversation_id,
                .request_id = draft.request_id,
                .type = "write",
                .status = "done",
                .entry = appended->id});
        if (!created) {
            return std::unexpected(created.error());
        }
        return created->id;
    }
    auto appended = append_input_entry(tx, conversation_id, input->content, input->entry, now);
    if (!appended) {
        return std::unexpected(appended.error());
    }
    auto created = tx.create_submission(SubmissionRecord{.id = {},
            .conversation_id = conversation_id,
            .request_id = draft.request_id,
            .type = "input",
            .status = "placed",
            .entry = appended->id});
    if (!created) {
        return std::unexpected(created.error());
    }
    auto started = start_run(tx, conversation_id, *live, {created->id}, *generation_);
    if (!started) {
        return std::unexpected(started.error());
    }
    return created->id;
}

support::Expected<Boundary> WorkflowRuntime::prepare_boundary(
        Transaction& tx, ConversationId conversation_id, chord::Draft inbox, std::vector<chord::Json> items) {
    // The head marker is the newest placed entry that carries a head. Frozen
    // pi resolves it through an indexed `latestHeadMarker` storage primitive;
    // the bounded C++ runtime has no index, so it scans the log newest first
    // and doubles the page until the page proves complete (fewer entries than
    // requested). The head can therefore never be silently missed past a
    // fixed window. Table reads precede the commit's first table write.
    // debt: head lookup scans O(n) entries; add an indexed latest-head query
    // when admission profiling shows this scan dominates large conversations.
    std::size_t limit = 4096;
    std::optional<EntryId> head;
    while (!head.has_value()) {
        auto entries = tx.scan_entries(conversation_id, limit);
        if (!entries) {
            return std::unexpected(entries.error());
        }
        for (const EntryRecord& entry : *entries) {
            if (entry.head.has_value()) {
                head = *entry.head;
                break;
            }
        }
        if (head.has_value() || entries->size() < limit) {
            break;
        }
        if (limit > std::numeric_limits<std::size_t>::max() / 2) {
            return std::unexpected(validation("Conversation head scan exceeds the supported entry count"));
        }
        limit *= 2;
    }
    return Boundary{.conversation_id = conversation_id,
            .inbox = std::move(inbox),
            .items = std::move(items),
            .steering_mode = settings_.steering_mode,
            .follow_up_mode = settings_.follow_up_mode,
            .head = head};
}

support::AsyncResult<void> WorkflowRuntime::abort_run(const TaskRecord& task, TaskRuntime& runtime) {
    return runtime.commit([task](Transaction& tx,
                                  const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
        auto live = tx.doc(live_document(), DocumentAddress{.conversation_id = task.conversation_id});
        if (!live) {
            return std::unexpected(live.error());
        }
        auto ended = end_run(
                tx, *live, task.id, SubmissionSettlement{.status = "unanswered", .reason = std::string{"aborted"}});
        if (!ended) {
            return std::unexpected(ended.error());
        }
        return std::optional<NextTaskState>{NextTaskState::terminal(TaskOutcome{.status = "aborted"})};
    });
}

support::AsyncResult<bool> WorkflowRuntime::apply_post_tools_boundary(const TaskRecord& task, TaskRuntime& runtime) {
    auto ended = std::make_shared<bool>(false);
    auto committed = runtime.commit([task, ended, this](Transaction& tx,
                                            const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
        const DocumentAddress address{.conversation_id = task.conversation_id};
        auto inbox = tx.doc(inbox_document(), address);
        if (!inbox) {
            return std::unexpected(inbox.error());
        }
        auto items = read_inbox_items(*inbox);
        if (!items) {
            return std::unexpected(items.error());
        }
        auto boundary = prepare_boundary(tx, task.conversation_id, std::move(*inbox), std::move(*items));
        if (!boundary) {
            return std::unexpected(boundary.error());
        }
        auto live = tx.doc(live_document(), address);
        if (!live) {
            return std::unexpected(live.error());
        }
        auto applied = apply_boundary(tx, *boundary, BoundaryPoint::PostTools, now());
        if (!applied) {
            return std::unexpected(applied.error());
        }
        // The placed users join this run: their entries seed the next
        // request's replay cache (frozen pi continues the run with the
        // placed users as new inputs).
        for (const EntryRecord& entry : applied->user_entries) {
            auto user = input_user(entry);
            if (!user) {
                return std::unexpected(user.error());
            }
            messages_.push_back(std::move(*user));
        }
        if (applied->reset) {
            // The queued reset cut the run's context before an
            // answer: the run ends unanswered and the placed users
            // start the next run. The replay cache clears with the reset:
            // the next run's request carries only its own placed users
            // (frozen pi walks the entry chain back to the reset head), not
            // this run's transcript.
            messages_.clear();
            auto ended_run = end_run(
                    tx, *live, task.id, SubmissionSettlement{.status = "unanswered", .reason = std::string{"reset"}});
            if (!ended_run) {
                return std::unexpected(ended_run.error());
            }
            if (!applied->users.empty()) {
                auto started = start_run(tx, task.conversation_id, *live, applied->users, *generation_);
                if (!started) {
                    return std::unexpected(started.error());
                }
            }
            *ended = true;
            return std::optional<NextTaskState>{NextTaskState::terminal(TaskOutcome{.status = "completed"})};
        }
        auto joined = join_run_inputs(*live, task.id, applied->users);
        if (!joined) {
            return std::unexpected(joined.error());
        }
        return std::optional<NextTaskState>{};
    });
    return support::AsyncResult<bool>{support::AsyncProducer<bool, support::Error>{
            [committed = std::move(committed), ended](
                    support::AsyncCompletion<bool, support::Error> done) mutable noexcept {
                std::move(committed).start(
                        [ended, done = std::move(done)](support::ExpectedVoid result) mutable noexcept {
                            if (!result) {
                                done(std::unexpected(result.error()));
                                return;
                            }
                            done(*ended);
                        });
            }}};
}

support::AsyncResult<void> WorkflowRuntime::generate(const TaskRecord& task, TaskRuntime& runtime) {
    return support::AsyncResult<void>{run(task, runtime)};
}

WorkflowRuntime::PromptPlan WorkflowRuntime::plan_prompt(const Registry::Snapshot& snapshot,
        std::span<const ai::Tool> shown_tools, std::span<const ai::SystemMessageSection> shown_sections,
        std::int64_t timestamp) {
    PromptPlan plan{.system = ai::SystemMessage{.timestamp = timestamp}};
    for (const auto& tool : snapshot.tools) {
        plan.context.tools.push_back(tool->declaration);
        if (std::ranges::none_of(shown_tools, [&tool](const auto& shown) { return shown.name == tool->declaration.name; })) {
            plan.system.tools_added.push_back(tool->declaration);
        }
    }
    for (const auto& section : snapshot.sections) {
        const std::string text = section.tag ? std::format("<{}>\n{}\n</{}>", section.key, section.text, section.key) : section.text;
        if (plan.context.system_prompt) *plan.context.system_prompt += "\n\n";
        else plan.context.system_prompt.emplace();
        *plan.context.system_prompt += text;
        if (std::ranges::none_of(shown_sections, [&section](const auto& shown) { return shown.name == section.key; })) {
            plan.system.sections.push_back(ai::SystemMessageSection{.name = section.key, .text = text});
        }
    }
    return plan;
}

ai::ToolResultMessage WorkflowRuntime::execute_tool(const ai::ToolCallContent& call,
        const Registry::Snapshot& snapshot, std::stop_token stop_token) {
    ai::ToolResultMessage result{.tool_call_id = call.id, .tool_name = call.name, .timestamp = now()};
    auto found = std::ranges::find_if(snapshot.tools, [&call](const auto& tool) { return tool->declaration.name == call.name; });
    if (found == snapshot.tools.end() || !call.arguments_valid || !call.arguments) {
        result.is_error = true;
        result.content.push_back(ai::text_content(found == snapshot.tools.end() ? std::format("Tool {} not found", call.name) : "Invalid tool arguments"));
        return result;
    }
    const auto started = std::chrono::steady_clock::now();
    auto output = await_result((*found)->execute(*call.arguments, stop_token));
    result.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    result.is_error = !output.has_value();
    result.content = output ? std::move(*output) : std::vector<ai::Content>{ai::text_content(output.error().message)};
    return result;
}

support::ExpectedVoid WorkflowRuntime::run(const TaskRecord& task, TaskRuntime& runtime) {
    auto run = committed_live_run(storage_, task.conversation_id);
    if (!run) {
        return std::unexpected(run.error());
    }
    if (!run->has_value()) {
        return std::unexpected(validation("Generation task has no run"));
    }
    if ((*run)->task != task.id) {
        return std::unexpected(validation("Generation task does not own the conversation run"));
    }
    const std::vector<SubmissionId> run_inputs = (*run)->inputs;

    // The placed run inputs seed the replay cache (frozen pi run input
    // conversion).
    for (const SubmissionId input : run_inputs) {
        auto submission = storage_.submission(input);
        if (!submission) {
            return std::unexpected(submission.error());
        }
        if (!submission->has_value() || !(**submission).entry.has_value()) {
            return std::unexpected(validation("Placed input entry not found"));
        }
        auto entry = storage_.entry(*(**submission).entry);
        if (!entry) {
            return std::unexpected(entry.error());
        }
        if (!entry->has_value()) {
            return std::unexpected(validation("Placed input entry not found"));
        }
        auto user = input_user(entry->value().entry);
        if (!user) {
            return std::unexpected(user.error());
        }
        messages_.push_back(std::move(*user));
    }

    while (!runtime.stop_token().stop_requested()) {
        const auto snapshot = registry_.snapshot();
        auto plan = plan_prompt(snapshot, shown_tools_, shown_sections_, now());
        auto& context = plan.context;
        const auto& system = plan.system;
        if (!system.sections.empty() || !system.tools_added.empty()) {
            auto committed = await_result(runtime.commit([id = task.conversation_id, system](Transaction& tx, const TaskRecord&)
                    -> support::Expected<std::optional<NextTaskState>> {
                auto entry = tx.append_entry(id, "pi.system", entry_data(system_json(system)), std::nullopt,
                            std::vector<chord::Json>{as_chord(system_json(system))});
                if (!entry) return std::unexpected(entry.error());
                return std::nullopt;
            }));
            if (!committed) return committed;
            messages_.push_back(system);
            for (const auto& section : system.sections) shown_sections_.push_back(section);
            shown_tools_ = context.tools;
        }
        context.messages = messages_;
        auto options = settings_.stream;
        if (thinking_level_) options.reasoning = thinking_level_;
        options.stop_token = runtime.stop_token();
        auto stream = stream_(model_, std::move(context), options);
        auto response = await_result(std::move(stream).run([](const ai::AssistantStreamEvent&) -> support::ExpectedVoid { return {}; }));
        if (!response) {
            // A transport failure settles the run's inputs unanswered (frozen
            // pi `failModelError`) and fails the task.
            const std::string message = response.error().message;
            auto failed = await_result(runtime.commit([task, message](Transaction& tx, const TaskRecord&)
                                                              -> support::Expected<std::optional<NextTaskState>> {
                auto live = tx.doc(live_document(), DocumentAddress{.conversation_id = task.conversation_id});
                if (!live) {
                    return std::unexpected(live.error());
                }
                auto ended = end_run(tx,
                        *live,
                        task.id,
                        SubmissionSettlement{
                                .status = "unanswered", .reason = std::string{"model_error"}, .detail = Json{message}});
                if (!ended) {
                    return std::unexpected(ended.error());
                }
                return std::optional<NextTaskState>{NextTaskState::terminal(
                        TaskOutcome{.status = "failed", .error = TaskOutcomeError{.message = message}})};
            }));
            if (!failed) {
                return std::unexpected(failed.error());
            }
            return {};
        }
        const bool tools = response->stop_reason == ai::AssistantStopReason::ToolUse;
        const bool failed = response->stop_reason == ai::AssistantStopReason::Error;
        const bool aborted = response->stop_reason == ai::AssistantStopReason::Aborted;
        if (tools) {
            auto committed = await_result(
                    runtime.commit([id = task.conversation_id, response = *response](Transaction& tx,
                                           const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
                        auto entry = tx.append_entry(id,
                                "pi.assistant",
                                entry_data(assistant_json(response)),
                                std::nullopt,
                                std::vector<chord::Json>{as_chord(assistant_json(response))});
                        if (!entry) return std::unexpected(entry.error());
                        return std::nullopt;
                    }));
            if (!committed) return committed;
            messages_.push_back(*response);
            for (const auto& content : response->content) {
                const auto* call = std::get_if<ai::ToolCallContent>(&content);
                if (call == nullptr) continue;
                auto result = execute_tool(*call, snapshot, runtime.stop_token());
                auto saved = await_result(
                        runtime.commit([id = task.conversation_id, tid = task.id, result](Transaction& tx,
                                               const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
                            auto entry = tx.append_entry(id,
                                    "pi.tool-result",
                                    entry_data(tool_result_json(result), tid),
                                    std::nullopt,
                                    std::vector<chord::Json>{as_chord(tool_result_json(result))},
                                    tid);
                            if (!entry) return std::unexpected(entry.error());
                            return std::nullopt;
                        }));
                if (!saved) {
                    return std::unexpected(validation("Tool result commit failed: " + saved.error().message));
                }
                messages_.push_back(std::move(result));
            }
            if (runtime.stop_token().stop_requested()) {
                // The abort invocation settles the run (frozen pi generation
                // `abort`); this loop only unwinds.
                break;
            }
            auto ended_here = await_result(apply_post_tools_boundary(task, runtime));
            if (!ended_here) {
                return std::unexpected(ended_here.error());
            }
            if (*ended_here) {
                // A queued reset ended the run at the postTools boundary;
                // the placed users start the next run.
                return {};
            }
            continue;
        }
        // Final answer, model error, or abort: one commit appends the answer
        // and settles the run at the final boundary (frozen pi `answer`).
        auto outcome = await_result(runtime.commit(
                [id = task.conversation_id, task, response = *response, aborted, failed, this](
                        Transaction& tx, const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
                    const DocumentAddress address{.conversation_id = id};
                    // A boundary reads the table before the commit's first
                    // table write (frozen pi `prepareBoundary`).
                    auto inbox = tx.doc(inbox_document(), address);
                    if (!inbox) {
                        return std::unexpected(inbox.error());
                    }
                    auto items = read_inbox_items(*inbox);
                    if (!items) {
                        return std::unexpected(items.error());
                    }
                    auto boundary = prepare_boundary(tx, id, std::move(*inbox), std::move(*items));
                    if (!boundary) {
                        return std::unexpected(boundary.error());
                    }
                    auto live = tx.doc(live_document(), address);
                    if (!live) {
                        return std::unexpected(live.error());
                    }
                    if (aborted) {
                        auto ended = end_run(tx,
                                *live,
                                task.id,
                                SubmissionSettlement{.status = "unanswered", .reason = std::string{"aborted"}});
                        if (!ended) {
                            return std::unexpected(ended.error());
                        }
                        return std::optional<NextTaskState>{NextTaskState::terminal(TaskOutcome{.status = "aborted"})};
                    }
                    auto entry = tx.append_entry(id,
                            "pi.assistant",
                            entry_data(assistant_json(response)),
                            std::nullopt,
                            std::vector<chord::Json>{as_chord(assistant_json(response))});
                    if (!entry) {
                        return std::unexpected(entry.error());
                    }
                    if (failed) {
                        const std::string message = response.error_message.value_or("Generation failed");
                        auto ended = end_run(tx,
                                *live,
                                task.id,
                                SubmissionSettlement{.status = "unanswered",
                                        .reason = std::string{"model_error"},
                                        .detail = Json{message}});
                        if (!ended) {
                            return std::unexpected(ended.error());
                        }
                        return std::optional<NextTaskState>{NextTaskState::terminal(
                                TaskOutcome{.status = "failed", .error = TaskOutcomeError{.message = message}})};
                    }
                    auto applied = apply_boundary(tx, *boundary, BoundaryPoint::Final, now());
                    if (!applied) {
                        return std::unexpected(applied.error());
                    }
                    if (applied->reset) {
                        // A queued reset reached the final boundary: the
                        // answered run still ends, and the next run's replay
                        // starts from the reset head, not this transcript.
                        messages_.clear();
                    }
                    auto ended =
                            end_run(tx, *live, task.id, SubmissionSettlement{.status = "done", .answer = entry->id});
                    if (!ended) {
                        return std::unexpected(ended.error());
                    }
                    if (!applied->users.empty()) {
                        auto started = start_run(tx, id, *live, applied->users, *generation_);
                        if (!started) {
                            return std::unexpected(started.error());
                        }
                    }
                    return std::optional<NextTaskState>{NextTaskState::terminal(TaskOutcome{
                            .status = "completed", .result = Json{Object{{"answer", numeric(entry->id.value)}}}})};
                }));
        if (!outcome) {
            return std::unexpected(outcome.error());
        }
        return {};
    }
    return std::unexpected(validation("Request was aborted"));
}

// ── Shared document definitions and run helpers (frozen pi `InboxDoc`, ─────
// ── `LiveDoc`, `endRun`, `withdrawQueuedInputs`, `removeInboxItem`) ────────

DocumentDefinition inbox_document() {
    return define_document(std::string{kInboxDocKind}, [] { return initial_inbox_value(); });
}

DocumentDefinition live_document() {
    return define_document(std::string{kLiveDocKind}, [] { return initial_live_value(); });
}

support::Expected<std::optional<LiveRunState>> committed_live_run(Storage& storage, ConversationId conversation_id) {
    auto found = storage.find_document(std::string{kLiveDocKind}, conversation_id);
    if (!found) {
        return std::unexpected(found.error());
    }
    if (!found->has_value()) {
        return std::optional<LiveRunState>{};
    }
    auto doc = storage.document(found->value().id);
    if (!doc) {
        return std::unexpected(doc.error());
    }
    if (!doc->has_value()) {
        return std::optional<LiveRunState>{};
    }
    const auto* value = (**doc).as_object();
    if (value == nullptr) {
        return std::unexpected(validation("Live document must be an object"));
    }
    const auto run = value->find("run");
    if (run == value->end()) {
        return std::optional<LiveRunState>{};
    }
    return parse_live_run(run->second);
}

support::ExpectedVoid end_run(
        Transaction& tx, chord::Draft& live, TaskId task, const SubmissionSettlement& settlement) {
    auto run = staged_live_run(live);
    if (!run) {
        return std::unexpected(run.error());
    }
    if (!run->has_value() || (*run)->task != task) {
        return {};
    }
    for (const SubmissionId input : (*run)->inputs) {
        auto settled = tx.settle_submission(input, settlement);
        if (!settled) {
            return std::unexpected(settled.error());
        }
    }
    return live.erase(live_run_path());
}

support::ExpectedVoid withdraw_queued_inputs(Transaction& tx, chord::Draft& inbox) {
    auto items = read_inbox_items(inbox);
    if (!items) {
        return std::unexpected(items.error());
    }
    for (std::size_t index = items->size(); index-- > 0;) {
        auto mode = inbox_item_mode((*items)[index]);
        if (!mode) {
            return std::unexpected(mode.error());
        }
        if (*mode == "write") {
            continue;
        }
        auto id = inbox_item_id(*(*items)[index].as_object());
        if (!id) {
            return std::unexpected(id.error());
        }
        auto settled = tx.settle_submission(SubmissionId{.value = *id},
                SubmissionSettlement{.status = "unanswered", .reason = std::string{"aborted"}});
        if (!settled) {
            return std::unexpected(settled.error());
        }
        items->erase(items->begin() + static_cast<std::ptrdiff_t>(index));
    }
    auto written = inbox.set(inbox_items_path(), Json{*items});
    if (!written) {
        return std::unexpected(written.error());
    }
    return {};
}

support::ExpectedVoid remove_inbox_item(chord::Draft& inbox, SubmissionId id) {
    auto items = read_inbox_items(inbox);
    if (!items) {
        return std::unexpected(items.error());
    }
    for (std::size_t index = 0; index < items->size(); ++index) {
        const auto* item = (*items)[index].as_object();
        if (item == nullptr) {
            return std::unexpected(validation("An inbox item must be an object"));
        }
        auto item_id = inbox_item_id(*item);
        if (!item_id) {
            return std::unexpected(item_id.error());
        }
        if (*item_id == id.value) {
            items->erase(items->begin() + static_cast<std::ptrdiff_t>(index));
            return inbox.set(inbox_items_path(), Json{*items});
        }
    }
    return {};
}

} // namespace cch::durable
