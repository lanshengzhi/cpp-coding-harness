#pragma once

#include <cch/durable/Storage.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cch::durable {

/// The validated record state shared by the durable Storage backends.
///
/// This is not an interface: it is the one implementation of the frozen
/// Storage record semantics (validation, ID and sequence allocation, scans,
/// document addressing). A backend decides *where* a batch is written; this
/// type decides whether the batch is legal and what it means.
///
/// A backend supplies an explicit sequence to [`apply`] because the sequence
/// is part of the persistent record, not a private counter: JSONL recovery
/// replays a marker's sequence, and reopening the same records must continue
/// the same ID and sequence namespaces.
class StorageState {
public:
    explicit StorageState(std::string closed_message);
    ~StorageState();

    StorageState(const StorageState&) = delete;
    StorageState& operator=(const StorageState&) = delete;
    StorageState(StorageState&&) = delete;
    StorageState& operator=(StorageState&&) = delete;

    /// Validate and apply one detached batch at `seq`, advancing the sequence
    /// high-water mark. A rejected batch changes nothing.
    [[nodiscard]] support::Expected<Seq> apply(std::span<const StorageWrite> writes, Seq seq);

    /// Validate one batch without changing state. A backend calls this before
    /// it writes, so an invalid batch leaves no marker behind.
    [[nodiscard]] support::ExpectedVoid validate(std::span<const StorageWrite> writes, Seq seq);

    /// Read one committed sequence back during recovery. Same validation as
    /// [`apply`], without consuming a new sequence.
    [[nodiscard]] support::ExpectedVoid restore(std::span<const StorageWrite> writes, Seq seq);

    [[nodiscard]] std::int64_t reserve_id();
    [[nodiscard]] Seq next_seq() const;

    [[nodiscard]] support::Expected<std::optional<ConversationRecord>> conversation(ConversationId id);
    [[nodiscard]] support::Expected<std::optional<StoredEntry>> entry(EntryId id);
    [[nodiscard]] support::Expected<std::vector<ConversationRecord>> scan_conversations(std::size_t limit);
    [[nodiscard]] support::Expected<std::vector<EntryRecord>> scan_entries(
            ConversationId conversation_id, std::size_t limit);
    [[nodiscard]] support::Expected<std::optional<DocumentRecord>> find_document(
            std::string_view kind, std::optional<ConversationId> conversation_id);
    [[nodiscard]] support::Expected<std::optional<chord::Json>> document(DocumentId id);

    [[nodiscard]] bool closed() const;
    void close();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cch::durable
