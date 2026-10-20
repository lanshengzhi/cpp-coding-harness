#pragma once

#include <cch/chord/Delta.hpp>
#include <cch/chord/Json.hpp>
#include <cch/support/Error.hpp>

#include <cstdint>
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

/// The sequence of one adopted storage commit (frozen pi `Seq`). Sequences
/// strictly increase per storage, one per adopted batch.
using Seq = std::int64_t;

/// The immutable record of one conversation (frozen pi `ConversationRecord`).
struct ConversationRecord {
    ConversationId id{};
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
    /// The record kind: `conversation` or `entry` (frozen
    /// `StorageWrite.type`).
    std::string type{};
    /// The conversation the record belongs to.
    ConversationId conversation_id{};
    /// The entry the record carries; absent for a conversation record.
    std::optional<EntryId> entry_id{};
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
    /// The write kind: `conversation`, `entry`, `document.create`,
    /// `document.change` or `document.retire` (frozen `StorageWrite.type`).
    std::string type{};
    /// The conversation record of a `conversation` write.
    std::optional<ConversationRecord> conversation{};
    /// The entry record of an `entry` write.
    std::optional<EntryRecord> entry{};
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
