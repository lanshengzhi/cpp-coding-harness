#pragma once

#include <cch/durable/Observation.hpp>
#include <cch/durable/Storage.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace cch::durable {

/// The mounted incarnation and definition version of one built-in document,
/// so a publication of another incarnation or version sets its view entry
/// whole instead of prefixing operations (frozen pi `ConversationViews` Mount
/// docs map).
struct MountedDocument {
    DocumentId id{};
    std::uint32_t version{1};
};

/// One hydrated conversation view: the view revision and the mounted
/// document incarnation per kind, so later publications of the same
/// incarnation prefix their operations into the view.
struct HydratedConversationView {
    chord::Json value{};
    std::map<std::string, MountedDocument> docs{};
};

/// Hydrate the committed task graph JSON (a `{"tasks": {...}}` object keyed
/// by decimal task ID) from committed records only.
[[nodiscard]] support::Expected<chord::Json> hydrate_task_graph(Storage& storage);

/// Hydrate one conversation's committed view (`{"conversation": ...,
/// "entries": [...], "docs": {...}}`) from committed records only; rejects a
/// missing conversation (frozen pi `ConversationViews.#build`).
[[nodiscard]] support::Expected<HydratedConversationView> hydrate_conversation_view(
        Storage& storage, ConversationId conversation_id);

/// Decode one hydrated task-graph JSON into its structured form.
[[nodiscard]] support::Expected<TaskGraph> decode_task_graph(const chord::Json& json);

/// One structural view mount: the current committed revision and the
/// watchers advancing from it. The mount is a read-only projection over
/// committed records: it owns no thread, timer, or scheduler and mutates
/// only from the Session's commit-publication line (or under `mutex` for a
/// watcher stopping from its own thread). A stop that arrives before a
/// listener is admitted cancels that delivery; an entered listener may finish
/// and is never joined. Cross-watch stop cycles are not supported.
struct ViewMount {
    explicit ViewMount(chord::Json initial) : value(std::move(initial)) {}

    std::mutex mutex;
    /// The mount's current revision, shared by every watcher.
    chord::Json value;
    /// The mounted document incarnation per kind; only a conversation view
    /// mounts documents.
    std::map<std::string, MountedDocument> docs{};
    std::vector<std::weak_ptr<ViewWatch::Impl>> watchers{};
};

/// Advance the task-graph mount from one committed publication (frozen pi
/// `task-graph.ts#advance`).
void advance_task_graph(ViewMount& mount, const CommitPublication& publication);

/// Advance one conversation view mount from one committed publication
/// (frozen pi `view.ts#advance`).
void advance_conversation_view(
        ViewMount& mount, ConversationId conversation_id, const CommitPublication& publication);

/// The view mounts one Harness owns: at most one task-graph mount and one
/// conversation-view mount per conversation, each built from committed state
/// on the Session mutation line and advanced from the Harness's single
/// commit-publication listener. Every method runs on the Session mutation
/// line; a mount outlives its registry entry through the watchers that hold
/// it, so a late `stop()` never touches freed state.
class ViewMountRegistry {
public:
    ViewMountRegistry() = default;
    ViewMountRegistry(ViewMountRegistry&&) = delete;
    ViewMountRegistry& operator=(ViewMountRegistry&&) = delete;
    ViewMountRegistry(const ViewMountRegistry&) = delete;
    ViewMountRegistry& operator=(const ViewMountRegistry&) = delete;
    ~ViewMountRegistry() = default;

    /// The task-graph mount, built from `hydrated` when no mount lives.
    [[nodiscard]] std::shared_ptr<ViewMount> task_graph_mount(const chord::Json& hydrated);

    /// The view mount of one conversation, built from `hydrated` when no
    /// mount lives.
    [[nodiscard]] std::shared_ptr<ViewMount> conversation_mount(
            ConversationId conversation_id, const HydratedConversationView& hydrated);

    /// Advance every mount from one committed publication (frozen pi
    /// `TaskGraphView`/`ConversationViews` commit listeners).
    void advance(const CommitPublication& publication);

    /// End every live watcher and drop every mount on Session end.
    void close(bool failed);

private:
    std::shared_ptr<ViewMount> graph_mount_{};
    std::map<std::int64_t, std::shared_ptr<ViewMount>> conversation_mounts_{};
};

} // namespace cch::durable
