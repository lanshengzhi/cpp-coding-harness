#include "StorageState.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
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

/// One committed document revision: a complete base or one delta after the
/// newest base (frozen pi `DocumentRevision`).
struct DocumentRevision {
    Seq seq{0};
    std::uint32_t version{1};
    bool base{true};
    chord::Json value{};
    chord::OpBatch ops{};
};

/// One stored document incarnation with its committed revisions.
struct StoredDocumentState {
    DocumentRecord record;
    /// The revisions in commit order; a current-only incarnation keeps at most
    /// the newest base.
    std::vector<DocumentRevision> revisions;
};

/// One stored entry with the sequence of the commit that persisted it.
struct StoredEntryState {
    EntryRecord record;
    Seq commit_seq{0};
};

/// Whether one incarnation is alive at one point (frozen pi `isAliveAt`):
/// `current` selects the unretired incarnation, an explicit commit selects the
/// one whose lifetime covers it.
[[nodiscard]] bool is_alive_at(const DocumentRecord& record, DocumentPoint at) {
    if (!at.at.has_value()) {
        return !record.retired_at.has_value();
    }
    return record.created_at <= *at.at && (!record.retired_at.has_value() || *at.at < *record.retired_at);
}

/// The materialized value of one incarnation at one point, replayed from the
/// newest base at or before that point (frozen pi
/// `MemoryStorage.materializeDocument`).
[[nodiscard]] support::Expected<std::optional<StoredDocument>> materialize(
        const StoredDocumentState& stored, DocumentPoint at) {
    if (at.at.has_value() && document_is_current_only(stored.record)) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation,
                "Document " + std::to_string(stored.record.id.value) + " does not retain historical content"));
    }
    if (!is_alive_at(stored.record, at)) {
        return std::optional<StoredDocument>{};
    }
    std::vector<const DocumentRevision*> revisions;
    revisions.reserve(stored.revisions.size());
    for (const DocumentRevision& revision : stored.revisions) {
        if (at.at.has_value() && revision.seq > *at.at) {
            break;
        }
        revisions.push_back(&revision);
    }
    std::size_t base_index = revisions.size();
    while (base_index > 0 && !revisions[base_index - 1]->base) {
        --base_index;
    }
    if (base_index == 0) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation,
                "Document " + std::to_string(stored.record.id.value) + " is missing a required base"));
    }
    const DocumentRevision& base = *revisions[base_index - 1];
    std::vector<chord::OpBatch> batches;
    batches.reserve(revisions.size() - base_index);
    for (std::size_t index = base_index; index < revisions.size(); ++index) {
        batches.push_back(revisions[index]->ops);
    }
    auto value = chord::apply_immutable_batches(base.value, batches);
    if (!value) {
        return std::unexpected(std::move(value.error()));
    }
    return std::optional<StoredDocument>{std::in_place,
            StoredDocument{.record = stored.record,
                    .version = base.version,
                    .value = std::move(*value),
                    .deltas_since_base = static_cast<std::int64_t>(batches.size())}};
}

} // namespace

struct StorageState::Impl {
    mutable std::recursive_mutex mutex;

    explicit Impl(std::string message) : closed_message(std::move(message)) {}

    Impl(const Impl& other)
        : conversations(other.conversations), conversation_ids(other.conversation_ids), entries(other.entries),
          entry_ids_by_conversation(other.entry_ids_by_conversation), documents(other.documents),
          document_ids_by_address(other.document_ids_by_address), tasks(other.tasks), task_ids(other.task_ids),
          submissions(other.submissions), submission_ids(other.submission_ids), next_id(other.next_id),
          next_seq(other.next_seq), closed_message(other.closed_message), closed(other.closed) {}

    std::map<std::int64_t, ConversationRecord> conversations;
    std::vector<std::int64_t> conversation_ids;
    std::map<std::int64_t, StoredEntryState> entries;
    std::map<std::int64_t, std::vector<std::int64_t>> entry_ids_by_conversation;
    std::map<std::int64_t, StoredDocumentState> documents;
    /// Every incarnation ever created at one address, oldest first; the last
    /// unretired one is the current incarnation.
    std::map<std::string, std::vector<std::int64_t>> document_ids_by_address;
    std::map<std::int64_t, TaskRecord> tasks;
    std::vector<std::int64_t> task_ids;
    std::map<std::int64_t, SubmissionRecord> submissions;
    std::vector<std::int64_t> submission_ids;
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
        std::map<std::int64_t, StoredDocumentState> next_documents = documents;
        std::map<std::string, std::vector<std::int64_t>> next_document_ids = document_ids_by_address;
        std::map<std::int64_t, TaskRecord> next_tasks = tasks;
        std::vector<std::int64_t> next_task_ids = task_ids;
        std::map<std::int64_t, SubmissionRecord> next_submissions = submissions;
        std::vector<std::int64_t> next_submission_ids = submission_ids;
        std::int64_t next_id = this->next_id;

        // An incarnation retired by this same batch frees its address for a
        // new incarnation created in the batch, so the address check must see
        // the batch's own retirements (frozen pi `checkDocumentActions`).
        std::vector<std::int64_t> retired_in_batch;
        for (const StorageWrite& write : detached) {
            if (write.type == "document.retire" && write.document_id.has_value()) {
                retired_in_batch.push_back(write.document_id->value);
            }
        }
        const auto retired_by_batch = [&retired_in_batch](std::int64_t id) {
            return std::ranges::find(retired_in_batch, id) != retired_in_batch.end();
        };

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
            } else if (write.type == "document.create" || write.type == "document.copy") {
                if (!write.document_record.has_value()) {
                    return std::unexpected(support::make_error(support::ErrorCode::Validation,
                            write.type == "document.copy" ? "Document copy write is incomplete"
                                                          : "Document create write is incomplete"));
                }
                DocumentRecord record = *write.document_record;
                if (next_documents.contains(record.id.value)) {
                    return std::unexpected(
                            support::make_error(support::ErrorCode::Validation, "Document already exists"));
                }
                const std::int64_t id = record.id.value;
                const std::string key = document_address_key(record.kind, record.address());
                // Only one unretired incarnation may occupy an address, and a
                // retirement in this batch frees it.
                for (const std::int64_t other : next_document_ids[key]) {
                    const auto stored = next_documents.find(other);
                    if (stored != next_documents.end() && !stored->second.record.retired_at.has_value() &&
                            !retired_by_batch(other)) {
                        return std::unexpected(support::make_error(
                                support::ErrorCode::Validation, "Document address already has a current incarnation"));
                    }
                }

                // A copy persists the source's exact base content at its
                // selected point, so the new incarnation starts with the
                // source revision and version (frozen pi `resolveDocumentCopies`).
                DocumentRevision base;
                if (write.type == "document.copy") {
                    if (!write.document_copy_source.has_value()) {
                        return std::unexpected(support::make_error(
                                support::ErrorCode::Validation, "Document copy write is incomplete"));
                    }
                    const DocumentCopySource& source = *write.document_copy_source;
                    const bool source_changed_in_batch =
                            std::ranges::any_of(detached, [&source](const StorageWrite& pending) {
                                if (pending.type == "document.create" || pending.type == "document.copy") {
                                    return pending.document_record.has_value() &&
                                           pending.document_record->id == source.id;
                                }
                                return (pending.type == "document.change" || pending.type == "document.retire") &&
                                       pending.document_id == source.id;
                            });
                    if (source_changed_in_batch) {
                        // The source is changed in the same batch: its persisted
                        // content is not the content being copied.
                        return std::unexpected(support::make_error(support::ErrorCode::Validation,
                                "Fork source document " + std::to_string(source.id.value) +
                                        " is changed in the copy batch"));
                    }
                    const auto source_stored = documents.find(source.id.value);
                    if (source_stored == documents.end()) {
                        return std::unexpected(support::make_error(support::ErrorCode::Validation,
                                "Fork source document " + std::to_string(source.id.value) + " cannot be read"));
                    }
                    auto materialized = materialize(source_stored->second, source.at);
                    if (!materialized) {
                        return std::unexpected(std::move(materialized.error()));
                    }
                    if (!(*materialized).has_value()) {
                        return std::unexpected(support::make_error(support::ErrorCode::Validation,
                                "Fork source document " + std::to_string(source.id.value) + " cannot be read"));
                    }
                    const DocumentRecord& source_record = source_stored->second.record;
                    if (source_record.scope() != DocumentScope::Conversation ||
                            record.scope() != DocumentScope::Conversation || source_record.kind != record.kind ||
                            source_record.key != record.key || source_record.history != record.history ||
                            source_record.fork != record.fork) {
                        return std::unexpected(support::make_error(support::ErrorCode::Validation,
                                "Fork source document " + std::to_string(source.id.value) +
                                        " does not match the copied record"));
                    }
                    base = DocumentRevision{.seq = seq,
                            .version = (*materialized)->version,
                            .base = true,
                            .value = (*materialized)->value};
                } else {
                    if (!write.document_value.has_value()) {
                        return std::unexpected(support::make_error(
                                support::ErrorCode::Validation, "Document create write is incomplete"));
                    }
                    base = DocumentRevision{.seq = seq,
                            .version = write.document_version,
                            .base = true,
                            .value = *write.document_value};
                }
                record.created_at = seq;
                next_documents.emplace(id, StoredDocumentState{.record = std::move(record), .revisions = {base}});
                next_document_ids[key].push_back(id);
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
                if (found->second.record.retired_at.has_value()) {
                    return std::unexpected(support::make_error(support::ErrorCode::Validation, "Document is retired"));
                }
                if (write.document_ops.empty()) {
                    if (!write.document_value.has_value()) {
                        return std::unexpected(support::make_error(
                                support::ErrorCode::Validation, "Document change write is incomplete"));
                    }
                    // A checkpoint base replaces the retained history of a
                    // current-only incarnation; a rewindable one keeps it so
                    // as-of reads still reach earlier revisions.
                    DocumentRevision revision{.seq = seq,
                            .version = write.document_version,
                            .base = true,
                            .value = *write.document_value};
                    if (document_is_current_only(found->second.record)) {
                        found->second.revisions.clear();
                    }
                    found->second.revisions.push_back(std::move(revision));
                } else {
                    // A delta must continue the newest stored revision's
                    // version, so a version transition is always a base.
                    if (found->second.revisions.empty()) {
                        return std::unexpected(
                                support::make_error(support::ErrorCode::Validation, "Document delta has no base"));
                    }
                    if (found->second.revisions.back().version != write.document_version) {
                        return std::unexpected(support::make_error(
                                support::ErrorCode::Validation, "Document version transition requires a base"));
                    }
                    auto applied = chord::apply_immutable(found->second.revisions.back().value, write.document_ops);
                    if (!applied) {
                        return std::unexpected(std::move(applied.error()));
                    }
                    found->second.revisions.push_back(DocumentRevision{.seq = seq,
                            .version = write.document_version,
                            .base = false,
                            .value = std::move(*applied),
                            .ops = write.document_ops});
                }
            } else if (write.type == "document.retire") {
                if (!write.document_id.has_value()) {
                    return std::unexpected(
                            support::make_error(support::ErrorCode::Validation, "Document retire write has no id"));
                }
                const auto found = next_documents.find(write.document_id->value);
                if (found == next_documents.end()) {
                    return std::unexpected(support::make_error(support::ErrorCode::Validation, "Unknown document"));
                }
                if (found->second.record.retired_at.has_value()) {
                    return std::unexpected(support::make_error(support::ErrorCode::Validation, "Document is retired"));
                }
                found->second.record.retired_at = seq;
                // A current-only incarnation keeps no history, so retirement
                // drops its revisions; a rewindable one keeps them addressable.
                if (document_is_current_only(found->second.record)) {
                    found->second.revisions.clear();
                }
            } else if (write.type == "task") {
                if (!write.task.has_value()) {
                    return std::unexpected(
                            support::make_error(support::ErrorCode::Validation, "Task write has no record"));
                }
                const TaskRecord& record = *write.task;
                const auto existing = next_tasks.find(record.id.value);
                if (existing != next_tasks.end()) {
                    if (existing->second.state.status == "terminal") {
                        return std::unexpected(
                                support::make_error(support::ErrorCode::Validation, "Task is already terminal"));
                    }
                    if (existing->second.conversation_id != record.conversation_id) {
                        return std::unexpected(support::make_error(
                                support::ErrorCode::Validation, "Task cannot change conversations"));
                    }
                    existing->second = record;
                } else {
                    next_tasks.emplace(record.id.value, record);
                    next_task_ids.push_back(record.id.value);
                }
                next_id = std::max(next_id, record.id.value + 1);
            } else if (write.type == "submission") {
                if (!write.submission.has_value()) {
                    return std::unexpected(
                            support::make_error(support::ErrorCode::Validation, "Submission write has no record"));
                }
                const SubmissionRecord& record = *write.submission;
                // Every submission write replaces the record (frozen pi
                // `MemoryStorage` submission case).
                if (!next_submissions.contains(record.id.value)) {
                    next_submission_ids.push_back(record.id.value);
                }
                next_submissions.insert_or_assign(record.id.value, record);
                next_id = std::max(next_id, record.id.value + 1);
            } else {
                return std::unexpected(support::make_error(support::ErrorCode::Validation, "Unknown storage write"));
            }
        }

        conversations = std::move(next_conversations);
        conversation_ids = std::move(next_conversation_ids);
        entries = std::move(next_entries);
        entry_ids_by_conversation = std::move(next_entry_ids);
        documents = std::move(next_documents);
        document_ids_by_address = std::move(next_document_ids);
        tasks = std::move(next_tasks);
        task_ids = std::move(next_task_ids);
        submissions = std::move(next_submissions);
        submission_ids = std::move(next_submission_ids);
        this->next_id = next_id;
        next_seq = std::max(next_seq, seq + 1);
        return consume_seq ? seq : next_seq;
    }
};

StorageState::StorageState(std::string closed_message) : impl_(std::make_unique<Impl>(std::move(closed_message))) {}

StorageState::~StorageState() = default;

support::Expected<Seq> StorageState::apply(std::span<const StorageWrite> writes, Seq seq) {
    std::lock_guard lock(impl_->mutex);
    return impl_->apply(writes, seq, true);
}

support::Expected<Seq> StorageState::apply_next(std::span<const StorageWrite> writes) {
    std::lock_guard lock(impl_->mutex);
    return impl_->apply(writes, impl_->next_seq, true);
}

support::ExpectedVoid StorageState::validate(std::span<const StorageWrite> writes, Seq seq) {
    std::lock_guard lock(impl_->mutex);
    Impl trial(*impl_);
    auto applied = trial.apply(writes, seq, true);
    if (!applied) {
        return std::unexpected(std::move(applied.error()));
    }
    return {};
}

support::ExpectedVoid StorageState::restore(std::span<const StorageWrite> writes, Seq seq) {
    std::lock_guard lock(impl_->mutex);
    auto applied = impl_->apply(writes, seq, false);
    if (!applied) {
        return std::unexpected(std::move(applied.error()));
    }
    return {};
}

std::int64_t StorageState::reserve_id() {
    std::lock_guard lock(impl_->mutex);
    const std::int64_t id = impl_->next_id;
    impl_->next_id += 1;
    return id;
}

Seq StorageState::next_seq() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->next_seq;
}

support::ExpectedVoid StorageState::restore_high_water_marks(std::int64_t next_id, Seq next_seq) {
    std::lock_guard lock(impl_->mutex);
    if (impl_->closed) {
        return std::unexpected(impl_->closed_error());
    }
    impl_->next_id = std::max(impl_->next_id, next_id);
    impl_->next_seq = std::max(impl_->next_seq, next_seq);
    return {};
}

support::Expected<std::optional<ConversationRecord>> StorageState::conversation(ConversationId id) {
    std::lock_guard lock(impl_->mutex);
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
    std::lock_guard lock(impl_->mutex);
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
    std::lock_guard lock(impl_->mutex);
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
    std::lock_guard lock(impl_->mutex);
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
        std::string_view kind, const DocumentAddress& address, DocumentPoint at) {
    std::lock_guard lock(impl_->mutex);
    if (impl_->closed) {
        return std::unexpected(impl_->closed_error());
    }
    const auto found = impl_->document_ids_by_address.find(document_address_key(kind, address));
    if (found == impl_->document_ids_by_address.end()) {
        return std::optional<DocumentRecord>{};
    }
    // The IDs are in creation order, so the last alive incarnation at the
    // point is the one the address selected then (frozen pi
    // `MemoryStorage.findDocument`).
    for (auto index = found->second.size(); index > 0; --index) {
        const auto document = impl_->documents.find(found->second[index - 1]);
        if (document == impl_->documents.end()) {
            continue;
        }
        if (is_alive_at(document->second.record, at)) {
            return std::optional<DocumentRecord>{document->second.record};
        }
    }
    return std::optional<DocumentRecord>{};
}

support::Expected<std::optional<StoredDocument>> StorageState::document(DocumentId id, DocumentPoint at) {
    std::lock_guard lock(impl_->mutex);
    if (impl_->closed) {
        return std::unexpected(impl_->closed_error());
    }
    const auto found = impl_->documents.find(id.value);
    if (found == impl_->documents.end()) {
        return std::optional<StoredDocument>{};
    }
    return materialize(found->second, at);
}

support::Expected<std::optional<TaskRecord>> StorageState::task(TaskId id) {
    std::lock_guard lock(impl_->mutex);
    if (impl_->closed) {
        return std::unexpected(impl_->closed_error());
    }
    const auto found = impl_->tasks.find(id.value);
    if (found == impl_->tasks.end()) {
        return std::optional<TaskRecord>{};
    }
    return std::optional<TaskRecord>{found->second};
}

support::Expected<std::vector<TaskRecord>> StorageState::scan_tasks(std::size_t limit) {
    std::lock_guard lock(impl_->mutex);
    if (impl_->closed) {
        return std::unexpected(impl_->closed_error());
    }
    std::vector<TaskRecord> records;
    records.reserve(std::min(limit, impl_->tasks.size()));
    for (const auto& [id, record] : impl_->tasks) {
        if (records.size() >= limit) {
            break;
        }
        records.push_back(record);
    }
    return records;
}

support::Expected<std::optional<SubmissionRecord>> StorageState::submission(SubmissionId id) {
    std::lock_guard lock(impl_->mutex);
    if (impl_->closed) {
        return std::unexpected(impl_->closed_error());
    }
    const auto found = impl_->submissions.find(id.value);
    if (found == impl_->submissions.end()) {
        return std::optional<SubmissionRecord>{};
    }
    return std::optional<SubmissionRecord>{found->second};
}

support::Expected<std::optional<SubmissionRecord>> StorageState::submission_by_request(
        ConversationId conversation_id, std::string_view request_id) {
    std::lock_guard lock(impl_->mutex);
    if (impl_->closed) {
        return std::unexpected(impl_->closed_error());
    }
    // The map iterates in ascending ID order, so the last match is the newest
    // holder of one conversation-scoped key (frozen pi
    // `MemoryStorage.submissionIdsByRequest`).
    std::optional<SubmissionRecord> latest;
    for (const auto& [id, record] : impl_->submissions) {
        if (record.conversation_id == conversation_id && record.request_id.has_value() &&
                *record.request_id == request_id) {
            latest = record;
        }
    }
    return latest;
}

support::Expected<std::vector<SubmissionRecord>> StorageState::scan_submissions(std::size_t limit) {
    std::lock_guard lock(impl_->mutex);
    if (impl_->closed) {
        return std::unexpected(impl_->closed_error());
    }
    std::vector<SubmissionRecord> records;
    records.reserve(std::min(limit, impl_->submissions.size()));
    for (const auto& [id, record] : impl_->submissions) {
        if (records.size() >= limit) {
            break;
        }
        records.push_back(record);
    }
    return records;
}

bool StorageState::closed() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->closed;
}

void StorageState::close() {
    std::lock_guard lock(impl_->mutex);
    impl_->closed = true;
}

} // namespace cch::durable
