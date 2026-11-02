#include "durable/ViewMounts.hpp"

#include "durable/ObservationTestHooks.hpp"
#include "durable/StorageEncoding.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <format>
#include <mutex>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace cch::durable {

struct ViewWatch::Impl {
    /// One delivery's admission state, guarded by the mount mutex. `Idle`
    /// means no delivery is in flight, `Claimed` means a publication selected
    /// this watcher but has not entered its listener, `Entered` means the
    /// listener call has begun, and `Stopped` means no later delivery may
    /// enter. A stop transitions `Idle` and `Claimed` directly to `Stopped`;
    /// an `Entered` delivery keeps its state until the callback returns.
    enum class DispatchState {
        Idle,
        Claimed,
        Entered,
        Stopped,
    };

    std::shared_ptr<ViewMount> mount{};
    /// The acquisition revision before the first delivery; the latest
    /// delivered revision afterwards.
    chord::Json value{};
    DispatchState dispatch{DispatchState::Idle};
    bool stopped{false};
    WatchEndReason end{WatchEndReason::Stopped};
    ViewFrameListener listener{};
};

struct AgentEventStream::Impl {
    /// Frozen pi `MAX_PENDING_WATCH_FRAMES`: the exact committed batches one
    /// unavailable listener may keep before the buffer is replaced.
    static constexpr std::size_t kMaxPendingBatches = 100;

    std::shared_ptr<ViewMount> mount{};
    ConversationId conversation_id{};
    /// The constant `snapshot` event at attachment.
    chord::Json snapshot{};
    /// Generation tasks already held at `completing` at attachment; the
    /// translation mutates it in commit order (frozen pi `held`).
    std::vector<TaskId> held{};
    std::shared_ptr<AgentEventListener> listener_holder{};
    std::vector<AgentEventBatch> pending{};
    bool started{false};
    bool stopped{false};
    bool delivering{false};
    WatchEndReason end{WatchEndReason::Stopped};
    std::optional<std::string> listener_error{};
    std::shared_ptr<ObservationReportSink> reporter{};
    std::unique_ptr<std::stop_callback<std::move_only_function<void()>>> cancellation{};
    std::mutex mutex{};

    /// Translate one publication and buffer its batch; delivers when started.
    void advance(const chord::Json& before,
            const chord::Json& after,
            const chord::OpBatch& ops,
            const CommitPublication& publication);

    /// End the stream once: drop buffered batches, detach the mount, and settle
    /// the terminal result. Never waits for an entered listener.
    void terminate(WatchEndReason reason, std::optional<std::string> error = std::nullopt);

    /// Deliver buffered batches in commit order until empty, stopped, or a
    /// listener reports an error.
    void drain();
};

namespace {

std::mutex view_dispatch_hook_mutex;
testing::ViewDispatchHook view_dispatch_hook;

/// Copy the test-only hook under its mutex, then run the copy outside it. A
/// hook that blocks therefore cannot block clearing the hook.
void run_view_dispatch_hook(const testing::ViewDispatchHookPoint point) {
    testing::ViewDispatchHook hook;
    {
        std::lock_guard lock(view_dispatch_hook_mutex);
        hook = view_dispatch_hook;
    }
    if (hook) hook(point);
}

/// Record a stop under the mount mutex. An idle or claimed delivery becomes
/// stopped immediately; the claimed delivery's recheck cancels it without a
/// wait. An entered callback keeps its state until it returns.
void mark_stopped(ViewWatch::Impl& watcher, const WatchEndReason reason) {
    if (watcher.stopped) return;
    watcher.stopped = true;
    watcher.end = reason;
    if (watcher.dispatch != ViewWatch::Impl::DispatchState::Entered) {
        watcher.dispatch = ViewWatch::Impl::DispatchState::Stopped;
    }
}

/// The built-in conversation document kinds (frozen pi `BUILTIN_DOC_KINDS`),
/// keyed by their full kind including the definition version.
constexpr std::string_view kAgentDocKind = "pi.agent:v1";
constexpr std::string_view kInboxDocKind = "pi.inbox:v1";
constexpr std::string_view kLiveDocKind = "pi.live:v1";
constexpr std::string_view kProviderDocKind = "pi.provider:v1";
constexpr std::string_view kUsageDocKind = "pi.usage:v1";
constexpr std::string_view kMountedDocKinds[] = {
        kAgentDocKind, kInboxDocKind, kLiveDocKind, kProviderDocKind, kUsageDocKind};

/// The frozen pi `ConversationView` keys its docs map by the bare document
/// kind without the definition version (`pi.agent`), so the view does too.
[[nodiscard]] std::string bare_doc_kind(const std::string& kind) {
    const std::size_t separator = kind.rfind(':');
    return separator == std::string::npos ? kind : kind.substr(0, separator);
}

// A committed task record whose state machine reached its terminal outcome
// carries the literal status `"terminal"` (the outcome itself names
// completed/failed/aborted); the scheduler holds it as `"completing"` while
// ordinary owned work drains, which stays visible in the graph.
[[nodiscard]] bool terminal_status(const std::string& status) {
    return status == "terminal";
}

[[nodiscard]] std::string task_key(const TaskRecord& task) {
    return std::to_string(task.id.value);
}

[[nodiscard]] chord::Json id_array(const std::vector<std::int64_t>& ids) {
    chord::Json::Array values;
    values.reserve(ids.size());
    for (const std::int64_t id : ids) {
        values.emplace_back(static_cast<double>(id));
    }
    return chord::Json(std::move(values));
}

/// One task-graph node value (frozen pi `TaskGraphNode`), with the
/// checkpoint and outcome payloads reduced to their display fields
/// (frozen pi `toTaskGraphState`).
[[nodiscard]] chord::Json node_json(const TaskRecord& task, const std::vector<ConversationId>& owned) {
    chord::Json::Object state;
    state.emplace("status", chord::Json(task.state.status));
    std::string phase{};
    if (task.state.checkpoint.has_value()) {
        if (const auto* checkpoint = task.state.checkpoint->as_object(); checkpoint != nullptr) {
            const auto member = checkpoint->find("phase");
            if (member != checkpoint->end() && member->second.as_string() != nullptr) {
                phase = *member->second.as_string();
            }
        }
    }
    state.emplace("phase", chord::Json(std::move(phase)));
    if (task.state.status == "waiting") {
        chord::Json::Array on;
        for (const TaskId dependency : task.state.on) {
            on.emplace_back(static_cast<double>(dependency.value));
        }
        state.emplace("on", chord::Json(std::move(on)));
        if (task.state.policy.has_value()) {
            state.emplace("policy", chord::Json(std::string(join_policy_name(*task.state.policy))));
        }
    }
    if (task.state.status == "completing" && task.state.outcome.has_value()) {
        state.emplace("outcome", chord::Json(task.state.outcome->status));
    }

    chord::Json::Object node;
    node.emplace("id", chord::Json(static_cast<double>(task.id.value)));
    node.emplace("kind", chord::Json(task.kind));
    node.emplace("conversationId", chord::Json(static_cast<double>(task.conversation_id.value)));
    if (task.owner.has_value()) {
        node.emplace("owner", chord::Json(static_cast<double>(task.owner->value)));
    }
    node.emplace("background", chord::Json(task.background));
    node.emplace("abortRequested", chord::Json(task.abort_requested));
    node.emplace("state", chord::Json(std::move(state)));
    std::vector<std::int64_t> conversation_ids;
    conversation_ids.reserve(owned.size());
    for (const ConversationId conversation : owned) {
        conversation_ids.push_back(conversation.value);
    }
    node.emplace("conversations", id_array(conversation_ids));
    return chord::Json(std::move(node));
}

[[nodiscard]] chord::Path field_path(const std::string& field) {
    return {chord::path_key(field)};
}

[[nodiscard]] chord::Path task_field_path(const std::string& key, const std::string& field) {
    return {chord::path_key("tasks"), chord::path_key(key), chord::path_key(field)};
}

/// Deliver one committed frame to every live watcher of a mount and advance
/// the shared revision. Runs on the Session commit-publication line; the
/// listeners run outside the mount lock so a listener may stop its own or
/// another watch, and every watch still receives frames in commit order.
void deliver(ViewMount& mount, chord::Json next, chord::OpBatch ops) {
    std::vector<std::pair<std::shared_ptr<ViewWatch::Impl>, ViewFrame>> deliveries;
    std::vector<std::weak_ptr<ViewWatch::Impl>> live;
    {
        std::lock_guard lock(mount.mutex);
        mount.value = next;
        for (const std::weak_ptr<ViewWatch::Impl>& weak : mount.watchers) {
            std::shared_ptr<ViewWatch::Impl> watcher = weak.lock();
            if (watcher == nullptr) {
                continue;
            }
            live.push_back(std::weak_ptr<ViewWatch::Impl>(watcher));
            if (watcher->stopped) {
                continue;
            }
            deliveries.emplace_back(std::move(watcher), ViewFrame{.value = next, .ops = ops});
        }
        mount.watchers = std::move(live);
    }
    for (auto& [watcher, frame] : deliveries) {
        {
            std::lock_guard lock(mount.mutex);
            // Claim only this delivery. Later deliveries stay idle so a
            // callback may stop another watch without waiting on itself.
            if (watcher->stopped) continue;
            watcher->dispatch = ViewWatch::Impl::DispatchState::Claimed;
        }
        run_view_dispatch_hook(testing::ViewDispatchHookPoint::AfterClaim);
        {
            std::lock_guard lock(mount.mutex);
            // Resolve the admission under the same lock used by stop(). A
            // stop that arrived after the claim has already transitioned this
            // delivery to `Stopped`; cancel it. A stop that arrives after this
            // transition observes `Entered` and never waits for the callback.
            if (watcher->stopped) {
                watcher->dispatch = ViewWatch::Impl::DispatchState::Stopped;
                continue;
            }
            watcher->dispatch = ViewWatch::Impl::DispatchState::Entered;
            watcher->value = next;
        }
        watcher->listener(std::move(frame));
        {
            std::lock_guard lock(mount.mutex);
            if (watcher->dispatch == ViewWatch::Impl::DispatchState::Entered) {
                watcher->dispatch = watcher->stopped ? ViewWatch::Impl::DispatchState::Stopped
                                                     : ViewWatch::Impl::DispatchState::Idle;
            }
        }
    }
}

/// End every live watcher of one mount (Session end; frozen pi
/// `WatchEvents.close`).
void end_watchers(ViewMount& mount, const bool failed) {
    std::vector<std::shared_ptr<AgentEventStream::Impl>> streams;
    {
        std::lock_guard lock(mount.mutex);
        const WatchEndReason reason = failed ? WatchEndReason::SessionFailed : WatchEndReason::SessionClosed;
        for (const std::weak_ptr<ViewWatch::Impl>& weak : mount.watchers) {
            if (const std::shared_ptr<ViewWatch::Impl> watcher = weak.lock(); watcher != nullptr) {
                // A claimed delivery is cancelled in place and observes `stopped`
                // on its recheck; an entered callback may finish. No wait, so a
                // blocked listener cannot hang Session end.
                mark_stopped(*watcher, reason);
            }
        }
        mount.watchers.clear();
        for (const std::weak_ptr<AgentEventStream::Impl>& weak : mount.event_streams) {
            if (const std::shared_ptr<AgentEventStream::Impl> stream = weak.lock(); stream != nullptr) {
                streams.push_back(std::move(stream));
            }
        }
        mount.event_streams.clear();
    }
    // Terminate outside the mount lock: a stream's terminate takes its own
    // mutex and a callback already entered may finish.
    const WatchEndReason reason = failed ? WatchEndReason::SessionFailed : WatchEndReason::SessionClosed;
    for (const std::shared_ptr<AgentEventStream::Impl>& stream : streams) {
        stream->terminate(reason);
    }
}

// ── Agent event stream translation (frozen pi `harness/events.ts`) ─────────
//
// One committed publication becomes zero or more frozen `AgentEvent`s in the
// frozen order. The translation reads only committed state: the conversation
// view before and after the commit, its operations, and the record changes of
// the publication. It introduces no scheduler and mutates nothing.

using JsonArray = chord::Json::Array;
using JsonObject = chord::Json::Object;

[[nodiscard]] const chord::Json* json_member(const chord::Json& value, std::string_view name) {
    const auto* object = value.as_object();
    if (object == nullptr) return nullptr;
    const auto found = object->find(std::string{name});
    return found == object->end() ? nullptr : &found->second;
}

[[nodiscard]] const JsonArray* json_array(const chord::Json& value, std::string_view name) {
    const chord::Json* member = json_member(value, name);
    return member == nullptr ? nullptr : member->as_array();
}

[[nodiscard]] std::optional<std::string> json_string(const chord::Json& value, std::string_view name) {
    const chord::Json* member = json_member(value, name);
    if (member == nullptr || member->as_string() == nullptr) return std::nullopt;
    return *member->as_string();
}

[[nodiscard]] std::optional<std::int64_t> json_integer(const chord::Json& value, std::string_view name) {
    const chord::Json* member = json_member(value, name);
    if (member == nullptr || member->as_number() == nullptr) return std::nullopt;
    return static_cast<std::int64_t>(*member->as_number());
}

/// Two optional members are equal when both are absent, or both are present and
/// equal as JSON values. Absent and present-but-null stay distinct.
[[nodiscard]] bool json_equal(const chord::Json* left, const chord::Json* right) {
    if (left == nullptr || right == nullptr) return left == right;
    return *left == *right;
}

[[nodiscard]] chord::Json number(const std::int64_t value) { return chord::Json(static_cast<double>(value)); }

[[nodiscard]] chord::Json array_or_empty(const chord::Json* value) {
    return value != nullptr && value->as_array() != nullptr ? *value : chord::Json(JsonArray{});
}

/// The `pi.usage` initial value a missing or retired usage document reads as.
[[nodiscard]] chord::Json empty_usage() {
    return chord::Json(JsonObject{{"models", chord::Json(JsonObject{})}, {"tools", chord::Json(JsonObject{})}});
}

[[nodiscard]] const chord::Json& doc_of(const chord::Json& view, const std::string_view bare_kind) {
    static const chord::Json kEmpty{};
    const chord::Json* docs = json_member(view, "docs");
    if (docs == nullptr) return kEmpty;
    const chord::Json* doc = json_member(*docs, bare_kind);
    return doc == nullptr ? kEmpty : *doc;
}

struct ViewParts {
    const chord::Json* live{nullptr};
    const chord::Json* inbox{nullptr};
    const chord::Json* agent{nullptr};
    const chord::Json* usage{nullptr};
};

[[nodiscard]] ViewParts view_parts(const chord::Json& view) {
    const chord::Json* docs = json_member(view, "docs");
    if (docs == nullptr) return {};
    return ViewParts{
            .live = json_member(*docs, "pi.live"),
            .inbox = json_member(*docs, "pi.inbox"),
            .agent = json_member(*docs, "pi.agent"),
            .usage = json_member(*docs, "pi.usage"),
    };
}

/// The queued inbox items an event reports (frozen `queued(inbox)`).
[[nodiscard]] chord::Json queued_items(const chord::Json* inbox) {
    JsonArray items;
    if (inbox != nullptr) {
        if (const JsonArray* listed = json_array(*inbox, "items"); listed != nullptr) {
            for (const chord::Json& item : *listed) {
                if (const auto id = json_integer(item, "id"); id.has_value()) {
                    if (const auto mode = json_string(item, "mode"); mode.has_value()) {
                        items.emplace_back(JsonObject{{"id", number(*id)}, {"mode", chord::Json(*mode)}});
                    }
                }
            }
        }
    }
    return chord::Json(std::move(items));
}

[[nodiscard]] std::optional<std::string> slot_call_id(const chord::Json& slot) { return json_string(slot, "callId"); }

[[nodiscard]] std::optional<std::string> slot_status(const chord::Json& slot) { return json_string(slot, "status"); }

/// The identifying members every tool event carries (frozen `ToolEventCall`).
void add_tool_call(JsonObject& event, const chord::Json& slot) {
    if (const auto call_id = slot_call_id(slot); call_id.has_value()) {
        event.insert_or_assign("toolCallId", chord::Json(*call_id));
    }
    if (const auto name = json_string(slot, "name"); name.has_value()) {
        event.insert_or_assign("toolName", chord::Json(*name));
    }
    if (const auto task = json_integer(slot, "taskId"); task.has_value()) {
        event.insert_or_assign("taskId", number(*task));
    }
}

[[nodiscard]] bool path_has_prefix(const chord::Path& path, const chord::Path& prefix) {
    return path.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), path.begin());
}

[[nodiscard]] const chord::Path* operation_path(const chord::Op& op) {
    return std::visit(
            [](const auto& value) -> const chord::Path* {
                using Operation = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<Operation, chord::ReplaceOp>) {
                    return nullptr;
                } else {
                    return &value.path;
                }
            },
            op);
}

/// Output, details and diagnostics changes of one running slot from the view
/// operations on it (frozen `toolUpdate`). Absent when the commit changed none
/// of them.
[[nodiscard]] std::optional<JsonObject> tool_update(
        const chord::OpBatch& ops, const std::size_t index, const chord::Json& slot, const chord::Json& previous) {
    const chord::Path output_path{chord::path_key("docs"),
            chord::path_key("pi.live"),
            chord::path_key("tools"),
            chord::path_index(index),
            chord::path_key("output")};
    std::int64_t trim_start = 0;
    std::string append{};
    bool set = false;
    for (const chord::Op& op : ops) {
        const chord::Path* path = operation_path(op);
        if (path == nullptr || !path_has_prefix(*path, output_path)) continue;
        if (const auto* truncate = std::get_if<chord::TruncateOp>(&op); truncate != nullptr) {
            trim_start += static_cast<std::int64_t>(truncate->count);
        } else if (const auto* appended = std::get_if<chord::AppendOp>(&op); appended != nullptr) {
            append += appended->text;
        } else {
            set = true;
        }
    }
    const chord::Json* slot_output = json_member(slot, "output");
    const chord::Json* previous_output = json_member(previous, "output");
    std::optional<chord::Json> output;
    if (set || (!json_equal(slot_output, previous_output) && trim_start == 0 && append.empty())) {
        output = chord::Json(JsonObject{{"set", slot_output != nullptr ? *slot_output : chord::Json(std::string{})}});
    } else if (trim_start > 0 || !append.empty()) {
        JsonObject window;
        if (trim_start > 0) window.emplace("trimStart", number(trim_start));
        if (!append.empty()) window.emplace("append", chord::Json(append));
        output = chord::Json(std::move(window));
    }
    JsonObject update;
    if (output.has_value()) update.emplace("output", *output);
    const chord::Json* slot_details = json_member(slot, "details");
    if (!json_equal(slot_details, json_member(previous, "details"))) {
        update.emplace("details", slot_details != nullptr ? *slot_details : chord::Json{});
    }
    const chord::Json* slot_diagnostics = json_member(slot, "diagnostics");
    if (!json_equal(slot_diagnostics, json_member(previous, "diagnostics"))) {
        update.emplace("diagnostics", array_or_empty(slot_diagnostics));
    }
    if (update.empty()) return std::nullopt;
    return update;
}

/// One tool end the batch has to place: before its result entry when that entry
/// is in this commit, otherwise after every entry (frozen `toolEnds`).
struct ToolEnd {
    chord::Json event{};
    std::optional<std::int64_t> entry_id{};
};

/// Translate one publication into frozen `AgentEvent` objects in frozen order.
/// `held` carries the generation tasks whose turn already ended at a
/// `completing` hold (frozen `watchEvents` `held`).
void translate_publication(const ConversationId conversation_id,
        const chord::Json& before,
        const chord::Json& after,
        const chord::OpBatch& ops,
        const CommitPublication& publication,
        std::vector<TaskId>& held,
        std::vector<chord::Json>& events) {
    std::vector<const EntryRecord*> entries;
    std::vector<const TaskRecord*> task_order;
    std::map<std::int64_t, const TaskRecord*> tasks;
    std::vector<const SubmissionRecord*> submissions;
    for (const CommitChange& change : publication.changes) {
        if (const auto* table = std::get_if<TableCommitChange>(&change); table != nullptr) {
            if (table->type == "entry" && table->entry.has_value() &&
                    table->conversation_id.value == conversation_id.value) {
                entries.push_back(&*table->entry);
            } else if (table->type == "task" && table->task.has_value() &&
                       table->task->conversation_id.value == conversation_id.value) {
                if (tasks.try_emplace(table->task->id.value, &*table->task).second) {
                    task_order.push_back(&*table->task);
                }
            }
            continue;
        }
        if (const auto* submission = std::get_if<SubmissionCommitChange>(&change); submission != nullptr) {
            if (submission->value.conversation_id.value == conversation_id.value) {
                submissions.push_back(&submission->value);
            }
        }
    }
    if (ops.empty() && entries.empty() && tasks.empty() && submissions.empty()) return;

    std::ranges::sort(entries, {}, [](const EntryRecord* entry) { return entry->id.value; });
    std::ranges::sort(submissions, {}, [](const SubmissionRecord* record) { return record->id.value; });

    const ViewParts was = view_parts(before);
    const ViewParts now = view_parts(after);
    const JsonArray* slots = now.live != nullptr ? json_array(*now.live, "tools") : nullptr;

    // Progress: tool starts, tool updates, retry and deferred state.
    std::map<std::string, const chord::Json*> slots_before;
    if (was.live != nullptr) {
        if (const JsonArray* listed = json_array(*was.live, "tools"); listed != nullptr) {
            for (const chord::Json& slot : *listed) {
                if (const auto call_id = slot_call_id(slot); call_id.has_value()) {
                    slots_before.emplace(*call_id, &slot);
                }
            }
        }
    }
    if (slots != nullptr) {
        for (const chord::Json& slot : *slots) {
            const auto call_id = slot_call_id(slot);
            if (!call_id.has_value()) continue;
            const auto previous = slots_before.find(*call_id);
            const bool running_now = slot_status(slot) == std::optional<std::string>{"running"};
            const bool running_before = previous != slots_before.end() &&
                                        slot_status(*previous->second) == std::optional<std::string>{"running"};
            if (!running_now || running_before) continue;
            JsonObject event{{"type", chord::Json(std::string{"tool_execution_start"})}};
            add_tool_call(event, slot);
            // A model-issued call's arguments are in its tool task's intent
            // checkpoint (frozen `tool_execution_start`); a call without a task
            // carries them on its slot.
            chord::Json args = chord::Json(JsonObject{});
            bool from_checkpoint = false;
            if (const auto task = json_integer(slot, "taskId"); task.has_value()) {
                if (const auto found = tasks.find(*task);
                        found != tasks.end() && found->second->state.checkpoint.has_value()) {
                    if (const chord::Json* declared = json_member(*found->second->state.checkpoint, "arguments");
                            declared != nullptr) {
                        args = *declared;
                        from_checkpoint = true;
                    }
                }
            }
            if (!from_checkpoint) {
                if (const chord::Json* declared = json_member(slot, "arguments"); declared != nullptr) {
                    args = *declared;
                }
            }
            event.emplace("args", args);
            events.push_back(chord::Json(std::move(event)));
        }
    }
    if (slots != nullptr) {
        for (std::size_t index = 0; index < slots->size(); ++index) {
            const chord::Json& slot = (*slots)[index];
            const auto call_id = slot_call_id(slot);
            if (!call_id.has_value()) continue;
            const auto previous = slots_before.find(*call_id);
            if (previous == slots_before.end()) continue;
            if (slot_status(slot) != std::optional<std::string>{"running"} ||
                    slot_status(*previous->second) != std::optional<std::string>{"running"}) {
                continue;
            }
            auto update = tool_update(ops, index, slot, *previous->second);
            if (!update.has_value()) continue;
            JsonObject event{{"type", chord::Json(std::string{"tool_execution_update"})}};
            add_tool_call(event, slot);
            for (auto& [key, value] : *update)
                event.insert_or_assign(key, std::move(value));
            events.push_back(chord::Json(std::move(event)));
        }
    }
    const chord::Json* generation = now.live != nullptr ? json_member(*now.live, "generation") : nullptr;
    const chord::Json* generation_before = was.live != nullptr ? json_member(*was.live, "generation") : nullptr;
    const chord::Json* retry = generation != nullptr ? json_member(*generation, "retry") : nullptr;
    const chord::Json* retry_before = generation_before != nullptr ? json_member(*generation_before, "retry") : nullptr;
    if (retry != nullptr && retry_before == nullptr) {
        JsonObject event{{"type", chord::Json(std::string{"auto_retry_start"})}};
        if (const auto attempt = json_integer(*generation, "attempt"); attempt.has_value()) {
            event.emplace("attempt", number(*attempt));
        }
        if (const chord::Json* at = json_member(*retry, "at"); at != nullptr) event.emplace("at", *at);
        if (const auto error = json_string(*retry, "error"); error.has_value()) {
            event.emplace("errorMessage", chord::Json(*error));
        }
        events.push_back(chord::Json(std::move(event)));
    }
    if (retry_before != nullptr && retry == nullptr) {
        JsonObject event{{"type", chord::Json(std::string{"auto_retry_end"})}};
        if (const auto attempt = json_integer(*generation_before, "attempt"); attempt.has_value()) {
            event.emplace("attempt", number(*attempt));
        }
        events.push_back(chord::Json(std::move(event)));
    }
    const chord::Json* deferred = generation != nullptr ? json_member(*generation, "deferred") : nullptr;
    const chord::Json* deferred_before =
            generation_before != nullptr ? json_member(*generation_before, "deferred") : nullptr;
    if (deferred != nullptr && !json_equal(deferred, deferred_before)) {
        if (const chord::Json* poll_at = json_member(*deferred, "pollAt"); poll_at != nullptr) {
            events.push_back(
                    chord::Json(JsonObject{{"type", chord::Json(std::string{"deferred_poll"})}, {"pollAt", *poll_at}}));
        }
    }

    // Tool ends: a slot that became done, one created done, or an unfinished
    // one that vanished because its run ended. A done slot that vanishes ended
    // earlier.
    std::vector<ToolEnd> tool_ends;
    const auto entry_json = [&entries](const std::int64_t id) -> std::optional<chord::Json> {
        for (const EntryRecord* entry : entries) {
            if (entry->id.value == id) return encode_entry_record(*entry);
        }
        return std::nullopt;
    };
    const auto result_of = [&entries](const std::string& call_id) -> std::optional<std::int64_t> {
        for (const EntryRecord* entry : entries) {
            if (entry->model.empty()) continue;
            const chord::Json& message = entry->model.front();
            if (const auto role = json_string(message, "role"); role != std::optional<std::string>{"toolResult"}) {
                continue;
            }
            if (json_string(message, "toolCallId") == std::optional<std::string>{call_id}) {
                return entry->id.value;
            }
        }
        return std::nullopt;
    };
    const auto end_tool = [&](const chord::Json& slot, const std::optional<std::int64_t> entry_id) {
        JsonObject event{{"type", chord::Json(std::string{"tool_execution_end"})}};
        add_tool_call(event, slot);
        if (entry_id.has_value()) {
            if (auto encoded = entry_json(*entry_id); encoded.has_value()) {
                event.emplace("entry", *encoded);
            }
        }
        tool_ends.push_back(ToolEnd{.event = chord::Json(std::move(event)), .entry_id = entry_id});
    };
    for (const auto& [call_id, previous] : slots_before) {
        if (slot_status(*previous) == std::optional<std::string>{"done"}) continue;
        const chord::Json* current = nullptr;
        if (slots != nullptr) {
            for (const chord::Json& slot : *slots) {
                if (slot_call_id(slot) == std::optional<std::string>{call_id}) {
                    current = &slot;
                    break;
                }
            }
        }
        if (current != nullptr && slot_status(*current) == std::optional<std::string>{"done"}) {
            end_tool(*current, json_integer(*current, "entry"));
        } else if (current == nullptr) {
            end_tool(*previous, result_of(call_id));
        }
    }
    if (slots != nullptr) {
        for (const chord::Json& slot : *slots) {
            const auto call_id = slot_call_id(slot);
            if (!call_id.has_value() || slots_before.contains(*call_id)) continue;
            if (slot_status(slot) != std::optional<std::string>{"done"}) continue;
            end_tool(slot, json_integer(slot, "entry"));
        }
    }

    // Entries in append order; a tool's end directly precedes its result's
    // message, as in the coding agent.
    for (const EntryRecord* entry : entries) {
        const chord::Json encoded = encode_entry_record(*entry);
        for (const ToolEnd& end : tool_ends) {
            if (end.entry_id == std::optional<std::int64_t>{entry->id.value}) events.push_back(end.event);
        }
        if (entry->model.empty()) {
            events.push_back(
                    chord::Json(JsonObject{{"type", chord::Json(std::string{"entry_appended"})}, {"entry", encoded}}));
            continue;
        }
        const chord::Json& message = entry->model.front();
        events.push_back(
                chord::Json(JsonObject{{"type", chord::Json(std::string{"message_start"})}, {"message", message}}));
        events.push_back(
                chord::Json(JsonObject{{"type", chord::Json(std::string{"message_end"})}, {"entry", encoded}}));
    }
    for (const ToolEnd& end : tool_ends) {
        if (!end.entry_id.has_value()) events.push_back(end.event);
    }

    // Task failures and turn ends. A generation's turn ends when its outcome is
    // committed, at a `completing` hold or at terminal, whichever comes first.
    bool turn_ended = false;
    for (const TaskRecord* task : task_order) {
        const std::string& status = task->state.status;
        if (task->kind == "pi.generation" && status == "completing" &&
                std::ranges::find(held, task->id) == held.end()) {
            held.push_back(task->id);
            turn_ended = true;
        }
        if (status != "terminal") continue;
        if (task->kind == "pi.generation") {
            if (const auto found = std::ranges::find(held, task->id); found != held.end()) {
                held.erase(found);
            } else {
                turn_ended = true;
            }
        }
        if (task->state.outcome.has_value()) {
            const std::string& outcome = task->state.outcome->status;
            if (outcome == "faulted" || outcome == "orphaned") {
                const std::string message = outcome == "faulted"
                                                    ? task->state.outcome->error.value_or(TaskOutcomeError{}).message
                                                    : task->state.outcome->reason.value_or("");
                events.push_back(chord::Json(JsonObject{{"type", chord::Json(std::string{"task_failed"})},
                        {"taskId", number(task->id.value)},
                        {"kind", chord::Json(task->kind)},
                        {"message", chord::Json(message)}}));
            }
        }
    }
    if (turn_ended) events.push_back(chord::Json(JsonObject{{"type", chord::Json(std::string{"turn_end"})}}));

    const chord::Json* run = now.live != nullptr ? json_member(*now.live, "run") : nullptr;
    const chord::Json* run_before = was.live != nullptr ? json_member(*was.live, "run") : nullptr;
    const auto first_input = [](const chord::Json* marker) -> const chord::Json* {
        if (marker == nullptr) return nullptr;
        const JsonArray* inputs = json_array(*marker, "inputs");
        return inputs == nullptr || inputs->empty() ? nullptr : &inputs->front();
    };
    const bool run_changed = !json_equal(first_input(run), first_input(run_before));
    if (run_before != nullptr && run_changed) {
        events.push_back(chord::Json(JsonObject{{"type", chord::Json(std::string{"run_end"})},
                {"inputs", array_or_empty(json_member(*run_before, "inputs"))}}));
    }

    // Submissions, document state, then what began.
    for (const SubmissionRecord* record : submissions) {
        events.push_back(chord::Json(JsonObject{
                {"type", chord::Json(std::string{"submission"})}, {"record", encode_submission_record(*record)}}));
    }
    if (!json_equal(now.inbox, was.inbox)) {
        events.push_back(chord::Json(
                JsonObject{{"type", chord::Json(std::string{"inbox_update"})}, {"items", queued_items(now.inbox)}}));
    }
    if (!json_equal(now.agent, was.agent)) {
        events.push_back(chord::Json(JsonObject{{"type", chord::Json(std::string{"agent_changed"})},
                {"agent", now.agent != nullptr ? *now.agent : chord::Json(JsonObject{})}}));
    }
    if (!json_equal(now.usage, was.usage)) {
        events.push_back(chord::Json(JsonObject{{"type", chord::Json(std::string{"usage_changed"})},
                {"usage",
                        now.usage != nullptr ? *now.usage
                                             : chord::Json(JsonObject{{"models", chord::Json(JsonObject{})},
                                                       {"tools", chord::Json(JsonObject{})}})}}));
    }
    if (run != nullptr && run_changed) {
        events.push_back(chord::Json(JsonObject{{"type", chord::Json(std::string{"run_start"})},
                {"inputs", array_or_empty(json_member(*run, "inputs"))}}));
    }
    if (run != nullptr &&
            json_integer(*run, "task") != (run_before != nullptr ? json_integer(*run_before, "task") : std::nullopt)) {
        const auto task = json_integer(*run, "task");
        if (task.has_value()) {
            const auto found = tasks.find(*task);
            if (found != tasks.end() && found->second->kind == "pi.generation") {
                events.push_back(chord::Json(JsonObject{{"type", chord::Json(std::string{"turn_start"})}}));
            }
        }
    }
}

/// Deliver one publication to every live event stream of a mount. Runs on the
/// Session commit-publication line, outside the mount lock so a listener may
/// stop its own or another stream.
void deliver_events(ViewMount& mount,
        const chord::Json& before,
        const chord::Json& after,
        const chord::OpBatch& ops,
        const CommitPublication& publication) {
    std::vector<std::shared_ptr<AgentEventStream::Impl>> streams;
    {
        std::lock_guard lock(mount.mutex);
        std::vector<std::weak_ptr<AgentEventStream::Impl>> live;
        for (const std::weak_ptr<AgentEventStream::Impl>& weak : mount.event_streams) {
            std::shared_ptr<AgentEventStream::Impl> stream = weak.lock();
            if (stream == nullptr) continue;
            live.push_back(weak);
            streams.push_back(std::move(stream));
        }
        mount.event_streams = std::move(live);
    }
    for (const std::shared_ptr<AgentEventStream::Impl>& stream : streams) {
        stream->advance(before, after, ops, publication);
    }
}

} // namespace

namespace testing {

void set_view_dispatch_hook(ViewDispatchHook hook) {
    std::lock_guard lock(view_dispatch_hook_mutex);
    view_dispatch_hook = std::move(hook);
}

void clear_view_dispatch_hook() {
    std::lock_guard lock(view_dispatch_hook_mutex);
    view_dispatch_hook = {};
}

} // namespace testing

AgentEventStream::AgentEventStream(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}

std::string AgentEvent::type() const {
    if (const chord::Json* member = json_member(value, "type"); member != nullptr) {
        if (const std::string* text = member->as_string(); text != nullptr) return *text;
    }
    return {};
}

void AgentEventStream::Impl::advance(const chord::Json& before,
        const chord::Json& after,
        const chord::OpBatch& ops,
        const CommitPublication& publication) {
    std::vector<chord::Json> translated;
    translate_publication(conversation_id, before, after, ops, publication, held, translated);
    if (translated.empty()) return;
    AgentEventBatch batch;
    batch.reserve(translated.size());
    for (chord::Json& event : translated) {
        batch.push_back(AgentEvent{.value = std::move(event)});
    }
    bool ready = false;
    {
        std::lock_guard lock(mutex);
        if (stopped) return;
        if (pending.size() >= kMaxPendingBatches) {
            // Frozen `CommittedWatch.advance`: overflow replaces every
            // undelivered batch with one snapshot of the newest committed view.
            pending.clear();
            pending.push_back(AgentEventBatch{AgentEvent{.value = agent_event_snapshot(after)}});
        } else {
            pending.push_back(std::move(batch));
        }
        ready = started && !delivering;
    }
    if (ready) drain();
}

void AgentEventStream::Impl::drain() {
    {
        std::lock_guard lock(mutex);
        if (stopped || !started || delivering) return;
        delivering = true;
    }
    for (;;) {
        AgentEventBatch batch;
        std::shared_ptr<AgentEventListener> listener;
        {
            std::lock_guard lock(mutex);
            if (stopped || pending.empty()) {
                delivering = false;
                return;
            }
            batch = std::move(pending.front());
            pending.erase(pending.begin());
            listener = listener_holder;
        }
        const support::ExpectedVoid outcome = (*listener)(batch);
        if (!outcome) {
            {
                std::lock_guard lock(mutex);
                delivering = false;
            }
            terminate(WatchEndReason::ListenerError, outcome.error().message);
            return;
        }
    }
}

void AgentEventStream::Impl::terminate(WatchEndReason reason, std::optional<std::string> error) {
    std::unique_ptr<std::stop_callback<std::move_only_function<void()>>> cancellation;
    std::shared_ptr<ObservationReportSink> reporter;
    std::optional<std::string> reported;
    {
        std::lock_guard lock(mutex);
        if (stopped) return;
        stopped = true;
        end = reason;
        listener_error = std::move(error);
        pending.clear();
        listener_holder.reset();
        cancellation = std::move(this->cancellation);
        mount.reset();
        if (reason == WatchEndReason::ListenerError) {
            reporter = this->reporter;
            reported = listener_error;
        }
    }
    if (reporter != nullptr) {
        std::lock_guard lock(reporter->mutex);
        if (reporter->report) {
            reporter->report(support::make_error(support::ErrorCode::Validation,
                    "An agent event listener reported an error" +
                            (reported.has_value() ? ": " + *reported : std::string{})));
        }
    }
    // Destroy the stop callback outside the lock: destroying one waits for a
    // running callback, which may be waiting for the stream mutex.
    cancellation.reset();
}

AgentEventStream::AgentEventStream(AgentEventStream&&) noexcept = default;
AgentEventStream& AgentEventStream::operator=(AgentEventStream&& other) noexcept {
    if (this != &other) {
        static_cast<void>(stop());
        impl_ = std::move(other.impl_);
    }
    return *this;
}

AgentEventStream::~AgentEventStream() {
    if (impl_ != nullptr) {
        static_cast<void>(stop());
    }
}

const chord::Json& AgentEventStream::snapshot() const noexcept {
    static const chord::Json kEmpty{};
    return impl_ == nullptr ? kEmpty : impl_->snapshot;
}

support::ExpectedVoid AgentEventStream::start(AgentEventListener listener) {
    if (impl_ == nullptr) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation, "Agent event stream is stopped"));
    }
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->started) {
            return std::unexpected(
                    support::make_error(support::ErrorCode::Validation, "Agent event stream is already started"));
        }
        if (impl_->stopped) {
            return std::unexpected(
                    support::make_error(support::ErrorCode::Validation, "Agent event stream is stopped"));
        }
        impl_->started = true;
        impl_->listener_holder = std::make_shared<AgentEventListener>(std::move(listener));
    }
    impl_->drain();
    return {};
}

WatchEndReason AgentEventStream::stop() {
    if (impl_ == nullptr) return WatchEndReason::Stopped;
    impl_->terminate(WatchEndReason::Stopped);
    std::lock_guard lock(impl_->mutex);
    return impl_->end;
}

std::optional<std::string> AgentEventStream::listener_error() const {
    if (impl_ == nullptr) return std::nullopt;
    std::lock_guard lock(impl_->mutex);
    return impl_->listener_error;
}

AgentEventStream AgentEventStream::attach(std::shared_ptr<ViewMount> mount,
        ConversationId conversation_id,
        chord::Json snapshot,
        std::vector<TaskId> held,
        std::shared_ptr<ObservationReportSink> reporter,
        std::stop_token stop) {
    std::shared_ptr<Impl> impl = std::make_shared<Impl>();
    impl->mount = mount;
    impl->conversation_id = conversation_id;
    impl->snapshot = std::move(snapshot);
    impl->held = std::move(held);
    impl->reporter = std::move(reporter);
    {
        std::lock_guard lock(impl->mount->mutex);
        impl->mount->event_streams.push_back(impl);
    }
    if (stop.stop_possible()) {
        impl->cancellation = std::make_unique<std::stop_callback<std::move_only_function<void()>>>(
                stop, [weak = std::weak_ptr<Impl>{impl}] {
                    if (const std::shared_ptr<Impl> stream = weak.lock(); stream != nullptr) {
                        stream->terminate(WatchEndReason::Cancelled);
                    }
                });
    }
    return AgentEventStream{std::move(impl)};
}

chord::Json agent_event_snapshot(const chord::Json& view) {
    const JsonArray* entries = json_array(view, "entries");
    const ViewParts parts = view_parts(view);
    JsonObject snapshot;
    snapshot.emplace("type", chord::Json(std::string{"snapshot"}));
    snapshot.emplace("entries", entries != nullptr ? chord::Json(*entries) : chord::Json(JsonArray{}));
    if (parts.live != nullptr) {
        if (const chord::Json* run = json_member(*parts.live, "run"); run != nullptr) {
            snapshot.emplace("run", chord::Json(JsonObject{{"inputs", array_or_empty(json_member(*run, "inputs"))}}));
        }
        if (const chord::Json* generation = json_member(*parts.live, "generation"); generation != nullptr) {
            snapshot.emplace("generation", *generation);
        }
        snapshot.emplace("tools", array_or_empty(json_member(*parts.live, "tools")));
        snapshot.emplace("nestedTools", array_or_empty(json_member(*parts.live, "nestedTools")));
        snapshot.emplace("compactions", array_or_empty(json_member(*parts.live, "compactions")));
        if (const chord::Json* failure = json_member(*parts.live, "compactionFailure"); failure != nullptr) {
            snapshot.emplace("compactionFailure", *failure);
        }
    } else {
        snapshot.emplace("tools", chord::Json(JsonArray{}));
        snapshot.emplace("nestedTools", chord::Json(JsonArray{}));
        snapshot.emplace("compactions", chord::Json(JsonArray{}));
    }
    snapshot.emplace("inbox", queued_items(parts.inbox));
    snapshot.emplace("agent", parts.agent != nullptr ? *parts.agent : chord::Json(JsonObject{}));
    snapshot.emplace("usage", parts.usage != nullptr ? *parts.usage : empty_usage());
    return chord::Json(std::move(snapshot));
}

ViewWatch::ViewWatch(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}

ViewWatch::ViewWatch(ViewWatch&&) noexcept = default;
ViewWatch& ViewWatch::operator=(ViewWatch&& other) noexcept {
    if (this != &other) {
        static_cast<void>(stop());
        impl_ = std::move(other.impl_);
    }
    return *this;
}
ViewWatch::~ViewWatch() {
    // A moved-from watch owns no mount: cancelling it is a no-op, not a crash.
    if (impl_ != nullptr) {
        static_cast<void>(stop());
    }
}

ViewWatch ViewWatch::attach(std::shared_ptr<ViewMount> mount, ViewFrameListener listener) {
    std::shared_ptr<Impl> impl = std::make_shared<Impl>();
    impl->mount = std::move(mount);
    {
        std::lock_guard lock(impl->mount->mutex);
        impl->value = impl->mount->value;
        impl->listener = std::move(listener);
        impl->mount->watchers.push_back(impl);
    }
    return ViewWatch{std::move(impl)};
}

chord::Json ViewWatch::value() const {
    // A moved-from watch owns no mount; return the documented empty value
    // instead of dereferencing a null impl.
    if (impl_ == nullptr) {
        return chord::Json{};
    }
    std::lock_guard lock(impl_->mount->mutex);
    return impl_->value;
}

WatchEndReason ViewWatch::stop() {
    // A moved-from or never-attached watch has nothing to cancel.
    if (impl_ == nullptr) {
        return WatchEndReason::Stopped;
    }
    WatchEndReason end = WatchEndReason::Stopped;
    {
        std::lock_guard lock(impl_->mount->mutex);
        if (impl_->stopped) {
            return impl_->end;
        }
        // Marking a claimed delivery stopped cancels it on its recheck. An
        // entered callback may finish after this returns; neither path waits.
        mark_stopped(*impl_, WatchEndReason::Stopped);
        end = impl_->end;
    }
    run_view_dispatch_hook(testing::ViewDispatchHookPoint::AfterStopMarked);
    return end;
}

support::Expected<chord::Json> hydrate_task_graph(Storage& storage) {
    auto records = storage.scan_tasks(SIZE_MAX);
    if (!records) {
        return std::unexpected(records.error());
    }
    std::vector<TaskRecord> live;
    for (const TaskRecord& task : *records) {
        if (!terminal_status(task.state.status)) {
            live.push_back(task);
        }
    }
    std::ranges::sort(live, {}, [](const TaskRecord& task) { return task.id.value; });

    auto conversations = storage.scan_conversations(SIZE_MAX);
    if (!conversations) {
        return std::unexpected(conversations.error());
    }

    chord::Json::Object tasks;
    for (const TaskRecord& task : live) {
        std::vector<ConversationId> owned;
        for (const ConversationRecord& conversation : *conversations) {
            if (conversation.owner.has_value() && conversation.owner->task_id == task.id) {
                owned.push_back(conversation.id);
            }
        }
        std::ranges::sort(owned, {}, [](const ConversationId id) { return id.value; });
        tasks.emplace(task_key(task), node_json(task, owned));
    }
    chord::Json::Object root;
    root.emplace("tasks", chord::Json(std::move(tasks)));
    return chord::Json(std::move(root));
}

support::Expected<TaskGraph> decode_task_graph(const chord::Json& json) {
    const auto* root = json.as_object();
    if (root == nullptr) {
        return std::unexpected(
                support::make_error(support::ErrorCode::Validation, "task graph value is not an object"));
    }
    const auto tasks_member = root->find("tasks");
    if (tasks_member == root->end() || tasks_member->second.as_object() == nullptr) {
        return std::unexpected(
                support::make_error(support::ErrorCode::Validation, "task graph value lacks a tasks object"));
    }
    std::vector<std::pair<std::int64_t, const chord::Json*>> ordered;
    for (const auto& [key, value] : *tasks_member->second.as_object()) {
        std::int64_t id = 0;
        const char* const begin = key.data();
        const char* const end = begin + key.size();
        const std::from_chars_result parsed = std::from_chars(begin, end, id);
        if (parsed.ec != std::errc{} || parsed.ptr != end) {
            return std::unexpected(support::make_error(
                    support::ErrorCode::Validation, std::format("task graph key '{}' is not a valid task id", key)));
        }
        ordered.emplace_back(id, &value);
    }
    std::ranges::sort(ordered, {}, &std::pair<std::int64_t, const chord::Json*>::first);

    TaskGraph graph;
    for (const auto& [id, node_value] : ordered) {
        const auto* node = node_value->as_object();
        if (node == nullptr) {
            return std::unexpected(support::make_error(
                    support::ErrorCode::Validation, std::format("task graph node {} is not an object", id)));
        }
        TaskGraphNode parsed{.id = TaskId{.value = id}};
        const auto assign_string = [&node](const char* field, std::string& target) -> bool {
            const auto member = node->find(field);
            if (member == node->end() || member->second.as_string() == nullptr) {
                return false;
            }
            target = *member->second.as_string();
            return true;
        };
        if (!assign_string("kind", parsed.kind)) {
            return std::unexpected(support::make_error(
                    support::ErrorCode::Validation, std::format("task graph node {} lacks a kind", id)));
        }
        if (const auto member = node->find("conversationId");
                member != node->end() && member->second.as_number() != nullptr) {
            parsed.conversation_id = ConversationId{.value = static_cast<std::int64_t>(*member->second.as_number())};
        }
        if (const auto member = node->find("owner");
                member != node->end() && member->second.as_number() != nullptr) {
            parsed.owner = TaskId{.value = static_cast<std::int64_t>(*member->second.as_number())};
        }
        if (const auto member = node->find("background");
                member != node->end() && member->second.as_boolean() != nullptr) {
            parsed.background = *member->second.as_boolean();
        }
        if (const auto member = node->find("abortRequested");
                member != node->end() && member->second.as_boolean() != nullptr) {
            parsed.abort_requested = *member->second.as_boolean();
        }
        if (const auto member = node->find("conversations");
                member != node->end() && member->second.as_array() != nullptr) {
            for (const chord::Json& item : *member->second.as_array()) {
                if (const double* value = item.as_number(); value != nullptr) {
                    parsed.conversations.push_back(ConversationId{.value = static_cast<std::int64_t>(*value)});
                }
            }
        }
        const auto state = node->find("state");
        if (state == node->end() || state->second.as_object() == nullptr) {
            return std::unexpected(support::make_error(
                    support::ErrorCode::Validation, std::format("task graph node {} lacks a state", id)));
        }
        const auto* state_obj = state->second.as_object();
        if (const auto member = state_obj->find("status");
                member != state_obj->end() && member->second.as_string() != nullptr) {
            parsed.state.status = *member->second.as_string();
        }
        if (const auto member = state_obj->find("phase");
                member != state_obj->end() && member->second.as_string() != nullptr) {
            parsed.state.phase = *member->second.as_string();
        }
        if (const auto member = state_obj->find("on");
                member != state_obj->end() && member->second.as_array() != nullptr) {
            for (const chord::Json& item : *member->second.as_array()) {
                if (const double* value = item.as_number(); value != nullptr) {
                    parsed.state.on.push_back(TaskId{.value = static_cast<std::int64_t>(*value)});
                }
            }
        }
        if (const auto member = state_obj->find("policy");
                member != state_obj->end() && member->second.as_string() != nullptr) {
            if (*member->second.as_string() == "allSettled") {
                parsed.state.policy = JoinPolicy::AllSettled;
            } else if (*member->second.as_string() == "failFast") {
                parsed.state.policy = JoinPolicy::FailFast;
            }
        }
        if (const auto member = state_obj->find("outcome");
                member != state_obj->end() && member->second.as_string() != nullptr) {
            parsed.state.outcome = *member->second.as_string();
        }
        graph.tasks.push_back(std::move(parsed));
    }
    return graph;
}

support::Expected<HydratedConversationView> hydrate_conversation_view(
        Storage& storage, ConversationId conversation_id) {
    auto conversation = storage.conversation(conversation_id);
    if (!conversation) {
        return std::unexpected(conversation.error());
    }
    if (!conversation->has_value()) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation,
                std::format("Conversation {} does not exist", conversation_id.value)));
    }

    HydratedConversationView hydrated;
    chord::Json::Object view;
    view.emplace("conversation", encode_conversation_record(**conversation));

    auto entries = storage.scan_entries(conversation_id, SIZE_MAX);
    if (!entries) {
        return std::unexpected(entries.error());
    }
    // Entries read oldest first; a head marker cuts the retained prefix the
    // way `Session.context_view` reports it (frozen pi `ConversationView`
    // entries keep the head entry and everything after it).
    std::vector<chord::Json> ordered;
    ordered.reserve(entries->size());
    for (auto it = entries->rbegin(); it != entries->rend(); ++it) {
        ordered.push_back(encode_entry_record(*it));
    }
    std::size_t head_index = 0;
    bool has_head = false;
    for (std::size_t index = 0; index < ordered.size(); ++index) {
        const auto* entry = ordered[index].as_object();
        if (entry != nullptr && entry->contains("head")) {
            head_index = index;
            has_head = true;
        }
    }
    chord::Json::Array visible;
    std::int64_t head_target = 0;
    if (has_head) {
        visible.push_back(ordered[head_index]);
        const auto* marker = ordered[head_index].as_object();
        if (const auto* target = marker->at("head").as_number(); target != nullptr) {
            head_target = static_cast<std::int64_t>(*target);
        }
    }
    for (const auto& item : ordered) {
        const auto* entry = item.as_object();
        if (entry == nullptr || entry->contains("head")) continue;
        if (has_head && static_cast<std::int64_t>(*entry->at("id").as_number()) < head_target) continue;
        visible.push_back(item);
    }
    view.emplace("entries", chord::Json(std::move(visible)));

    const DocumentAddress address{.conversation_id = conversation_id};
    const DocumentPoint at = DocumentPoint::current();
    chord::Json::Object docs;
    for (const std::string_view kind : kMountedDocKinds) {
        auto record = storage.find_document(kind, address, at);
        if (!record) {
            return std::unexpected(record.error());
        }
        if (!record->has_value()) {
            continue;
        }
        auto stored = storage.document(record->value().id, at);
        if (!stored) {
            return std::unexpected(stored.error());
        }
        if (!stored->has_value()) {
            continue;
        }
        docs.emplace(bare_doc_kind(std::string(kind)), (*stored)->value);
        hydrated.docs.emplace(std::string(kind),
                MountedDocument{.id = (*stored)->record.id, .version = (*stored)->version});
    }
    view.emplace("docs", chord::Json(std::move(docs)));
    hydrated.value = chord::Json(std::move(view));
    return hydrated;
}

void advance_task_graph(ViewMount& mount, const CommitPublication& publication) {
    const auto* root = mount.value.as_object();
    if (root == nullptr) {
        return;
    }
    const auto tasks_member = root->find("tasks");
    chord::Json::Object tasks =
            tasks_member != root->end() ? *tasks_member->second.as_object() : chord::Json::Object{};
    chord::OpBatch ops;

    for (const CommitChange& change : publication.changes) {
        const auto* table = std::get_if<TableCommitChange>(&change);
        if (table == nullptr || table->type != "task" || !table->task.has_value()) {
            continue;
        }
        const TaskRecord& task = *table->task;
        const std::string key = task_key(task);
        if (terminal_status(task.state.status)) {
            if (tasks.contains(key)) {
                ops.push_back(chord::DeleteOp{{chord::path_key("tasks"), chord::path_key(key)}});
                tasks.erase(key);
            }
            continue;
        }
        const chord::Json node = node_json(task, [&] {
            std::vector<ConversationId> owned;
            const auto existing = tasks.find(key);
            if (existing != tasks.end()) {
                if (const auto* member = existing->second.as_object(); member != nullptr) {
                    if (const auto conversations = member->find("conversations");
                            conversations != member->end() && conversations->second.as_array() != nullptr) {
                        for (const chord::Json& item : *conversations->second.as_array()) {
                            if (const double* value = item.as_number(); value != nullptr) {
                                owned.push_back(ConversationId{.value = static_cast<std::int64_t>(*value)});
                            }
                        }
                    }
                }
            }
            return owned;
        }());
        const auto existing = tasks.find(key);
        if (existing != tasks.end() && existing->second == node) {
            continue;
        }
        ops.push_back(chord::SetOp{{chord::path_key("tasks"), chord::path_key(key)}, node});
        tasks.insert_or_assign(key, node);
    }

    // A conversation created under a task adds an owned-conversation edge to
    // its owner's node; the owner record itself is unchanged.
    for (const CommitChange& change : publication.changes) {
        const auto* table = std::get_if<TableCommitChange>(&change);
        if (table == nullptr || table->type != "conversation" || !table->conversation.has_value()
                || !table->conversation->owner.has_value()) {
            continue;
        }
        const std::string key = std::to_string(table->conversation->owner->task_id.value);
        const auto node_member = tasks.find(key);
        if (node_member == tasks.end()) {
            continue;
        }
        std::vector<std::int64_t> owned;
        if (const auto* node = node_member->second.as_object(); node != nullptr) {
            if (const auto conversations = node->find("conversations");
                    conversations != node->end() && conversations->second.as_array() != nullptr) {
                for (const chord::Json& item : *conversations->second.as_array()) {
                    if (const double* value = item.as_number(); value != nullptr) {
                        owned.push_back(static_cast<std::int64_t>(*value));
                    }
                }
            }
        }
        const std::int64_t created = table->conversation->id.value;
        if (std::ranges::find(owned, created) == owned.end()) {
            owned.push_back(created);
            std::ranges::sort(owned);
        }
        const chord::Json value = id_array(owned);
        ops.push_back(chord::SetOp{task_field_path(key, "conversations"), value});
        chord::Json::Object node = *node_member->second.as_object();
        node.insert_or_assign("conversations", value);
        tasks.insert_or_assign(key, chord::Json(std::move(node)));
    }

    if (ops.empty()) {
        return;
    }
    chord::Json::Object next;
    next.emplace("tasks", chord::Json(std::move(tasks)));
    deliver(mount, chord::Json(std::move(next)), std::move(ops));
}

void advance_conversation_view(
        ViewMount& mount, ConversationId conversation_id, const CommitPublication& publication) {
    const auto* root = mount.value.as_object();
    if (root == nullptr) {
        return;
    }
    // The revision before this commit: the event translation compares it with
    // the revision after the ops, and the view watchers receive the latter.
    const chord::Json before = mount.value;
    const auto* initial_entries = root->find("entries") != root->end() ? root->at("entries").as_array() : nullptr;
    chord::Json::Array entries = initial_entries != nullptr ? *initial_entries : chord::Json::Array{};
    // The document operations precede the entry operations in the frame,
    // matching the frozen pi `view.ts#advance` order. The delivered value is
    // always `apply(previous value, ops)`: replaying the frame's ops reproduces
    // the frame's value by construction, with no second bookkeeping path that
    // could drift from the op semantics (frozen pi `view.ts#advance`).
    chord::OpBatch ops;

    for (const CommitChange& change : publication.changes) {
        const auto* document = std::get_if<DocumentCommitChange>(&change);
        if (document == nullptr || !document->conversation_id.has_value()
                || document->conversation_id->value != conversation_id.value) {
            continue;
        }
        const std::string& kind = document->record.kind;
        if (std::ranges::find(kMountedDocKinds, kind) == std::end(kMountedDocKinds)) continue;
        const std::string bare_kind = bare_doc_kind(kind);
        const auto mounted = mount.docs.find(kind);
        if (document->value.is_null()) {
            // The document retired from this conversation.
            if (mounted != mount.docs.end() && mounted->second.id == document->record.id) {
                ops.push_back(chord::DeleteOp{{chord::path_key("docs"), chord::path_key(bare_kind)}});
                mount.docs.erase(kind);
            }
            continue;
        }
        const std::uint32_t version = document->version.value_or(1);
        if (mounted != mount.docs.end() && mounted->second.id == document->record.id
                && mounted->second.version == version) {
            // The same incarnation advanced: prefix the committed document
            // operations into the view under the bare-kind key.
            for (const chord::Op& op : document->ops) {
                if (const auto* replace = std::get_if<chord::ReplaceOp>(&op); replace != nullptr) {
                    ops.push_back(chord::SetOp{{chord::path_key("docs"), chord::path_key(bare_kind)}, replace->value});
                    continue;
                }
                std::visit([&](const auto& original) {
                    using Operation = std::decay_t<decltype(original)>;
                    if constexpr (!std::is_same_v<Operation, chord::ReplaceOp>) {
                        auto prefixed = original;
                        prefixed.path.insert(prefixed.path.begin(), chord::path_key(bare_kind));
                        prefixed.path.insert(prefixed.path.begin(), chord::path_key("docs"));
                        ops.push_back(std::move(prefixed));
                    }
                }, op);
            }
            // The publication carried this incarnation's next version; keep
            // the mount in step so the following publication replays as ops
            // instead of remounting whole.
            mount.docs.at(kind).version = version;
            continue;
        }
        // Another incarnation or version owns the document now: mount its
        // current committed value whole (frozen pi `view.ts#advance`).
        mount.docs.insert_or_assign(kind,
                MountedDocument{.id = document->record.id, .version = version});
        ops.push_back(chord::SetOp{{chord::path_key("docs"), chord::path_key(bare_kind)}, document->value});
    }

    for (const CommitChange& change : publication.changes) {
        const auto* table = std::get_if<TableCommitChange>(&change);
        if (table == nullptr || table->type != "entry" || !table->entry.has_value()
                || table->conversation_id.value != conversation_id.value) {
            continue;
        }
        const EntryRecord& entry = *table->entry;
        const chord::Json encoded = encode_entry_record(entry);
        const std::size_t size = entries.size();
        if (entry.head.has_value()) {
            // The newest entry may cut the retained history to the head
            // marker, exactly the prefix rule `hydrate_conversation_view`
            // applies: the head marker stores the first retained entry's ID,
            // and entry IDs increase monotonically per conversation (frozen
            // pi `view.ts#advance`). The cut removes the retired prefix and
            // appends the new entry after the retained suffix.
            std::size_t kept = size;
            for (std::size_t index = 0; index < size; ++index) {
                const auto* item = entries[index].as_object();
                if (item == nullptr || item->contains("head")) {
                    continue;
                }
                const auto id_member = item->find("id");
                if (id_member != item->end() && id_member->second.as_number() != nullptr
                        && static_cast<std::int64_t>(*id_member->second.as_number()) >= entry.head->value) {
                    kept = index;
                    break;
                }
            }
            ops.push_back(chord::SpliceOp{
                    .path = field_path("entries"), .index = 0, .remove = kept, .items = {encoded}});
            entries.erase(entries.begin(), entries.begin() + static_cast<std::ptrdiff_t>(kept));
            entries.insert(entries.begin(), encoded);
        } else {
            ops.push_back(chord::SpliceOp{
                    .path = field_path("entries"), .index = size, .remove = 0, .items = {encoded}});
            entries.push_back(encoded);
        }
    }

    chord::Json next = before;
    if (!ops.empty()) {
        next = mount.value;
        if (auto applied = chord::apply(next, ops); !applied) {
            // A frame whose ops do not apply to the mounted value can never
            // replay; skip the delivery rather than publish a broken frame.
            return;
        }
        deliver(mount, next, ops);
    }
    // Every publication reaches the event streams, including one whose view
    // operations are empty: a task or submission change still produces
    // lifecycle events (frozen pi `ViewObserver.publication`).
    deliver_events(mount, before, next, ops, publication);
}

std::shared_ptr<ViewMount> ViewMountRegistry::task_graph_mount(const chord::Json& hydrated) {
    if (graph_mount_ == nullptr) {
        graph_mount_ = std::make_shared<ViewMount>(hydrated);
    }
    return graph_mount_;
}

std::shared_ptr<ViewMount> ViewMountRegistry::conversation_mount(
        ConversationId conversation_id, const HydratedConversationView& hydrated) {
    const auto [it, inserted] = conversation_mounts_.try_emplace(conversation_id.value);
    if (inserted) {
        std::shared_ptr<ViewMount> mount = std::make_shared<ViewMount>(hydrated.value);
        mount->docs = hydrated.docs;
        it->second = std::move(mount);
    }
    return it->second;
}

void ViewMountRegistry::advance(const CommitPublication& publication) {
    if (graph_mount_ != nullptr) {
        advance_task_graph(*graph_mount_, publication);
    }
    for (const auto& [id, mount] : conversation_mounts_) {
        advance_conversation_view(*mount, ConversationId{.value = id}, publication);
    }
}

void ViewMountRegistry::close(bool failed) {
    if (graph_mount_ != nullptr) {
        end_watchers(*graph_mount_, failed);
        graph_mount_.reset();
    }
    for (const auto& [id, mount] : conversation_mounts_) {
        static_cast<void>(id);
        end_watchers(*mount, failed);
    }
    conversation_mounts_.clear();
}

} // namespace cch::durable
