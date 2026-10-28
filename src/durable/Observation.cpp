#include "durable/ViewMounts.hpp"

#include "durable/ObservationTestHooks.hpp"
#include "durable/StorageEncoding.hpp"

#include <algorithm>
#include <condition_variable>
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
    /// enter. `stopped` is separate because a stop can arrive while a delivery
    /// is claimed or entered; the delivery then finishes or cancels into
    /// `Stopped`.
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
    /// Wakes a stop/end waiting for a claimed delivery to cancel or enter.
    /// The wait uses the mount mutex.
    std::condition_variable dispatch_cv{};
    ViewFrameListener listener{};
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

/// Record a stop under the mount mutex. A claimed or entered delivery keeps
/// its state until it resolves; an idle watcher becomes stopped immediately.
void mark_stopped(ViewWatch::Impl& watcher, const WatchEndReason reason) {
    if (watcher.stopped) return;
    watcher.stopped = true;
    watcher.end = reason;
    if (watcher.dispatch == ViewWatch::Impl::DispatchState::Idle) {
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
            std::unique_lock lock(mount.mutex);
            // Resolve the admission under the same lock used by stop(). A
            // stop that arrived after the claim cancels this delivery; one
            // that arrives after this transition observes it as entered and
            // may not wait for the callback to finish. User code always runs
            // outside the lock.
            if (watcher->stopped) {
                watcher->dispatch = ViewWatch::Impl::DispatchState::Stopped;
                watcher->dispatch_cv.notify_all();
                continue;
            }
            watcher->dispatch = ViewWatch::Impl::DispatchState::Entered;
            watcher->value = next;
            watcher->dispatch_cv.notify_all();
        }
        watcher->listener(std::move(frame));
        {
            std::lock_guard lock(mount.mutex);
            if (watcher->dispatch == ViewWatch::Impl::DispatchState::Entered) {
                watcher->dispatch = watcher->stopped ? ViewWatch::Impl::DispatchState::Stopped
                                                     : ViewWatch::Impl::DispatchState::Idle;
                watcher->dispatch_cv.notify_all();
            }
        }
    }
}

/// End every live watcher of one mount (Session end; frozen pi
/// `WatchEvents.close`).
void end_watchers(ViewMount& mount, const bool failed) {
    std::unique_lock lock(mount.mutex);
    const WatchEndReason reason = failed ? WatchEndReason::SessionFailed : WatchEndReason::SessionClosed;
    for (const std::weak_ptr<ViewWatch::Impl>& weak : mount.watchers) {
        if (const std::shared_ptr<ViewWatch::Impl> watcher = weak.lock(); watcher != nullptr) {
            mark_stopped(*watcher, reason);
        }
    }
    // A claimed delivery has not entered its listener yet. Wait for it to
    // cancel or enter; an entered callback is allowed to finish and is never
    // joined here. The wait releases the mount mutex for the delivery.
    for (const std::weak_ptr<ViewWatch::Impl>& weak : mount.watchers) {
        if (const std::shared_ptr<ViewWatch::Impl> watcher = weak.lock(); watcher != nullptr) {
            watcher->dispatch_cv.wait(
                    lock, [&watcher] { return watcher->dispatch != ViewWatch::Impl::DispatchState::Claimed; });
        }
    }
    mount.watchers.clear();
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
    std::lock_guard lock(impl_->mount->mutex);
    return impl_->value;
}

WatchEndReason ViewWatch::stop() {
    // A moved-from or never-attached watch has nothing to cancel.
    if (impl_ == nullptr) {
        return WatchEndReason::Stopped;
    }
    std::unique_lock lock(impl_->mount->mutex);
    if (impl_->stopped) {
        return impl_->end;
    }
    mark_stopped(*impl_, WatchEndReason::Stopped);
    lock.unlock();
    run_view_dispatch_hook(testing::ViewDispatchHookPoint::AfterStopMarked);
    lock.lock();
    // Wait only for an admission decision, never for an entered callback to
    // finish: a running listener may return after stop() has returned.
    impl_->dispatch_cv.wait(lock, [this] { return impl_->dispatch != Impl::DispatchState::Claimed; });
    return impl_->end;
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
        ordered.emplace_back(std::stoll(key), &value);
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

    if (ops.empty()) {
        return;
    }
    chord::Json next = mount.value;
    if (auto applied = chord::apply(next, ops); !applied) {
        // A frame whose ops do not apply to the mounted value can never
        // replay; skip the delivery rather than publish a broken frame.
        return;
    }
    deliver(mount, std::move(next), std::move(ops));
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
