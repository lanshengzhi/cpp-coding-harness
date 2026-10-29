#pragma once

#include <cch/chord/Delta.hpp>
#include <cch/chord/Json.hpp>
#include <cch/durable/Harness.hpp>

#include <map>
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
    [[nodiscard]] support::AsyncResult<ConversationRecord> fork_conversation(ConversationId parent, EntryId at);
    [[nodiscard]] support::AsyncResult<void> reset(ConversationId conversation_id, std::optional<std::string> handoff);
    [[nodiscard]] support::AsyncResult<SubmissionId> submit(ConversationId conversation_id, SubmissionDraft draft);

    /// Install the built-in `pi.compaction` definition used when preparation
    /// or overflow recovery creates a compaction task (frozen pi
    /// `createCompaction`). Called once before the worker starts.
    void install_compaction_definition(std::shared_ptr<const TaskDefinition> definition);
    /// The model configured at root creation (frozen pi `HarnessOptions.model`).
    [[nodiscard]] const ai::Model& model() const noexcept;
    [[nodiscard]] const HarnessSettings& settings() const noexcept;
    [[nodiscard]] ai::ModelStream stream(ai::AiContext context, ai::SimpleStreamOptions options);
    /// Whether the pinned provider/model can still be served. The C++ slice
    /// has no model catalog, so the Harness model is the only source (frozen
    /// pi `runtime.models.getModel`).
    [[nodiscard]] bool model_available(std::string_view provider, std::string_view model_id) const noexcept;
    /// Admit a conversation-owned compaction summary as a write submission
    /// (frozen pi `placeSummary.admitSubmission`): placed at once when idle,
    /// queued to the next boundary when busy, or settled `stale`.
    [[nodiscard]] support::Expected<SubmissionId> admit_compaction_summary(
            Transaction& tx, ConversationId conversation_id, const SubmissionDraft& draft, std::int64_t now);

private:
    /// Settle the run's inputs `unanswered/model_error` and fail with `text`
    /// (frozen pi generation `failModelError`).
    [[nodiscard]] support::ExpectedVoid fail_model_error(
            const TaskRecord& task, TaskRuntime& runtime, std::string_view text);
    struct PromptPlan {
        ai::AiContext context{};
        std::vector<ai::SystemMessage> systems{};
        std::vector<ContextEdit> edits{};
    };

    [[nodiscard]] support::Expected<TaskDefinition> build_definition();
    [[nodiscard]] PromptPlan plan_prompt(const Registry::Snapshot& snapshot,
            std::span<const ai::Tool> shown_tools,
            std::span<const ai::SystemMessageSection> shown_sections,
            const Transaction::ContextView& view,
            std::int64_t timestamp);
    /// The preparation half of one request (frozen pi generation `prepare`):
    /// resume a finished compaction wait, plan the prompt baseline, decide
    /// the compaction threshold, and commit the `request` checkpoint.
    [[nodiscard]] support::ExpectedVoid prepare(const TaskRecord& task, TaskRuntime& runtime);
    [[nodiscard]] support::ExpectedVoid retry(const TaskRecord& task, TaskRuntime& runtime);
    /// The request half of one run (frozen pi generation `request`): stream,
    /// tool rounds, final answer, and overflow compaction recovery.
    [[nodiscard]] support::ExpectedVoid request(const TaskRecord& task, TaskRuntime& runtime);
    /// Rebuild the replay cache and prompt baseline from the committed
    /// context after a compaction landed: the summary marker leads the kept
    /// range, and a head marker with no later `pi.system` entry is rebaselined
    /// by the next preparation (frozen pi `planSystemEntries`).
    /// Create one compaction task of this conversation in `tx` (frozen pi
    /// `createCompaction`); blocking (owned by the generation) or background.
    [[nodiscard]] support::Expected<TaskId> create_compaction(
            Transaction& tx, const TaskRecord& task, std::string_view reason, bool background) const;
    /// Execute one model-issued call inside the generation invocation: run
    /// the built-in `pi.tool` `beforeTool` hooks, execute with the arguments
    /// they leave, then let `afterTool` replace the result before it is
    /// committed (frozen pi `ToolTask.call`).
    [[nodiscard]] support::Expected<ai::ToolResultMessage> execute_tool(
            const ai::ToolCallContent& call, const Registry::Snapshot& snapshot, TaskRuntime& runtime);
    [[nodiscard]] support::Expected<bool> execute_tool_round(const TaskRecord& task,
            const Registry::Snapshot& snapshot,
            TaskRuntime& runtime,
            std::vector<ai::MessageVariant>& messages,
            bool recovery = false);
    /// Both halves of the built-in generation definition, dispatched by the
    /// checkpoint's `phase` (frozen pi generation `prepare`/`request`).
    [[nodiscard]] support::AsyncResult<void> dispatch(const TaskRecord& task, TaskRuntime& runtime);
    /// The abort handler of the built-in generation definition: settle the
    /// run's inputs `unanswered/aborted` (frozen pi generation `abort`).
    [[nodiscard]] support::AsyncResult<void> abort_run(const TaskRecord& task, TaskRuntime& runtime);
    /// The postTools point of the run loop: place queued writes and selected
    /// steers, joining them to the run (frozen `finishToolRound`). Returns
    /// true when a queued reset ended the run at this boundary.
    [[nodiscard]] support::AsyncResult<bool> apply_post_tools_boundary(const TaskRecord& task, TaskRuntime& runtime);
    /// Run the first `onYield` decision of one final answer and return the
    /// continuation content it asked for (frozen pi `GenerationTask.onYield`).
    /// Ordinary hook errors stay isolated through `report`.
    [[nodiscard]] support::Expected<std::optional<chord::Json>> on_yield(
            const Registry::Snapshot& snapshot, const chord::Json& answer, TaskRuntime& runtime);
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

    struct ReplayState {
        std::vector<ai::MessageVariant> messages{};
        std::vector<ai::Tool> shown_tools{};
        std::vector<ai::SystemMessageSection> shown_sections{};
    };

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
    /// The built-in compaction definition; installed before the worker starts.
    std::shared_ptr<const TaskDefinition> compaction_{};
    ai::Model model_{};
    std::optional<ai::ThinkingLevel> thinking_level_{std::nullopt};
    std::map<std::int64_t, ReplayState> replay_;
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
