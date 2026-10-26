#pragma once

#include <cch/chord/Delta.hpp>
#include <cch/chord/Json.hpp>
#include <cch/durable/Harness.hpp>

#include <optional>
#include <span>
#include <vector>

namespace cch::durable {

/// The live run marker of one conversation: the generation task and its
/// placed submission inputs (frozen pi `LiveRun`).
struct LiveRunState {
    TaskId task;
    std::vector<SubmissionId> inputs;
};

/// What a boundary reads before the commit's first table write, and the
/// newest head it has seen so far (frozen pi `Boundary`).
struct Boundary {
    ConversationId conversation_id{};
    chord::Draft inbox;
    std::vector<chord::Json> items{};
    QueueMode steering_mode{QueueMode::OneAtATime};
    QueueMode follow_up_mode{QueueMode::OneAtATime};
    std::optional<EntryId> head{std::nullopt};
};

/// Selected user items of one boundary, the entries they placed, and whether
/// a queued reset ended the run (frozen pi `BoundaryResult`).
struct BoundaryResult {
    std::vector<SubmissionId> users{};
    std::vector<EntryRecord> user_entries{};
    bool reset{false};
};

/// Harness-owned workflow state. Harness shutdown joins its task invocations
/// before destroying this object; Session, Storage, registry and the clock
/// outlive every operation and task callback below.
class WorkflowRuntime {
public:
    WorkflowRuntime(Session& session, Storage& storage, HarnessOptions& options,
            std::shared_ptr<std::move_only_function<std::int64_t()>> clock);
    WorkflowRuntime(const WorkflowRuntime&) = delete;
    WorkflowRuntime& operator=(const WorkflowRuntime&) = delete;
    ~WorkflowRuntime() = default;

    /// The built-in `pi.generation` definition whose phase and abort handlers
    /// drive the run loop below.
    [[nodiscard]] std::shared_ptr<TaskDefinition> generation() const noexcept;
    [[nodiscard]] support::AsyncResult<ConversationId> root(RootOptions options);
    [[nodiscard]] support::AsyncResult<SubmissionId> submit(ConversationId conversation_id, SubmissionDraft draft);

private:
    struct PromptPlan {
        ai::AiContext context{};
        ai::SystemMessage system{};
    };

    [[nodiscard]] support::Expected<TaskDefinition> build_definition();
    [[nodiscard]] PromptPlan plan_prompt(const Registry::Snapshot& snapshot,
            std::span<const ai::Tool> shown_tools,
            std::span<const ai::SystemMessageSection> shown_sections,
            std::int64_t timestamp);
    [[nodiscard]] ai::ToolResultMessage execute_tool(const ai::ToolCallContent& call,
            const Registry::Snapshot& snapshot, std::stop_token stop_token);
    [[nodiscard]] support::AsyncResult<void> generate(const TaskRecord& task, TaskRuntime& runtime);
    [[nodiscard]] support::ExpectedVoid run(const TaskRecord& task, TaskRuntime& runtime);
    /// The abort handler of the built-in generation definition: settle the
    /// run's inputs `unanswered/aborted` (frozen pi generation `abort`).
    [[nodiscard]] support::AsyncResult<void> abort_run(const TaskRecord& task, TaskRuntime& runtime);
    /// The postTools point of the run loop: place queued writes and selected
    /// steers, joining them to the run (frozen `finishToolRound`). Returns
    /// true when a queued reset ended the run at this boundary.
    [[nodiscard]] support::AsyncResult<bool> apply_post_tools_boundary(const TaskRecord& task, TaskRuntime& runtime);
    /// The admission state machine of one commit (frozen pi `admitSubmission`),
    /// in the frozen order: dedup by request ID, display-only write shortcut,
    /// busy reject, boundary preparation (before the first table write), then
    /// queue or place.
    [[nodiscard]] support::Expected<SubmissionId> admit_submission(
            Transaction& tx, ConversationId conversation_id, const SubmissionDraft& draft, std::int64_t now);
    /// A boundary's table read and the newest head marker, prepared before
    /// the commit's first table write (frozen pi `prepareBoundary`).
    [[nodiscard]] support::Expected<Boundary> prepare_boundary(
            Transaction& tx, ConversationId conversation_id, chord::Draft inbox, std::vector<chord::Json> items);
    [[nodiscard]] std::int64_t now() const;

    Session& session_; // outlives every operation (Harness ownership)
    Storage& storage_; // outlives every operation (Harness::open contract)
    Registry empty_registry_;
    Registry& registry_; // outlives the Harness
    ai::ModelStreamFactory stream_;
    HarnessSettings settings_;
    std::shared_ptr<std::move_only_function<std::int64_t()>> clock_;
    /// The built-in generation definition; every run it starts is created
    /// with this definition.
    std::shared_ptr<TaskDefinition> generation_;
    ai::Model model_{};
    std::optional<ai::ThinkingLevel> thinking_level_{std::nullopt};
    // debt: the workflow's replay cache covers one open Harness. #1082 must
    // rebuild model context and prompt deltas from committed entries on reopen.
    std::vector<ai::MessageVariant> messages_;
    std::vector<ai::Tool> shown_tools_;
    std::vector<ai::SystemMessageSection> shown_sections_;
};

/// The committed live-run marker of one conversation, if any.
[[nodiscard]] support::Expected<std::optional<LiveRunState>> committed_live_run(
        Storage& storage, ConversationId conversation_id);

/// The live-run marker staged in a live document draft, if any.
[[nodiscard]] support::Expected<std::optional<LiveRunState>> staged_live_run(chord::Draft& live);

/// Settle every input of the live run and clear the marker, but only when
/// the marker names `task` (frozen pi `endRun`).
[[nodiscard]] support::ExpectedVoid end_run(
        Transaction& tx, chord::Draft& live, TaskId task, const SubmissionSettlement& settlement);

/// The built-in `pi.inbox:v1` conversation document definition (frozen pi
/// `InboxDoc`).
[[nodiscard]] DocumentDefinition inbox_document();

/// The built-in `pi.live:v1` conversation document definition (frozen pi
/// `LiveDoc`).
[[nodiscard]] DocumentDefinition live_document();

/// Withdraw every queued input of the conversation: each settles
/// `unanswered/aborted` and leaves the inbox; queued writes stay for later
/// placement (frozen pi `withdrawQueuedInputs`).
[[nodiscard]] support::ExpectedVoid withdraw_queued_inputs(Transaction& tx, chord::Draft& inbox);

/// Remove one withdrawn submission's inbox item; the caller settles the
/// submission (frozen pi `removeInboxItem`).
[[nodiscard]] support::ExpectedVoid remove_inbox_item(chord::Draft& inbox, SubmissionId id);

} // namespace cch::durable
