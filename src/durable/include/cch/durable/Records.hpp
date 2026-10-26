#pragma once

#include <cch/chord/Delta.hpp>
#include <cch/chord/Json.hpp>
#include <cch/support/Error.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
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

/// Complete durable execution state of a task (frozen pi `TaskState`).
struct TaskState {
    /// Status: "pending", "running", "waiting", "completing", or "terminal".
    std::string status{};
    std::optional<chord::Json> checkpoint{std::nullopt};
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
    std::optional<ConversationId> conversation_id{std::nullopt};
    std::optional<TaskId> owner{std::nullopt};
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

/// The immutable record of one conversation (frozen pi `ConversationRecord`).
struct ConversationRecord {
    ConversationId id{};
};

/// Model messages and execution provenance carried by an entry draft (frozen
/// pi `EntryDraft.model` and `EntryDraft.byTaskId`).
struct EntryOptions {
    std::vector<chord::Json> model{};
    std::optional<TaskId> by_task_id{std::nullopt};
};

/// The immutable record of one transcript entry (frozen pi `EntryRecord`).
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
    std::optional<TaskId> by_task_id{std::nullopt};
};

/// The immutable record of one document incarnation (frozen pi
/// `DocumentRecord`).
struct DocumentRecord {
    DocumentId id{};
    /// The stable document definition kind (frozen `DocumentRecord.kind`).
    std::string kind{};
    /// The commit that created the incarnation (frozen
    /// `DocumentRecord.createdAt`).
    Seq created_at{0};
};

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
    std::optional<ConversationId> conversation_id{};
    /// The exact adopted immutable revision (frozen
    /// `DocumentCommitChange.value`).
    chord::Json value{};
    /// The exact adopted operations (frozen `DocumentCommitChange.ops`); empty
    /// for a creation.
    chord::OpBatch ops{};
};

/// Every immutable change from one successful Session commit (frozen pi
/// `CommitChange`).
using CommitChange = std::variant<TableCommitChange, DocumentCommitChange>;

/// Every immutable change from one successful Session commit (frozen pi
/// `CommitPublication`). The change order is unspecified.
struct CommitPublication {
    Seq seq{0};
    std::vector<CommitChange> changes{};
};

/// One record or document mutation in an atomic storage commit (frozen pi
/// `StorageWrite`).
struct StorageWrite {
    /// The write kind: `conversation`, `entry`, `task`, `document.create`,
    /// `document.change` or `document.retire` (frozen `StorageWrite.type`).
    std::string type{};
    /// The conversation record of a `conversation` write.
    std::optional<ConversationRecord> conversation{};
    /// The entry record of an `entry` write.
    std::optional<EntryRecord> entry{};
    /// The task record of a `task` write.
    std::optional<TaskRecord> task{};
    /// The document record of a `document.create` write.
    std::optional<DocumentRecord> document_record{};
    /// The incarnation a `document.change` or `document.retire` write targets.
    std::optional<DocumentId> document_id{};
    /// The conversation owning the document incarnation; absent for a Session
    /// document. The frozen document scope (frozen `DocumentRecord.scope`).
    std::optional<ConversationId> document_conversation_id{};
    /// The committed value of a `document.create` or `document.change` write.
    std::optional<chord::Json> document_value{};
    /// The committed operations of a `document.change` write.
    chord::OpBatch document_ops{};
};

} // namespace cch::durable
