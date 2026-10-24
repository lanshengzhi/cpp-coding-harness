#include "StorageState.hpp"

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
    std::optional<ConversationId> conversation_id;
    chord::Json value;
    std::optional<Seq> retired_at;
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

struct StorageState::Impl {
    explicit Impl(std::string message) : closed_message(std::move(message)) {}

    std::map<std::int64_t, ConversationRecord> conversations;
    std::vector<std::int64_t> conversation_ids;
    std::map<std::int64_t, StoredEntryState> entries;
    std::map<std::int64_t, std::vector<std::int64_t>> entry_ids_by_conversation;
    std::map<std::int64_t, StoredDocument> documents;
    std::map<std::string, std::int64_t> document_id_by_address;
    std::int64_t next_id{kFirstId};
    Seq next_seq{kFirstSeq};
    std::string closed_message;
    bool closed{false};

    [[nodiscard]] support::Error closed_error() const {
        return support::make_error(support::ErrorCode::Session, closed_message);
    }

    [[nodiscard]] support::Expected<Seq> apply(std::span<const StorageWrite> writes, Seq seq, bool consume_seq) {
        if (closed) {
            return std::unexpected(closed_error());
        }
        if (writes.empty()) {
            next_seq = std::max(next_seq, seq + 1);
            return consume_seq ? seq : next_seq;
        }

        // The frozen backend validates and detaches the batch before it changes
        // the observable state, so a rejected batch consumes no sequence and
        // leaves every record untouched.
        std::vector<StorageWrite> detached(writes.begin(), writes.end());
        std::map<std::int64_t, ConversationRecord> next_conversations = conversations;
        std::vector<std::int64_t> next_conversation_ids = conversation_ids;
        std::map<std::int64_t, StoredEntryState> next_entries = entries;
        std::map<std::int64_t, std::vector<std::int64_t>> next_entry_ids = entry_ids_by_conversation;
        std::map<std::int64_t, StoredDocument> next_documents = documents;
        std::map<std::string, std::int64_t> next_document_ids = document_id_by_address;
        std::int64_t next_id = this->next_id;

        for (const StorageWrite& write : detached) {
            if (write.type == "conversation") {
                if (!write.conversation.has_value()) {
                    return std::unexpected(
                            support::make_error(support::ErrorCode::Validation, "Conversation write has no record"));
                }
                const ConversationRecord& record = *write.conversation;
                if (next_conversations.contains(record.id.value)) {
                    return std::unexpected(
                            support::make_error(support::ErrorCode::Validation, "Conversation already exists"));
                }
                next_conversations.emplace(record.id.value, record);
                next_conversation_ids.push_back(record.id.value);
                next_id = std::max(next_id, record.id.value + 1);
            } else if (write.type == "entry") {
                if (!write.entry.has_value()) {
                    return std::unexpected(
                            support::make_error(support::ErrorCode::Validation, "Entry write has no record"));
                }
                const EntryRecord& record = *write.entry;
                if (next_entries.contains(record.id.value)) {
                    return std::unexpected(support::make_error(support::ErrorCode::Validation, "Entry already exists"));
                }
                if (!next_conversations.contains(record.conversation_id.value)) {
                    return std::unexpected(support::make_error(support::ErrorCode::Validation, "Unknown conversation"));
                }
                next_entries.emplace(record.id.value, StoredEntryState{.record = record, .commit_seq = seq});
                next_entry_ids[record.conversation_id.value].push_back(record.id.value);
                next_id = std::max(next_id, record.id.value + 1);
            } else if (write.type == "document.create") {
                if (!write.document_record.has_value() || !write.document_value.has_value()) {
                    return std::unexpected(
                            support::make_error(support::ErrorCode::Validation, "Document create write is incomplete"));
                }
                DocumentRecord record = *write.document_record;
                if (next_documents.contains(record.id.value)) {
                    return std::unexpected(
                            support::make_error(support::ErrorCode::Validation, "Document already exists"));
                }
                record.created_at = seq;
                const std::int64_t id = record.id.value;
                const std::string address_key = document_address_key(record.kind, write.document_conversation_id);
                if (next_document_ids.contains(address_key)) {
                    return std::unexpected(support::make_error(
                            support::ErrorCode::Validation, "Document address already has a current incarnation"));
                }
                next_documents.emplace(id,
                        StoredDocument{.record = std::move(record),
                                .conversation_id = write.document_conversation_id,
                                .value = *write.document_value,
                                .retired_at = std::nullopt});
                next_document_ids[address_key] = id;
                next_id = std::max(next_id, id + 1);
            } else if (write.type == "document.change") {
                if (!write.document_id.has_value()) {
                    return std::unexpected(
                            support::make_error(support::ErrorCode::Validation, "Document change write has no id"));
                }
                const auto found = next_documents.find(write.document_id->value);
                if (found == next_documents.end()) {
                    return std::unexpected(support::make_error(support::ErrorCode::Validation, "Unknown document"));
                }
                if (!write.document_value.has_value()) {
                    return std::unexpected(
                            support::make_error(support::ErrorCode::Validation, "Document change write is incomplete"));
                }
                found->second.value = *write.document_value;
            } else if (write.type == "document.retire") {
                if (!write.document_id.has_value()) {
                    return std::unexpected(
                            support::make_error(support::ErrorCode::Validation, "Document retire write has no id"));
                }
                const auto found = next_documents.find(write.document_id->value);
                if (found == next_documents.end()) {
                    return std::unexpected(support::make_error(support::ErrorCode::Validation, "Unknown document"));
                }
                found->second.retired_at = seq;
                const std::string address =
                        document_address_key(found->second.record.kind, found->second.conversation_id);
                const auto current = next_document_ids.find(address);
                if (current != next_document_ids.end() && current->second == write.document_id->value) {
                    next_document_ids.erase(current);
                }
            } else {
                return std::unexpected(support::make_error(support::ErrorCode::Validation, "Unknown storage write"));
            }
        }

        conversations = std::move(next_conversations);
        conversation_ids = std::move(next_conversation_ids);
        entries = std::move(next_entries);
        entry_ids_by_conversation = std::move(next_entry_ids);
        documents = std::move(next_documents);
        document_id_by_address = std::move(next_document_ids);
        this->next_id = next_id;
        next_seq = std::max(next_seq, seq + 1);
        return consume_seq ? seq : next_seq;
    }
};

StorageState::StorageState(std::string closed_message) : impl_(std::make_unique<Impl>(std::move(closed_message))) {}

StorageState::~StorageState() = default;

support::Expected<Seq> StorageState::apply(std::span<const StorageWrite> writes, Seq seq) {
    return impl_->apply(writes, seq, true);
}

support::ExpectedVoid StorageState::validate(std::span<const StorageWrite> writes, Seq seq) {
    Impl trial(*impl_);
    auto applied = trial.apply(writes, seq, true);
    if (!applied) {
        return std::unexpected(std::move(applied.error()));
    }
    return {};
}

support::ExpectedVoid StorageState::restore(std::span<const StorageWrite> writes, Seq seq) {
    auto applied = impl_->apply(writes, seq, false);
    if (!applied) {
        return std::unexpected(std::move(applied.error()));
    }
    return {};
}

std::int64_t StorageState::reserve_id() {
    const std::int64_t id = impl_->next_id;
    impl_->next_id += 1;
    return id;
}

Seq StorageState::next_seq() const { return impl_->next_seq; }

support::ExpectedVoid StorageState::restore_high_water_marks(std::int64_t next_id, Seq next_seq) {
    if (impl_->closed) {
        return std::unexpected(impl_->closed_error());
    }
    impl_->next_id = std::max(impl_->next_id, next_id);
    impl_->next_seq = std::max(impl_->next_seq, next_seq);
    return {};
}

support::Expected<std::optional<ConversationRecord>> StorageState::conversation(ConversationId id) {
    if (impl_->closed) {
        return std::unexpected(impl_->closed_error());
    }
    const auto found = impl_->conversations.find(id.value);
    if (found == impl_->conversations.end()) {
        return std::optional<ConversationRecord>{};
    }
    return std::optional<ConversationRecord>{found->second};
}

support::Expected<std::optional<StoredEntry>> StorageState::entry(EntryId id) {
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

support::Expected<std::vector<ConversationRecord>> StorageState::scan_conversations(std::size_t limit) {
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

support::Expected<std::vector<EntryRecord>> StorageState::scan_entries(
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

support::Expected<std::optional<DocumentRecord>> StorageState::find_document(
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

support::Expected<std::optional<chord::Json>> StorageState::document(DocumentId id) {
    if (impl_->closed) {
        return std::unexpected(impl_->closed_error());
    }
    const auto found = impl_->documents.find(id.value);
    if (found == impl_->documents.end()) {
        return std::optional<chord::Json>{};
    }
    return std::optional<chord::Json>{found->second.value};
}

bool StorageState::closed() const { return impl_->closed; }

void StorageState::close() { impl_->closed = true; }

} // namespace cch::durable
