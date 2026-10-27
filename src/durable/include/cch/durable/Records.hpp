#pragma once

#include <cch/chord/Delta.hpp>
#include <cch/chord/Json.hpp>
#include <cch/support/Error.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace cch::durable {

/// One durable conversation identity (frozen pi `ConversationId`). The
/// numeric value comes from the Session-global ID namespace, so it orders
/// conversations by creation.
struct ConversationId {
    std::int64_t value{0};

    [[nodiscard]] bool operator==(const ConversationId&) const = default;
};

/// One durable entry identity (frozen pi `EntryId`).
struct EntryId {
    std::int64_t value{0};

    [[nodiscard]] bool operator==(const EntryId&) const = default;
};

/// One durable document incarnation identity (frozen pi `DocumentId`). An
/// incarnation ID is never reused when the same logical document is
/// recreated.
struct DocumentId {
    std::int64_t value{0};

    [[nodiscard]] bool operator==(const DocumentId&) const = default;
};

/// One durable task identity (frozen pi `TaskId`).
struct TaskId {
    std::int64_t value{0};

    [[nodiscard]] bool operator==(const TaskId&) const = default;
};

/// One durable submission identity (frozen pi `SubmissionId`). The numeric
/// value comes from the Session-global ID namespace, like task and entry IDs.
struct SubmissionId {
    std::int64_t value{0};

    [[nodiscard]] bool operator==(const SubmissionId&) const = default;
};

/// Complete replacement record for one admitted user input or passive entry
/// write (frozen pi `SubmissionRecord`). The flat C++ form mirrors the frozen
/// state machine: an `input` submission is `queued`, `placed` (carrying its
/// transcript `entry`), `done` (also carrying the `answer` entry), or
/// `unanswered` (carrying a `reason` and optional JSON `detail`, keeping its
/// `entry` when it was placed); a `write` submission is `queued`, `done`
/// (carrying its `entry`), or `unanswered`. Every write replaces the record.
struct SubmissionRecord {
    SubmissionId id{};
    ConversationId conversation_id{};
    /// Host-provided deduplication key, scoped to the conversation; absent
    /// when the submission has no dedup key (frozen
    /// `SubmissionRecord.requestId`).
    std::optional<std::string> request_id{std::nullopt};
    /// The submission discriminator: `"input"` or `"write"` (frozen
    /// `SubmissionRecord.type`).
    std::string type{};
    /// `"queued"`, `"placed"`, `"done"` or `"unanswered"` (frozen
    /// `SubmissionRecord.status`).
    std::string status{};
    /// The transcript entry the submission was placed at; present for placed
    /// and done records (frozen `SubmissionRecord.entry`).
    std::optional<EntryId> entry{std::nullopt};
    /// The answer entry of a done input submission (frozen
    /// `SubmissionRecord.answer`).
    std::optional<EntryId> answer{std::nullopt};
    /// The terminal reason of an unanswered submission (frozen
    /// `SubmissionRecord.reason`).
    std::optional<std::string> reason{std::nullopt};
    /// Optional structured diagnostic data of an unanswered submission
    /// (frozen `SubmissionRecord.detail`).
    std::optional<chord::Json> detail{std::nullopt};

    [[nodiscard]] bool operator==(const SubmissionRecord&) const = default;
};

/// Terminal settlement staged for one submission (frozen pi
/// `SubmissionSettlement`): `done` carries the `answer` entry,
/// `unanswered` carries the `reason` and optional JSON `detail`.
struct SubmissionSettlement {
    /// `"done"` or `"unanswered"` (frozen `SubmissionSettlement.status`).
    std::string status{};
    std::optional<EntryId> answer{std::nullopt};
    std::optional<std::string> reason{std::nullopt};
    std::optional<chord::Json> detail{std::nullopt};
};

/// JSON-safe error snapshot persisted instead of a runtime error (frozen pi `TaskOutcomeError`).
struct TaskOutcomeError {
    std::string message{};
    std::optional<chord::Json> detail{std::nullopt};

    [[nodiscard]] bool operator==(const TaskOutcomeError&) const = default;
};

/// Durable reason and optional result recorded when a task becomes terminal (frozen pi `TaskOutcome`).
struct TaskOutcome {
    /// Terminal outcome status: "completed", "failed", "aborted", "orphaned", or "faulted".
    std::string status{};
    std::optional<chord::Json> result{std::nullopt};
    std::optional<TaskOutcomeError> error{std::nullopt};
    std::optional<std::string> reason{std::nullopt};

    [[nodiscard]] bool operator==(const TaskOutcome&) const = default;
};

/// How a waiting task reacts when one of the tasks it waits for ends without
/// `completed` (frozen pi `JoinPolicy`).
///
/// `FailFast` marks the other live members abort; `AllSettled` lets every
/// member finish on its own. Both resume at the waiting checkpoint once every
/// member is terminal.
enum class JoinPolicy {
    AllSettled,
    FailFast,
};

/// The frozen `JoinPolicy` spelling: `"allSettled"` or `"failFast"`.
[[nodiscard]] constexpr std::string_view join_policy_name(JoinPolicy policy) noexcept {
    return policy == JoinPolicy::FailFast ? "failFast" : "allSettled";
}

/// Parse the frozen `JoinPolicy` spelling.
[[nodiscard]] inline std::optional<JoinPolicy> join_policy_from_name(std::string_view name) noexcept {
    if (name == "failFast") return JoinPolicy::FailFast;
    if (name == "allSettled") return JoinPolicy::AllSettled;
    return std::nullopt;
}

/// Complete durable execution state of a task (frozen pi `TaskState`).
struct TaskState {
    /// Status: "pending", "running", "waiting", "completing", or "terminal".
    std::string status{};
    std::optional<chord::Json> checkpoint{std::nullopt};
    /// The tasks a waiting task waits for (frozen `TaskState.on`); empty for
    /// every other status.
    std::vector<TaskId> on{};
    /// The join policy of a waiting task (frozen `TaskState.policy`); absent
    /// for every other status.
    std::optional<JoinPolicy> policy{std::nullopt};
    std::optional<TaskOutcome> outcome{std::nullopt};

    [[nodiscard]] bool operator==(const TaskState&) const = default;
};

/// Complete replacement record for one durable task state machine (frozen pi `TaskRecord`).
struct TaskRecord {
    TaskId id{};
    ConversationId conversation_id{};
    std::string kind{};
    std::int64_t version{1};
    chord::Json input{};
    std::optional<TaskId> owner{std::nullopt};
    bool background{false};
    bool abort_requested{false};
    std::optional<std::string> abort_reason{std::nullopt};
    std::optional<std::int64_t> started_at{std::nullopt};
    std::optional<std::int64_t> ended_at{std::nullopt};
    TaskState state{};
    std::map<std::string, chord::Json> memos{};

    [[nodiscard]] bool operator==(const TaskRecord&) const = default;
};

/// Task options when creating a task (frozen pi `TaskOptions`).
struct TaskOptions {
    /// The conversation the task lives in. A child task inherits its owner's
    /// conversation, and naming another one is rejected.
    std::optional<ConversationId> conversation_id{std::nullopt};
    /// The owning task (frozen `TaskOptions.ownership` `{kind: "task"}`);
    /// absent creates a conversation-owned task.
    std::optional<TaskId> owner{std::nullopt};
    /// Whether the task's subtree is a background one that an ordinary scope
    /// walk does not enter (frozen `TaskOptions.background`).
    bool background{false};
    bool abandon_on_restart{false};
    std::optional<chord::Json> checkpoint{std::nullopt};
};

/// Derived scheduler state of one live task (frozen pi `TaskInspection.state`).
struct TaskInspectionState {
    /// "running", "ready", "waiting", "completing", or "blocked".
    std::string kind{};
    /// Present when `kind == "ready"`.
    std::optional<bool> migrates{std::nullopt};
    /// Present when `kind == "waiting"`.
    std::vector<TaskId> on{};
    /// Present when `kind == "blocked"`: "missing_task", "task_too_old", or "migration_failed".
    std::optional<std::string> reason{std::nullopt};
    /// Present when `kind == "blocked"` and a migration failed.
    std::optional<support::Error> error{std::nullopt};

    [[nodiscard]] bool operator==(const TaskInspectionState&) const = default;
};

/// Live task and what the scheduler would do with it under the current registry (frozen pi `TaskInspection`).
struct TaskInspection {
    TaskRecord record{};
    TaskInspectionState state{};

    [[nodiscard]] bool operator==(const TaskInspection&) const = default;
};

/// Point-in-time view of live work (frozen pi `HarnessInspection`).
struct HarnessInspection {
    /// "paused", "running", or "closing".
    std::string scheduling{};
    std::vector<TaskInspection> tasks{};

    [[nodiscard]] bool operator==(const HarnessInspection&) const = default;
};

/// The sequence of one adopted storage commit (frozen pi `Seq`). Sequences
/// strictly increase per storage, one per adopted batch.
using Seq = std::int64_t;

/// The creator edge of one conversation (frozen pi
/// `ConversationRecord.owner`): the task that created it, and the conversation
/// that task lives in. The edge is the conversation's parent node in the
/// ownership tree, so subtree aborts and subtree idle waits reach it.
struct ConversationOwner {
    ConversationId conversation_id{};
    TaskId task_id{};

    [[nodiscard]] bool operator==(const ConversationOwner&) const = default;
};

/// The immutable record of one conversation (frozen pi `ConversationRecord`).
struct ConversationRecord {
    ConversationId id{};
    /// Inclusive transcript ancestry selected by a fork.
    struct Parent {
        ConversationId conversation_id{};
        EntryId at{};

        [[nodiscard]] bool operator==(const Parent&) const = default;
    };
    std::optional<Parent> parent{std::nullopt};
    /// The creator edge (frozen `ConversationRecord.owner`); absent for an
    /// ownerless conversation, the reserved root included.
    std::optional<ConversationOwner> owner{std::nullopt};

    [[nodiscard]] bool operator==(const ConversationRecord&) const = default;
};

/// Model messages and execution provenance carried by an entry draft (frozen
/// pi `EntryDraft.model` and `EntryDraft.byTaskId`).
struct EntryOptions {
    std::vector<chord::Json> model{};
    std::optional<TaskId> by_task_id{std::nullopt};
};

/// The immutable record of one transcript entry (frozen pi `EntryRecord`).
/// A context-only override of an earlier entry's model contribution.
struct ContextEdit {
    EntryId target{};
    std::string action{};
    std::vector<chord::Json> messages{};

    [[nodiscard]] bool operator==(const ContextEdit&) const = default;
};

struct EntryRecord {
    EntryId id{};
    ConversationId conversation_id{};
    /// The application-defined entry discriminator (frozen `EntryRecord.kind`).
    std::string kind{};
    /// The entry payload (frozen `EntryRecord.data`).
    chord::Json data{};
    /// The first entry of the active context selected by this entry (frozen
    /// `EntryRecord.head`); absent when the entry selects no context.
    std::optional<EntryId> head{};
    std::vector<chord::Json> model{};
    /// Context-only overrides applied to earlier entries in the context range.
    std::vector<ContextEdit> edits{};
    std::optional<TaskId> by_task_id{std::nullopt};
};

/// The ownership and lifetime of a document (frozen pi `DocumentSemantics`).
///
/// Only a conversation document declares `history` and `fork` behavior;
/// Session and task documents are current-only and are never copied by a
/// conversation fork.
enum class DocumentScope {
    Session,
    Conversation,
    Task,
};

/// Whether a conversation document keeps the revisions an as-of read needs
/// (frozen `LatestConversationSemantics` / `RewindableConversationSemantics`).
///
/// A `latest` document keeps only its current state: a checkpoint base
/// replaces its history and a historical read is rejected. A `rewindable`
/// document keeps every revision so an entry's commit can address it.
enum class DocumentHistory {
    Latest,
    Rewindable,
};

/// What a conversation fork initializes a copied document from (frozen
/// `DocumentRecord.fork`): the source state as of the fork entry, the source
/// current state, or the definition's initial value.
enum class DocumentFork {
    AsOf,
    Current,
    Initial,
};

/// Exact logical identity of one singleton or keyed family member (frozen pi
/// `DocumentAddress`).
///
/// A Session document names no owner, a conversation document names its
/// conversation and a task document names its task; `key` selects one family
/// member. Two addresses are the same document only when the kind, the scope
/// owner and the key all match, so a Session document and a conversation
/// document of the same kind never share an incarnation.
struct DocumentAddress {
    /// The owning conversation; absent unless the scope is conversation.
    std::optional<ConversationId> conversation_id{};
    /// The owning task; absent unless the scope is task.
    std::optional<TaskId> task_id{};
    /// The family member key; absent for a singleton document.
    std::optional<std::string> key{};

    /// The scope this address selects (frozen `DocumentAddress.scope.kind`).
    [[nodiscard]] DocumentScope scope() const noexcept {
        if (task_id.has_value()) return DocumentScope::Task;
        if (conversation_id.has_value()) return DocumentScope::Conversation;
        return DocumentScope::Session;
    }

    [[nodiscard]] bool operator==(const DocumentAddress&) const = default;
};

/// The immutable record of one document incarnation (frozen pi
/// `DocumentRecord`).
///
/// The record is the whole identity of the incarnation: its numeric ID is
/// never reused, so recreating the same logical document produces a new
/// record with a new ID, and the retired record stays addressable by ID.
struct DocumentRecord {
    DocumentId id{};
    /// The stable document definition kind (frozen `DocumentRecord.kind`).
    std::string kind{};
    /// The commit that created the incarnation (frozen
    /// `DocumentRecord.createdAt`).
    Seq created_at{0};
    /// The owning conversation; absent unless the scope is conversation.
    std::optional<ConversationId> conversation_id{};
    /// The owning task; absent unless the scope is task.
    std::optional<TaskId> task_id{};
    /// The family member key; absent for a singleton document.
    std::optional<std::string> key{};
    /// Whether the incarnation keeps every revision (frozen
    /// `DocumentRecord.history`); always `latest` outside conversation scope.
    DocumentHistory history{DocumentHistory::Latest};
    /// What a conversation fork initializes this incarnation's copy from
    /// (frozen `DocumentRecord.fork`); always `current` outside conversation
    /// scope.
    DocumentFork fork{DocumentFork::Current};
    /// The commit that retired the incarnation (frozen
    /// `DocumentRecord.retiredAt`); absent while it is the current incarnation
    /// of its address.
    std::optional<Seq> retired_at{};

    /// The logical address this incarnation occupies.
    [[nodiscard]] DocumentAddress address() const {
        return DocumentAddress{.conversation_id = conversation_id, .task_id = task_id, .key = key};
    }

    [[nodiscard]] DocumentScope scope() const noexcept { return address().scope(); }

    [[nodiscard]] bool operator==(const DocumentRecord&) const = default;
};

/// Current state or one historical commit sequence used for document
/// membership and content reads (frozen pi `DocumentPoint`).
struct DocumentPoint {
    /// Absent selects the current incarnation; present selects the state as of
    /// that commit sequence.
    std::optional<Seq> at{};

    [[nodiscard]] static DocumentPoint current() noexcept { return DocumentPoint{}; }

    [[nodiscard]] static DocumentPoint as_of(Seq seq) noexcept { return DocumentPoint{.at = seq}; }

    [[nodiscard]] bool operator==(const DocumentPoint&) const = default;
};

/// Detached materialized value and stored definition version at one point
/// (frozen pi `StoredDocument`).
struct StoredDocument {
    DocumentRecord record{};
    /// The definition version the materialized value has.
    std::uint32_t version{1};
    /// The materialized value at the selected point.
    chord::Json value{};
    /// Deltas replayed after the selected base to materialize `value`.
    std::int64_t deltas_since_base{0};
};

/// Exact persisted source selected for a definition-free document copy
/// (frozen pi `DocumentCopySource`).
struct DocumentCopySource {
    DocumentId id{};
    DocumentPoint at{};

    [[nodiscard]] bool operator==(const DocumentCopySource&) const = default;
};

/// The stable string identity of one logical document address (frozen pi
/// `addressId`).
///
/// Kind, scope owner and family key are all part of the identity, so the same
/// kind under different scopes or keys names different documents.
[[nodiscard]] std::string document_address_key(std::string_view kind, const DocumentAddress& address);

/// The persisted name of one document scope (frozen `scope.kind`).
[[nodiscard]] std::string_view document_scope_name(DocumentScope scope) noexcept;

/// Whether one incarnation retains only its current state (frozen pi
/// `isCurrentOnly`): every non-conversation document and every `latest`
/// conversation document.
[[nodiscard]] bool document_is_current_only(const DocumentRecord& record) noexcept;

/// One committed table record (frozen pi `TableCommitChange`).
struct TableCommitChange {
    /// The record kind: `conversation`, `entry` or `task` (frozen
    /// `StorageWrite.type`).
    std::string type{};
    /// The conversation the record belongs to.
    ConversationId conversation_id{};
    /// The entry the record carries; absent for a conversation or task record.
    std::optional<EntryId> entry_id{};
    /// The task the record carries; absent for a conversation or entry record.
    std::optional<TaskId> task_id{};
    /// The full task record of a task commit change.
    std::optional<TaskRecord> task{};
};

/// One committed document change (frozen pi `DocumentCommitChange`).
struct DocumentCommitChange {
    DocumentRecord record{};
    /// The conversation owning the document; absent for a Session document.
    /// A task document derives it from its task record.
    std::optional<ConversationId> conversation_id{};
    /// The exact adopted immutable revision, or JSON null when this commit
    /// retired the incarnation (frozen `DocumentCommitChange.value`).
    chord::Json value{};
    /// The exact adopted operations (frozen `DocumentCommitChange.ops`); empty
    /// for a creation and for a retirement.
    chord::OpBatch ops{};
    /// Definition version of `value`; absent when this commit retired the
    /// incarnation (frozen `DocumentCommitChange.version`).
    std::optional<std::uint32_t> version{};
    /// The persisted source of a definition-free copy publication (frozen
    /// `DocumentCommitChange` of type `document.copy`); absent otherwise.
    std::optional<DocumentCopySource> source{};
};

/// One committed submission change (frozen pi `CommitChange` for a
/// submission write).
struct SubmissionCommitChange {
    /// The record kind: `"submission"` (frozen `StorageWrite.type`).
    std::string type{};
    /// The full replaced submission record (frozen
    /// `SubmissionCommitChange.value`).
    SubmissionRecord value{};
};

/// Every immutable change from one successful Session commit (frozen pi
/// `CommitChange`).
using CommitChange = std::variant<TableCommitChange, DocumentCommitChange, SubmissionCommitChange>;

/// Every immutable change from one successful Session commit (frozen pi
/// `CommitPublication`). The change order is unspecified.
struct CommitPublication {
    Seq seq{0};
    std::vector<CommitChange> changes{};
};

/// One record or document mutation in an atomic storage commit (frozen pi
/// `StorageWrite`).
struct StorageWrite {
    /// The write kind: `conversation`, `entry`, `task`, `submission`,
    /// `document.create`, `document.copy`, `document.change` or
    /// `document.retire` (frozen `StorageWrite.type`).
    std::string type{};
    /// The conversation record of a `conversation` write.
    std::optional<ConversationRecord> conversation{};
    /// The entry record of an `entry` write.
    std::optional<EntryRecord> entry{};
    /// The task record of a `task` write.
    std::optional<TaskRecord> task{};
    /// The full record of a `submission` write; every submission write
    /// replaces the record.
    std::optional<SubmissionRecord> submission{};
    /// The document record of a `document.create` or `document.copy` write.
    std::optional<DocumentRecord> document_record{};
    /// The incarnation a `document.change` or `document.retire` write targets.
    std::optional<DocumentId> document_id{};
    /// The persisted source of a `document.copy` write. The backend resolves
    /// it to the source's exact base content at that point.
    std::optional<DocumentCopySource> document_copy_source{};
    /// The definition version of a `document.create` or `document.change`
    /// write.
    std::uint32_t document_version{1};
    /// The committed value of a `document.create` or `document.change` write.
    std::optional<chord::Json> document_value{};
    /// The committed operations of a `document.change` write; empty selects a
    /// complete base revision instead of a delta.
    chord::OpBatch document_ops{};
};

} // namespace cch::durable
