#include <cch/durable/Storage.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::durable {

namespace {

/// The first ID the frozen memory backend mints (frozen pi
/// `MemoryStorage.nextId`).
constexpr std::int64_t kFirstId = 2;

/// The first commit sequence the frozen memory backend hands out (frozen pi
/// `MemoryStorage.nextSeq`).
constexpr Seq kFirstSeq = 1;

/// One stored document incarnation with its committed value.
struct StoredDocument {
    DocumentRecord record;
    chord::Json value;
};

/// One stored entry with the sequence of the commit that persisted it.
struct StoredEntryState {
    EntryRecord record;
    Seq commit_seq{0};
};

/// The address key of one document: a Session document and a conversation
/// document are different documents.
[[nodiscard]] std::string document_address_key(std::string_view kind, std::optional<ConversationId> conversation_id) {
    std::string key{kind};
    key += '|';
    if (conversation_id.has_value()) {
        key += std::to_string(conversation_id->value);
    } else {
        key += "session";
    }
    return key;
}

} // namespace

struct MemoryStorage::Impl {
    std::map<std::int64_t, ConversationRecord> conversations;
    std::vector<std::int64_t> conversation_ids;
    std::map<std::int64_t, StoredEntryState> entries;
    std::map<std::int64_t, std::vector<std::int64_t>> entry_ids_by_conversation;
    std::map<std::int64_t, StoredDocument> documents;
    std::map<std::string, std::int64_t> document_id_by_address;
    std::int64_t next_id{kFirstId};
    Seq next_seq{kFirstSeq};
    bool closed{false};

    [[nodiscard]] support::Error closed_error() const {
        return support::make_error(support::ErrorCode::Session, "MemoryStorage is closed");
    }
};

MemoryStorage::MemoryStorage() : impl_(std::make_unique<Impl>()) {}

MemoryStorage::~MemoryStorage() = default;

support::AsyncResult<Seq> MemoryStorage::commit(std::span<const StorageWrite> writes) {
    if (impl_->closed) {
        return support::AsyncResult<Seq>{support::Expected<Seq>{std::unexpected(impl_->closed_error())}};
    }
    if (writes.empty()) {
        return support::AsyncResult<Seq>{support::Expected<Seq>{impl_->next_seq}};
    }

    // The frozen backend validates and detaches the batch before it changes
    // the observable state, so a rejected batch consumes no sequence and
    // leaves every record untouched.
    std::vector<StorageWrite> detached(writes.begin(), writes.end());
    std::map<std::int64_t, ConversationRecord> next_conversations = impl_->conversations;
    std::vector<std::int64_t> next_conversation_ids = impl_->conversation_ids;
    std::map<std::int64_t, StoredEntryState> next_entries = impl_->entries;
    std::map<std::int64_t, std::vector<std::int64_t>> next_entry_ids = impl_->entry_ids_by_conversation;
    std::map<std::int64_t, StoredDocument> next_documents = impl_->documents;
    std::map<std::string, std::int64_t> next_document_ids = impl_->document_id_by_address;
    std::int64_t next_id = impl_->next_id;

    for (const StorageWrite& write : detached) {
        if (write.type == "conversation") {
            if (!write.conversation.has_value()) {
                return support::AsyncResult<Seq>{support::Expected<Seq>{std::unexpected(
                        support::make_error(support::ErrorCode::Validation, "Conversation write has no record"))}};
            }
            const ConversationRecord& record = *write.conversation;
            if (next_conversations.contains(record.id.value)) {
                return support::AsyncResult<Seq>{support::Expected<Seq>{std::unexpected(
                        support::make_error(support::ErrorCode::Validation, "Conversation already exists"))}};
            }
            next_conversations.emplace(record.id.value, record);
            next_conversation_ids.push_back(record.id.value);
            next_id = std::max(next_id, record.id.value + 1);
        } else if (write.type == "entry") {
            if (!write.entry.has_value()) {
                return support::AsyncResult<Seq>{support::Expected<Seq>{std::unexpected(
                        support::make_error(support::ErrorCode::Validation, "Entry write has no record"))}};
            }
            const EntryRecord& record = *write.entry;
            if (next_entries.contains(record.id.value)) {
                return support::AsyncResult<Seq>{support::Expected<Seq>{
                        std::unexpected(support::make_error(support::ErrorCode::Validation, "Entry already exists"))}};
            }
            if (!next_conversations.contains(record.conversation_id.value)) {
                return support::AsyncResult<Seq>{support::Expected<Seq>{
                        std::unexpected(support::make_error(support::ErrorCode::Validation, "Unknown conversation"))}};
            }
            next_entries.emplace(record.id.value, StoredEntryState{.record = record, .commit_seq = impl_->next_seq});
            next_entry_ids[record.conversation_id.value].push_back(record.id.value);
            next_id = std::max(next_id, record.id.value + 1);
        } else if (write.type == "document.create") {
            if (!write.document_record.has_value() || !write.document_value.has_value()) {
                return support::AsyncResult<Seq>{support::Expected<Seq>{std::unexpected(
                        support::make_error(support::ErrorCode::Validation, "Document create write is incomplete"))}};
            }
            DocumentRecord record = *write.document_record;
            if (next_documents.contains(record.id.value)) {
                return support::AsyncResult<Seq>{support::Expected<Seq>{std::unexpected(
                        support::make_error(support::ErrorCode::Validation, "Document already exists"))}};
            }
            record.created_at = impl_->next_seq;
            const std::int64_t id = record.id.value;
            const std::string address_key = document_address_key(record.kind, write.document_conversation_id);
            next_documents.emplace(id, StoredDocument{.record = std::move(record), .value = *write.document_value});
            next_document_ids[address_key] = id;
            next_id = std::max(next_id, id + 1);
        } else if (write.type == "document.change") {
            if (!write.document_id.has_value()) {
                return support::AsyncResult<Seq>{support::Expected<Seq>{std::unexpected(
                        support::make_error(support::ErrorCode::Validation, "Document change write has no id"))}};
            }
            const auto found = next_documents.find(write.document_id->value);
            if (found == next_documents.end()) {
                return support::AsyncResult<Seq>{support::Expected<Seq>{
                        std::unexpected(support::make_error(support::ErrorCode::Validation, "Unknown document"))}};
            }
            if (write.document_value.has_value()) {
                found->second.value = *write.document_value;
            }
        } else if (write.type == "document.retire") {
            if (!write.document_id.has_value()) {
                return support::AsyncResult<Seq>{support::Expected<Seq>{std::unexpected(
                        support::make_error(support::ErrorCode::Validation, "Document retire write has no id"))}};
            }
            const auto found = next_documents.find(write.document_id->value);
            if (found == next_documents.end()) {
                return support::AsyncResult<Seq>{support::Expected<Seq>{
                        std::unexpected(support::make_error(support::ErrorCode::Validation, "Unknown document"))}};
            }
            next_document_ids.erase(document_address_key(found->second.record.kind, write.document_conversation_id));
            next_documents.erase(found);
        } else {
            return support::AsyncResult<Seq>{support::Expected<Seq>{
                    std::unexpected(support::make_error(support::ErrorCode::Validation, "Unknown storage write"))}};
        }
    }

    impl_->conversations = std::move(next_conversations);
    impl_->conversation_ids = std::move(next_conversation_ids);
    impl_->entries = std::move(next_entries);
    impl_->entry_ids_by_conversation = std::move(next_entry_ids);
    impl_->documents = std::move(next_documents);
    impl_->document_id_by_address = std::move(next_document_ids);
    impl_->next_id = next_id;
    const Seq seq = impl_->next_seq;
    impl_->next_seq = seq + 1;
    return support::AsyncResult<Seq>{support::Expected<Seq>{seq}};
}

support::Expected<std::int64_t> MemoryStorage::mint_id() {
    if (impl_->closed) {
        return std::unexpected(impl_->closed_error());
    }
    const std::int64_t id = impl_->next_id;
    impl_->next_id += 1;
    return id;
}

support::Expected<std::optional<ConversationRecord>> MemoryStorage::conversation(ConversationId id) {
    if (impl_->closed) {
        return std::unexpected(impl_->closed_error());
    }
    const auto found = impl_->conversations.find(id.value);
    if (found == impl_->conversations.end()) {
        return std::optional<ConversationRecord>{};
    }
    return std::optional<ConversationRecord>{found->second};
}

support::Expected<std::optional<StoredEntry>> MemoryStorage::entry(EntryId id) {
    if (impl_->closed) {
        return std::unexpected(impl_->closed_error());
    }
    const auto found = impl_->entries.find(id.value);
    if (found == impl_->entries.end()) {
        return std::optional<StoredEntry>{};
    }
    return std::optional<StoredEntry>{
            StoredEntry{.entry = found->second.record, .commit_seq = found->second.commit_seq}};
}

support::Expected<std::vector<ConversationRecord>> MemoryStorage::scan_conversations(std::size_t limit) {
    if (impl_->closed) {
        return std::unexpected(impl_->closed_error());
    }
    std::vector<ConversationRecord> records;
    for (const std::int64_t id : impl_->conversation_ids) {
        if (records.size() >= limit) {
            break;
        }
        const auto found = impl_->conversations.find(id);
        if (found != impl_->conversations.end()) {
            records.push_back(found->second);
        }
    }
    return records;
}

support::Expected<std::vector<EntryRecord>> MemoryStorage::scan_entries(
        ConversationId conversation_id, std::size_t limit) {
    if (impl_->closed) {
        return std::unexpected(impl_->closed_error());
    }
    std::vector<EntryRecord> records;
    const auto found = impl_->entry_ids_by_conversation.find(conversation_id.value);
    if (found == impl_->entry_ids_by_conversation.end()) {
        return records;
    }
    // The frozen backend scans entries newest first.
    for (auto index = found->second.size(); index > 0; --index) {
        if (records.size() >= limit) {
            break;
        }
        const auto entry = impl_->entries.find(found->second[index - 1]);
        if (entry != impl_->entries.end()) {
            records.push_back(entry->second.record);
        }
    }
    return records;
}

support::Expected<std::optional<DocumentRecord>> MemoryStorage::find_document(
        std::string_view kind, std::optional<ConversationId> conversation_id) {
    if (impl_->closed) {
        return std::unexpected(impl_->closed_error());
    }
    const auto found = impl_->document_id_by_address.find(document_address_key(kind, conversation_id));
    if (found == impl_->document_id_by_address.end()) {
        return std::optional<DocumentRecord>{};
    }
    const auto document = impl_->documents.find(found->second);
    if (document == impl_->documents.end()) {
        return std::optional<DocumentRecord>{};
    }
    return std::optional<DocumentRecord>{document->second.record};
}

support::Expected<std::optional<chord::Json>> MemoryStorage::document(DocumentId id) {
    if (impl_->closed) {
        return std::unexpected(impl_->closed_error());
    }
    const auto found = impl_->documents.find(id.value);
    if (found == impl_->documents.end()) {
        return std::optional<chord::Json>{};
    }
    return std::optional<chord::Json>{found->second.value};
}

support::ExpectedVoid MemoryStorage::close() {
    impl_->closed = true;
    return {};
}

} // namespace cch::durable
