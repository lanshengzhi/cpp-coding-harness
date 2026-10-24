#pragma once

#include <cch/durable/Storage.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace cch::durable {

/// Configuration for one SQLite-backed Storage connection.
struct SqliteStorageOptions {
    /// Prefix every durable table and index with this SQLite identifier.
    std::string prefix{};
    /// Milliseconds to wait for a competing SQLite lock (default 5,000).
    std::int32_t busy_timeout_ms{5'000};
    /// WAL pages written before SQLite runs an automatic checkpoint (default 1,000; zero disables it).
    std::int32_t wal_auto_checkpoint_pages{1'000};
};

/// The SQLite backend of the durable Storage contract (schema version 1).
class SqliteStorage final : public Storage {
public:
    [[nodiscard]] static support::Expected<std::unique_ptr<SqliteStorage>> open(
            const std::filesystem::path& database, SqliteStorageOptions options = {});

    SqliteStorage(SqliteStorage&&) noexcept;
    SqliteStorage& operator=(SqliteStorage&&) noexcept;
    ~SqliteStorage() override;
    SqliteStorage(const SqliteStorage&) = delete;
    SqliteStorage& operator=(const SqliteStorage&) = delete;

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

    explicit SqliteStorage(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};

} // namespace cch::durable
