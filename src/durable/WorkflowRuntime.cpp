#include "StorageEncoding.hpp"
#include "WorkflowRuntime.hpp"

#include "durable/CommittedContext.hpp"
#include "durable/Compaction.hpp"
#include "support/Json.hpp"

#include <cch/ai/InferenceFailure.hpp>
#include <cch/ai/StreamEvent.hpp>
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
#include <set>
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

/// The call as tool hooks see it (frozen pi `ToolHookCall`): the model's
/// arguments, or the arguments a `beforeTool` rewrite left for the next
/// handler and for execution.
Json tool_call_json(const ai::ToolCallContent& call, const Json& arguments) {
    Object value{{"type", "toolCall"}, {"id", call.id}, {"name", call.name}, {"arguments", arguments}};
    if (call.thought_signature) value.emplace("thoughtSignature", *call.thought_signature);
    return Json{std::move(value)};
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
    Array removed;
    for (const auto& tool : message.tools_removed)
        removed.push_back(Json{Object{{"name", tool.name}}});
    if (!removed.empty()) result.emplace("toolsRemoved", Json{std::move(removed)});
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

struct FailureCapture {
    std::optional<ai::InferenceFailure> inference_failure{};
};

/// Error text for a generation whose blocking compaction ended without
/// completing (frozen pi `compactionFailure`).
std::string compaction_failure_text(const TaskOutcome& outcome) {
    if (outcome.status == "failed" || outcome.status == "faulted") {
        return "Compaction failed: " + outcome.error.value_or(TaskOutcomeError{}).message;
    }
    if (outcome.status == "orphaned") {
        return "Compaction failed: " + outcome.reason.value_or("orphaned");
    }
    return "Compaction was aborted";
}

std::string thinking_name(ai::ThinkingLevel level) {
    for (const auto& [model_level, name] : ai::kModelThinkingLevels) {
        (void)model_level;
        if (ai::parse_stream_thinking_level(name) == level) return std::string{name};
    }
    std::terminate();
}

/// Every stored change of these documents is a complete base (frozen pi
/// `AgentDoc.checkpointWhen`, `ProviderDoc.checkpointWhen` and
/// `UsageDoc.checkpointWhen`).
bool always_checkpoint(const chord::Json&, const chord::OpBatch&, std::int64_t) { return true; }

/// The starting live document of every conversation (frozen pi
/// `LiveDoc.initial`).
Json initial_live_value() { return Json{Object{}}; }

/// A complete base whenever nothing runs: no live run publication (frozen pi
/// `LiveDoc.checkpointWhen`, spec §8.2).
bool live_idle_checkpoint(const chord::Json& value, const chord::OpBatch&, std::int64_t) {
    const auto* object = value.as_object();
    return object == nullptr || object->find("run") == object->end();
}

/// The definitions the root conversation seeds itself with (frozen pi
/// `AgentDoc` / `InboxDoc` / `LiveDoc` / `ProviderDoc` / `UsageDoc`). Seeding
/// reuses the same definitions the runtime reads with, so the stored record
/// always matches the accessor's scope, history and fork semantics.
std::vector<DocumentDefinition> initial_document_definitions(const RootOptions& options, std::string provider_session) {
    Object agent;
    if (!options.model.id.empty()) {
        agent.emplace("model", Json{Object{{"provider", options.model.provider}, {"modelId", options.model.id}}});
    }
    if (options.thinking_level) agent.emplace("thinkingLevel", thinking_name(*options.thinking_level));
    std::vector<DocumentDefinition> definitions;
    definitions.reserve(5);
    definitions.push_back(detail::builtin_document(
            "pi.agent:v1",
            DocumentHistory::Rewindable,
            DocumentFork::AsOf,
            [value = Json{std::move(agent)}] { return value; },
            always_checkpoint));
    definitions.push_back(inbox_document());
    definitions.push_back(live_document());
    definitions.push_back(detail::builtin_document(
            "pi.provider:v1",
            DocumentHistory::Latest,
            DocumentFork::Initial,
            [session = std::move(provider_session)] { return Json{Object{{"sessionId", Json{session}}}}; },
            always_checkpoint));
    definitions.push_back(detail::builtin_document(
            "pi.usage:v1",
            DocumentHistory::Latest,
            DocumentFork::Initial,
            [] { return Json{Object{{"models", Json{Object{}}}, {"tools", Json{Object{}}}}}; },
            always_checkpoint));
    return definitions;
}

support::Error validation(std::string message) {
    return support::make_error(support::ErrorCode::Validation, std::move(message));
}

const Json* member(const Json& value, std::string_view key) {
    const auto* object = value.as_object();
    if (object == nullptr) return nullptr;
    const auto found = object->find(std::string{key});
    return found == object->end() ? nullptr : &found->second;
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

std::optional<ai::UserMessage> user_message_from_json(const Json& message);

std::optional<ai::Tool> tool_from_json(const Json& value) {
    const auto* object = value.as_object();
    if (object == nullptr) return std::nullopt;
    const auto name = object->find("name");
    const auto description = object->find("description");
    const auto parameters = object->find("parameters");
    if (name == object->end() || name->second.as_string() == nullptr || description == object->end() ||
            description->second.as_string() == nullptr || parameters == object->end())
        return std::nullopt;
    ai::Tool tool{.name = *name->second.as_string(),
            .description = *description->second.as_string(),
            .parameters = parameters->second.to_support()};
    if (const auto found = object->find("outputSchema"); found != object->end()) {
        tool.output_schema = found->second.to_support();
    }
    return tool;
}

std::optional<ai::MessageVariant> message_from_json(const Json& message) {
    const auto* object = message.as_object();
    if (object == nullptr) return std::nullopt;
    const auto role = object->find("role");
    if (role == object->end() || role->second.as_string() == nullptr) return std::nullopt;
    const std::string& role_name = *role->second.as_string();
    const auto timestamp_field = object->find("timestamp");
    const auto timestamp = timestamp_field != object->end() && timestamp_field->second.as_number() != nullptr
                                   ? static_cast<std::int64_t>(*timestamp_field->second.as_number())
                                   : 0;
    const auto content = object->find("content");
    if (role_name == "user") {
        auto user = user_message_from_json(message);
        if (!user) return std::nullopt;
        return std::optional<ai::MessageVariant>{std::in_place, ai::MessageVariant{std::move(*user)}};
    }
    if (role_name == "system") {
        ai::SystemMessage result{.timestamp = timestamp};
        if (content != object->end()) {
            if (const std::string* text = content->second.as_string())
                result.content = *text;
            else if (const auto* blocks = content->second.as_array())
                for (const Json& item : *blocks) {
                    const auto* block = item.as_object();
                    if (block == nullptr) continue;
                    const auto text = block->find("text");
                    if (text != block->end() && text->second.as_string() != nullptr)
                        result.content += *text->second.as_string();
                }
        }
        if (const auto sections = object->find("sections"); sections != object->end()) {
            if (const auto* fields = sections->second.as_object())
                for (const auto& [name, value] : *fields) {
                    result.sections.push_back(ai::SystemMessageSection{.name = name,
                            .text = value.as_string() ? std::optional<std::string>{*value.as_string()} : std::nullopt});
                }
        }
        if (const auto tools = object->find("toolsAdded"); tools != object->end()) {
            if (const auto* values = tools->second.as_array())
                for (const Json& tool : *values) {
                    if (auto parsed = tool_from_json(tool); parsed.has_value())
                        result.tools_added.push_back(std::move(*parsed));
                }
        }
        if (const auto tools = object->find("toolsRemoved"); tools != object->end()) {
            if (const auto* values = tools->second.as_array())
                for (const Json& tool : *values) {
                    if (const auto* reference = tool.as_object()) {
                        const auto name = reference->find("name");
                        if (name != reference->end() && name->second.as_string() != nullptr)
                            result.tools_removed.push_back(ai::ToolReference{.name = *name->second.as_string()});
                    }
                }
        }
        return ai::MessageVariant{std::move(result)};
    }
    const auto blocks = [content, object]() {
        std::vector<ai::Content> parsed;
        if (content == object->end()) return parsed;
        if (const auto* values = content->second.as_array())
            for (const Json& item : *values) {
                const auto* block = item.as_object();
                if (block == nullptr) continue;
                const auto type = block->find("type");
                if (type == block->end() || type->second.as_string() == nullptr) continue;
                if (*type->second.as_string() == "text") {
                    const auto text = block->find("text");
                    if (text != block->end() && text->second.as_string() != nullptr)
                        parsed.push_back(ai::text_content(*text->second.as_string()));
                } else if (*type->second.as_string() == "thinking") {
                    const auto thinking = block->find("thinking");
                    if (thinking != block->end() && thinking->second.as_string() != nullptr)
                        parsed.push_back(ai::thinking_content(*thinking->second.as_string()));
                } else if (*type->second.as_string() == "image") {
                    const auto data = block->find("data");
                    const auto mime = block->find("mimeType");
                    if (data != block->end() && data->second.as_string() != nullptr && mime != block->end() &&
                            mime->second.as_string() != nullptr)
                        parsed.push_back(ai::image_content(*data->second.as_string(), *mime->second.as_string()));
                }
            }
        return parsed;
    }();
    if (role_name == "toolResult") {
        const auto call_id = object->find("toolCallId");
        const auto tool_name = object->find("toolName");
        const auto is_error = object->find("isError");
        return ai::MessageVariant{ai::ToolResultMessage{
                .tool_call_id = call_id != object->end() && call_id->second.as_string() ? *call_id->second.as_string()
                                                                                        : std::string{},
                .tool_name = tool_name != object->end() && tool_name->second.as_string()
                                     ? *tool_name->second.as_string()
                                     : std::string{},
                .content = blocks,
                .is_error = is_error != object->end() && is_error->second.as_boolean() != nullptr &&
                            *is_error->second.as_boolean(),
                .timestamp = timestamp}};
    }
    if (role_name == "assistant") {
        ai::AssistantMessage result{.timestamp = timestamp};
        if (const auto found = object->find("stopReason");
                found != object->end() && found->second.as_string() != nullptr) {
            result.stop_reason =
                    ai::stop_reason_from_string(*found->second.as_string()).value_or(ai::AssistantStopReason::Error);
        }
        if (const auto found = object->find("api"); found != object->end() && found->second.as_string())
            result.api = *found->second.as_string();
        if (const auto found = object->find("provider"); found != object->end() && found->second.as_string())
            result.provider = *found->second.as_string();
        if (const auto found = object->find("model"); found != object->end() && found->second.as_string())
            result.model = *found->second.as_string();
        if (const auto found = object->find("errorMessage"); found != object->end() && found->second.as_string())
            result.error_message = *found->second.as_string();
        for (const auto& block : blocks) {
            if (const auto* text = std::get_if<ai::TextContent>(&block))
                result.content.emplace_back(*text);
            else if (const auto* thinking = std::get_if<ai::ThinkingContent>(&block))
                result.content.emplace_back(*thinking);
        }
        return ai::MessageVariant{std::move(result)};
    }
    return std::nullopt;
}

std::optional<ai::UserMessage> user_message_from_json(const Json& message) {
    const auto* object = message.as_object();
    if (object == nullptr) return std::nullopt;
    const auto role = object->find("role");
    if (role == object->end() || role->second.as_string() == nullptr || *role->second.as_string() != "user") {
        return std::nullopt;
    }
    const auto content = object->find("content");
    if (content == object->end()) return std::nullopt;
    std::vector<ai::Content> blocks;
    if (const std::string* text = content->second.as_string()) {
        blocks.push_back(ai::text_content(*text));
    } else if (const auto* values = content->second.as_array()) {
        for (const Json& block_json : *values) {
            const auto* block = block_json.as_object();
            if (block == nullptr) continue;
            const auto type = block->find("type");
            if (type == block->end() || type->second.as_string() == nullptr) continue;
            if (*type->second.as_string() == "text") {
                const auto text = block->find("text");
                if (text != block->end() && text->second.as_string() != nullptr)
                    blocks.push_back(ai::text_content(*text->second.as_string()));
            } else if (*type->second.as_string() == "image") {
                const auto data = block->find("data");
                const auto mime = block->find("mimeType");
                if (data != block->end() && data->second.as_string() != nullptr && mime != block->end() &&
                        mime->second.as_string() != nullptr) {
                    blocks.push_back(ai::image_content(*data->second.as_string(), *mime->second.as_string()));
                }
            }
        }
    }
    std::int64_t timestamp = 0;
    if (const auto value = object->find("timestamp"); value != object->end() && value->second.as_number() != nullptr) {
        timestamp = static_cast<std::int64_t>(*value->second.as_number());
    }
    return ai::UserMessage{.content = std::move(blocks), .timestamp = timestamp};
}

// ── Lane C admission helpers (frozen pi inbox.ts, live.ts, submissions.ts) ──

constexpr std::string_view kInboxDocKind{"pi.inbox:v1"};
constexpr std::string_view kLiveDocKind{"pi.live:v1"};
/// The built-in task kind whose hooks a model-issued tool call dispatches
/// (frozen pi `ToolTask`).
constexpr std::string_view kToolTaskKind{"pi.tool"};
constexpr std::string_view kGenerationTaskKind{"pi.generation"};

chord::Path inbox_items_path() { return chord::Path{chord::path_key("items")}; }

chord::Path live_run_path() { return chord::Path{chord::path_key("run")}; }

/// The starting inbox of every conversation; `root()` creates the same
/// incarnation lazily (frozen pi `InboxDoc.initial`).
Json initial_inbox_value() { return Json{Object{{"items", Json{Array{}}}}}; }

/// The starting live document of every conversation (frozen pi
/// `LiveDoc.initial`).
/// Working copy of the staged inbox's item array (frozen `readInboxItems`).
support::Expected<Array> read_inbox_items(chord::Draft& inbox) {
    auto items = inbox.get(inbox_items_path());
    if (!items) return std::unexpected(items.error());
    auto* array = items->has_value() ? (**items).as_array() : nullptr;
    if (array == nullptr) return std::unexpected(validation("Inbox items must be an array"));
    return *array;
}

/// A write is display-only when it pins neither model messages nor a head or edits
/// (presence counts, even when empty; frozen `isDisplayOnly`).
bool is_display_only(const WriteEntry& entry) {
    return !entry.model.has_value() && !entry.head.has_value() && entry.edits.empty();
}

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
    if (const auto it = value->find("edits"); it != value->end()) {
        const auto* edits = it->second.as_array();
        if (edits == nullptr) return std::unexpected(validation("A write entry edits must be an array"));
        for (const Json& item : *edits) {
            const auto* edit = item.as_object();
            if (edit == nullptr) return std::unexpected(validation("A context edit must be an object"));
            const auto target = edit->find("target");
            const auto action = edit->find("action");
            if (target == edit->end() || action == edit->end() || action->second.as_string() == nullptr) {
                return std::unexpected(validation("A context edit must have a target and action"));
            }
            auto target_id = as_integer(target->second);
            if (!target_id) return std::unexpected(target_id.error());
            ContextEdit decoded{.target = EntryId{.value = *target_id}, .action = *action->second.as_string()};
            if (decoded.action != "omit" && decoded.action != "replace") {
                return std::unexpected(validation("A context edit action must be \"omit\" or \"replace\""));
            }
            if (const auto messages = edit->find("messages"); messages != edit->end()) {
                const auto* array = messages->second.as_array();
                if (array == nullptr) return std::unexpected(validation("Context edit messages must be an array"));
                decoded.messages = *array;
            } else if (decoded.action == "replace") {
                return std::unexpected(validation("A replace context edit must carry messages"));
            }
            entry.edits.push_back(std::move(decoded));
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
    return tx.append_entry(conversation_id,
            entry.kind,
            std::move(data),
            head,
            entry.model.value_or(std::vector<Json>{}),
            std::nullopt,
            entry.edits);
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

/// Hand the live run to a successor generation task, keeping its inputs
/// (frozen pi `handOver`). The successor is created in the same commit that
/// publishes the new run marker, so no observer sees the run name a terminal
/// task.
support::ExpectedVoid hand_over(
        Transaction& tx, ConversationId conversation_id, chord::Draft& live, const TaskDefinition& definition) {
    auto run = staged_live_run(live);
    if (!run) return std::unexpected(run.error());
    if (!run->has_value()) return std::unexpected(validation("A live run must exist to hand over"));
    return start_run(tx, conversation_id, live, (*run)->inputs, definition);
}

/// Decode the content blocks a hook returns (frozen pi `Content`).
support::Expected<std::vector<ai::Content>> content_from_json(const Json& value) {
    const auto* blocks = value.as_array();
    if (blocks == nullptr) return std::unexpected(validation("Hook content must be an array"));
    std::vector<ai::Content> content;
    content.reserve(blocks->size());
    for (const Json& item : *blocks) {
        const Json* type = member(item, "type");
        if (type == nullptr || type->as_string() == nullptr) {
            return std::unexpected(validation("A content block must name its type"));
        }
        if (*type->as_string() == "text") {
            const Json* text = member(item, "text");
            if (text == nullptr || text->as_string() == nullptr) {
                return std::unexpected(validation("A text block must carry text"));
            }
            content.push_back(ai::text_content(*text->as_string()));
        } else if (*type->as_string() == "image") {
            const Json* data = member(item, "data");
            const Json* mime = member(item, "mimeType");
            if (data == nullptr || data->as_string() == nullptr || mime == nullptr || mime->as_string() == nullptr) {
                return std::unexpected(validation("An image block must carry data and mimeType"));
            }
            content.push_back(ai::image_content(*data->as_string(), *mime->as_string()));
        } else if (*type->as_string() == "thinking") {
            const Json* thinking = member(item, "thinking");
            if (thinking == nullptr || thinking->as_string() == nullptr) {
                return std::unexpected(validation("A thinking block must carry thinking"));
            }
            content.push_back(ai::thinking_content(*thinking->as_string()));
        } else {
            return std::unexpected(validation("A tool result block must be text, image or thinking"));
        }
    }
    return content;
}

/// The content of a harness-produced tool result: its diagnostic rendered in
/// the frozen `<harness>` block (frozen pi `appendToolResult` of
/// `harnessError`).
std::vector<ai::Content> harness_error_content(std::string_view message) {
    return {ai::text_content(std::format("<harness>\n[error] {}\n</harness>", message))};
}

/// Invoke every handler registered for `name` on task kind `kind` in
/// extension order: ordinary errors are isolated through the runtime's report
/// sink and cancellation propagates (frozen pi `HookRunner.each`).
/// `TaskRuntime::each_hook` filters on the invocation's own task kind and
/// freezes the handlers at the invocation's first request; the built-in
/// generation dispatches its hooks from the request's registry snapshot
/// instead, because one C++ phase invocation issues several frozen requests.
support::AsyncResult<void> each_kind_hook(std::span<const std::shared_ptr<TaskHookRegistration>> hooks,
        std::string_view kind,
        std::string_view name,
        TaskRuntime& runtime,
        HookInvokeHook invoke) {
    if (!invoke) return support::AsyncResult<void>{support::ExpectedVoid{}};
    const std::string hook_name{name};
    for (const auto& hook_reg : hooks) {
        if (runtime.stop_token().stop_requested()) {
            return support::AsyncResult<void>{
                    std::unexpected(support::make_error(support::ErrorCode::Cancelled, "Task invocation was aborted"))};
        }
        if (!hook_reg || std::string_view{hook_reg->task} != kind) continue;
        const auto handler = hook_reg->handlers.find(hook_name);
        if (handler == hook_reg->handlers.end() || !handler->second) continue;
        auto invoked = await_result(invoke(handler->second));
        if (!invoked) {
            if (runtime.stop_token().stop_requested()) {
                return support::AsyncResult<void>{std::unexpected(invoked.error())};
            }
            runtime.report(invoked.error());
        } else if (runtime.stop_token().stop_requested()) {
            return support::AsyncResult<void>{
                    std::unexpected(support::make_error(support::ErrorCode::Cancelled, "Task invocation was aborted"))};
        }
    }
    return support::AsyncResult<void>{support::ExpectedVoid{}};
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
        const std::string_view kind = entry.has_value() ? std::string_view{entry->kind} : std::string_view{"pi.user"};
        if (kind == "pi.reset") {
            result.user_entries.clear();
            result.users.clear();
        }
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

TaskHookRegistration tool_hook(std::string hook_name, TaskHook handler) {
    return task_hook(std::string{kToolTaskKind}, std::move(hook_name), std::move(handler));
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
    // The frozen v1.3.0 generation alternates `prepare` (compaction resume,
    // prompt planning, threshold decision) and `request` (stream, tool
    // rounds, overflow recovery); each phase run is one scheduler round.
    builder.initial([](const Json&) { return Json{Object{{"phase", "prepare"}, {"attempt", numeric(1)}}}; });
    // This runtime is owned by Harness::Impl, which joins every execution
    // before releasing the definition or these callbacks' borrowed pointer.
    builder.phase("prepare", [this](const TaskRecord& task, TaskRuntime& runtime) { return dispatch(task, runtime); });
    builder.phase("request", [this](const TaskRecord& task, TaskRuntime& runtime) { return dispatch(task, runtime); });
    builder.phase("retry", [this](const TaskRecord& task, TaskRuntime& runtime) { return dispatch(task, runtime); });
    // The generation's hook surface (frozen pi `GenerationHooks`): a final
    // answer may ask for one continuation that keeps the admitted run.
    builder.hook("onYield");
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
                    const auto documents = initial_document_definitions(
                            *selected, boost::uuids::to_string(boost::uuids::random_generator{}()));
                    for (const DocumentDefinition& definition : documents) {
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

support::AsyncResult<ConversationRecord> WorkflowRuntime::fork_conversation(ConversationId parent, EntryId at) {
    auto forked = std::make_shared<std::optional<ConversationRecord>>();
    return support::AsyncResult<ConversationRecord>{support::AsyncProducer<ConversationRecord, support::Error>{
            [this, parent, at, forked](
                    support::AsyncCompletion<ConversationRecord, support::Error> done) mutable noexcept {
                session_.commit([forked, parent, at](Transaction& tx) -> support::ExpectedVoid {
                            auto record = tx.fork_conversation(parent, at);
                            if (!record) return std::unexpected(record.error());
                            *forked = std::move(*record);
                            return {};
                        })
                        .start([forked, done = std::move(done)](support::ExpectedVoid outcome) mutable noexcept {
                            if (!outcome)
                                done(std::unexpected(outcome.error()));
                            else
                                done(std::move(**forked));
                        });
            }}};
}

support::AsyncResult<void> WorkflowRuntime::reset(ConversationId conversation_id, std::optional<std::string> handoff) {
    WriteEntry entry{.kind = "pi.reset", .head = EntryHeadVariant{SelfEntryHead{}}};
    if (handoff.has_value()) {
        const Json message{Object{{"role", "user"}, {"content", *handoff}, {"timestamp", numeric(now())}}};
        entry.model = std::vector<Json>{message};
    }
    SubmissionDraft draft{.body = WriteSubmissionDraft{.entry = std::move(entry)}};
    auto submitted = submit(conversation_id, std::move(draft));
    return support::AsyncResult<void>{support::AsyncProducer<void, support::Error>{
            [submitted = std::move(submitted)](support::AsyncCompletion<void, support::Error> done) mutable noexcept {
                std::move(submitted).start(
                        [done = std::move(done)](support::Expected<SubmissionId> outcome) mutable noexcept {
                            if (!outcome)
                                done(std::unexpected(outcome.error()));
                            else
                                done({});
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
    if (!conversation->has_value()) {
        return std::unexpected(validation("The conversation does not exist"));
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
            // The queued reset reached the final boundary through admission;
            // clear model history while retaining prompt deltas already shown.
            replay_[conversation_id.value].messages.clear();
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
        if (write->entry.kind == "pi.reset") {
            ReplayState& replay = replay_[conversation_id.value];
            replay.messages.clear();
            const std::vector<Json> model = write->entry.model.value_or(std::vector<Json>{});
            const auto handoff = std::ranges::find_if(model, [](const Json& value) {
                const auto* object = value.as_object();
                if (object == nullptr) return false;
                const auto role = object->find("role");
                return role != object->end() && role->second.as_string() != nullptr &&
                       *role->second.as_string() == "user";
            });
            if (handoff != model.end()) {
                if (auto parsed = message_from_json(*handoff); parsed.has_value())
                    replay.messages.push_back(std::move(*parsed));
            }
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
    auto running = staged_live_run(*live);
    if (!running) return std::unexpected(running.error());
    const bool run_already_exists = running->has_value();
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
    if (run_already_exists) {
        if (!appended->model.empty()) {
            if (auto parsed = message_from_json(appended->model.front()); parsed.has_value()) {
                replay_[conversation_id.value].messages.push_back(std::move(*parsed));
            }
        }
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
        if (applied->reset) {
            // The queued reset cut the run's context before an answer; the
            // selected user entries begin the replacement run's context.
            replay_[task.conversation_id.value].messages.clear();
            for (const EntryRecord& entry : applied->user_entries) {
                auto user = input_user(entry);
                if (!user) return std::unexpected(user.error());
                replay_[task.conversation_id.value].messages.push_back(std::move(*user));
            }
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
        // The placed users join this run and seed its next request.
        for (const EntryRecord& entry : applied->user_entries) {
            auto user = input_user(entry);
            if (!user) return std::unexpected(user.error());
            replay_[task.conversation_id.value].messages.push_back(std::move(*user));
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

support::AsyncResult<void> WorkflowRuntime::dispatch(const TaskRecord& task, TaskRuntime& runtime) {
    const Json* checkpoint = task.state.checkpoint ? &*task.state.checkpoint : nullptr;
    const std::string_view phase = [checkpoint]() -> std::string_view {
        if (checkpoint == nullptr) return "prepare";
        const Json* value = member(*checkpoint, "phase");
        if (value == nullptr) return "prepare";
        const std::string* text = value->as_string();
        return text == nullptr ? std::string_view{"prepare"} : std::string_view{*text};
    }();
    if (phase == "retry") {
        auto retried = retry(task, runtime);
        if (!retried) return support::AsyncResult<void>{std::unexpected(retried.error())};
        return support::AsyncResult<void>{support::ExpectedVoid{}};
    }
    if (phase == "request") {
        auto requested = request(task, runtime);
        if (!requested) return support::AsyncResult<void>{std::unexpected(requested.error())};
        return support::AsyncResult<void>{support::ExpectedVoid{}};
    }
    auto prepared = prepare(task, runtime);
    if (!prepared) return support::AsyncResult<void>{std::unexpected(prepared.error())};
    return support::AsyncResult<void>{support::ExpectedVoid{}};
}

/// Settle the run's inputs `unanswered/model_error` and fail with `text`
/// (frozen pi generation `failModelError`).
support::ExpectedVoid WorkflowRuntime::retry(const TaskRecord& task, TaskRuntime& runtime) {
    if (!task.state.checkpoint) return std::unexpected(validation("Retry phase needs a checkpoint"));
    const Json* until_value = member(*task.state.checkpoint, "until");
    const Json* attempt_value = member(*task.state.checkpoint, "attempt");
    if (!until_value || !attempt_value) return std::unexpected(validation("Retry checkpoint is incomplete"));
    auto until = as_integer(*until_value);
    auto attempt = as_integer(*attempt_value);
    if (!until) return std::unexpected(until.error());
    if (!attempt) return std::unexpected(attempt.error());
    std::mutex mutex;
    std::condition_variable ready;
    std::stop_callback wake(runtime.stop_token(), [&ready] { ready.notify_all(); });
    std::unique_lock lock(mutex);
    if (*until > now())
        ready.wait_for(
                lock, std::chrono::milliseconds(*until - now()), [&] { return runtime.stop_token().stop_requested(); });
    if (runtime.stop_token().stop_requested()) return {};
    Object checkpoint{{"phase", "prepare"}, {"attempt", numeric(*attempt + 1)}};
    if (const Json* compacted = member(*task.state.checkpoint, "compacted"))
        checkpoint.emplace("compacted", *compacted);
    auto committed = await_result(
            runtime.commit([task, checkpoint = Json{std::move(checkpoint)}, attempt = *attempt + 1](Transaction& tx,
                                   const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
                auto live = tx.doc(live_document(), DocumentAddress{.conversation_id = task.conversation_id});
                if (!live) return std::unexpected(live.error());
                auto set = live->set(
                        chord::Path{chord::path_key("generation")}, Json{Object{{"attempt", numeric(attempt)}}});
                if (!set) return std::unexpected(set.error());
                return std::optional<NextTaskState>{NextTaskState::running(checkpoint)};
            }));
    if (!committed) return std::unexpected(committed.error());
    return {};
}

support::ExpectedVoid WorkflowRuntime::fail_model_error(
        const TaskRecord& task, TaskRuntime& runtime, std::string_view text) {
    const std::string message{text};
    auto committed = await_result(runtime.commit([task, message](Transaction& tx, const TaskRecord&)
                                                         -> support::Expected<std::optional<NextTaskState>> {
        auto live = tx.doc(live_document(), DocumentAddress{.conversation_id = task.conversation_id});
        if (!live) return std::unexpected(live.error());
        auto ended = end_run(tx,
                *live,
                task.id,
                SubmissionSettlement{
                        .status = "unanswered", .reason = std::string{"model_error"}, .detail = Json{message}});
        if (!ended) return std::unexpected(ended.error());
        return std::optional<NextTaskState>{NextTaskState::terminal(TaskOutcome{.status = "failed",
                .error = TaskOutcomeError{.message = message, .detail = Json{Object{{"reason", "model_error"}}}}})};
    }));
    if (!committed) return std::unexpected(committed.error());
    return {};
}

support::Expected<TaskId> WorkflowRuntime::create_compaction(
        Transaction& tx, const TaskRecord& task, std::string_view reason, bool background) const {
    if (compaction_ == nullptr) {
        return std::unexpected(validation("No compaction definition is installed"));
    }
    const Json input{Object{{"reason", std::string{reason}}}};
    return create_compaction_task(tx,
            *compaction_,
            task.conversation_id,
            input,
            background ? std::optional<TaskId>{} : std::optional<TaskId>{task.id},
            background);
}

void WorkflowRuntime::install_compaction_definition(std::shared_ptr<const TaskDefinition> definition) {
    compaction_ = std::move(definition);
}

const ai::Model& WorkflowRuntime::model() const noexcept { return model_; }
const HarnessSettings& WorkflowRuntime::settings() const noexcept { return settings_; }
ai::ModelStream WorkflowRuntime::stream(ai::AiContext context, ai::SimpleStreamOptions options) {
    return stream_(model_, std::move(context), std::move(options));
}

bool WorkflowRuntime::model_available(std::string_view provider, std::string_view model_id) const noexcept {
    // The bounded C++ slice has no model catalog: the Harness model is the
    // only one that can be served (frozen pi `runtime.models.getModel`).
    return !model_.id.empty() && model_.id == model_id && model_.provider == provider;
}

support::Expected<SubmissionId> WorkflowRuntime::admit_compaction_summary(
        Transaction& tx, ConversationId conversation_id, const SubmissionDraft& draft, std::int64_t now) {
    // The ordinary admission path is the frozen `placeSummary` contract: a
    // conversation-owned summary is placed at once when idle, queued to the
    // next boundary when busy, or settled `stale` when its head target left
    // the range.
    return admit_submission(tx, conversation_id, draft, now);
}

namespace {

/// Order-sensitive JSON value equality, the C++ form of frozen
/// `declarationsEqual`'s JSON round-trip comparison.
bool json_equal(const support::JsonValue& left, const support::JsonValue& right) {
    const auto left_text = support::write_json(left);
    const auto right_text = support::write_json(right);
    return left_text && right_text && *left_text == *right_text;
}

/// Whether two tools declare the same interface (frozen `declarationsEqual`).
/// C++ also persists `outputSchema`, so a changed schema is a changed
/// declaration and is removed and re-added like any other change.
bool declarations_equal(const ai::Tool& left, const ai::Tool& right) {
    if (left.name != right.name || left.description != right.description ||
            !json_equal(left.parameters, right.parameters)) {
        return false;
    }
    if (left.output_schema.has_value() != right.output_schema.has_value()) return false;
    if (left.output_schema && !json_equal(*left.output_schema, *right.output_schema)) return false;
    if (left.constrained_sampling.has_value() != right.constrained_sampling.has_value()) return false;
    return !left.constrained_sampling || left.constrained_sampling->variants == right.constrained_sampling->variants;
}

struct ToolChanges {
    std::vector<ai::ToolReference> removed{};
    std::vector<ai::Tool> added{};
};

/// Frozen `planTools`: keep unchanged declarations in place and append
/// additions; when that replay would not yield the desired order, remove every
/// offered tool and re-add every desired tool in order.
ToolChanges plan_tools(std::span<const ai::Tool> offered, std::span<const ai::Tool> desired) {
    std::vector<const ai::Tool*> kept;
    for (const auto& tool : offered) {
        const auto wanted = std::ranges::find_if(desired, [&tool](const ai::Tool& candidate) {
            return candidate.name == tool.name && declarations_equal(tool, candidate);
        });
        if (wanted != desired.end()) kept.push_back(&tool);
    }
    const auto is_kept = [&kept](const ai::Tool& tool) {
        return std::ranges::any_of(kept, [&tool](const ai::Tool* each) { return each->name == tool.name; });
    };
    std::vector<const ai::Tool*> added;
    for (const auto& tool : desired)
        if (!is_kept(tool)) added.push_back(&tool);
    std::vector<std::string_view> replayed;
    for (const auto* tool : kept)
        replayed.push_back(tool->name);
    for (const auto* tool : added)
        replayed.push_back(tool->name);
    bool order_matches = replayed.size() == desired.size();
    for (std::size_t index = 0; order_matches && index < replayed.size(); ++index)
        order_matches = replayed[index] == desired[index].name;
    ToolChanges changes;
    if (!order_matches) {
        for (const auto& tool : offered)
            changes.removed.push_back(ai::ToolReference{.name = tool.name});
        for (const auto& tool : desired)
            changes.added.push_back(tool);
        return changes;
    }
    for (const auto& tool : offered)
        if (!is_kept(tool)) changes.removed.push_back(ai::ToolReference{.name = tool.name});
    for (const auto* tool : added)
        changes.added.push_back(*tool);
    return changes;
}

using SectionPatch = std::vector<ai::SystemMessageSection>;

/// Frozen `planSections`: no patch, the minimal patch of changed values and
/// `null` removals, or a remove-all/re-add-all pair when the minimal patch
/// would leave a different order.
std::vector<SectionPatch> plan_sections(
        std::span<const ai::SystemMessageSection> shown, std::span<const ai::SystemMessageSection> desired) {
    const auto wanted = [&desired](std::string_view name) {
        return std::ranges::find_if(
                desired, [name](const ai::SystemMessageSection& section) { return section.name == name; });
    };
    const auto is_shown = [&shown](const ai::SystemMessageSection& section) {
        return std::ranges::any_of(shown, [&section](const auto& each) { return each.name == section.name; });
    };
    std::vector<std::string_view> patched_order;
    for (const auto& section : shown)
        if (wanted(section.name) != desired.end()) patched_order.push_back(section.name);
    for (const auto& section : desired)
        if (!is_shown(section)) patched_order.push_back(section.name);
    bool order_matches = patched_order.size() == desired.size();
    for (std::size_t index = 0; order_matches && index < patched_order.size(); ++index)
        order_matches = patched_order[index] == desired[index].name;
    if (!order_matches) {
        SectionPatch removal;
        for (const auto& section : shown)
            removal.push_back(ai::SystemMessageSection{.name = section.name, .text = std::nullopt});
        return {std::move(removal), SectionPatch{desired.begin(), desired.end()}};
    }
    SectionPatch patch;
    for (const auto& section : shown) {
        const auto next = wanted(section.name);
        if (next == desired.end())
            patch.push_back(ai::SystemMessageSection{.name = section.name, .text = std::nullopt});
        else if (next->text != section.text)
            patch.push_back(*next);
    }
    for (const auto& section : desired)
        if (!is_shown(section)) patch.push_back(section);
    if (patch.empty()) return {};
    return {std::move(patch)};
}

} // namespace

WorkflowRuntime::PromptPlan WorkflowRuntime::plan_prompt(const Registry::Snapshot& snapshot,
        std::span<const ai::Tool> shown_tools,
        std::span<const ai::SystemMessageSection> shown_sections,
        const Transaction::ContextView& view,
        std::int64_t timestamp) {
    PromptPlan plan;
    std::vector<ai::SystemMessageSection> desired;
    for (const auto& section : snapshot.sections) {
        const std::string text = section.tag ? std::format("<{}>\n{}\n</{}>", section.key, section.text, section.key) : section.text;
        if (plan.context.system_prompt) *plan.context.system_prompt += "\n\n";
        else plan.context.system_prompt.emplace();
        *plan.context.system_prompt += text;
        desired.push_back(ai::SystemMessageSection{.name = section.key, .text = text});
    }
    for (const auto& tool : snapshot.tools)
        plan.context.tools.push_back(tool->declaration);
    const auto later_system = [&] {
        return view.head && std::ranges::any_of(view.entries, [&view](const EntryRecord& e) {
            return e.kind == "pi.system" && e.id.value > view.head->id.value;
        });
    };
    if (view.head && !later_system()) {
        ai::SystemMessage baseline{.sections = desired, .tools_added = plan.context.tools, .timestamp = timestamp};
        for (const auto& entry : view.entries)
            if (entry.kind == "pi.system") plan.edits.push_back(ContextEdit{.target = entry.id, .action = "omit"});
        plan.systems.push_back(std::move(baseline));
        return plan;
    }
    auto section_patches = plan_sections(shown_sections, desired);
    auto tool_changes = plan_tools(shown_tools, plan.context.tools);
    const bool tools_changed = !tool_changes.removed.empty() || !tool_changes.added.empty();
    if (!tools_changed) {
        for (auto& patch : section_patches) {
            ai::SystemMessage message{.timestamp = timestamp};
            message.sections = std::move(patch);
            plan.systems.push_back(std::move(message));
        }
        return plan;
    }
    if (section_patches.empty()) {
        ai::SystemMessage message{.timestamp = timestamp};
        message.tools_removed = std::move(tool_changes.removed);
        message.tools_added = std::move(tool_changes.added);
        plan.systems.push_back(std::move(message));
        return plan;
    }
    // Tool changes ride on the last planned section entry (frozen
    // `planSystemEntries`).
    for (std::size_t index = 0; index < section_patches.size(); ++index) {
        ai::SystemMessage message{.timestamp = timestamp};
        message.sections = std::move(section_patches[index]);
        if (index + 1 == section_patches.size()) {
            message.tools_removed = std::move(tool_changes.removed);
            message.tools_added = std::move(tool_changes.added);
        }
        plan.systems.push_back(std::move(message));
    }
    return plan;
}

/// One final answer's `onYield` decision, taken before the commit that
/// appends the answer (frozen pi generation `answer`): the first hook that
/// returns a `continue` content wins and later handlers are not invoked.
support::Expected<std::optional<Json>> WorkflowRuntime::on_yield(
        const Registry::Snapshot& snapshot, const Json& answer, TaskRuntime& runtime) {
    auto continuation = std::make_shared<std::optional<Json>>();
    auto hooked = await_result(each_kind_hook(snapshot.hooks,
            kGenerationTaskKind,
            "onYield",
            runtime,
            [answer, continuation, &runtime](TaskHook& handler) -> support::AsyncResult<void> {
                if (continuation->has_value()) return support::AsyncResult<void>{support::ExpectedVoid{}};
                auto returned = handler(answer, runtime);
                return support::AsyncResult<void>{support::AsyncProducer<void, support::Error>{
                        [returned = std::move(returned), continuation](
                                support::AsyncCompletion<void, support::Error> done) mutable noexcept {
                            std::move(returned).start([continuation, done = std::move(done)](
                                                              support::Expected<Json> value) mutable noexcept {
                                if (!value) {
                                    done(std::unexpected(value.error()));
                                    return;
                                }
                                const Json* next = member(*value, "continue");
                                if (next != nullptr && (next->as_string() != nullptr || next->as_array() != nullptr)) {
                                    continuation->emplace(*next);
                                }
                                done(support::ExpectedVoid{});
                            });
                        }}};
            }));
    if (!hooked) return std::unexpected(hooked.error());
    return *continuation;
}

support::Expected<ai::ToolResultMessage> WorkflowRuntime::execute_tool(
        const ai::ToolCallContent& call, const Registry::Snapshot& snapshot, TaskRuntime& runtime) {
    ai::ToolResultMessage result{.tool_call_id = call.id, .tool_name = call.name, .timestamp = now()};
    auto found = std::ranges::find_if(snapshot.tools, [&call](const auto& tool) { return tool->declaration.name == call.name; });
    if (found == snapshot.tools.end()) {
        // A call to a tool the request did not offer settles before the tool
        // task, with no hook and no duration (frozen pi `startToolRound`).
        result.is_error = true;
        result.content = harness_error_content(std::format("Tool {} is not available", call.name));
        return result;
    }
    if (!call.arguments_valid || !call.arguments) {
        result.is_error = true;
        result.content = harness_error_content("Invalid tool arguments");
        return result;
    }
    // The call as tool hooks see it keeps the model's arguments; a `beforeTool`
    // rewrite changes what executes, never the call the later hooks read
    // (frozen pi `ToolTask` reads the call, the intent carries the rewrite).
    Json arguments = *Json::from_support(*call.arguments);
    const Json original = tool_call_json(call, arguments);
    // beforeTool: the first block wins, every `arguments` decision replaces
    // the call's arguments, and a throwing handler blocks the call (frozen pi
    // `ToolTask.call`). A blocked call never executes and skips afterTool.
    std::optional<std::string> block;
    for (const auto& hook_reg : snapshot.hooks) {
        if (block.has_value()) break;
        if (!hook_reg || std::string_view{hook_reg->task} != kToolTaskKind) continue;
        const auto handler = hook_reg->handlers.find("beforeTool");
        if (handler == hook_reg->handlers.end() || !handler->second) continue;
        if (runtime.stop_token().stop_requested()) {
            return std::unexpected(support::make_error(support::ErrorCode::Cancelled, "Task invocation was aborted"));
        }
        auto decided = await_result(handler->second(tool_call_json(call, arguments), runtime));
        if (!decided) {
            // A throw blocks with its message; cancellation is not a block
            // (frozen pi `ToolTask.call`).
            if (runtime.stop_token().stop_requested()) return std::unexpected(decided.error());
            block = decided.error().message;
            continue;
        }
        if (const Json* reason = member(*decided, "block"); reason != nullptr && reason->as_string() != nullptr) {
            block = *reason->as_string();
        } else if (const Json* rewritten = member(*decided, "arguments");
                rewritten != nullptr && rewritten->as_object() != nullptr) {
            arguments = *rewritten;
        }
    }
    if (block.has_value()) {
        result.is_error = true;
        result.content = harness_error_content(std::format("Tool call blocked: {}", *block));
        return result;
    }
    const auto started = std::chrono::steady_clock::now();
    auto output = await_result((*found)->execute(arguments.to_support(), runtime.stop_token()));
    result.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    result.is_error = !output.has_value();
    result.content = output ? std::move(*output) : std::vector<ai::Content>{ai::text_content(output.error().message)};
    // afterTool: every replacement becomes the next handler's result and the
    // committed entry carries the last one (frozen pi `finalResult`); ordinary
    // handler errors stay isolated.
    Json final_result = Json{Object{{"output", blocks_json(result.content)}, {"isError", result.is_error}}};
    auto hooked = await_result(each_kind_hook(snapshot.hooks,
            kToolTaskKind,
            "afterTool",
            runtime,
            [original, final = &final_result, &runtime](TaskHook& handler) -> support::AsyncResult<void> {
                auto returned = handler(Json{Object{{"call", original}, {"result", *final}}}, runtime);
                return support::AsyncResult<void>{support::AsyncProducer<void, support::Error>{
                        [returned = std::move(returned), final](
                                support::AsyncCompletion<void, support::Error> done) mutable noexcept {
                            std::move(returned).start(
                                    [final, done = std::move(done)](support::Expected<Json> value) mutable noexcept {
                                        if (!value) {
                                            done(std::unexpected(value.error()));
                                            return;
                                        }
                                        if (value->as_object() != nullptr) *final = std::move(*value);
                                        done(support::ExpectedVoid{});
                                    });
                        }}};
            }));
    if (!hooked) return std::unexpected(hooked.error());
    if (const Json* blocks = member(final_result, "output"); blocks != nullptr) {
        auto decoded = content_from_json(*blocks);
        if (!decoded) return std::unexpected(decoded.error());
        result.content = std::move(*decoded);
    }
    if (const Json* is_error = member(final_result, "isError");
            is_error != nullptr && is_error->as_boolean() != nullptr) {
        result.is_error = *is_error->as_boolean();
    }
    return result;
}

support::ExpectedVoid WorkflowRuntime::prepare(const TaskRecord& task, TaskRuntime& runtime) {
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
    const Json* checkpoint = task.state.checkpoint ? &*task.state.checkpoint : nullptr;
    const std::int64_t attempt = [&]() -> std::int64_t {
        const Json* value = checkpoint == nullptr ? nullptr : member(*checkpoint, "attempt");
        if (value == nullptr) return 1;
        auto parsed = as_integer(*value);
        return parsed ? *parsed : 1;
    }();
    const std::optional<TaskId> compacted = [&]() -> std::optional<TaskId> {
        const Json* value = checkpoint == nullptr ? nullptr : member(*checkpoint, "compacted");
        if (value == nullptr) return std::nullopt;
        auto parsed = as_integer(*value);
        if (!parsed) return std::nullopt;
        return TaskId{.value = *parsed};
    }();
    const bool threshold_blocked = [&]() -> bool {
        const Json* value = checkpoint == nullptr ? nullptr : member(*checkpoint, "threshold");
        return value != nullptr && value->as_boolean() != nullptr && *value->as_boolean();
    }();
    const std::optional<std::string> overflow_text = [&]() -> std::optional<std::string> {
        const Json* value = checkpoint == nullptr ? nullptr : member(*checkpoint, "overflow");
        if (value == nullptr) return std::nullopt;
        const std::string* text = value->as_string();
        return text == nullptr ? std::optional<std::string>{} : std::optional<std::string>{*text};
    }();

    if (compacted.has_value()) {
        auto outcome = storage_.task(*compacted);
        if (!outcome) return std::unexpected(outcome.error());
        if (overflow_text.has_value()) {
            // The overflow recovery must have appended a summary; otherwise
            // the original overflow error stands (frozen pi generation
            // `prepare`).
            const bool answered = outcome->has_value() && (*outcome)->state.status == "terminal" &&
                                  (*outcome)->state.outcome.has_value() &&
                                  (*outcome)->state.outcome->status == "completed" &&
                                  (*outcome)->state.outcome->result.has_value() &&
                                  member(*(*outcome)->state.outcome->result, "entryId") != nullptr;
            if (!answered) {
                return fail_model_error(task, runtime, *overflow_text);
            }
        } else if (threshold_blocked && outcome->has_value() && (*outcome)->state.status == "terminal" &&
                   (*outcome)->state.outcome.has_value() && (*outcome)->state.outcome->status != "completed") {
            // A threshold compaction that failed left a context it judged too
            // large: sending it would at best leave no room for the answer
            // (frozen pi generation `prepare`).
            return fail_model_error(task, runtime, compaction_failure_text(*(*outcome)->state.outcome));
        }
    }

    auto loaded_view = derive_committed_context(session_, task.conversation_id);
    if (!loaded_view) return std::unexpected(loaded_view.error());
    auto& view = *loaded_view;
    ReplayState replay;
    for (const auto& message : view.messages) {
        auto parsed = message_from_json(message);
        if (!parsed) continue;
        if (const auto* system = std::get_if<ai::SystemMessage>(&*parsed)) {
            for (const auto& section : system->sections) {
                auto it = std::ranges::find_if(
                        replay.shown_sections, [&section](const auto& value) { return value.name == section.name; });
                if (!section.text) {
                    if (it != replay.shown_sections.end()) replay.shown_sections.erase(it);
                } else if (it != replay.shown_sections.end())
                    *it = section;
                else
                    replay.shown_sections.push_back(section);
            }
            for (const auto& tool : system->tools_removed)
                std::erase_if(replay.shown_tools, [&tool](const auto& value) { return value.name == tool.name; });
            for (const auto& tool : system->tools_added) {
                std::erase_if(replay.shown_tools, [&tool](const auto& value) { return value.name == tool.name; });
                replay.shown_tools.push_back(tool);
            }
        }
    }
    auto plan = plan_prompt(registry_.snapshot(), replay.shown_tools, replay.shown_sections, view, now());
    std::vector<Json> planned;
    for (const auto& system : plan.systems)
        planned.push_back(system_json(system));

    // The threshold decision (frozen pi generation `prepare`): `blocking`
    // above `contextWindow - reserveTokens`, `background` above the
    // background threshold, and only when range selection finds a cut.
    enum class Threshold { None, Background, Blocking };
    Threshold threshold = Threshold::None;
    if (!compacted.has_value() && settings_.compaction.enabled && model_.context_window > 0) {
        const std::int64_t tokens = estimate_context_tokens(view, planned);
        const std::int64_t blocking_line =
                static_cast<std::int64_t>(model_.context_window) - settings_.compaction.reserve_tokens;
        const std::int64_t background_line = blocking_line - settings_.compaction.background_tokens;
        if (tokens > blocking_line) {
            threshold = Threshold::Blocking;
        } else if (settings_.compaction.background_tokens > 0 && tokens > background_line) {
            threshold = Threshold::Background;
        }
        if (threshold != Threshold::None && !select_cut(view, settings_.compaction.keep_recent_tokens).has_value()) {
            threshold = Threshold::None;
        }
    }

    if (threshold == Threshold::Blocking) {
        // Compact first and prepare again; the transcript is unchanged until
        // the compaction appends (frozen pi generation `prepare`).
        auto waited = await_result(runtime.commit([this, task, attempt](Transaction& tx, const TaskRecord&)
                                                          -> support::Expected<std::optional<NextTaskState>> {
            auto child = create_compaction(tx, task, "threshold", false);
            if (!child) return std::unexpected(child.error());
            return std::optional<NextTaskState>{NextTaskState::waiting(Json{Object{{"phase", "prepare"},
                                                                               {"attempt", numeric(attempt)},
                                                                               {"compacted", numeric(child->value)},
                                                                               {"threshold", true}}},
                    {*child},
                    JoinPolicy::AllSettled)};
        }));
        if (!waited) return std::unexpected(waited.error());
        return {};
    }

    auto committed = await_result(runtime.commit(
            [this, task, attempt, compacted, threshold, planned = std::move(planned), edits = plan.edits](
                    Transaction& tx, const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
                auto newest = tx.scan_entries(task.conversation_id, 1);
                if (!newest) return std::unexpected(newest.error());
                std::optional<EntryId> cutoff;
                if (!newest->empty()) cutoff = newest->front().id;
                for (std::size_t i = 0; i < planned.size(); ++i) {
                    auto entry = tx.append_entry(task.conversation_id,
                            "pi.system",
                            entry_data(planned[i]),
                            std::nullopt,
                            std::vector<chord::Json>{as_chord(planned[i])},
                            std::nullopt,
                            i == 0 ? edits : std::vector<ContextEdit>{});
                    if (!entry) return std::unexpected(entry.error());
                    cutoff = entry->id;
                }
                if (!cutoff.has_value()) return std::unexpected(validation("Conversation has no entries to send"));
                if (threshold == Threshold::Background) {
                    auto live = tx.doc(live_document(), DocumentAddress{.conversation_id = task.conversation_id});
                    if (!live) return std::unexpected(live.error());
                    auto listed = live_has_compaction(*live);
                    if (!listed) return std::unexpected(listed.error());
                    if (!*listed) {
                        auto child = create_compaction(tx, task, "threshold", true);
                        if (!child) return std::unexpected(child.error());
                    }
                }
                Object next{{"phase", "request"}, {"attempt", numeric(attempt)}, {"cutoff", numeric(cutoff->value)}};
                if (compacted) next.emplace("compacted", numeric(compacted->value));
                return std::optional<NextTaskState>{NextTaskState::running(Json{std::move(next)})};
            }));
    if (!committed) return std::unexpected(committed.error());
    return {};
}

support::ExpectedVoid WorkflowRuntime::request(const TaskRecord& task, TaskRuntime& runtime) {
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
    const Json* checkpoint = task.state.checkpoint ? &*task.state.checkpoint : nullptr;
    const std::int64_t attempt = [&]() -> std::int64_t {
        const Json* value = checkpoint == nullptr ? nullptr : member(*checkpoint, "attempt");
        if (value == nullptr) return 1;
        auto parsed = as_integer(*value);
        return parsed ? *parsed : 1;
    }();
    const std::optional<TaskId> compacted = [&]() -> std::optional<TaskId> {
        const Json* value = checkpoint == nullptr ? nullptr : member(*checkpoint, "compacted");
        if (value == nullptr) return std::nullopt;
        auto parsed = as_integer(*value);
        if (!parsed) return std::nullopt;
        return TaskId{.value = *parsed};
    }();
    const std::optional<EntryId> cutoff = [&]() -> std::optional<EntryId> {
        const Json* value = checkpoint == nullptr ? nullptr : member(*checkpoint, "cutoff");
        if (value == nullptr) return std::nullopt;
        auto parsed = as_integer(*value);
        if (!parsed) return std::nullopt;
        return EntryId{.value = *parsed};
    }();

    ReplayState& replay = replay_[task.conversation_id.value];
    replay = ReplayState{};
    Transaction::ContextView view;
    auto load_view = [&]() -> support::ExpectedVoid {
        auto committed = await_result(session_.commit([&](Transaction& tx) -> support::ExpectedVoid {
            auto loaded = tx.context_view(task.conversation_id);
            if (!loaded) return std::unexpected(loaded.error());
            view = std::move(*loaded);
            return {};
        }));
        if (!committed) return std::unexpected(committed.error());
        replay = ReplayState{};
        for (const auto& message : view.messages) {
            auto parsed = message_from_json(message);
            if (!parsed) continue;
            if (const auto* system = std::get_if<ai::SystemMessage>(&*parsed)) {
                for (const auto& section : system->sections) {
                    auto it = std::ranges::find_if(
                            replay.shown_sections, [&section](const auto& s) { return s.name == section.name; });
                    if (!section.text) {
                        if (it != replay.shown_sections.end()) replay.shown_sections.erase(it);
                    } else if (it != replay.shown_sections.end())
                        *it = section;
                    else
                        replay.shown_sections.push_back(section);
                }
                for (const auto& tool : system->tools_removed)
                    std::erase_if(replay.shown_tools, [&tool](const auto& t) { return t.name == tool.name; });
                for (const auto& tool : system->tools_added) {
                    std::erase_if(replay.shown_tools, [&tool](const auto& t) { return t.name == tool.name; });
                    replay.shown_tools.push_back(tool);
                }
            }
            replay.messages.push_back(std::move(*parsed));
        }
        return {};
    };

    while (!runtime.stop_token().stop_requested()) {
        const auto snapshot = registry_.snapshot();
        auto loaded = load_view();
        if (!loaded) return std::unexpected(loaded.error());
        auto plan = plan_prompt(snapshot, replay.shown_tools, replay.shown_sections, view, now());
        if (!plan.systems.empty()) {
            auto committed = await_result(runtime.commit(
                    [id = task.conversation_id, systems = plan.systems, edits = plan.edits](
                            Transaction& tx, const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
                        for (std::size_t i = 0; i < systems.size(); ++i) {
                            const auto message = system_json(systems[i]);
                            auto entry = tx.append_entry(id,
                                    "pi.system",
                                    entry_data(message),
                                    std::nullopt,
                                    std::vector<chord::Json>{as_chord(message)},
                                    std::nullopt,
                                    i == 0 ? edits : std::vector<ContextEdit>{});
                            if (!entry) return std::unexpected(entry.error());
                        }
                        return std::nullopt;
                    }));
            if (!committed) return std::unexpected(committed.error());
            loaded = load_view();
            if (!loaded) return std::unexpected(loaded.error());
        }
        auto& context = plan.context;
        context.messages = replay.messages;
        // Rendered sections and tools are carried in the provider context;
        // transcript system entries record their incremental history.
        if (!context.messages.empty()) {
            const auto* first_system = std::get_if<ai::SystemMessage>(&context.messages.front());
            if (first_system &&
                    (context.system_prompt ||
                            (first_system->content.empty() && first_system->sections.empty() &&
                                    first_system->tools_added.empty() && first_system->tools_removed.empty()))) {
                context.messages.erase(context.messages.begin());
            }
        }
        auto options = settings_.stream;
        if (thinking_level_) options.reasoning = thinking_level_;
        options.stop_token = runtime.stop_token();
        // The overflow retry decision reads the structured classification of
        // the terminal error event; the diagnostic text is never consulted
        // (#1083 contract, frozen pi `isRetryableAssistantError`).
        const auto captured = std::make_shared<FailureCapture>();
        auto stream = stream_(model_, std::move(context), options);
        auto response = await_result(
                std::move(stream).run([captured](const ai::AssistantStreamEvent& event) -> support::ExpectedVoid {
                    const auto* error = std::get_if<ai::AssistantErrorEvent>(&event);
                    if (error != nullptr) captured->inference_failure = error->inference_failure;
                    return {};
                }));
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
            replay.messages.push_back(*response);
            for (const auto& content : response->content) {
                const auto* call = std::get_if<ai::ToolCallContent>(&content);
                if (call == nullptr) continue;
                auto result = execute_tool(*call, snapshot, runtime);
                if (!result) return std::unexpected(result.error());
                auto saved = await_result(
                        runtime.commit([id = task.conversation_id, tid = task.id, result = *result](Transaction& tx,
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
                replay.messages.push_back(std::move(*result));
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
        // Overflow recovery: a context-overflow terminal error compacts the
        // committed context first and retries with the attempt and its
        // budget intact (frozen pi generation `classify`).
        std::optional<std::string> overflow_text;
        if (failed && captured->inference_failure.has_value() &&
                captured->inference_failure->kind == ai::InferenceFailureKind::ContextOverflow) {
            overflow_text = response->error_message.value_or("Context overflow");
        }
        if (overflow_text.has_value() && !compacted.has_value() && settings_.compaction.enabled && cutoff.has_value()) {
            auto view = derive_committed_context(session_, task.conversation_id, cutoff);
            if (!view) return std::unexpected(view.error());
            if (select_cut(*view, settings_.compaction.keep_recent_tokens).has_value()) {
                const std::string text = *overflow_text;
                auto waited = await_result(
                        runtime.commit([this, task, attempt, response = *response, text](Transaction& tx,
                                               const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
                            // The error entry lands in the same commit as the
                            // compaction task (frozen pi generation `classify`).
                            auto entry = tx.append_entry(task.conversation_id,
                                    "pi.assistant",
                                    entry_data(assistant_json(response)),
                                    std::nullopt,
                                    std::vector<chord::Json>{as_chord(assistant_json(response))});
                            if (!entry) return std::unexpected(entry.error());
                            auto child = create_compaction(tx, task, "overflow", false);
                            if (!child) return std::unexpected(child.error());
                            return std::optional<NextTaskState>{
                                    NextTaskState::waiting(Json{Object{{"phase", "prepare"},
                                                                   {"attempt", numeric(attempt)},
                                                                   {"compacted", numeric(child->value)},
                                                                   {"overflow", text}}},
                                            {*child},
                                            JoinPolicy::AllSettled)};
                        }));
                if (!waited) return std::unexpected(waited.error());
                return {};
            }
        }
        const bool retryable = failed && !overflow_text && captured->inference_failure &&
                               ai::is_retryable_inference_failure(*captured->inference_failure) &&
                               settings_.retry.enabled && attempt <= settings_.retry.max_retries;
        if (retryable) {
            const double multiplier = std::pow(2.0, static_cast<double>(std::max<std::int64_t>(0, attempt - 1)));
            const auto delay = static_cast<std::int64_t>(std::min<double>(settings_.retry.max_agent_delay_ms,
                    std::max<double>(0, settings_.retry.base_delay_ms) * multiplier));
            const auto until = now() + delay;
            auto retried = await_result(
                    runtime.commit([task, attempt, compacted, until, response = *response](Transaction& tx,
                                           const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
                        auto live = tx.doc(live_document(), DocumentAddress{.conversation_id = task.conversation_id});
                        if (!live) return std::unexpected(live.error());
                        auto entry = tx.append_entry(task.conversation_id,
                                "pi.assistant",
                                entry_data(assistant_json(response)),
                                std::nullopt,
                                std::vector<chord::Json>{as_chord(assistant_json(response))});
                        if (!entry) return std::unexpected(entry.error());
                        auto set = live->set(chord::Path{chord::path_key("generation")},
                                Json{Object{{"attempt", numeric(attempt)},
                                        {"retry",
                                                Json{Object{{"at", numeric(until)},
                                                        {"error", response.error_message.value_or("")}}}}}});
                        if (!set) return std::unexpected(set.error());
                        Object checkpoint{{"phase", "retry"}, {"attempt", numeric(attempt)}, {"until", numeric(until)}};
                        if (compacted) checkpoint.emplace("compacted", numeric(compacted->value));
                        return std::optional<NextTaskState>{NextTaskState::running(Json{std::move(checkpoint)})};
                    }));
            if (!retried) return std::unexpected(retried.error());
            return {};
        }
        // Final answer, model error, or abort: one commit appends the answer
        // and settles the run at the final boundary (frozen pi `answer`). The
        // `onYield` continuation is decided before the commit, so a hook may
        // read committed state and share the task's memos.
        auto continuation = std::make_shared<std::optional<Json>>();
        if (!aborted && !failed) {
            auto decided = on_yield(snapshot, assistant_json(*response), runtime);
            if (!decided) return std::unexpected(decided.error());
            *continuation = std::move(*decided);
        }
        auto outcome = await_result(runtime.commit(
                [id = task.conversation_id, task, response = *response, aborted, failed, continuation, this](
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
                        replay_[id.value].messages.clear();
                    }
                    if (continuation->has_value() && applied->users.empty() && !applied->reset) {
                        // The continuation appends a plain user entry, which is
                        // not a submission, and hands the same admitted run to
                        // a successor generation task in this commit (frozen pi
                        // `answer`): the run's inputs stay unsettled and settle
                        // once, under the successor.
                        auto appended = append_input_entry(tx, id, **continuation, std::nullopt, now());
                        if (!appended) return std::unexpected(appended.error());
                        auto handed = hand_over(tx, id, *live, *generation_);
                        if (!handed) return std::unexpected(handed.error());
                        return std::optional<NextTaskState>{NextTaskState::terminal(TaskOutcome{
                                .status = "completed", .result = Json{Object{{"entryId", numeric(entry->id.value)}}}})};
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
                            .status = "completed", .result = Json{Object{{"entryId", numeric(entry->id.value)}}}})};
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
    return detail::builtin_document(
            std::string{kInboxDocKind},
            DocumentHistory::Latest,
            DocumentFork::Initial,
            [] { return initial_inbox_value(); },
            // A drained inbox stores a complete base instead of a delta
            // chain (frozen pi `InboxDoc.checkpointWhen`).
            [](const chord::Json& value, const chord::OpBatch&, std::int64_t) {
                const auto* object = value.as_object();
                if (object == nullptr) return true;
                const auto it = object->find("items");
                if (it == object->end()) return true;
                const auto* items = it->second.as_array();
                return items == nullptr || items->empty();
            });
}

DocumentDefinition live_document() {
    return detail::builtin_document(
            std::string{kLiveDocKind},
            DocumentHistory::Latest,
            DocumentFork::Initial,
            [] { return initial_live_value(); },
            live_idle_checkpoint);
}

support::Expected<std::optional<LiveRunState>> committed_live_run(Storage& storage, ConversationId conversation_id) {
    auto found = storage.find_document(
            std::string{kLiveDocKind}, DocumentAddress{.conversation_id = conversation_id}, DocumentPoint::current());
    if (!found) {
        return std::unexpected(found.error());
    }
    if (!found->has_value()) {
        return std::optional<LiveRunState>{};
    }
    auto doc = storage.document(found->value().id, DocumentPoint::current());
    if (!doc) {
        return std::unexpected(doc.error());
    }
    if (!doc->has_value()) {
        return std::optional<LiveRunState>{};
    }
    const auto* value = (**doc).value.as_object();
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
