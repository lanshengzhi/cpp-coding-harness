#pragma once

#include <cch/durable/Storage.hpp>

#include <filesystem>
#include <memory>

namespace cch::durable {

/// The JSONL backend of the frozen `Storage` contract (frozen pi
/// `JsonlStorage`, format 1).
///
/// The directory holds `main.jsonl`, one commit marker per line, and one
/// `doc-<id>.jsonl` sidecar per document incarnation. A commit appends every
/// sidecar record first and the main marker last; the marker confirms the
/// sidecar records by `(seq, ordinal)`. Recovery truncates an unconfirmed
/// tail, rejects a confirmed record that is missing, and requires main
/// sequences to strictly increase.
///
/// A failed commit I/O poisons this instance: later commits and reads reject
/// until the directory is reopened. Opening recovers only the published
/// prefix. This is not a power-loss or fsync guarantee.
class JsonlStorage final : public Storage {
public:
    /// Open or create one JSONL storage directory and recover its published
    /// prefix. A corrupt or incompletely published store is rejected rather
    /// than silently repaired across a missing confirmed record.
    [[nodiscard]] static support::Expected<std::unique_ptr<JsonlStorage>> open(const std::filesystem::path& directory);

    JsonlStorage(const JsonlStorage&) = delete;
    JsonlStorage& operator=(const JsonlStorage&) = delete;
    ~JsonlStorage() override;

    [[nodiscard]] support::AsyncResult<Seq> commit(std::span<const StorageWrite> writes) override;

    [[nodiscard]] support::Expected<std::int64_t> mint_id() override;

    [[nodiscard]] support::Expected<std::optional<ConversationRecord>> conversation(ConversationId id) override;

    [[nodiscard]] support::Expected<std::optional<StoredEntry>> entry(EntryId id) override;

    [[nodiscard]] support::Expected<std::vector<ConversationRecord>> scan_conversations(std::size_t limit) override;

    [[nodiscard]] support::Expected<std::vector<EntryRecord>> scan_entries(
            ConversationId conversation_id, std::size_t limit) override;

    [[nodiscard]] support::Expected<std::optional<DocumentRecord>> find_document(
            std::string_view kind, const DocumentAddress& address, DocumentPoint at) override;

    [[nodiscard]] support::Expected<std::optional<StoredDocument>> document(DocumentId id, DocumentPoint at) override;

    [[nodiscard]] support::Expected<std::optional<TaskRecord>> task(TaskId id) override;

    [[nodiscard]] support::Expected<std::vector<TaskRecord>> scan_tasks(std::size_t limit) override;

    [[nodiscard]] support::Expected<std::optional<SubmissionRecord>> submission(SubmissionId id) override;

    [[nodiscard]] support::Expected<std::optional<SubmissionRecord>> submission_by_request(
            ConversationId conversation_id, std::string_view request_id) override;

    [[nodiscard]] support::Expected<std::vector<SubmissionRecord>> scan_submissions(std::size_t limit) override;

    [[nodiscard]] support::ExpectedVoid close() override;

private:
    struct Impl;

    explicit JsonlStorage(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};

} // namespace cch::durable
