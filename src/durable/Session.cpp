#include <cch/durable/Session.hpp>

#include <cch/durable/Storage.hpp>
#include <cch/durable/Task.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace cch::durable {

namespace {

/// The frozen message of a table read after the first table write (frozen pi
/// `ReadAfterWrite`).
[[nodiscard]] support::Error read_after_write_error(std::string_view method) {
    return support::make_error(support::ErrorCode::Validation,
            std::string{"Tx."} + std::string{method} + "() cannot read tables after the first table write");
}

/// The frozen message of a rejected operation on a closed Session (frozen pi
/// `SessionImpl.assertUsable`).
[[nodiscard]] support::Error session_closed_error() {
    return support::make_error(support::ErrorCode::Session, "Session is closed");
}

/// The frozen message of a Session that failed (frozen pi `SessionFailed`).
[[nodiscard]] support::Error session_failed_error() {
    return support::make_error(
            support::ErrorCode::Session, "Session failed after a storage error; close and reopen it");
}

/// The address key of one document: a Session document and a conversation
/// document are different documents.
[[nodiscard]] std::string address_key(std::string_view kind, std::optional<ConversationId> conversation_id) {
    std::string key{kind};
    key += '|';
    if (conversation_id.has_value()) {
        key += std::to_string(conversation_id->value);
    } else {
        key += "session";
    }
    return key;
}

/// One staged document incarnation: its record, its address, its Chord tracker
/// holding the candidate revision, and the open change the draft came from.
struct StagedDocument {
    DocumentRecord record{};
    DocumentAddress address{};
    std::string kind{};
    chord::Tracker tracker;
    chord::Change change;
    bool created{false};
};

/// One staged submission change: a settlement, or the placement of a queued
/// submission at its transcript entry (frozen pi `SubmissionChange`).
struct SubmissionChange {
    SubmissionId id{};
    SubmissionSettlement settlement{};
    /// The placement entry; absent for a settlement.
    std::optional<EntryId> placed_entry{std::nullopt};
};

/// One committed document incarnation the Session knows, by address key.
struct CachedDocument {
    DocumentRecord record{};
    chord::Json value{};
};

} // namespace

// ── DocumentDefinition ──────────────────────────────────────────────────────

struct DocumentDefinition::Impl {
    std::string kind;
    std::move_only_function<chord::Json()> initial;
};

DocumentDefinition::DocumentDefinition(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

DocumentDefinition::DocumentDefinition(DocumentDefinition&&) noexcept = default;

DocumentDefinition& DocumentDefinition::operator=(DocumentDefinition&&) noexcept = default;

DocumentDefinition::~DocumentDefinition() = default;

const std::string& DocumentDefinition::kind() const noexcept { return impl_->kind; }

chord::Json DocumentDefinition::initial_value() const { return impl_->initial(); }

DocumentDefinition define_document(std::string kind, std::move_only_function<chord::Json()> initial) {
    auto impl = std::make_unique<DocumentDefinition::Impl>();
    impl->kind = std::move(kind);
    impl->initial = std::move(initial);
    return DocumentDefinition(std::move(impl));
}

// ── DocumentWatch ───────────────────────────────────────────────────────────

struct DocumentWatch::Impl {
    chord::Json value{};
    bool started{false};
    bool stopped{false};
    WatchEndReason end{WatchEndReason::Stopped};
    DocumentFrameListener listener{};
    /// The frames committed since the acquisition, in commit order.
    std::vector<DocumentFrame> pending{};
};

// ── Session ─────────────────────────────────────────────────────────────────

struct Session::Impl {
    struct PendingCommit {
        std::move_only_function<support::Expected<void>(Transaction&)> change{};
        support::AsyncCompletion<void, support::Error> completion{};
    };

    /// One admitted commit whose batch the storage has not settled yet. The
    /// prepared documents stay alive until the settlement, and the committed
    /// values become observable only after it.
    struct CommitState {
        Session::Impl* session{nullptr};
        Storage* storage{nullptr};
        std::vector<StorageWrite> writes{};
        std::vector<StagedDocument> documents{};
        std::vector<chord::Prepared> prepared{};

        /// Settle the admitted commit: adopt the prepared revisions, publish
        /// the commit, and deliver its document frames. A storage failure
        /// fails the Session and publishes nothing.
        [[nodiscard]] support::ExpectedVoid settle(std::expected<Seq, support::Error> outcome) noexcept {
            if (!outcome) {
                // The call that reached the storage failure reports the
                // STORAGE's own error; every later call reports the frozen
                // Session failure message.
                support::Error storage_error = std::move(outcome.error());
                session->fail(storage_error);
                return std::unexpected(std::move(storage_error));
            }
            const Seq seq = *outcome;

            // The adoption happens after the storage settled the batch: the
            // committed revision is the adopted one.
            std::vector<DocumentCommitChange> document_changes;
            document_changes.reserve(documents.size());
            for (std::size_t index = 0; index < documents.size(); ++index) {
                StagedDocument& document = documents[index];
                const chord::Prepared& prepared = this->prepared[index];
                auto adopted = document.tracker.adopt(prepared);
                if (!adopted) {
                    // The storage already committed; a failed adoption leaves
                    // memory behind durable state, so the Session fails.
                    session->fail(std::move(adopted.error()));
                    return std::unexpected(session->failure_error());
                }
                DocumentRecord record = document.record;
                if (document.created) {
                    record.created_at = seq;
                }
                document_changes.push_back(DocumentCommitChange{.record = record,
                        .conversation_id = document.address.conversation_id,
                        .value = prepared.value(),
                        .ops = document.created ? chord::OpBatch{} : prepared.ops()});
            }

            // The committed document incarnations become observable now.
            for (std::size_t index = 0; index < documents.size(); ++index) {
                const StagedDocument& document = documents[index];
                const std::string key = address_key(document.kind, document.address.conversation_id);
                session->cached_documents[key] = CachedDocument{
                        .record = document_changes[index].record, .value = document_changes[index].value};
            }

            // The publication carries every committed change.
            CommitPublication publication{.seq = seq, .changes = {}};
            for (const StorageWrite& write : writes) {
                if (write.type == "conversation" && write.conversation.has_value()) {
                    publication.changes.push_back(TableCommitChange{.type = "conversation",
                            .conversation_id = write.conversation->id,
                            .entry_id = std::nullopt});
                } else if (write.type == "entry" && write.entry.has_value()) {
                    publication.changes.push_back(TableCommitChange{.type = "entry",
                            .conversation_id = write.entry->conversation_id,
                            .entry_id = write.entry->id});
                } else if (write.type == "task" && write.task.has_value()) {
                    publication.changes.push_back(TableCommitChange{.type = "task",
                            .conversation_id = write.task->conversation_id,
                            .entry_id = std::nullopt,
                            .task_id = write.task->id,
                            .task = write.task});
                } else if (write.type == "submission" && write.submission.has_value()) {
                    publication.changes.push_back(
                            SubmissionCommitChange{.type = "submission", .value = *write.submission});
                }
            }
            for (const DocumentCommitChange& change : document_changes) {
                publication.changes.push_back(change);
            }

            // The committed document frames reach every watch of that
            // incarnation.
            for (const DocumentCommitChange& change : document_changes) {
                const std::string key = address_key(change.record.kind, change.conversation_id);
                const auto registered = session->watches.find(key);
                if (registered == session->watches.end()) {
                    continue;
                }
                for (const std::shared_ptr<DocumentWatch::Impl>& watch : registered->second) {
                    if (watch->stopped) {
                        continue;
                    }
                    watch->pending.push_back(DocumentFrame{.value = change.value, .ops = change.ops});
                    if (!watch->started) {
                        // A watch that is not started yet buffers its frames.
                        continue;
                    }
                    std::vector<DocumentFrame> pending = std::move(watch->pending);
                    watch->pending.clear();
                    for (DocumentFrame& frame : pending) {
                        watch->value = frame.value;
                        watch->listener(frame);
                    }
                }
            }

            for (CommitListener& listener : session->commit_listeners) {
                listener(publication);
            }
            return {};
        }
    };

    /// The caller keeps Storage alive through the Session and its operations.
    Storage* storage{nullptr};
    bool closed{false};
    bool close_notified{false};
    bool storage_closed{false};
    bool failed{false};
    bool commit_active{false};
    bool pumping_commits{false};
    bool close_finishing{false};
    bool pump_requested{false};
    std::optional<std::string> failure_message{};
    std::deque<PendingCommit> pending_commits{};
    std::vector<support::AsyncCompletion<void, support::Error>> close_completions{};
    std::vector<CommitListener> commit_listeners{};
    std::vector<CloseListener> close_listeners{};
    /// The committed document incarnations this Session knows, by address key.
    std::map<std::string, CachedDocument> cached_documents{};
    /// The watches bound to a committed incarnation, by address key.
    std::map<std::string, std::vector<std::shared_ptr<DocumentWatch::Impl>>> watches{};

    /// The error every later operation reports after the Session failed.
    [[nodiscard]] support::Error failure_error() const { return session_failed_error(); }

    /// Fail the Session: the failure is final, the close listeners run, and
    /// the queued commits are rejected in admission order once the committing
    /// callback has settled.
    void fail(support::Error error) {
        if (failed) {
            return;
        }
        failed = true;
        failure_message = error.message;
        closed = true;
        notify_close();
    }

    /// Reject every commit still queued when the Session failed.
    void reject_pending_commits(const support::Error& error) {
        while (!pending_commits.empty()) {
            PendingCommit pending = std::move(pending_commits.front());
            pending_commits.pop_front();
            pending.completion(std::unexpected(error));
        }
    }

    /// Prepare and commit one callback while holding the mutation line.
    [[nodiscard]] support::AsyncResult<void> commit_now(
            std::move_only_function<support::Expected<void>(Transaction&)> change);

    /// Close Storage after every admitted commit settles.
    void finish_close_if_ready() {
        if (!closed || close_finishing || pumping_commits || commit_active || !pending_commits.empty()) {
            return;
        }
        close_finishing = true;
        close_storage();
        std::vector<support::AsyncCompletion<void, support::Error>> completions = std::move(close_completions);
        close_completions.clear();
        for (support::AsyncCompletion<void, support::Error>& completion : completions) {
            completion(support::ExpectedVoid{});
        }
    }

    /// Run queued transactions in FIFO order, without recursing when Storage
    /// settles synchronously.
    void run_next_commit() {
        if (pumping_commits) {
            pump_requested = true;
            return;
        }
        pumping_commits = true;
        do {
            pump_requested = false;
            if (commit_active || pending_commits.empty()) {
                break;
            }
            if (failed) {
                // Reject in admission order, after the failing commit's own
                // completion was delivered.
                reject_pending_commits(failure_error());
                break;
            }
            commit_active = true;
            PendingCommit pending = std::move(pending_commits.front());
            pending_commits.pop_front();
            std::move(commit_now(std::move(pending.change)))
                    .start([this, completion = std::move(pending.completion)](
                                   std::expected<void, support::Error> outcome) mutable noexcept {
                        completion(std::move(outcome));
                        commit_active = false;
                        pump_requested = true;
                        if (!pumping_commits) {
                            run_next_commit();
                        }
                        finish_close_if_ready();
                    });
        } while (pump_requested && !commit_active);
        pumping_commits = false;
        if (failed) {
            reject_pending_commits(failure_error());
        }
        finish_close_if_ready();
    }

    /// Admit one transaction to the serialized Session line.
    void enqueue_commit(PendingCommit pending) {
        pending_commits.push_back(std::move(pending));
        run_next_commit();
    }

    /// Run the close listeners once.
    void notify_close() {
        if (close_notified) {
            return;
        }
        close_notified = true;
        std::vector<CloseListener> listeners = std::move(close_listeners);
        close_listeners.clear();
        for (CloseListener& listener : listeners) {
            listener();
        }
    }

    /// Close the storage backend exactly once.
    void close_storage() {
        if (storage_closed) {
            return;
        }
        storage_closed = true;
        (void)storage->close();
    }

    /// The committed value of one incarnation, from the cache or the storage.
    [[nodiscard]] support::Expected<std::optional<chord::Json>> committed_value(
            const DocumentDefinition& definition, const DocumentAddress& address) {
        const std::string key = address_key(definition.kind(), address.conversation_id);
        if (const auto cached = cached_documents.find(key); cached != cached_documents.end()) {
            return std::optional<chord::Json>{cached->second.value};
        }
        auto found = storage->find_document(definition.kind(), address.conversation_id);
        if (!found) {
            return std::unexpected(std::move(found.error()));
        }
        if (!(*found).has_value()) {
            return std::optional<chord::Json>{};
        }
        const DocumentRecord& record = **found;
        auto stored = storage->document(record.id);
        if (!stored) {
            return std::unexpected(std::move(stored.error()));
        }
        if (!(*stored).has_value()) {
            return std::optional<chord::Json>{};
        }
        cached_documents[key] = CachedDocument{.record = record, .value = *(*stored)};
        return std::optional<chord::Json>{std::in_place, *(*stored)};
    }
};

// ── Transaction ─────────────────────────────────────────────────────────────

struct Transaction::Impl {
    /// The caller keeps Session and Storage alive through transaction settlement.
    Session::Impl* session{nullptr};
    Storage* storage{nullptr};
    bool has_table_write{false};
    bool sealed{false};
    std::vector<StorageWrite> writes{};
    std::vector<StagedDocument> documents{};
    /// The submission candidates this transaction created or replaced so far,
    /// by ID (frozen pi `Tx.#submissions`).
    std::map<std::int64_t, SubmissionRecord> staged_submissions{};
    /// The staged submission changes resolved at assembly (frozen pi
    /// `Tx.#submissionChanges`).
    std::vector<SubmissionChange> submission_changes{};

    [[nodiscard]] support::ExpectedVoid reject_read(std::string_view method) const {
        if (sealed) {
            return std::unexpected(support::make_error(support::ErrorCode::Validation, "Transaction has settled"));
        }
        if (has_table_write) {
            return std::unexpected(read_after_write_error(method));
        }
        return {};
    }

    [[nodiscard]] support::ExpectedVoid reject_write() const {
        if (sealed) {
            return std::unexpected(support::make_error(support::ErrorCode::Validation, "Transaction has settled"));
        }
        return {};
    }

    /// Whether one conversation is visible to this transaction: one it staged
    /// or one committed state holds (frozen pi `Tx.#requireConversation`).
    [[nodiscard]] bool has_conversation(ConversationId id) const {
        for (const StorageWrite& write : writes) {
            if (write.type == "conversation" && write.conversation.has_value() && write.conversation->id == id) {
                return true;
            }
        }
        return false;
    }
};

/// Apply one staged change to the current candidate record (frozen pi
/// `applySubmissionChange`). Placement turns a queued input placed and a
/// queued write done; only a placed input can be answered; a terminal record
/// stays unchanged.
[[nodiscard]] support::Expected<SubmissionRecord> apply_submission_change(
        const SubmissionRecord& current, const SubmissionChange& change) {
    if (current.status == "done" || current.status == "unanswered") {
        return current;
    }
    if (change.placed_entry.has_value()) {
        if (current.status != "queued") {
            return std::unexpected(support::make_error(support::ErrorCode::Validation,
                    std::string{"Submission "} + std::to_string(current.id.value) + " is not queued"));
        }
        SubmissionRecord placed = current;
        placed.status = current.type == "input" ? "placed" : "done";
        placed.entry = *change.placed_entry;
        return placed;
    }
    if (change.settlement.status == "done" && current.status != "placed") {
        return std::unexpected(support::make_error(support::ErrorCode::Validation,
                std::string{"Submission "} + std::to_string(current.id.value) + " is not a placed input"));
    }
    // Queued and placed records carry no answer, reason, or detail; an
    // unanswered input keeps its entry.
    SubmissionRecord settled = current;
    settled.status = change.settlement.status;
    settled.answer = change.settlement.answer;
    settled.reason = change.settlement.reason;
    settled.detail = change.settlement.detail;
    return settled;
}

Transaction::Transaction(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Transaction::~Transaction() = default;

support::Expected<ConversationRecord> Transaction::create_root_conversation() {
    if (auto rejected = impl_->reject_write(); !rejected) {
        return std::unexpected(std::move(rejected.error()));
    }
    ConversationRecord record{.id = ConversationId{.value = 1}};
    impl_->has_table_write = true;
    impl_->writes.push_back(StorageWrite{.type = "conversation", .conversation = record});
    return record;
}

support::Expected<ConversationRecord> Transaction::create_conversation() {
    if (auto rejected = impl_->reject_write(); !rejected) {
        return std::unexpected(std::move(rejected.error()));
    }
    auto id = impl_->storage->mint_id();
    if (!id) {
        return std::unexpected(std::move(id.error()));
    }
    ConversationRecord record{.id = ConversationId{.value = *id}};
    impl_->has_table_write = true;
    impl_->writes.push_back(StorageWrite{.type = "conversation", .conversation = record});
    return record;
}

support::Expected<EntryRecord> Transaction::append_entry(ConversationId conversation_id,
        std::string kind,
        chord::Json data,
        std::optional<EntryHeadVariant> head,
        std::vector<chord::Json> model,
        std::optional<TaskId> by_task_id) {
    if (auto rejected = impl_->reject_write(); !rejected) {
        return std::unexpected(std::move(rejected.error()));
    }
    auto id = impl_->storage->mint_id();
    if (!id) {
        return std::unexpected(std::move(id.error()));
    }
    std::optional<EntryId> resolved_head;
    if (head.has_value()) {
        if (const EntryId* existing_head = std::get_if<EntryId>(&*head)) {
            resolved_head = *existing_head;
        } else {
            resolved_head = EntryId{.value = *id};
        }
    }
    EntryRecord record{.id = EntryId{.value = *id},
            .conversation_id = conversation_id,
            .kind = std::move(kind),
            .data = std::move(data),
            .head = resolved_head,
            .model = std::move(model),
            .by_task_id = by_task_id};
    impl_->has_table_write = true;
    impl_->writes.push_back(StorageWrite{.type = "entry", .entry = record});
    return record;
}

support::Expected<std::optional<ConversationRecord>> Transaction::conversation(ConversationId id) {
    if (auto rejected = impl_->reject_read("conversation"); !rejected) {
        return std::unexpected(std::move(rejected.error()));
    }
    return impl_->storage->conversation(id);
}

support::Expected<std::optional<EntryRecord>> Transaction::entry(EntryId id) {
    if (auto rejected = impl_->reject_read("entry"); !rejected) {
        return std::unexpected(std::move(rejected.error()));
    }
    auto stored = impl_->storage->entry(id);
    if (!stored) {
        return std::unexpected(std::move(stored.error()));
    }
    if (!(*stored).has_value()) {
        return std::optional<EntryRecord>{};
    }
    return std::optional<EntryRecord>{std::in_place, (*stored)->entry};
}

support::Expected<std::vector<ConversationRecord>> Transaction::scan_conversations(std::size_t limit) {
    if (auto rejected = impl_->reject_read("scanConversations"); !rejected) {
        return std::unexpected(std::move(rejected.error()));
    }
    return impl_->storage->scan_conversations(limit);
}

support::Expected<std::vector<EntryRecord>> Transaction::scan_entries(
        ConversationId conversation_id, std::size_t limit) {
    if (auto rejected = impl_->reject_read("scanEntries"); !rejected) {
        return std::unexpected(std::move(rejected.error()));
    }
    return impl_->storage->scan_entries(conversation_id, limit);
}

support::Expected<chord::Draft> Transaction::doc(const DocumentDefinition& definition, DocumentAddress address) {
    if (auto rejected = impl_->reject_write(); !rejected) {
        return std::unexpected(std::move(rejected.error()));
    }
    const std::string key = address_key(definition.kind(), address.conversation_id);

    // The incarnation is loaded from the committed state when it exists, and
    // created otherwise. A document write is not a table write: reading and
    // writing documents stays available after the first table write.
    std::optional<DocumentRecord> record;
    chord::Json base;
    bool created = false;
    if (impl_->session != nullptr) {
        if (const auto cached = impl_->session->cached_documents.find(key);
                cached != impl_->session->cached_documents.end()) {
            record = cached->second.record;
            base = cached->second.value;
        }
    }
    if (!record.has_value()) {
        auto found = impl_->storage->find_document(definition.kind(), address.conversation_id);
        if (!found) {
            return std::unexpected(std::move(found.error()));
        }
        if ((*found).has_value()) {
            record = *(*found);
            auto stored = impl_->storage->document(record->id);
            if (!stored) {
                return std::unexpected(std::move(stored.error()));
            }
            if ((*stored).has_value()) {
                base = *(*stored);
            }
        } else {
            auto id = impl_->storage->mint_id();
            if (!id) {
                return std::unexpected(std::move(id.error()));
            }
            // The frozen backend stamps the incarnation with the sequence of
            // the commit that created it; the Session does not know that
            // sequence before the commit settles, so the record keeps the
            // placeholder and the Session stamps it after the settlement.
            record = DocumentRecord{.id = DocumentId{.value = *id}, .kind = definition.kind(), .created_at = 0};
            base = definition.initial_value();
            created = true;
        }
    }

    auto tracker = chord::track(std::move(base));
    auto change = tracker.begin_change();
    auto draft = change.draft();

    impl_->documents.push_back(StagedDocument{.record = *record,
            .address = std::move(address),
            .kind = definition.kind(),
            .tracker = std::move(tracker),
            .change = std::move(change),
            .created = created});
    return draft;
}

support::Expected<TaskId> Transaction::create_task(
        const TaskDefinition& definition, chord::Json input, TaskOptions options) {
    if (auto rejected = impl_->reject_write(); !rejected) {
        return std::unexpected(std::move(rejected.error()));
    }
    auto id = impl_->storage->mint_id();
    if (!id) {
        return std::unexpected(std::move(id.error()));
    }
    ConversationId conv_id = options.conversation_id.value_or(ConversationId{1});
    TaskRecord record{
            .id = TaskId{.value = *id},
            .conversation_id = conv_id,
            .kind = definition.kind(),
            .version = definition.version(),
            .input = input,
            .owner = options.owner,
            .background = options.background,
            .abort_requested = false,
            .abort_reason = std::nullopt,
            .started_at = std::nullopt,
            .ended_at = std::nullopt,
            .state =
                    TaskState{
                            .status = "pending",
                            .checkpoint =
                                    options.checkpoint.has_value() ? options.checkpoint : definition.initial(input),
                            .outcome = std::nullopt,
                    },
            .memos = {},
    };
    impl_->has_table_write = true;
    impl_->writes.push_back(StorageWrite{.type = "task", .task = record});
    return TaskId{.value = *id};
}

support::Expected<std::optional<TaskRecord>> Transaction::task(TaskId id) {
    if (auto rejected = impl_->reject_read("task"); !rejected) {
        return std::unexpected(std::move(rejected.error()));
    }
    return impl_->storage->task(id);
}

support::ExpectedVoid Transaction::set_task(TaskRecord record) {
    if (auto rejected = impl_->reject_write(); !rejected) {
        return std::unexpected(std::move(rejected.error()));
    }
    impl_->has_table_write = true;
    impl_->writes.push_back(StorageWrite{.type = "task", .task = std::move(record)});
    return {};
}

support::Expected<std::vector<TaskRecord>> Transaction::scan_tasks(std::size_t limit) {
    if (auto rejected = impl_->reject_read("scanTasks"); !rejected) {
        return std::unexpected(std::move(rejected.error()));
    }
    return impl_->storage->scan_tasks(limit);
}

support::Expected<std::optional<SubmissionRecord>> Transaction::submission(SubmissionId id) {
    if (auto rejected = impl_->reject_read("submission"); !rejected) {
        return std::unexpected(std::move(rejected.error()));
    }
    return impl_->storage->submission(id);
}

support::Expected<std::optional<SubmissionRecord>> Transaction::submission_by_request(
        ConversationId conversation_id, std::string_view request_id) {
    if (auto rejected = impl_->reject_read("submissionByRequest"); !rejected) {
        return std::unexpected(std::move(rejected.error()));
    }
    return impl_->storage->submission_by_request(conversation_id, request_id);
}

support::Expected<SubmissionRecord> Transaction::create_submission(SubmissionRecord record) {
    if (auto rejected = impl_->reject_write(); !rejected) {
        return std::unexpected(std::move(rejected.error()));
    }
    impl_->has_table_write = true;
    if (!impl_->has_conversation(record.conversation_id)) {
        auto conversation = impl_->storage->conversation(record.conversation_id);
        if (!conversation) {
            return std::unexpected(std::move(conversation.error()));
        }
        if (!(*conversation).has_value()) {
            return std::unexpected(support::make_error(support::ErrorCode::Validation,
                    std::string{"Conversation "} + std::to_string(record.conversation_id.value) + " does not exist"));
        }
    }
    auto id = impl_->storage->mint_id();
    if (!id) {
        return std::unexpected(std::move(id.error()));
    }
    record.id = SubmissionId{.value = *id};
    impl_->staged_submissions.emplace(record.id.value, record);
    return record;
}

support::ExpectedVoid Transaction::settle_submission(SubmissionId id, SubmissionSettlement settlement) {
    if (auto rejected = impl_->reject_write(); !rejected) {
        return std::unexpected(std::move(rejected.error()));
    }
    // The change stages and resolves at assembly; it is not a caller table
    // read and works after the first table write.
    impl_->has_table_write = true;
    impl_->submission_changes.push_back(
            SubmissionChange{.id = id, .settlement = std::move(settlement), .placed_entry = std::nullopt});
    return {};
}

support::ExpectedVoid Transaction::place_submission(SubmissionId id, EntryId entry) {
    if (auto rejected = impl_->reject_write(); !rejected) {
        return std::unexpected(std::move(rejected.error()));
    }
    impl_->has_table_write = true;
    impl_->submission_changes.push_back(SubmissionChange{.id = id, .settlement = {}, .placed_entry = entry});
    return {};
}

// ── DocumentWatch ───────────────────────────────────────────────────────────

DocumentWatch::DocumentWatch(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}

DocumentWatch::DocumentWatch(DocumentWatch&&) noexcept = default;

DocumentWatch& DocumentWatch::operator=(DocumentWatch&&) noexcept = default;

DocumentWatch::~DocumentWatch() = default;

const chord::Json& DocumentWatch::value() const noexcept { return impl_->value; }

support::ExpectedVoid DocumentWatch::start(DocumentFrameListener listener) {
    if (impl_->stopped) {
        return std::unexpected(support::make_error(support::ErrorCode::Session, "Watch is stopped"));
    }
    if (impl_->started) {
        return std::unexpected(support::make_error(support::ErrorCode::Session, "Watch is already started"));
    }
    impl_->started = true;
    impl_->listener = std::move(listener);
    // The buffered frames are delivered in commit order, one at a time.
    std::vector<DocumentFrame> pending = std::move(impl_->pending);
    impl_->pending.clear();
    for (DocumentFrame& frame : pending) {
        impl_->value = frame.value;
        impl_->listener(frame);
    }
    return {};
}

WatchEndReason DocumentWatch::stop() {
    if (!impl_->stopped) {
        impl_->stopped = true;
        impl_->end = WatchEndReason::Stopped;
        impl_->pending.clear();
    }
    return impl_->end;
}

// ── Session ─────────────────────────────────────────────────────────────────

Session::Session(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Session::Session(Session&&) noexcept = default;

Session& Session::operator=(Session&&) noexcept = default;

Session::~Session() = default;

std::unique_ptr<Session> Session::create(Storage& storage) {
    auto impl = std::make_unique<Session::Impl>();
    impl->storage = &storage;
    return std::unique_ptr<Session>(new Session(std::move(impl)));
}

bool Session::failed() const noexcept { return impl_->failed; }

const std::optional<std::string>& Session::failure_message() const noexcept { return impl_->failure_message; }

SessionEnd Session::closed() const noexcept {
    if (impl_->failed) {
        return SessionEnd{.reason = SessionEndReason::Failed, .failed = true, .error_message = impl_->failure_message};
    }
    if (impl_->storage_closed) {
        return SessionEnd{.reason = SessionEndReason::Closed, .failed = false, .error_message = std::nullopt};
    }
    return SessionEnd{.reason = SessionEndReason::Open, .failed = false, .error_message = std::nullopt};
}

support::AsyncResult<void> Session::commit(std::move_only_function<support::Expected<void>(Transaction&)> change) {
    if (impl_->failed) {
        return support::AsyncResult<void>{support::ExpectedVoid{std::unexpected(session_failed_error())}};
    }
    if (impl_->closed) {
        return support::AsyncResult<void>{support::ExpectedVoid{std::unexpected(session_closed_error())}};
    }
    return support::AsyncResult<void>{support::AsyncProducer<void, support::Error>{
            [session = this, change = std::move(change)](
                    support::AsyncCompletion<void, support::Error> completion) mutable noexcept {
                if (session->impl_->failed) {
                    completion(std::unexpected(session_failed_error()));
                    return;
                }
                if (session->impl_->closed) {
                    completion(std::unexpected(session_closed_error()));
                    return;
                }
                session->impl_->enqueue_commit(
                        Session::Impl::PendingCommit{.change = std::move(change), .completion = std::move(completion)});
            }}};
}

support::AsyncResult<void> Session::Impl::commit_now(
        std::move_only_function<support::Expected<void>(Transaction&)> change) {
    auto transaction_impl = std::make_unique<Transaction::Impl>();
    transaction_impl->session = this;
    transaction_impl->storage = storage;
    Transaction transaction{std::move(transaction_impl)};

    auto outcome = change(transaction);
    if (!outcome) {
        // A failed callback publishes nothing and leaves the Session usable.
        // The staged writes are discarded and every staged change is aborted,
        // so no draft escapes the failed transaction.
        transaction.impl_->sealed = true;
        for (StagedDocument& document : transaction.impl_->documents) {
            document.change.abort();
        }
        return support::AsyncResult<void>{support::ExpectedVoid{std::unexpected(std::move(outcome.error()))}};
    }

    // Resolve the staged submission changes at assembly, against this
    // transaction's staged candidates first, then committed state (frozen pi
    // `Tx.#assemble`). A failure rejects the commit and publishes nothing,
    // like a failed callback.
    const auto reject_assembly = [&transaction](support::Error error) {
        transaction.impl_->sealed = true;
        for (StagedDocument& document : transaction.impl_->documents) {
            document.change.abort();
        }
        return support::AsyncResult<void>{support::ExpectedVoid{std::unexpected(std::move(error))}};
    };
    for (const SubmissionChange& change : transaction.impl_->submission_changes) {
        std::optional<SubmissionRecord> committed;
        SubmissionRecord* current = nullptr;
        if (auto staged = transaction.impl_->staged_submissions.find(change.id.value);
                staged != transaction.impl_->staged_submissions.end()) {
            current = &staged->second;
        } else {
            auto stored = storage->submission(change.id);
            if (!stored) {
                return reject_assembly(std::move(stored.error()));
            }
            if (!(*stored).has_value()) {
                return reject_assembly(support::make_error(support::ErrorCode::Validation,
                        std::string{"Submission "} + std::to_string(change.id.value) + " does not exist"));
            }
            committed = **stored;
            current = &*committed;
        }
        auto next = apply_submission_change(*current, change);
        if (!next) {
            return reject_assembly(std::move(next.error()));
        }
        // A change that leaves a committed record unchanged publishes no write
        // (frozen pi `Tx.#assemble` replaces the candidate only when the
        // resolved record differs); a staged candidate was already part of
        // the batch.
        if (!committed.has_value() || *next != *current) {
            transaction.impl_->staged_submissions.insert_or_assign(change.id.value, *next);
        }
    }
    // Every resolved candidate joins the batch as a submission write.
    for (const auto& [id, record] : transaction.impl_->staged_submissions) {
        transaction.impl_->writes.push_back(StorageWrite{.type = "submission", .submission = record});
    }

    auto state = std::make_shared<Session::Impl::CommitState>();
    state->session = this;
    state->storage = storage;
    state->documents = std::move(transaction.impl_->documents);
    state->writes = std::move(transaction.impl_->writes);
    transaction.impl_->sealed = true;

    // The staged documents are prepared before the batch is admitted, so a
    // prepared draft is never observable before the storage settles it.
    for (StagedDocument& document : state->documents) {
        auto prepared = document.change.prepare();
        if (!prepared) {
            return support::AsyncResult<void>{support::ExpectedVoid{std::unexpected(std::move(prepared.error()))}};
        }
        state->prepared.push_back(std::move(*prepared));
    }

    // The document writes join the batch. A creation carries the record and
    // the base value; a change carries the incarnation and the exact
    // operations.
    for (std::size_t index = 0; index < state->documents.size(); ++index) {
        const StagedDocument& document = state->documents[index];
        const chord::Prepared& prepared = state->prepared[index];
        StorageWrite write{.type = document.created ? "document.create" : "document.change"};
        write.document_conversation_id = document.address.conversation_id;
        if (document.created) {
            write.document_record = document.record;
        } else {
            write.document_id = document.record.id;
            write.document_ops = prepared.ops();
        }
        write.document_value = prepared.value();
        state->writes.push_back(std::move(write));
    }

    if (state->writes.empty()) {
        return support::AsyncResult<void>{support::ExpectedVoid{}};
    }

    return support::AsyncResult<void>{support::AsyncProducer<void, support::Error>{
            [state = std::move(state)](support::AsyncCompletion<void, support::Error> completion) mutable noexcept {
                state->storage->commit(state->writes)
                        .start([state, completion = std::move(completion)](
                                       std::expected<Seq, support::Error> outcome) mutable noexcept {
                            support::ExpectedVoid settled = state->settle(std::move(outcome));
                            completion(std::move(settled));
                        });
            }}};
}

support::AsyncResult<void> Session::close() {
    impl_->closed = true;
    impl_->notify_close();
    impl_->run_next_commit();
    impl_->finish_close_if_ready();
    if (impl_->storage_closed) {
        return support::AsyncResult<void>{support::ExpectedVoid{}};
    }
    return support::AsyncResult<void>{support::AsyncProducer<void, support::Error>{
            [session = this](support::AsyncCompletion<void, support::Error> completion) mutable noexcept {
                if (session->impl_->storage_closed) {
                    completion(support::ExpectedVoid{});
                    return;
                }
                session->impl_->close_completions.push_back(std::move(completion));
            }}};
}

support::ExpectedVoid Session::subscribe_commits(CommitListener listener) {
    if (impl_->failed) {
        return std::unexpected(session_failed_error());
    }
    if (impl_->closed) {
        return std::unexpected(session_closed_error());
    }
    impl_->commit_listeners.push_back(std::move(listener));
    return {};
}

support::ExpectedVoid Session::subscribe_close(CloseListener listener) {
    if (impl_->closed) {
        return std::unexpected(session_closed_error());
    }
    impl_->close_listeners.push_back(std::move(listener));
    return {};
}

support::AsyncResult<std::optional<chord::Json>> Session::snapshot(
        const DocumentDefinition& definition, DocumentAddress address) {
    if (impl_->failed) {
        return support::AsyncResult<std::optional<chord::Json>>{std::unexpected(session_failed_error())};
    }
    if (impl_->closed) {
        return support::AsyncResult<std::optional<chord::Json>>{std::unexpected(session_closed_error())};
    }
    auto value = impl_->committed_value(definition, address);
    if (!value) {
        return support::AsyncResult<std::optional<chord::Json>>{std::unexpected(std::move(value.error()))};
    }
    return support::AsyncResult<std::optional<chord::Json>>{std::move(*value)};
}

support::AsyncResult<std::optional<DocumentWatch>> Session::watch_doc(
        const DocumentDefinition& definition, DocumentAddress address) {
    if (impl_->failed) {
        return support::AsyncResult<std::optional<DocumentWatch>>{std::unexpected(session_failed_error())};
    }
    if (impl_->closed) {
        return support::AsyncResult<std::optional<DocumentWatch>>{std::unexpected(session_closed_error())};
    }
    auto value = impl_->committed_value(definition, address);
    if (!value) {
        return support::AsyncResult<std::optional<DocumentWatch>>{std::unexpected(std::move(value.error()))};
    }
    if (!value->has_value()) {
        return support::AsyncResult<std::optional<DocumentWatch>>{std::optional<DocumentWatch>{}};
    }

    auto watch_impl = std::make_shared<DocumentWatch::Impl>();
    watch_impl->value = *(*value);

    // The watch receives every committed frame of this incarnation from now
    // on; the frames committed before it is started are buffered.
    const std::string key = address_key(definition.kind(), address.conversation_id);
    impl_->watches[key].push_back(watch_impl);
    return support::AsyncResult<std::optional<DocumentWatch>>{std::optional<DocumentWatch>{DocumentWatch{watch_impl}}};
}

} // namespace cch::durable
