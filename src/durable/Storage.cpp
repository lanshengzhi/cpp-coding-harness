#include <cch/durable/Storage.hpp>

#include "StorageState.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cch::durable {

struct MemoryStorage::Impl final : StorageState {
    Impl() : StorageState("MemoryStorage is closed") {}
};

MemoryStorage::MemoryStorage() : impl_(std::make_unique<Impl>()) {}

MemoryStorage::~MemoryStorage() = default;

support::AsyncResult<Seq> MemoryStorage::commit(std::span<const StorageWrite> writes) {
    return support::AsyncResult<Seq>{impl_->apply(writes, impl_->next_seq())};
}

support::Expected<std::int64_t> MemoryStorage::mint_id() {
    if (impl_->closed()) {
        return std::unexpected(support::make_error(support::ErrorCode::Session, "MemoryStorage is closed"));
    }
    return impl_->reserve_id();
}

support::Expected<std::optional<ConversationRecord>> MemoryStorage::conversation(ConversationId id) {
    return impl_->conversation(id);
}

support::Expected<std::optional<StoredEntry>> MemoryStorage::entry(EntryId id) { return impl_->entry(id); }

support::Expected<std::vector<ConversationRecord>> MemoryStorage::scan_conversations(std::size_t limit) {
    return impl_->scan_conversations(limit);
}

support::Expected<std::vector<EntryRecord>> MemoryStorage::scan_entries(
        ConversationId conversation_id, std::size_t limit) {
    return impl_->scan_entries(conversation_id, limit);
}

support::Expected<std::optional<DocumentRecord>> MemoryStorage::find_document(
        std::string_view kind, std::optional<ConversationId> conversation_id) {
    return impl_->find_document(kind, conversation_id);
}

support::Expected<std::optional<chord::Json>> MemoryStorage::document(DocumentId id) { return impl_->document(id); }

support::ExpectedVoid MemoryStorage::close() {
    impl_->close();
    return {};
}

} // namespace cch::durable
