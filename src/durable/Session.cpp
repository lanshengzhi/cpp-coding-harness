#include <cch/durable/Session.hpp>

#include <cch/durable/Storage.hpp>
#include <cch/durable/Task.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace cch::durable {

std::string_view document_scope_name(DocumentScope scope) noexcept {
    switch (scope) {
    case DocumentScope::Session:
        return "session";
    case DocumentScope::Conversation:
        return "conversation";
    case DocumentScope::Task:
        return "task";
    }
    return "session";
}

bool document_is_current_only(const DocumentRecord& record) noexcept {
    return record.scope() != DocumentScope::Conversation || record.history == DocumentHistory::Latest;
}

std::string document_address_key(std::string_view kind, const DocumentAddress& address) {
    std::string key{kind};
    key += '|';
    key += document_scope_name(address.scope());
    key += ':';
    if (address.task_id.has_value()) {
        key += std::to_string(address.task_id->value);
    } else if (address.conversation_id.has_value()) {
        key += std::to_string(address.conversation_id->value);
    } else {
        key += "session";
    }
    key += '|';
    if (address.key.has_value()) {
        key += "family:";
        key += *address.key;
    } else {
        key += "singleton";
    }
    return key;
}

namespace {

/// The shared checkpoint predicate of one document definition; sharing lets a
/// staged transaction keep the hook without owning the definition token.
using DocumentCheckpointFn = std::move_only_function<bool(const chord::Json&, const chord::OpBatch&, std::int64_t)>;

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

[[nodiscard]] support::Error validation_error(std::string message) {
    return support::make_error(support::ErrorCode::Validation, std::move(message));
}

/// The message of typed access to an incarnation whose persisted scope,
/// history or fork semantics disagree with the supplied definition (frozen pi
/// `checkRecordScope`).
[[nodiscard]] support::Error scope_mismatch_error(const DocumentRecord& record) {
    return validation_error("Document " + std::to_string(record.id.value) + " (" + record.kind +
                            ") does not match the supplied definition semantics");
}

/// The message of typed access to a newer stored version than the definition
/// can read (frozen pi `checkRecordVersion`).
[[nodiscard]] support::Error newer_version_error(
        const DocumentRecord& record, std::uint32_t version, std::uint32_t definition_version) {
    return validation_error("Document " + std::to_string(record.id.value) + " (" + record.kind +
                            ") has newer version " + std::to_string(version) + " than " +
                            std::to_string(definition_version));
}

/// The message of typed access to an older stored version without a migrate
/// callback (frozen pi `checkRecordVersion`).
[[nodiscard]] support::Error migration_required_error(const DocumentRecord& record, std::uint32_t version) {
    return validation_error("Document " + std::to_string(record.id.value) + " (" + record.kind +
                            ") requires migration from version " + std::to_string(version));
}

/// Whether one persisted incarnation matches the scope, history and fork
/// semantics of the supplied definition (frozen pi `checkRecordScope`).
[[nodiscard]] support::ExpectedVoid check_record_scope(
        const DocumentDefinition& definition, const DocumentRecord& record) {
    if (record.scope() != definition.scope()) {
        return std::unexpected(scope_mismatch_error(record));
    }
    if (record.scope() == DocumentScope::Conversation &&
            (record.history != definition.history() || record.fork != definition.fork())) {
        return std::unexpected(scope_mismatch_error(record));
    }
    return {};
}

/// Reject a stored version this definition cannot read (frozen pi
/// `checkRecordVersion`).
[[nodiscard]] support::ExpectedVoid check_record_version(
        const DocumentDefinition& definition, const DocumentRecord& record, std::uint32_t version) {
    if (version > definition.version()) {
        return std::unexpected(newer_version_error(record, version, definition.version()));
    }
    if (version < definition.version() && !definition.can_migrate()) {
        return std::unexpected(migration_required_error(record, version));
    }
    return {};
}

/// Validate and materialize one detached stored value for typed access
/// (frozen pi `materializeDocument`). A value written by an older definition
/// version is migrated in memory; nothing is persisted until the next commit.
[[nodiscard]] support::Expected<chord::Json> materialize_document(const DocumentDefinition& definition,
        const DocumentRecord& record,
        std::uint32_t version,
        const chord::Json& value) {
    if (auto scoped = check_record_scope(definition, record); !scoped) {
        return std::unexpected(std::move(scoped.error()));
    }
    if (auto current = check_record_version(definition, record, version); !current) {
        return std::unexpected(std::move(current.error()));
    }
    if (version == definition.version()) {
        return value;
    }
    auto migrated = definition.migrate(value, version);
    if (!migrated.has_value()) {
        return std::unexpected(migration_required_error(record, version));
    }
    return std::move(*migrated);
}

/// The record a new incarnation of one address carries (frozen pi
/// `documentCreate`).
[[nodiscard]] DocumentRecord document_create_record(
        const DocumentDefinition& definition, const DocumentAddress& address, DocumentId id) {
    DocumentRecord record{.id = id, .kind = definition.kind()};
    record.conversation_id = address.conversation_id;
    record.task_id = address.task_id;
    record.key = address.key;
    if (definition.scope() == DocumentScope::Conversation) {
        record.history = definition.history();
        record.fork = definition.fork();
    }
    return record;
}

/// Whether one staged address agrees with the definition's scope (frozen pi
/// `resolveAddress`).
[[nodiscard]] support::ExpectedVoid check_address(
        const DocumentDefinition& definition, const DocumentAddress& address) {
    if (address.scope() != definition.scope()) {
        return std::unexpected(validation_error("Document " + definition.kind() + " requires a " +
                                                std::string{document_scope_name(definition.scope())} + " address"));
    }
    if (definition.family() != address.key.has_value()) {
        return std::unexpected(validation_error("Document " + definition.kind() + " requires " +
                                                (definition.family() ? "a family key" : "no family key")));
    }
    return {};
}

/// One staged document incarnation: its record, its address, its Chord tracker
/// holding the candidate revision, and the open change the draft came from.
struct StagedDocument {
    DocumentRecord record{};
    DocumentAddress address{};
    std::string kind{};
    chord::Tracker tracker;
    /// The open change; absent for a retirement of an incarnation this
    /// transaction never acquired.
    std::optional<chord::Change> change{};
    bool created{false};
    /// The stored definition version the tracked value was materialized from;
    /// below the definition version after a migration that has not been
    /// persisted yet.
    std::uint32_t stored_version{1};
    /// Deltas stored after the newest base at load time.
    std::int64_t deltas_since_base{0};
    /// Whether this staged entry retires the incarnation instead of
    /// publishing a content change.
    bool retire{false};
    /// The definition version the tracked value has (frozen pi
    /// `DocumentEntry.definition.version`).
    std::uint32_t definition_version{1};
    /// The checkpoint predicate of the staged definition (frozen pi
    /// `CommonDocDefinition.checkpointWhen`).
    std::shared_ptr<DocumentCheckpointFn> checkpoint{};
    /// The persisted source of a definition-free copy; present only for a
    /// fork-copy incarnation this transaction did not create from a value.
    std::optional<DocumentCopySource> fork_source{};
};

/// One staged submission change: a settlement, or the placement of a queued
/// submission at its transcript entry (frozen pi `SubmissionChange`).
struct SubmissionChange {
    SubmissionId id{};
    SubmissionSettlement settlement{};
    /// The placement entry; absent for a settlement.
    std::optional<EntryId> placed_entry{std::nullopt};
};

/// One committed document incarnation the Session knows, by address key
/// (frozen pi `LoadedDocument`).
struct CachedDocument {
    DocumentRecord record{};
    chord::Json value{};
    /// The definition version the cached value has (frozen pi
    /// `LoadedDocument.valueVersion`).
    std::uint32_t value_version{1};
    /// The persisted definition version; below `value_version` while a
    /// migration is materialized only in memory (frozen
    /// `LoadedDocument.storedVersion`).
    std::uint32_t stored_version{1};
    std::int64_t deltas_since_base{0};
};

/// Reject new owned work below an owner that cannot take it (frozen pi
/// `Tx.#validateOwners`): new owned work needs a live owner, so a terminal,
/// `completing`, or abort-marked task cannot take a child task or a
/// task-owned conversation.
[[nodiscard]] support::ExpectedVoid require_live_owner(std::string_view what, TaskId owner, const TaskRecord& record) {
    const std::string prefix = std::string{what} + " " + std::to_string(owner.value);
    if (record.state.status == "terminal" || record.state.status == "completing") {
        return std::unexpected(
                support::make_error(support::ErrorCode::Validation, prefix + " is " + record.state.status));
    }
    if (record.abort_requested) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation, prefix + " is abort-marked"));
    }
    return {};
}

} // namespace

// ── DocumentDefinitionBuilder ───────────────────────────────────────────────

struct DocumentDefinitionBuilder::Impl {
    std::string kind;
    std::uint32_t version{1};
    DocumentScope scope{DocumentScope::Session};
    DocumentHistory history{DocumentHistory::Latest};
    DocumentFork fork{DocumentFork::Current};
    std::move_only_function<chord::Json(const chord::Json&, std::uint32_t)> migrate{};
    std::shared_ptr<DocumentCheckpointFn> checkpoint{};
};

DocumentDefinitionBuilder::DocumentDefinitionBuilder(std::string kind) : impl_(std::make_unique<Impl>()) {
    impl_->kind = std::move(kind);
}

DocumentDefinitionBuilder::DocumentDefinitionBuilder(DocumentDefinitionBuilder&&) noexcept = default;

DocumentDefinitionBuilder& DocumentDefinitionBuilder::operator=(DocumentDefinitionBuilder&&) noexcept = default;

DocumentDefinitionBuilder::~DocumentDefinitionBuilder() = default;

DocumentDefinitionBuilder&& DocumentDefinitionBuilder::version(std::uint32_t value) && {
    impl_->version = value;
    return std::move(*this);
}

DocumentDefinitionBuilder&& DocumentDefinitionBuilder::conversation(DocumentHistory history, DocumentFork fork) && {
    impl_->scope = DocumentScope::Conversation;
    impl_->history = history;
    impl_->fork = fork;
    return std::move(*this);
}

DocumentDefinitionBuilder&& DocumentDefinitionBuilder::task() && {
    impl_->scope = DocumentScope::Task;
    impl_->history = DocumentHistory::Latest;
    impl_->fork = DocumentFork::Current;
    return std::move(*this);
}

DocumentDefinitionBuilder&& DocumentDefinitionBuilder::migrate(
        std::move_only_function<chord::Json(const chord::Json&, std::uint32_t)> value) && {
    impl_->migrate = std::move(value);
    return std::move(*this);
}

DocumentDefinitionBuilder&& DocumentDefinitionBuilder::checkpoint_when(
        std::move_only_function<bool(const chord::Json&, const chord::OpBatch&, std::int64_t)> value) && {
    impl_->checkpoint = std::make_shared<DocumentCheckpointFn>(std::move(value));
    return std::move(*this);
}

// ── DocumentDefinition ──────────────────────────────────────────────────────

struct DocumentDefinition::Impl {
    std::string kind;
    std::uint32_t version{1};
    DocumentScope scope{DocumentScope::Session};
    DocumentHistory history{DocumentHistory::Latest};
    DocumentFork fork{DocumentFork::Current};
    bool family{false};
    std::move_only_function<chord::Json()> initial{};
    std::move_only_function<chord::Json(const chord::Json&)> family_initial{};
    std::move_only_function<chord::Json(const chord::Json&, std::uint32_t)> migrate{};
    std::shared_ptr<DocumentCheckpointFn> checkpoint{};
};

DocumentDefinition::DocumentDefinition(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

DocumentDefinition::DocumentDefinition(DocumentDefinition&&) noexcept = default;

DocumentDefinition& DocumentDefinition::operator=(DocumentDefinition&&) noexcept = default;

DocumentDefinition::~DocumentDefinition() = default;

const std::string& DocumentDefinition::kind() const noexcept { return impl_->kind; }

std::uint32_t DocumentDefinition::version() const noexcept { return impl_->version; }

DocumentScope DocumentDefinition::scope() const noexcept { return impl_->scope; }

DocumentHistory DocumentDefinition::history() const noexcept { return impl_->history; }

DocumentFork DocumentDefinition::fork() const noexcept { return impl_->fork; }

bool DocumentDefinition::family() const noexcept { return impl_->family; }

chord::Json DocumentDefinition::initial_value() const { return impl_->initial(); }

chord::Json DocumentDefinition::initial_value(const chord::Json& seed) const { return impl_->family_initial(seed); }

bool DocumentDefinition::can_migrate() const noexcept { return static_cast<bool>(impl_->migrate); }

std::optional<chord::Json> DocumentDefinition::migrate(const chord::Json& value, std::uint32_t from_version) const {
    if (!impl_->migrate) {
        return std::nullopt;
    }
    return impl_->migrate(value, from_version);
}

bool DocumentDefinition::checkpoint(
        const chord::Json& value, const chord::OpBatch& ops, std::int64_t deltas_since_base) const {
    if (!impl_->checkpoint) {
        return false;
    }
    return (*impl_->checkpoint)(value, ops, deltas_since_base);
}

namespace detail {

/// The internal construction steps of one document definition token: the
/// builder's fields become a definition, and the definition factory is the
/// only caller.
struct DocumentDefinitionAccess {
    static std::unique_ptr<DocumentDefinition::Impl> make() { return std::make_unique<DocumentDefinition::Impl>(); }

    static DocumentDefinition wrap(std::unique_ptr<DocumentDefinition::Impl> impl) {
        return DocumentDefinition{std::move(impl)};
    }

    [[nodiscard]] static const DocumentDefinitionBuilder::Impl& fields(const DocumentDefinitionBuilder& builder) {
        return *builder.impl_;
    }

    /// Reject a definition that could never be persisted (frozen pi
    /// `validateDefinition` plus the scope semantics the persisted record
    /// needs).
    [[nodiscard]] static support::ExpectedVoid validate(const DocumentDefinitionBuilder& builder) {
        const DocumentDefinitionBuilder::Impl& definition = *builder.impl_;
        if (definition.kind.empty()) {
            return std::unexpected(validation_error("Document kind must be a non-empty string"));
        }
        if (definition.version < 1) {
            return std::unexpected(
                    validation_error("Document " + definition.kind + " version must be a positive integer"));
        }
        // Only a conversation document declares history and fork behavior; the
        // other scopes are current-only and are never copied by a conversation
        // fork, so declaring them would persist semantics nothing can honor.
        if (definition.scope != DocumentScope::Conversation &&
                (definition.history != DocumentHistory::Latest || definition.fork != DocumentFork::Current)) {
            return std::unexpected(validation_error(
                    "Document " + definition.kind + " declares history or fork semantics outside conversation scope"));
        }
        if (definition.scope == DocumentScope::Conversation && definition.history == DocumentHistory::Latest &&
                definition.fork == DocumentFork::AsOf) {
            return std::unexpected(
                    validation_error("Document " + definition.kind + " cannot fork as-of without rewindable history"));
        }
        return {};
    }

    /// Copy the checkpoint predicate out of a staged definition so a
    /// transaction can keep the hook without keeping the definition token.
    [[nodiscard]] static std::shared_ptr<DocumentCheckpointFn> checkpoint_hook(const DocumentDefinition& definition) {
        return definition.impl_->checkpoint;
    }

    [[nodiscard]] static std::unique_ptr<DocumentDefinition::Impl> take(
            DocumentDefinitionBuilder definition, bool family) {
        auto impl = make();
        impl->kind = std::move(definition.impl_->kind);
        impl->version = definition.impl_->version;
        impl->scope = definition.impl_->scope;
        impl->history = definition.impl_->history;
        impl->fork = definition.impl_->fork;
        impl->family = family;
        impl->migrate = std::move(definition.impl_->migrate);
        impl->checkpoint = std::move(definition.impl_->checkpoint);
        return impl;
    }
};

} // namespace detail

DocumentDefinition define_document(std::string kind, std::move_only_function<chord::Json()> initial) {
    auto impl = detail::DocumentDefinitionAccess::make();
    impl->kind = std::move(kind);
    impl->scope = DocumentScope::Conversation;
    impl->history = DocumentHistory::Latest;
    impl->fork = DocumentFork::Current;
    impl->initial = std::move(initial);
    return detail::DocumentDefinitionAccess::wrap(std::move(impl));
}

namespace detail {

/// Trusted construction for the built-in harness documents: the kind is a
/// compile-time constant and the scope/history/fork combination is valid by
/// construction, so `define_document` validation cannot fail (frozen pi
/// `InboxDoc` / `LiveDoc` / `AgentDoc` / `ProviderDoc` / `UsageDoc`). The
/// checkpoint predicate selects a complete base instead of a delta (frozen
/// `CommonDocDefinition.checkpointWhen`).
DocumentDefinition builtin_document(std::string kind,
        DocumentHistory history,
        DocumentFork fork,
        std::move_only_function<chord::Json()> initial,
        std::move_only_function<bool(const chord::Json&, const chord::OpBatch&, std::int64_t)> checkpoint) {
    auto impl = DocumentDefinitionAccess::make();
    impl->kind = std::move(kind);
    impl->scope = DocumentScope::Conversation;
    impl->history = history;
    impl->fork = fork;
    impl->initial = std::move(initial);
    if (checkpoint) {
        impl->checkpoint = std::make_shared<DocumentCheckpointFn>(std::move(checkpoint));
    }
    return DocumentDefinitionAccess::wrap(std::move(impl));
}

} // namespace detail

support::Expected<DocumentDefinition> define_document(
        DocumentDefinitionBuilder definition, std::move_only_function<chord::Json()> initial) {
    if (!initial) {
        return std::unexpected(
                validation_error("Document " + detail::DocumentDefinitionAccess::fields(definition).kind +
                                 " requires an initial value"));
    }
    if (auto valid = detail::DocumentDefinitionAccess::validate(definition); !valid) {
        return std::unexpected(std::move(valid.error()));
    }
    auto impl = detail::DocumentDefinitionAccess::take(std::move(definition), false);
    impl->initial = std::move(initial);
    return detail::DocumentDefinitionAccess::wrap(std::move(impl));
}

support::Expected<DocumentDefinition> define_document_family(
        DocumentDefinitionBuilder definition, std::move_only_function<chord::Json(const chord::Json&)> initial) {
    if (!initial) {
        return std::unexpected(
                validation_error("Document family " + detail::DocumentDefinitionAccess::fields(definition).kind +
                                 " requires an initial value"));
    }
    if (auto valid = detail::DocumentDefinitionAccess::validate(definition); !valid) {
        return std::unexpected(std::move(valid.error()));
    }
    auto impl = detail::DocumentDefinitionAccess::take(std::move(definition), true);
    impl->family_initial = std::move(initial);
    return detail::DocumentDefinitionAccess::wrap(std::move(impl));
}

// ── DocumentWatch ───────────────────────────────────────────────────────────

struct DocumentWatch::Impl {
    chord::Json value{};
    bool started{false};
    bool stopped{false};
    WatchEndReason end{WatchEndReason::Stopped};
    DocumentFrameListener listener{};
    /// The definition version the acquisition value was materialized for; an
    /// observer of another version receives root replacements (frozen pi
    /// `observedOperations`).
    std::uint32_t observed_version{1};
    /// The frames committed since the acquisition, in commit order.
    std::vector<DocumentFrame> pending{};
};

// ── Session ─────────────────────────────────────────────────────────────────

struct Session::Impl {
    // Harness callers, task workers and storage completions can enter from
    // different threads. Hold one mutation line across queue changes,
    // preparation, adoption and publication. Reentrancy lets observers enqueue
    // their next commit on this same line; commit_active preserves FIFO order
    // while asynchronous storage is outstanding. No thread or scheduler is
    // owned here, and the lock is not held across an asynchronous wait.
    mutable std::recursive_mutex mutation_line;

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
        /// The prepared revision of each staged document, aligned with
        /// `documents`; absent for a retirement of an incarnation this
        /// transaction never acquired.
        std::vector<std::optional<chord::Prepared>> prepared{};
        /// Whether each staged document produced a storage write, aligned
        /// with `documents`: a creation always writes; a loaded incarnation
        /// writes only when its prepared delta is non-empty (frozen pi
        /// `Transaction.#assemble`: only a change stores a delta).
        std::vector<bool> document_written{};
        /// Whether each storage write stores a complete base, aligned with
        /// `documents`.
        std::vector<bool> document_base{};
        /// One published document change and the staged document it belongs
        /// to, in publication order.
        struct PublishedDocument {
            std::size_t index{0};
            std::size_t change_index{0};
            /// Whether the stored revision is a complete base.
            bool base{false};
        };
        std::vector<PublishedDocument> published{};

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
            // committed revision is the adopted one. A creation, a change and
            // a retirement each publish their own immutable change; an
            // acquired-but-unchanged incarnation publishes nothing (frozen pi
            // `Transaction.adopt` and `publishes`).
            std::vector<DocumentCommitChange> document_changes;
            document_changes.reserve(documents.size());
            for (std::size_t index = 0; index < documents.size(); ++index) {
                StagedDocument& document = documents[index];
                DocumentRecord record = document.record;
                if (document.created) {
                    record.created_at = seq;
                }
                if (document.retire) {
                    record.retired_at = seq;
                }

                const std::optional<chord::Prepared>& prepared = this->prepared[index];
                if (this->document_written[index] && prepared.has_value()) {
                    auto adopted = document.tracker.adopt(*prepared);
                    if (!adopted) {
                        // The storage already committed; a failed adoption
                        // leaves memory behind durable state, so the Session
                        // fails.
                        session->fail(std::move(adopted.error()));
                        return std::unexpected(session->failure_error());
                    }
                }

                if (document.retire) {
                    // The canonical retirement publication carries no version
                    // and a JSON null value (frozen pi `Transaction.adopt`).
                    this->published.push_back(
                            PublishedDocument{.index = index, .change_index = document_changes.size(), .base = false});
                    document_changes.push_back(DocumentCommitChange{.record = record,
                            .conversation_id = document.address.conversation_id,
                            .value = chord::Json{},
                            .ops = chord::OpBatch{},
                            .version = std::nullopt});
                    continue;
                }
                if (!prepared.has_value()) {
                    continue;
                }
                this->published.push_back(PublishedDocument{
                        .index = index, .change_index = document_changes.size(), .base = this->document_base[index]});
                if (document.created && document.fork_source.has_value()) {
                    // A definition-free copy publishes its exact persisted
                    // source so consumers hydrate through state (frozen pi
                    // `DocumentCommitChange` of type `document.copy`).
                    document_changes.push_back(DocumentCommitChange{.record = record,
                            .conversation_id = document.address.conversation_id,
                            .value = prepared->value(),
                            .ops = chord::OpBatch{},
                            .version = document.definition_version,
                            .source = document.fork_source});
                    continue;
                }
                document_changes.push_back(DocumentCommitChange{.record = record,
                        .conversation_id = document.address.conversation_id,
                        .value = prepared->value(),
                        .ops = document.created ? chord::OpBatch{} : prepared->ops(),
                        .version = document.definition_version});
            }

            // The committed document incarnations become observable now; a
            // retirement evicts its incarnation so the next acquisition
            // creates a new one.
            for (const PublishedDocument& published : this->published) {
                const StagedDocument& document = documents[published.index];
                const std::string key = document_address_key(document.kind, document.address);
                const DocumentCommitChange& change = document_changes[published.change_index];
                if (document.retire) {
                    session->cached_documents.erase(key);
                    continue;
                }
                session->cached_documents[key] = CachedDocument{.record = change.record,
                        .value = change.value,
                        .value_version = document.definition_version,
                        .stored_version = document.definition_version,
                        .deltas_since_base = published.base ? 0 : document.deltas_since_base + 1};
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
            // incarnation. A retirement delivers its canonical null frame and
            // then closes the watch with `Retired`; the watch never follows a
            // replacement incarnation of the same address (frozen pi
            // `CommittedWatch`).
            for (const DocumentCommitChange& change : document_changes) {
                const std::string key = document_address_key(change.record.kind, change.record.address());
                const auto registered = session->watches.find(key);
                if (registered == session->watches.end()) {
                    continue;
                }
                const bool retired = change.record.retired_at.has_value();
                for (const std::shared_ptr<DocumentWatch::Impl>& watch : registered->second) {
                    if (watch->stopped) {
                        continue;
                    }
                    // The operations one observer applies for one committed
                    // change (frozen pi `observedOperations`): a retirement is
                    // the canonical root-null replacement; an observer of the
                    // same definition version receives the exact committed
                    // operations; an observer of another version receives the
                    // new value as a root replacement.
                    chord::OpBatch observed;
                    if (retired) {
                        observed.push_back(chord::ReplaceOp{.value = chord::Json{}});
                    } else if (change.version.has_value() && *change.version == watch->observed_version) {
                        observed = change.ops;
                    } else {
                        if (change.version.has_value()) {
                            watch->observed_version = *change.version;
                        }
                        observed.push_back(chord::ReplaceOp{.value = change.value});
                    }
                    // A migration-only base changes nothing for an observer of
                    // the new version (frozen pi `Session.#observeCommits`).
                    if (observed.empty()) {
                        continue;
                    }
                    watch->pending.push_back(DocumentFrame{.value = change.value, .ops = std::move(observed)});
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
                    if (retired) {
                        watch->stopped = true;
                        watch->end = WatchEndReason::Retired;
                        watch->pending.clear();
                    }
                }
                if (retired) {
                    session->watches.erase(registered);
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
                        std::lock_guard lock(mutation_line);
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

    /// One committed incarnation as the Session knows it, from the cache or
    /// the storage (frozen pi `#loadDocument`). A tracker serves only tokens
    /// of the version its value was materialized for; another version reloads.
    [[nodiscard]] support::Expected<std::optional<CachedDocument>> load_document(
            const DocumentDefinition& definition, const DocumentAddress& address) {
        const std::string key = document_address_key(definition.kind(), address);
        if (const auto cached = cached_documents.find(key); cached != cached_documents.end()) {
            if (cached->second.value_version == definition.version()) {
                return std::optional<CachedDocument>{cached->second};
            }
            cached_documents.erase(cached);
        }
        auto found = storage->find_document(definition.kind(), address, DocumentPoint::current());
        if (!found) {
            return std::unexpected(std::move(found.error()));
        }
        if (!(*found).has_value()) {
            return std::optional<CachedDocument>{};
        }
        const DocumentRecord& record = **found;
        auto stored = storage->document(record.id, DocumentPoint::current());
        if (!stored) {
            return std::unexpected(std::move(stored.error()));
        }
        if (!(*stored).has_value()) {
            return std::unexpected(validation_error(
                    "Current document " + std::to_string(record.id.value) + " (" + record.kind + ") cannot be read"));
        }
        const StoredDocument& committed = **stored;
        auto value = materialize_document(definition, committed.record, committed.version, committed.value);
        if (!value) {
            return std::unexpected(std::move(value.error()));
        }
        CachedDocument loaded{.record = committed.record,
                .value = std::move(*value),
                .value_version = definition.version(),
                .stored_version = committed.version,
                .deltas_since_base = committed.deltas_since_base};
        cached_documents[key] = loaded;
        return std::optional<CachedDocument>{std::move(loaded)};
    }

    /// The committed value of one incarnation, from the cache or the storage.
    [[nodiscard]] support::Expected<std::optional<chord::Json>> committed_value(
            const DocumentDefinition& definition, const DocumentAddress& address) {
        auto loaded = load_document(definition, address);
        if (!loaded) {
            return std::unexpected(std::move(loaded.error()));
        }
        if (!(*loaded).has_value()) {
            return std::optional<chord::Json>{};
        }
        return std::optional<chord::Json>{(*loaded)->value};
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
    /// The newest staged document per address key: one transaction acquires
    /// one draft per address, and a reacquisition after a staged retirement
    /// creates the replacement incarnation (frozen pi
    /// `Tx.#latestDocumentByAddress`).
    std::map<std::string, std::size_t> latest_documents{};
    /// The submission candidates this transaction created or replaced so far,
    /// by ID (frozen pi `Tx.#submissions`).
    std::map<std::int64_t, SubmissionRecord> staged_submissions{};
    /// The staged submission changes resolved at assembly (frozen pi
    /// `Tx.#submissionChanges`).
    std::vector<SubmissionChange> submission_changes{};
    /// The latest staged candidate per task this transaction created or
    /// replaced (frozen pi `Tx.#tasksById`); a child owner is judged against it
    /// first, so a task cannot create owned work in the commit that ends it.
    std::map<std::int64_t, TaskRecord> staged_tasks{};

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

    /// The latest candidate of one task: this transaction's staged write first,
    /// then committed state (frozen pi `Tx.#currentTask`). A caller table read,
    /// so it is only used before the first table write of a child creation.
    [[nodiscard]] support::Expected<std::optional<TaskRecord>> current_task(TaskId id) {
        if (const auto staged = staged_tasks.find(id.value); staged != staged_tasks.end()) {
            return std::optional<TaskRecord>{staged->second};
        }
        return storage->task(id);
    }

    /// Record the latest candidate of one task write, so a later child-owner or
    /// conversation-owner validation of this transaction sees it.
    void stage_task(TaskRecord record) { staged_tasks.insert_or_assign(record.id.value, std::move(record)); }

    /// The tasks this transaction creates (frozen pi `Tx.#tasksById` entries
    /// whose write kind is `create`); only a created task's owner edge is
    /// validated again at assembly.
    std::set<std::int64_t> created_tasks{};

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

    /// The staged task write for one task, when this transaction created or
    /// replaced it.
    [[nodiscard]] const TaskRecord* staged_task(TaskId id) const {
        for (const StorageWrite& write : writes) {
            if (write.type == "task" && write.task.has_value() && write.task->id == id) {
                return &*write.task;
            }
        }
        return nullptr;
    }

    /// Validate the owner of one document address (frozen pi
    /// `Tx.#requireConversation`, `Tx.#currentTask` and
    /// `Tx.#assertTaskDocumentsOpen`): a conversation document needs an
    /// existing conversation, and a task document needs a live task.
    [[nodiscard]] support::ExpectedVoid check_document_owner(const DocumentAddress& address) {
        if (address.scope() == DocumentScope::Conversation) {
            if (has_conversation(*address.conversation_id)) {
                return {};
            }
            auto stored = storage->conversation(*address.conversation_id);
            if (!stored) {
                return std::unexpected(std::move(stored.error()));
            }
            if (!(*stored).has_value()) {
                return std::unexpected(validation_error(
                        "Conversation " + std::to_string(address.conversation_id->value) + " does not exist"));
            }
            return {};
        }
        if (address.scope() == DocumentScope::Task) {
            if (const TaskRecord* staged = staged_task(*address.task_id); staged != nullptr) {
                if (staged->state.status == "terminal") {
                    return std::unexpected(
                            validation_error("Task " + std::to_string(address.task_id->value) + " is terminal"));
                }
                return {};
            }
            auto stored = storage->task(*address.task_id);
            if (!stored) {
                return std::unexpected(std::move(stored.error()));
            }
            if (!(*stored).has_value()) {
                return std::unexpected(
                        validation_error("Task " + std::to_string(address.task_id->value) + " does not exist"));
            }
            if ((*stored)->state.status == "terminal") {
                return std::unexpected(
                        validation_error("Task " + std::to_string(address.task_id->value) + " is terminal"));
            }
            return {};
        }
        return {};
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

support::Expected<ConversationRecord> Transaction::fork_conversation(ConversationId parent, EntryId at) {
    if (auto rejected = impl_->reject_write(); !rejected) return std::unexpected(rejected.error());
    auto parent_record = impl_->storage->conversation(parent);
    if (!parent_record) return std::unexpected(parent_record.error());
    if (!parent_record->has_value()) {
        return std::unexpected(validation_error("Conversation " + std::to_string(parent.value) + " does not exist"));
    }
    auto visible = impl_->storage->scan_entries(parent, static_cast<std::size_t>(-1));
    if (!visible) return std::unexpected(visible.error());
    const auto selected = std::ranges::find_if(*visible, [at](const EntryRecord& entry) { return entry.id == at; });
    if (selected == visible->end()) {
        return std::unexpected(validation_error("Entry " + std::to_string(at.value) +
                                                " is not visible from conversation " + std::to_string(parent.value)));
    }
    auto selected_stored = impl_->storage->entry(at);
    if (!selected_stored) return std::unexpected(selected_stored.error());
    if (!selected_stored->has_value()) return std::unexpected(validation_error("Fork entry does not exist"));
    auto as_of_documents = impl_->storage->scan_conversation_documents(selected->conversation_id,
            DocumentPoint::as_of(selected_stored->value().commit_seq),
            static_cast<std::size_t>(-1));
    if (!as_of_documents) return std::unexpected(as_of_documents.error());
    auto current_documents =
            impl_->storage->scan_conversation_documents(parent, DocumentPoint::current(), static_cast<std::size_t>(-1));
    if (!current_documents) return std::unexpected(current_documents.error());
    struct Copy {
        DocumentRecord record;
        DocumentPoint point;
    };
    std::vector<Copy> copies;
    std::set<std::string> addresses;
    const auto gather = [&](const std::vector<DocumentRecord>& documents,
                                DocumentPoint point,
                                DocumentFork policy) -> support::ExpectedVoid {
        for (const DocumentRecord& source : documents) {
            if (source.fork != policy) continue;
            const std::string address = source.kind + "\n" + source.key.value_or("");
            if (!addresses.insert(address).second) {
                return std::unexpected(validation_error("Multiple documents share fork address " + source.kind));
            }
            copies.push_back(Copy{.record = source, .point = point});
        }
        return {};
    };
    if (auto as_of = gather(
                *as_of_documents, DocumentPoint::as_of(selected_stored->value().commit_seq), DocumentFork::AsOf);
            !as_of)
        return std::unexpected(as_of.error());
    if (auto current = gather(*current_documents, DocumentPoint::current(), DocumentFork::Current); !current) {
        return std::unexpected(current.error());
    }
    auto id = impl_->storage->mint_id();
    if (!id) return std::unexpected(id.error());
    ConversationRecord record{.id = ConversationId{.value = *id},
            .parent = ConversationRecord::Parent{.conversation_id = parent, .at = at}};
    std::vector<StorageWrite> staged;
    staged.push_back(StorageWrite{.type = "conversation", .conversation = record});
    for (const Copy& source : copies) {
        auto document_id = impl_->storage->mint_id();
        if (!document_id) return std::unexpected(std::move(document_id.error()));
        DocumentRecord copy = source.record;
        copy.id = DocumentId{.value = *document_id};
        copy.created_at = 0;
        copy.retired_at.reset();
        copy.conversation_id = record.id;
        staged.push_back(StorageWrite{.type = "document.copy",
                .document_record = std::move(copy),
                .document_copy_source = DocumentCopySource{.id = source.record.id, .at = source.point}});
    }
    impl_->writes.insert(
            impl_->writes.end(), std::make_move_iterator(staged.begin()), std::make_move_iterator(staged.end()));
    impl_->has_table_write = true;
    return record;
}

support::Expected<ConversationRecord> Transaction::create_conversation(TaskId owner) {
    if (auto rejected = impl_->reject_write(); !rejected) {
        return std::unexpected(std::move(rejected.error()));
    }
    auto owner_record = impl_->current_task(owner);
    if (!owner_record) {
        return std::unexpected(std::move(owner_record.error()));
    }
    if (!(*owner_record).has_value()) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation,
                "Conversation owner task " + std::to_string(owner.value) + " does not exist"));
    }
    // Liveness is judged on the owner's final candidate at assembly time
    // (frozen pi `Tx.#validateOwners`), after every write of this transaction
    // has staged; here only existence is validated.
    auto id = impl_->storage->mint_id();
    if (!id) {
        return std::unexpected(std::move(id.error()));
    }
    ConversationRecord record{.id = ConversationId{.value = *id},
            .owner = ConversationOwner{.conversation_id = (*owner_record)->conversation_id, .task_id = owner}};
    impl_->has_table_write = true;
    impl_->writes.push_back(StorageWrite{.type = "conversation", .conversation = record});
    return record;
}

support::Expected<EntryRecord> Transaction::append_entry(ConversationId conversation_id,
        std::string kind,
        chord::Json data,
        std::optional<EntryHeadVariant> head,
        std::vector<chord::Json> model,
        std::optional<TaskId> by_task_id,
        std::vector<ContextEdit> edits) {
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
            .edits = std::move(edits),
            .by_task_id = by_task_id};
    impl_->has_table_write = true;
    impl_->writes.push_back(StorageWrite{.type = "entry", .entry = record});
    return record;
}

support::Expected<std::vector<chord::Json>> Transaction::context_messages(
        ConversationId conversation_id, std::optional<EntryId> at) {
    if (auto rejected = impl_->reject_read("context"); !rejected) return std::unexpected(rejected.error());
    auto entries = impl_->storage->scan_entries(conversation_id, static_cast<std::size_t>(-1));
    if (!entries) return std::unexpected(entries.error());
    std::ranges::reverse(*entries);
    if (at.has_value()) {
        const auto selected =
                std::ranges::find_if(*entries, [at](const EntryRecord& entry) { return entry.id == *at; });
        if (selected == entries->end()) {
            return std::unexpected(
                    validation_error("Entry " + std::to_string(at->value) + " is not visible from conversation " +
                                     std::to_string(conversation_id.value)));
        }
        entries->erase(std::next(selected), entries->end());
    }
    std::optional<EntryId> head;
    for (const EntryRecord& entry : *entries)
        if (entry.head.has_value()) head = entry.head;
    if (head.has_value()) {
        const auto first =
                std::ranges::find_if(*entries, [head](const EntryRecord& entry) { return entry.id == *head; });
        if (first == entries->end()) return std::unexpected(validation_error("Context head is not visible"));
        entries->erase(entries->begin(), first);
    }
    std::map<std::int64_t, ContextEdit> edits;
    for (const EntryRecord& entry : *entries)
        for (const ContextEdit& edit : entry.edits)
            edits.insert_or_assign(edit.target.value, edit);
    std::vector<chord::Json> messages;
    std::vector<std::vector<chord::Json>> open;
    for (const EntryRecord& entry : *entries) {
        if (entry.head.has_value() && entry.id != *head) continue;
        const auto edit = edits.find(entry.id.value);
        if (edit != edits.end() && edit->second.action == "omit") continue;
        const std::vector<chord::Json>& contribution =
                edit != edits.end() && edit->second.action == "replace" ? edit->second.messages : entry.model;
        for (const chord::Json& message : contribution) {
            const auto* object = message.as_object();
            if (object == nullptr) continue;
            const auto role = object->find("role");
            if (role == object->end() || role->second.as_string() == nullptr) continue;
            const std::string& role_name = *role->second.as_string();
            if (role_name == "assistant") {
                auto stop_reason = object->find("stopReason");
                if (stop_reason != object->end() && stop_reason->second.as_string() != nullptr &&
                        (*stop_reason->second.as_string() == "aborted" || *stop_reason->second.as_string() == "error" ||
                                *stop_reason->second.as_string() == "deferred"))
                    continue;
                if (open.size() > 1) messages.insert(messages.end(), open.begin(), open.end() - 1);
                open.clear();
            }
            open.push_back({message});
        }
    }
    for (const auto& group : open)
        messages.insert(messages.end(), group.begin(), group.end());
    return messages;
}

support::Expected<std::vector<chord::Json>> Transaction::conversation_messages(ConversationId conversation_id) {
    if (auto rejected = impl_->reject_read("conversationMessages"); !rejected) {
        return std::unexpected(rejected.error());
    }
    auto entries = impl_->storage->scan_entries(conversation_id, static_cast<std::size_t>(-1));
    if (!entries) return std::unexpected(entries.error());
    std::ranges::reverse(*entries);
    std::optional<EntryId> head;
    for (const EntryRecord& entry : *entries)
        if (entry.head.has_value()) head = entry.head;
    if (head.has_value()) {
        const auto first =
                std::ranges::find_if(*entries, [head](const EntryRecord& entry) { return entry.id == *head; });
        if (first == entries->end()) return std::unexpected(validation_error("Context head is not visible"));
        entries->erase(entries->begin(), first);
    }
    std::map<std::int64_t, ContextEdit> edits;
    for (const EntryRecord& entry : *entries) {
        for (const ContextEdit& edit : entry.edits)
            edits.insert_or_assign(edit.target.value, edit);
    }
    std::vector<chord::Json> messages;
    std::vector<chord::Json> open;
    for (const EntryRecord& entry : *entries) {
        if (entry.kind == "pi.reset") {
            messages.clear();
            open.clear();
        }
        if (entry.head.has_value() && entry.id != *head) continue;
        const auto edit = edits.find(entry.id.value);
        if (edit != edits.end() && edit->second.action == "omit") continue;
        const std::vector<chord::Json>& contribution =
                edit != edits.end() && edit->second.action == "replace" ? edit->second.messages : entry.model;
        for (const chord::Json& message : contribution) {
            const auto* object = message.as_object();
            if (object == nullptr) continue;
            const auto role = object->find("role");
            if (role == object->end() || role->second.as_string() == nullptr) continue;
            const std::string& role_name = *role->second.as_string();
            if (role_name == "assistant") {
                const auto stop_reason = object->find("stopReason");
                if (stop_reason != object->end() && stop_reason->second.as_string() != nullptr &&
                        (*stop_reason->second.as_string() == "aborted" || *stop_reason->second.as_string() == "error" ||
                                *stop_reason->second.as_string() == "deferred"))
                    continue;
                if (!open.empty()) {
                    messages.insert(messages.end(), open.begin(), open.end());
                    open.clear();
                }
            }
            open.push_back(message);
        }
    }
    messages.insert(messages.end(), open.begin(), open.end());
    return messages;
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
    if (auto scoped = check_address(definition, address); !scoped) {
        return std::unexpected(std::move(scoped.error()));
    }
    // A family member is created from the caller's first seed; a singleton
    // definition owns its initial value.
    if (definition.family()) {
        return std::unexpected(
                validation_error("Document " + definition.kind() + " requires a family seed to create a member"));
    }
    if (auto owner = impl_->check_document_owner(address); !owner) {
        return std::unexpected(std::move(owner.error()));
    }

    // One transaction acquires one draft per address: a second doc() returns
    // the same staged draft (frozen pi `Tx.#latestDocumentByAddress`).
    const std::string key = document_address_key(definition.kind(), address);
    const auto latest = impl_->latest_documents.find(key);
    const bool retired_in_tx = latest != impl_->latest_documents.end() && impl_->documents[latest->second].retire;
    if (latest != impl_->latest_documents.end() && !retired_in_tx) {
        StagedDocument& staged = impl_->documents[latest->second];
        if (staged.change.has_value()) {
            return staged.change->draft();
        }
    }

    // The incarnation is loaded from the committed state when it exists, and
    // created otherwise; a reacquisition after a retirement staged in this
    // transaction creates the replacement incarnation instead of loading the
    // retiring one. A document write is not a table write: reading and
    // writing documents stays available after the first table write.
    std::optional<CachedDocument> loaded;
    if (!retired_in_tx && impl_->session != nullptr) {
        auto found = impl_->session->load_document(definition, address);
        if (!found) {
            return std::unexpected(std::move(found.error()));
        }
        loaded = std::move(*found);
    }

    chord::Json base;
    DocumentRecord record;
    bool created = false;
    if (loaded.has_value()) {
        if (auto scoped = check_record_scope(definition, loaded->record); !scoped) {
            return std::unexpected(std::move(scoped.error()));
        }
        if (auto current = check_record_version(definition, loaded->record, loaded->stored_version); !current) {
            return std::unexpected(std::move(current.error()));
        }
        record = loaded->record;
        base = loaded->value;
    } else {
        auto id = impl_->storage->mint_id();
        if (!id) {
            return std::unexpected(std::move(id.error()));
        }
        // The frozen backend stamps the incarnation with the sequence of the
        // commit that created it; the Session does not know that sequence
        // before the commit settles, so the record keeps the placeholder and
        // the Session stamps it after the settlement.
        record = document_create_record(definition, address, DocumentId{.value = *id});
        base = definition.initial_value();
        created = true;
    }

    auto tracker = chord::track(std::move(base));
    auto change = tracker.begin_change();
    auto draft = change.draft();

    impl_->documents.push_back(StagedDocument{.record = record,
            .address = address,
            .kind = definition.kind(),
            .tracker = std::move(tracker),
            .change = std::move(change),
            .created = created,
            .stored_version = loaded.has_value() ? loaded->stored_version : definition.version(),
            .deltas_since_base = loaded.has_value() ? loaded->deltas_since_base : 0,
            .retire = false,
            .definition_version = definition.version(),
            .checkpoint = detail::DocumentDefinitionAccess::checkpoint_hook(definition)});
    impl_->latest_documents[key] = impl_->documents.size() - 1;
    return draft;
}

support::Expected<chord::Draft> Transaction::doc(
        const DocumentDefinition& definition, DocumentAddress address, chord::Json seed) {
    if (auto rejected = impl_->reject_write(); !rejected) {
        return std::unexpected(std::move(rejected.error()));
    }
    if (auto scoped = check_address(definition, address); !scoped) {
        return std::unexpected(std::move(scoped.error()));
    }
    if (!definition.family()) {
        return std::unexpected(validation_error("Document " + definition.kind() + " is not a family"));
    }
    if (auto owner = impl_->check_document_owner(address); !owner) {
        return std::unexpected(std::move(owner.error()));
    }

    const std::string key = document_address_key(definition.kind(), address);
    const auto latest = impl_->latest_documents.find(key);
    const bool retired_in_tx = latest != impl_->latest_documents.end() && impl_->documents[latest->second].retire;
    if (latest != impl_->latest_documents.end() && !retired_in_tx) {
        StagedDocument& staged = impl_->documents[latest->second];
        if (staged.change.has_value()) {
            return staged.change->draft();
        }
    }

    std::optional<CachedDocument> loaded;
    if (!retired_in_tx && impl_->session != nullptr) {
        auto found = impl_->session->load_document(definition, address);
        if (!found) {
            return std::unexpected(std::move(found.error()));
        }
        loaded = std::move(*found);
    }

    chord::Json base;
    DocumentRecord record;
    bool created = false;
    if (loaded.has_value()) {
        if (auto scoped = check_record_scope(definition, loaded->record); !scoped) {
            return std::unexpected(std::move(scoped.error()));
        }
        if (auto current = check_record_version(definition, loaded->record, loaded->stored_version); !current) {
            return std::unexpected(std::move(current.error()));
        }
        // An existing member ignores the seed.
        record = loaded->record;
        base = loaded->value;
    } else {
        auto id = impl_->storage->mint_id();
        if (!id) {
            return std::unexpected(std::move(id.error()));
        }
        record = document_create_record(definition, address, DocumentId{.value = *id});
        base = definition.initial_value(seed);
        created = true;
    }

    auto tracker = chord::track(std::move(base));
    auto change = tracker.begin_change();
    auto draft = change.draft();

    impl_->documents.push_back(StagedDocument{.record = record,
            .address = address,
            .kind = definition.kind(),
            .tracker = std::move(tracker),
            .change = std::move(change),
            .created = created,
            .stored_version = loaded.has_value() ? loaded->stored_version : definition.version(),
            .deltas_since_base = loaded.has_value() ? loaded->deltas_since_base : 0,
            .retire = false,
            .definition_version = definition.version(),
            .checkpoint = detail::DocumentDefinitionAccess::checkpoint_hook(definition)});
    impl_->latest_documents[key] = impl_->documents.size() - 1;
    return draft;
}

support::ExpectedVoid Transaction::retire_doc(const DocumentDefinition& definition, DocumentAddress address) {
    if (auto rejected = impl_->reject_write(); !rejected) {
        return std::unexpected(std::move(rejected.error()));
    }
    if (auto scoped = check_address(definition, address); !scoped) {
        return std::unexpected(std::move(scoped.error()));
    }
    // A retirement of an incarnation this transaction acquired persists its
    // final content first: the retirement is the last revision.
    for (StagedDocument& document : impl_->documents) {
        if (document.address != address || document.kind != definition.kind()) {
            continue;
        }
        if (document.retire) {
            return {};
        }
        if (auto scoped = check_record_scope(definition, document.record); !scoped) {
            return std::unexpected(std::move(scoped.error()));
        }
        document.retire = true;
        impl_->latest_documents[document_address_key(definition.kind(), address)] =
                static_cast<std::size_t>(&document - impl_->documents.data());
        return {};
    }

    if (auto owner = impl_->check_document_owner(address); !owner) {
        return std::unexpected(std::move(owner.error()));
    }
    std::optional<CachedDocument> loaded;
    if (impl_->session != nullptr) {
        auto found = impl_->session->load_document(definition, address);
        if (!found) {
            return std::unexpected(std::move(found.error()));
        }
        loaded = std::move(*found);
    }
    if (!loaded.has_value()) {
        // An address with no current incarnation stages nothing.
        return {};
    }
    if (auto scoped = check_record_scope(definition, loaded->record); !scoped) {
        return std::unexpected(std::move(scoped.error()));
    }
    impl_->documents.push_back(StagedDocument{.record = loaded->record,
            .address = address,
            .kind = definition.kind(),
            .tracker = chord::track(loaded->value),
            .change = std::nullopt,
            .created = false,
            .stored_version = loaded->stored_version,
            .deltas_since_base = loaded->deltas_since_base,
            .retire = true,
            .definition_version = definition.version(),
            .checkpoint = detail::DocumentDefinitionAccess::checkpoint_hook(definition)});
    impl_->latest_documents[document_address_key(definition.kind(), address)] = impl_->documents.size() - 1;
    return {};
}

support::Expected<TaskId> Transaction::create_task(
        const TaskDefinition& definition, chord::Json input, TaskOptions options) {
    if (auto rejected = impl_->reject_write(); !rejected) {
        return std::unexpected(std::move(rejected.error()));
    }
    std::optional<TaskRecord> owner_record;
    if (options.owner.has_value()) {
        auto current = impl_->current_task(*options.owner);
        if (!current) {
            return std::unexpected(std::move(current.error()));
        }
        if (!(*current).has_value()) {
            return std::unexpected(support::make_error(support::ErrorCode::Validation,
                    "Task owner " + std::to_string(options.owner->value) + " does not exist"));
        }
        owner_record = **current;
        // Liveness is judged on the owner's final candidate at assembly time
        // (frozen pi `Tx.#validateOwners`), after every write of this
        // transaction has staged; here only shape is validated.
        if (options.background) {
            return std::unexpected(
                    support::make_error(support::ErrorCode::Validation, "A child task cannot be background"));
        }
        if (options.conversation_id.has_value() && *options.conversation_id != owner_record->conversation_id) {
            return std::unexpected(support::make_error(support::ErrorCode::Validation,
                    "A child task lives in its owner's conversation " +
                            std::to_string(owner_record->conversation_id.value)));
        }
    }
    const ConversationId conv_id = owner_record.has_value() ? owner_record->conversation_id
                                                            : options.conversation_id.value_or(ConversationId{1});
    if (owner_record.has_value()) {
        // The child hangs from its owner's conversation, which must exist.
        if (auto conversation = impl_->storage->conversation(conv_id); !conversation) {
            return std::unexpected(std::move(conversation.error()));
        }
    }
    auto id = impl_->storage->mint_id();
    if (!id) {
        return std::unexpected(std::move(id.error()));
    }
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
    impl_->stage_task(record);
    impl_->created_tasks.insert(record.id.value);
    return TaskId{.value = *id};
}

support::Expected<std::optional<TaskRecord>> Transaction::task(TaskId id) {
    if (auto rejected = impl_->reject_read("task"); !rejected) {
        return std::unexpected(std::move(rejected.error()));
    }
    return impl_->storage->task(id);
}

support::Expected<std::vector<TaskRecord>> Transaction::overlay_tasks() {
    if (impl_->sealed) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation, "Transaction has settled"));
    }
    auto committed = impl_->storage->scan_tasks(1024);
    if (!committed) {
        return std::unexpected(committed.error());
    }
    std::map<std::int64_t, TaskRecord> merged;
    for (TaskRecord& record : *committed) {
        merged.insert_or_assign(record.id.value, std::move(record));
    }
    for (const auto& [id, record] : impl_->staged_tasks) {
        merged.insert_or_assign(id, record);
    }
    std::vector<TaskRecord> candidates;
    candidates.reserve(merged.size());
    for (auto& [id, record] : merged) {
        (void)id;
        candidates.push_back(std::move(record));
    }
    return candidates;
}

support::ExpectedVoid Transaction::set_task(TaskRecord record) {
    if (auto rejected = impl_->reject_write(); !rejected) {
        return std::unexpected(std::move(rejected.error()));
    }
    impl_->has_table_write = true;
    impl_->stage_task(record);
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

bool Session::failed() const noexcept {
    std::lock_guard lock(impl_->mutation_line);
    return impl_->failed;
}

const std::optional<std::string>& Session::failure_message() const noexcept { return impl_->failure_message; }

SessionEnd Session::closed() const noexcept {
    std::lock_guard lock(impl_->mutation_line);
    if (impl_->failed) {
        return SessionEnd{.reason = SessionEndReason::Failed, .failed = true, .error_message = impl_->failure_message};
    }
    if (impl_->storage_closed) {
        return SessionEnd{.reason = SessionEndReason::Closed, .failed = false, .error_message = std::nullopt};
    }
    return SessionEnd{.reason = SessionEndReason::Open, .failed = false, .error_message = std::nullopt};
}

support::AsyncResult<void> Session::commit(std::move_only_function<support::Expected<void>(Transaction&)> change) {
    std::lock_guard lock(impl_->mutation_line);
    if (impl_->failed) {
        return support::AsyncResult<void>{support::ExpectedVoid{std::unexpected(session_failed_error())}};
    }
    if (impl_->closed) {
        return support::AsyncResult<void>{support::ExpectedVoid{std::unexpected(session_closed_error())}};
    }
    return support::AsyncResult<void>{support::AsyncProducer<void, support::Error>{
            [session = this, change = std::move(change)](
                    support::AsyncCompletion<void, support::Error> completion) mutable noexcept {
                std::lock_guard lock(session->impl_->mutation_line);
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
            if (document.change.has_value()) {
                document.change->abort();
            }
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
            if (document.change.has_value()) {
                document.change->abort();
            }
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

    // New owned work needs a live owner judged on the owner's final candidate
    // in this commit: a task cannot take a child task or a task-owned
    // conversation in the commit that settles it (frozen pi `Tx.#validateOwners`
    // runs at assembly over every owner edge, against staged candidates first
    // and the committed record otherwise).
    const auto final_candidate = [this, &transaction](TaskId owner) -> support::Expected<std::optional<TaskRecord>> {
        const auto staged = transaction.impl_->staged_tasks.find(owner.value);
        if (staged != transaction.impl_->staged_tasks.end()) {
            return std::optional<TaskRecord>{staged->second};
        }
        return storage->task(owner);
    };
    for (const std::int64_t created : transaction.impl_->created_tasks) {
        const auto staged = transaction.impl_->staged_tasks.find(created);
        if (staged == transaction.impl_->staged_tasks.end() || !staged->second.owner.has_value()) continue;
        auto owner = final_candidate(*staged->second.owner);
        if (!owner) {
            return reject_assembly(std::move(owner.error()));
        }
        if (!owner->has_value()) continue; // create-time existence check already rejected this
        if (auto live = require_live_owner("Task owner", *staged->second.owner, **owner); !live) {
            return reject_assembly(std::move(live.error()));
        }
    }
    for (const StorageWrite& write : transaction.impl_->writes) {
        if (write.type != "conversation" || !write.conversation.has_value() || !write.conversation->owner.has_value()) {
            continue;
        }
        auto owner = final_candidate(write.conversation->owner->task_id);
        if (!owner) {
            return reject_assembly(std::move(owner.error()));
        }
        if (!owner->has_value()) continue; // create-time existence check already rejected this
        if (auto live = require_live_owner("Conversation owner task", write.conversation->owner->task_id, **owner);
                !live) {
            return reject_assembly(std::move(live.error()));
        }
    }

    auto state = std::make_shared<Session::Impl::CommitState>();
    state->session = this;
    state->storage = storage;
    state->documents = std::move(transaction.impl_->documents);
    state->writes = std::move(transaction.impl_->writes);
    transaction.impl_->sealed = true;

    // The staged documents are prepared before the batch is admitted, so a
    // prepared draft is never observable before the storage settles it. A
    // retirement of an incarnation this transaction never acquired has no
    // change to prepare.
    for (StagedDocument& document : state->documents) {
        if (!document.change.has_value()) {
            state->prepared.push_back(std::nullopt);
            continue;
        }
        auto prepared = document.change->prepare();
        if (!prepared) {
            return support::AsyncResult<void>{support::ExpectedVoid{std::unexpected(std::move(prepared.error()))}};
        }
        state->prepared.push_back(std::move(*prepared));
    }

    // The document writes join the batch. A creation carries the record and
    // the base value; a copy carries its persisted source instead of content;
    // a change carries the incarnation and either a complete base or the exact
    // operations. A loaded incarnation that prepared no operations writes
    // nothing, so acquiring a document without modifying it adds no write to
    // the batch (frozen pi `Transaction.#assemble`).
    for (std::size_t index = 0; index < state->documents.size(); ++index) {
        StagedDocument& document = state->documents[index];
        const std::optional<chord::Prepared>& prepared = state->prepared[index];
        const bool has_change = prepared.has_value();
        const bool migration_only = !document.created && document.stored_version < document.definition_version;
        const bool written =
                document.created || document.retire || migration_only || (has_change && !prepared->ops().empty());
        // A checkpoint predicate decides whether an ordinary change stores a
        // complete base instead of a delta (frozen pi `checkpointWhen`). The
        // predicate runs last, after every validation.
        const bool checkpoint = !document.created && !migration_only && has_change && !prepared->ops().empty() &&
                                document.checkpoint != nullptr &&
                                (*document.checkpoint)(prepared->value(), prepared->ops(), document.deltas_since_base);
        const bool base = document.created || migration_only || checkpoint;
        state->document_written.push_back(written);
        state->document_base.push_back(base);
        if (!written) continue;
        // A retirement applies after any creation of the same incarnation so
        // the batch is ordered for the storage; the publication stays the
        // canonical retirement either way (frozen pi `Transaction.#assemble`).
        const bool born_and_retired = document.created && document.retire;
        if (document.retire && !born_and_retired) {
            state->writes.push_back(StorageWrite{.type = "document.retire", .document_id = document.record.id});
            continue;
        }
        if (document.created && document.fork_source.has_value()) {
            state->writes.push_back(StorageWrite{.type = "document.copy",
                    .document_record = document.record,
                    .document_copy_source = document.fork_source,
                    .document_version = document.definition_version});
            continue;
        }
        StorageWrite write{.type = document.created ? "document.create" : "document.change",
                .document_version = document.definition_version};
        if (document.created) {
            write.document_record = document.record;
        } else {
            write.document_id = document.record.id;
            if (!base) {
                write.document_ops = prepared->ops();
            }
        }
        write.document_value = prepared->value();
        state->writes.push_back(std::move(write));
        if (born_and_retired) {
            state->writes.push_back(StorageWrite{.type = "document.retire", .document_id = document.record.id});
        }
    }

    if (state->writes.empty()) {
        return support::AsyncResult<void>{support::ExpectedVoid{}};
    }

    return support::AsyncResult<void>{support::AsyncProducer<void, support::Error>{
            [state = std::move(state)](support::AsyncCompletion<void, support::Error> completion) mutable noexcept {
                state->storage->commit(state->writes)
                        .start([state, completion = std::move(completion)](
                                       std::expected<Seq, support::Error> outcome) mutable noexcept {
                            std::lock_guard lock(state->session->mutation_line);
                            support::ExpectedVoid settled = state->settle(std::move(outcome));
                            completion(std::move(settled));
                        });
            }}};
}

support::AsyncResult<void> Session::close() {
    std::lock_guard lock(impl_->mutation_line);
    impl_->closed = true;
    impl_->notify_close();
    impl_->run_next_commit();
    impl_->finish_close_if_ready();
    if (impl_->storage_closed) {
        return support::AsyncResult<void>{support::ExpectedVoid{}};
    }
    return support::AsyncResult<void>{support::AsyncProducer<void, support::Error>{
            [session = this](support::AsyncCompletion<void, support::Error> completion) mutable noexcept {
                std::lock_guard lock(session->impl_->mutation_line);
                if (session->impl_->storage_closed) {
                    completion(support::ExpectedVoid{});
                    return;
                }
                session->impl_->close_completions.push_back(std::move(completion));
            }}};
}

support::ExpectedVoid Session::subscribe_commits(CommitListener listener) {
    std::lock_guard lock(impl_->mutation_line);
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
    std::lock_guard lock(impl_->mutation_line);
    if (impl_->closed) {
        return std::unexpected(session_closed_error());
    }
    impl_->close_listeners.push_back(std::move(listener));
    return {};
}

support::AsyncResult<std::optional<chord::Json>> Session::snapshot(
        const DocumentDefinition& definition, DocumentAddress address) {
    std::lock_guard lock(impl_->mutation_line);
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

support::AsyncResult<std::optional<chord::Json>> Session::snapshot_as_of(
        const DocumentDefinition& definition, DocumentAddress address, EntryId at) {
    std::lock_guard lock(impl_->mutation_line);
    if (impl_->failed) {
        return support::AsyncResult<std::optional<chord::Json>>{std::unexpected(session_failed_error())};
    }
    if (impl_->closed) {
        return support::AsyncResult<std::optional<chord::Json>>{std::unexpected(session_closed_error())};
    }
    if (address.scope() != DocumentScope::Conversation) {
        return support::AsyncResult<std::optional<chord::Json>>{
                std::unexpected(validation_error("Session.snapshot_as_of() requires a conversation document"))};
    }
    if (definition.scope() != DocumentScope::Conversation) {
        return support::AsyncResult<std::optional<chord::Json>>{
                std::unexpected(validation_error("Session.snapshot_as_of() requires a conversation document"))};
    }
    if (definition.history() != DocumentHistory::Rewindable) {
        return support::AsyncResult<std::optional<chord::Json>>{
                std::unexpected(validation_error("Document " + definition.kind() + " does not retain history"))};
    }

    // The entry resolves to the commit that appended it, and the document is
    // materialized at that commit (frozen pi `Session.snapshotAsOf`).
    auto stored_entry = impl_->storage->entry(at);
    if (!stored_entry) {
        return support::AsyncResult<std::optional<chord::Json>>{std::unexpected(std::move(stored_entry.error()))};
    }
    if (!(*stored_entry).has_value() || (*stored_entry)->entry.conversation_id != *address.conversation_id) {
        return support::AsyncResult<std::optional<chord::Json>>{std::unexpected(
                validation_error("Entry " + std::to_string(at.value) + " is not visible from conversation " +
                                 std::to_string(address.conversation_id->value)))};
    }
    const Seq commit_seq = (*stored_entry)->commit_seq;

    auto found = impl_->storage->find_document(definition.kind(), address, DocumentPoint::as_of(commit_seq));
    if (!found) {
        return support::AsyncResult<std::optional<chord::Json>>{std::unexpected(std::move(found.error()))};
    }
    if (!(*found).has_value()) {
        return support::AsyncResult<std::optional<chord::Json>>{std::optional<chord::Json>{}};
    }
    const DocumentRecord& record = **found;
    auto stored = impl_->storage->document(record.id, DocumentPoint::as_of(commit_seq));
    if (!stored) {
        return support::AsyncResult<std::optional<chord::Json>>{std::unexpected(std::move(stored.error()))};
    }
    if (!(*stored).has_value()) {
        return support::AsyncResult<std::optional<chord::Json>>{std::unexpected(validation_error(
                "Historical document " + std::to_string(record.id.value) + " (" + record.kind + ") cannot be read"))};
    }
    auto value = materialize_document(definition, record, (*stored)->version, (*stored)->value);
    if (!value) {
        return support::AsyncResult<std::optional<chord::Json>>{std::unexpected(std::move(value.error()))};
    }
    return support::AsyncResult<std::optional<chord::Json>>{std::optional<chord::Json>{std::move(*value)}};
}

support::AsyncResult<std::optional<StoredDocument>> Session::document(DocumentId id, DocumentPoint at) {
    std::lock_guard lock(impl_->mutation_line);
    if (impl_->failed) {
        return support::AsyncResult<std::optional<StoredDocument>>{std::unexpected(session_failed_error())};
    }
    if (impl_->closed) {
        return support::AsyncResult<std::optional<StoredDocument>>{std::unexpected(session_closed_error())};
    }
    auto stored = impl_->storage->document(id, at);
    if (!stored) {
        return support::AsyncResult<std::optional<StoredDocument>>{std::unexpected(std::move(stored.error()))};
    }
    return support::AsyncResult<std::optional<StoredDocument>>{std::move(*stored)};
}

support::AsyncResult<std::optional<DocumentWatch>> Session::watch_doc(
        const DocumentDefinition& definition, DocumentAddress address) {
    std::lock_guard lock(impl_->mutation_line);
    if (impl_->failed) {
        return support::AsyncResult<std::optional<DocumentWatch>>{std::unexpected(session_failed_error())};
    }
    if (impl_->closed) {
        return support::AsyncResult<std::optional<DocumentWatch>>{std::unexpected(session_closed_error())};
    }
    auto loaded = impl_->load_document(definition, address);
    if (!loaded) {
        return support::AsyncResult<std::optional<DocumentWatch>>{std::unexpected(std::move(loaded.error()))};
    }
    if (!(*loaded).has_value()) {
        return support::AsyncResult<std::optional<DocumentWatch>>{std::optional<DocumentWatch>{}};
    }

    auto watch_impl = std::make_shared<DocumentWatch::Impl>();
    watch_impl->value = (*loaded)->value;
    watch_impl->observed_version = (*loaded)->value_version;

    // The watch receives every committed frame of this incarnation from now
    // on; the frames committed before it is started are buffered.
    const std::string key = document_address_key(definition.kind(), address);
    impl_->watches[key].push_back(watch_impl);
    return support::AsyncResult<std::optional<DocumentWatch>>{std::optional<DocumentWatch>{DocumentWatch{watch_impl}}};
}

} // namespace cch::durable
