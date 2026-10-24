#pragma once

#include <cch/durable/Records.hpp>
#include <cch/support/AsyncResult.hpp>
#include <cch/support/Error.hpp>

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace cch::durable {

/// One committed entry and the sequence of the commit that persisted it
/// (frozen pi `Storage.entry`).
struct StoredEntry {
    EntryRecord entry;
    Seq commit_seq{0};
};

/// The atomic persistence boundary for Session records (frozen pi `Storage`).
///
/// Storage trusts the owning Session to supply semantically valid records and
/// references. Implementations enforce atomicity, global ID ownership,
/// immutable conversation and entry creation, and detached values; the
/// Session serializes commits.
///
/// An error a method reports is final: it fails the Session, which nothing
/// retries, so a transient failure is retried inside the method. A read that
/// fails for an invalid request (an unknown conversation, a foreign cursor)
/// fails only that read and has no durable effect.
class Storage {
public:
    Storage() = default;
    Storage(const Storage&) = delete;
    Storage& operator=(const Storage&) = delete;
    virtual ~Storage() = default;

    /// Atomically persist one batch and return its commit sequence. Once
    /// resolved, later reads through this storage observe it (frozen pi
    /// `Storage.commit`).
    [[nodiscard]] virtual support::AsyncResult<Seq> commit(std::span<const StorageWrite> writes) = 0;

    /// Return a fresh candidate from the Session-global numeric ID namespace,
    /// greater than every ID minted or stored before, also across a reopen
    /// (frozen pi `Storage.mintId`).
    [[nodiscard]] virtual support::Expected<std::int64_t> mint_id() = 0;

    /// Look up one conversation by exact ID (frozen pi `Storage.conversation`).
    [[nodiscard]] virtual support::Expected<std::optional<ConversationRecord>> conversation(ConversationId id) = 0;

    /// Look up one global entry and the sequence of the commit that persisted
    /// it (frozen pi `Storage.entry`).
    [[nodiscard]] virtual support::Expected<std::optional<StoredEntry>> entry(EntryId id) = 0;

    /// Scan the conversations, oldest first, returning at most `limit`
    /// conversations (frozen pi `Storage.scanConversations`).
    [[nodiscard]] virtual support::Expected<std::vector<ConversationRecord>> scan_conversations(std::size_t limit) = 0;

    /// Scan the visible entries of one conversation, newest first, returning at
    /// most `limit` entries (frozen pi `Storage.scanEntries`).
    [[nodiscard]] virtual support::Expected<std::vector<EntryRecord>> scan_entries(
            ConversationId conversation_id, std::size_t limit) = 0;

    /// Look up the incarnation occupying one exact logical address (frozen pi
    /// `Storage.findDocument`).
    [[nodiscard]] virtual support::Expected<std::optional<DocumentRecord>> find_document(
            std::string_view kind, std::optional<ConversationId> conversation_id) = 0;

    /// Materialize one specific incarnation by ID (frozen pi
    /// `Storage.document`).
    [[nodiscard]] virtual support::Expected<std::optional<chord::Json>> document(DocumentId id) = 0;

    /// Look up the latest complete record for one task (frozen pi
    /// `Storage.task`).
    [[nodiscard]] virtual support::Expected<std::optional<TaskRecord>> task(TaskId id) = 0;

    /// Scan task records, oldest first, returning at most `limit` tasks (frozen
    /// pi `Storage.scanTasks`).
    [[nodiscard]] virtual support::Expected<std::vector<TaskRecord>> scan_tasks(std::size_t limit) = 0;

    /// Release backend resources; all later operations must reject (frozen pi
    /// `Storage.close`).
    [[nodiscard]] virtual support::ExpectedVoid close() = 0;
};

/// The detached in-memory reference implementation of `Storage` (frozen pi
/// `MemoryStorage`).
///
/// Reads and retained writes are cloned to match the ownership boundary of
/// serialization-backed stores. The batch is validated and detached before it
/// is applied, so a rejected batch leaves the observable state unchanged and
/// consumes no sequence.
class MemoryStorage final : public Storage {
public:
    MemoryStorage();
    MemoryStorage(const MemoryStorage&) = delete;
    MemoryStorage& operator=(const MemoryStorage&) = delete;
    ~MemoryStorage() override;

    [[nodiscard]] support::AsyncResult<Seq> commit(std::span<const StorageWrite> writes) override;

    [[nodiscard]] support::Expected<std::int64_t> mint_id() override;

    [[nodiscard]] support::Expected<std::optional<ConversationRecord>> conversation(ConversationId id) override;

    [[nodiscard]] support::Expected<std::optional<StoredEntry>> entry(EntryId id) override;

    [[nodiscard]] support::Expected<std::vector<ConversationRecord>> scan_conversations(std::size_t limit) override;

    [[nodiscard]] support::Expected<std::vector<EntryRecord>> scan_entries(
            ConversationId conversation_id, std::size_t limit) override;

    [[nodiscard]] support::Expected<std::optional<DocumentRecord>> find_document(
            std::string_view kind, std::optional<ConversationId> conversation_id) override;

    [[nodiscard]] support::Expected<std::optional<chord::Json>> document(DocumentId id) override;

    [[nodiscard]] support::Expected<std::optional<TaskRecord>> task(TaskId id) override;

    [[nodiscard]] support::Expected<std::vector<TaskRecord>> scan_tasks(std::size_t limit) override;

    [[nodiscard]] support::ExpectedVoid close() override;

private:
    struct Impl;

    std::unique_ptr<Impl> impl_;
};

} // namespace cch::durable
