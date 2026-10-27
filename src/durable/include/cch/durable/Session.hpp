#pragma once

#include <cch/durable/Records.hpp>
#include <cch/support/AsyncResult.hpp>
#include <cch/support/Error.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>
namespace cch::durable {

class Storage;
class TaskDefinition;

/// One logical document definition with its initial value (frozen pi
/// `defineDoc`).
///
/// The definition owns the identity of its kind: a token is created once and
/// the kind it carries is part of the public protocol, so a copy of the token
/// is the same definition rather than a second definition claiming the same
/// kind. The token is therefore move-only, and the initial-value operation is
/// a stored move-only operation (CODING_STANDARDS.md §6.2).
///
/// Frozen authority: `CommonDocDefinition.initial(): T` is a method of the
/// definition, so the initial value is supplied once when the token is
/// defined, not per access.
class DocumentDefinition {
public:
    DocumentDefinition(DocumentDefinition&&) noexcept;
    DocumentDefinition& operator=(DocumentDefinition&&) noexcept;
    ~DocumentDefinition();

    DocumentDefinition(const DocumentDefinition&) = delete;
    DocumentDefinition& operator=(const DocumentDefinition&) = delete;

    /// The stable document definition kind (frozen `DocumentDefinition.kind`).
    [[nodiscard]] const std::string& kind() const noexcept;

    /// The initial value of a newly created incarnation (frozen
    /// `DocumentDefinition.initial`).
    [[nodiscard]] chord::Json initial_value() const;

private:
    friend DocumentDefinition define_document(std::string kind, std::move_only_function<chord::Json()> initial);

    struct Impl;

    explicit DocumentDefinition(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};

/// Define one document kind (frozen pi `defineDoc`). The returned token owns
/// the identity of `kind`; the caller keeps the only copy.
[[nodiscard]] DocumentDefinition define_document(std::string kind, std::move_only_function<chord::Json()> initial);

/// Select the entry being appended as the first entry of its active context.
struct SelfEntryHead {};

/// An explicit context head, or the entry being appended (`"self"`).
using EntryHeadVariant = std::variant<EntryId, SelfEntryHead>;

/// One entry record with its application payload decoded to a C++ type.
template <typename Data> struct TypedEntry {
    EntryId id{};
    ConversationId conversation_id{};
    std::string kind{};
    Data data{};
    std::optional<EntryId> head{};
};

/// Definition of an entry kind and its JSON payload codec (frozen pi
/// `defineEntry`). Copies share one kind and codec contract, as upstream entry
/// tokens are reusable handles.
template <typename Data> class EntryDefinition {
public:
    using EncodeHook = std::move_only_function<support::Expected<chord::Json>(const Data&) const>;
    using DecodeHook = std::move_only_function<support::Expected<Data>(const chord::Json&) const>;

    /// The stable entry discriminator carried by this token.
    [[nodiscard]] const std::string& kind() const noexcept { return impl_->kind; }

    /// Whether a stored entry has this token's discriminator (frozen `Entry.is`).
    [[nodiscard]] bool is(const EntryRecord& entry) const noexcept { return entry.kind == impl_->kind; }

    /// Convert a typed payload to its persisted JSON representation.
    [[nodiscard]] support::Expected<chord::Json> encode(const Data& data) const { return impl_->encoder(data); }

    /// Decode a persisted JSON payload to this entry kind's C++ value.
    [[nodiscard]] support::Expected<Data> decode(const chord::Json& data) const { return impl_->decoder(data); }

private:
    struct Impl {
        std::string kind;
        EncodeHook encoder;
        DecodeHook decoder;
    };

    template <typename T>
    friend support::Expected<EntryDefinition<T>> define_entry(
            std::string, typename EntryDefinition<T>::EncodeHook, typename EntryDefinition<T>::DecodeHook);

    EntryDefinition(std::string kind, EncodeHook encoder, DecodeHook decoder)
        : impl_(std::make_shared<Impl>(
                  Impl{.kind = std::move(kind), .encoder = std::move(encoder), .decoder = std::move(decoder)})) {}

    std::shared_ptr<const Impl> impl_;
};

/// Define a typed entry kind with codecs between its C++ payload and stored JSON.
template <typename Data>
[[nodiscard]] support::Expected<EntryDefinition<Data>> define_entry(std::string kind,
        typename EntryDefinition<Data>::EncodeHook encoder,
        typename EntryDefinition<Data>::DecodeHook decoder) {
    if (kind.empty()) {
        return std::unexpected(
                support::make_error(support::ErrorCode::Validation, "Entry kind must be a non-empty string"));
    }
    if (!encoder || !decoder) {
        return std::unexpected(
                support::make_error(support::ErrorCode::Validation, "Entry kind requires payload codecs"));
    }
    return EntryDefinition<Data>{std::move(kind), std::move(encoder), std::move(decoder)};
}

/// Define a JSON-backed entry kind that stores and returns its payload directly.
[[nodiscard]] inline support::Expected<EntryDefinition<chord::Json>> define_entry(std::string kind) {
    return define_entry<chord::Json>(
            std::move(kind),
            [](const chord::Json& data) -> support::Expected<chord::Json> { return data; },
            [](const chord::Json& data) -> support::Expected<chord::Json> { return data; });
}

/// One logical document address: a Session document has no conversation, a
/// conversation document names its conversation (frozen pi `DocumentAddress`).
struct DocumentAddress {
    std::optional<ConversationId> conversation_id;
};

/// The transaction surface of one Session commit callback (frozen pi `Tx`).
///
/// Table reads and creation results are trusted immutable values and may be
/// shared with internal commit state. A table read after the first table write
/// of the same transaction is rejected: read every required row before
/// writing.
class Transaction {
public:
    Transaction(const Transaction&) = delete;
    Transaction(Transaction&&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    Transaction& operator=(Transaction&&) = delete;
    ~Transaction();

    /// Create the reserved root (frozen pi `Tx.createRootConversation`).
    [[nodiscard]] support::Expected<ConversationRecord> create_root_conversation();

    /// Create an ownerless conversation (frozen pi `Tx.createConversation` with
    /// `{kind: "ownerless"}`).
    [[nodiscard]] support::Expected<ConversationRecord> create_conversation();

    /// Create a conversation owned by `owner` (frozen pi `Tx.createConversation`
    /// with `{kind: "task"}`). The owner task must exist and be live: a
    /// missing, terminal, `completing`, or abort-marked owner is rejected, as is
    /// an owner that a later commit of this transaction settles.
    [[nodiscard]] support::Expected<ConversationRecord> create_conversation(TaskId owner);

    /// Append one entry to a conversation (frozen pi `Tx.appendEntry`).
    [[nodiscard]] support::Expected<EntryRecord> append_entry(ConversationId conversation_id,
            std::string kind,
            chord::Json data,
            std::optional<EntryHeadVariant> head = std::nullopt,
            std::vector<chord::Json> model = {},
            std::optional<TaskId> by_task_id = std::nullopt);

    /// Append a typed entry with its kind and payload codec supplied by the
    /// definition (frozen `Tx.appendEntry<D>`); `head` selects an existing
    /// context head or `SelfEntryHead` to select this entry.
    template <typename Data>
    [[nodiscard]] support::Expected<TypedEntry<Data>> append_entry(const EntryDefinition<Data>& definition,
            ConversationId conversation_id,
            Data data,
            std::optional<EntryHeadVariant> head = std::nullopt) {
        auto encoded = definition.encode(data);
        if (!encoded) {
            return std::unexpected(std::move(encoded.error()));
        }
        auto record = append_entry(conversation_id, definition.kind(), std::move(*encoded), head);
        if (!record) {
            return std::unexpected(std::move(record.error()));
        }
        return TypedEntry<Data>{.id = record->id,
                .conversation_id = record->conversation_id,
                .kind = std::move(record->kind),
                .data = std::move(data),
                .head = record->head};
    }

    /// Look up one conversation (frozen pi `Tx.conversation`). Rejected with
    /// `Tx.conversation() cannot read tables after the first table write` when
    /// the transaction already wrote a table.
    [[nodiscard]] support::Expected<std::optional<ConversationRecord>> conversation(ConversationId id);

    /// Look up one entry (frozen pi `Tx.entry`). Rejected with
    /// `Tx.entry() cannot read tables after the first table write` when the
    /// transaction already wrote a table.
    [[nodiscard]] support::Expected<std::optional<EntryRecord>> entry(EntryId id);

    /// Look up an entry through a typed definition. A missing entry or a
    /// different kind returns an empty optional; invalid stored JSON returns
    /// the definition decoder's error.
    template <typename Data>
    [[nodiscard]] support::Expected<std::optional<TypedEntry<Data>>> entry(
            const EntryDefinition<Data>& definition, EntryId id) {
        auto record = entry(id);
        if (!record) {
            return std::unexpected(std::move(record.error()));
        }
        if (!record->has_value() || !definition.is(**record)) {
            return std::optional<TypedEntry<Data>>{};
        }
        auto data = definition.decode((**record).data);
        if (!data) {
            return std::unexpected(std::move(data.error()));
        }
        const EntryRecord& stored = **record;
        return std::optional<TypedEntry<Data>>{std::in_place,
                TypedEntry<Data>{.id = stored.id,
                        .conversation_id = stored.conversation_id,
                        .kind = stored.kind,
                        .data = std::move(*data),
                        .head = stored.head}};
    }

    /// Scan the conversations (frozen pi `Tx.scanConversations`). Rejected
    /// with `Tx.scanConversations() cannot read tables after the first table
    /// write` when the transaction already wrote a table.
    [[nodiscard]] support::Expected<std::vector<ConversationRecord>> scan_conversations(std::size_t limit);

    /// Scan the visible entries of one conversation (frozen pi
    /// `Tx.scanEntries`). Rejected with
    /// `Tx.scanEntries() cannot read tables after the first table write` when
    /// the transaction already wrote a table.
    [[nodiscard]] support::Expected<std::vector<EntryRecord>> scan_entries(
            ConversationId conversation_id, std::size_t limit);

    /// Acquire the draft of one document incarnation, creating the incarnation
    /// when it does not exist yet (frozen pi `Tx.doc`). The returned draft is
    /// a handle: every operation on it fails once the transaction settles.
    [[nodiscard]] support::Expected<chord::Draft> doc(const DocumentDefinition& definition, DocumentAddress address);

    /// Create a task (frozen pi `Tx.createTask`).
    ///
    /// A child task (`options.owner`) is rejected when its owner does not exist,
    /// is terminal, `completing`, or abort-marked, when the child is
    /// `background`, or when it names another conversation than its owner's. The
    /// child inherits its owner's conversation.
    [[nodiscard]] support::Expected<TaskId> create_task(
            const TaskDefinition& definition, chord::Json input, TaskOptions options = {});

    /// Look up one task by ID (frozen pi `Tx.task`).
    [[nodiscard]] support::Expected<std::optional<TaskRecord>> task(TaskId id);

    /// Look up one submission by ID (frozen pi `Tx.submission`). Rejected with
    /// `Tx.submission() cannot read tables after the first table write` when
    /// the transaction already wrote a table.
    [[nodiscard]] support::Expected<std::optional<SubmissionRecord>> submission(SubmissionId id);

    /// Look up one submission by its conversation-scoped dedup key (frozen pi
    /// `Tx.submissionByRequest`). Rejected with
    /// `Tx.submissionByRequest() cannot read tables after the first table
    /// write` when the transaction already wrote a table.
    [[nodiscard]] support::Expected<std::optional<SubmissionRecord>> submission_by_request(
            ConversationId conversation_id, std::string_view request_id);

    /// Set or replace one task record (frozen pi `Tx.setTask`).
    support::ExpectedVoid set_task(TaskRecord record);

    /// The scheduler's view of every task candidate in this transaction: the
    /// committed records with this transaction's staged writes applied (frozen
    /// pi `overlayOf(tx)`). It is not a caller table read, so it stays
    /// available after the first table write; the scheduler uses it to judge a
    /// `completing` hold and to validate a wait, both of which must count work
    /// the same commit creates.
    [[nodiscard]] support::Expected<std::vector<TaskRecord>> overlay_tasks();

    /// Scan task records (frozen pi `Tx.scanTasks`).
    [[nodiscard]] support::Expected<std::vector<TaskRecord>> scan_tasks(std::size_t limit);

    /// Create one submission record with a fresh Session-global ID (frozen pi
    /// `Tx.createSubmission`); no admission rules apply. The supplied record
    /// carries `id == 0`; the staged create works for every initial state.
    [[nodiscard]] support::Expected<SubmissionRecord> create_submission(SubmissionRecord record);

    /// Settle one submission. The change STAGES and resolves at transaction
    /// assembly against this transaction's latest staged candidate first,
    /// then committed state, so it is not a caller table read and works after
    /// the first table write (frozen pi `Tx.settleSubmission`).
    support::ExpectedVoid settle_submission(SubmissionId id, SubmissionSettlement settlement);

    /// Place one queued submission at its transcript entry. The change STAGES
    /// and resolves at transaction assembly like `settle_submission()` (frozen
    /// pi `Tx.placeSubmission`).
    support::ExpectedVoid place_submission(SubmissionId id, EntryId entry);

private:
    friend class Session;

    struct Impl;

    explicit Transaction(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};

/// One committed document frame delivered to a watch (frozen pi `WatchFrame`).
struct DocumentFrame {
    /// The exact adopted immutable revision.
    chord::Json value;
    /// The exact adopted operations; empty for a creation.
    chord::OpBatch ops;
};

/// Receives one committed document frame (frozen pi `WatchHandle.start`).
///
/// Serialized domain: every frame delivery of one watch must run in the one
/// serialized execution domain the caller chooses (the Session's mutation
/// line when the Session publishes from it).
using DocumentFrameListener = std::move_only_function<void(DocumentFrame)>;

/// The terminal result of one document watch (frozen pi `WatchEnd`).
enum class WatchEndReason {
    Stopped,
    Cancelled,
    SessionClosed,
    SessionFailed,
};

/// One committed document watch (frozen pi `DocumentWatch`).
///
/// The watch holds the revision it was acquired at until it is started, and
/// buffers every committed frame until then. A started watch delivers the
/// buffered frames in commit order, one at a time.
class DocumentWatch {
public:
    DocumentWatch(DocumentWatch&&) noexcept;
    DocumentWatch& operator=(DocumentWatch&&) noexcept;
    ~DocumentWatch();

    DocumentWatch(const DocumentWatch&) = delete;
    DocumentWatch& operator=(const DocumentWatch&) = delete;

    /// The acquisition revision before `start`; the latest delivered revision
    /// afterwards.
    [[nodiscard]] const chord::Json& value() const noexcept;

    /// Install the sole listener and deliver every buffered frame in commit
    /// order. Single-use: an already-started or stopped watch is rejected.
    [[nodiscard]] support::ExpectedVoid start(DocumentFrameListener listener);

    /// Idempotently stop future deliveries and return this watch's terminal
    /// result (frozen pi `WatchHandle.stop`).
    [[nodiscard]] WatchEndReason stop();

private:
    friend class Session;

    struct Impl;

    explicit DocumentWatch(std::shared_ptr<Impl> impl);

    /// The watch and the Session's registry share the watch state: the
    /// Session delivers the committed frames to it, and the watch outlives
    /// the Session's own reference to it.
    std::shared_ptr<Impl> impl_;
};

/// Observes every committed batch (frozen pi `Session.subscribeCommits`).
using CommitListener = std::move_only_function<void(const CommitPublication&)>;

/// Observes the end of the Session (frozen pi `Session.subscribeClose`).
using CloseListener = std::move_only_function<void()>;

/// How a Session ended (frozen pi `SessionEnd`).
enum class SessionEndReason {
    /// The Session is still open.
    Open,
    /// The Session ended by an ordinary close.
    Closed,
    /// The Session ended by a failure.
    Failed,
};

/// How a Session ended (frozen pi `SessionEnd`).
struct SessionEnd {
    /// The terminal reason of the Session.
    SessionEndReason reason{SessionEndReason::Open};
    /// Whether the Session ended by a failure.
    bool failed{false};
    /// The error that failed the Session; absent for an ordinary close.
    std::optional<std::string> error_message{};
};

/// One durable Session kernel over one storage backend (frozen pi `Session`).
///
/// The Session owns one mutation line: every commit callback, preparation,
/// storage settlement, adoption and publication runs while that line is held,
/// and only committed state is observable. The line runs in the one
/// serialized execution domain the caller chooses; this package owns no
/// thread, loop or scheduler.
///
/// Storage failures are final. The first error a storage method reports fails
/// the Session: that call gets the error, every later call and every storage
/// call still underway gets the frozen `SessionFailed` message, and the
/// Session then closes itself, storage included.
class Session {
public:
    Session(Session&&) noexcept;
    Session& operator=(Session&&) noexcept;
    ~Session();

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    /// Open a Session over one storage backend (frozen pi `createSession`).
    /// The storage must outlive the Session.
    [[nodiscard]] static std::unique_ptr<Session> create(Storage& storage);

    /// Run one atomic transaction on the Session mutation line (frozen pi
    /// `Session.commit`). The callback receives the transaction's view. A
    /// callback that reports an error rolls the transaction back, publishes
    /// nothing, and leaves the Session usable. Commits execute in admission
    /// order, and the next callback starts after prior Storage settlement.
    /// The Session must outlive the operation until its completion callback
    /// returns, because the mutation line continues there.
    [[nodiscard]] support::AsyncResult<void> commit(
            std::move_only_function<support::Expected<void>(Transaction&)> change);

    /// Seal admission and stop observers immediately, settle admitted commits,
    /// then close Storage (frozen pi `Session.close`). Idempotent. The Session
    /// must outlive the operation until its completion callback returns.
    [[nodiscard]] support::AsyncResult<void> close();

    /// Observe every committed batch (frozen pi `Session.subscribeCommits`).
    /// The listener runs in the Session's mutation line.
    [[nodiscard]] support::ExpectedVoid subscribe_commits(CommitListener listener);

    /// Observe the end of the Session (frozen pi `Session.subscribeClose`).
    [[nodiscard]] support::ExpectedVoid subscribe_close(CloseListener listener);

    /// Read the committed value of one document without creating it (frozen pi
    /// `Session.snapshot`).
    [[nodiscard]] support::AsyncResult<std::optional<chord::Json>> snapshot(
            const DocumentDefinition& definition, DocumentAddress address);

    /// Acquire a watch bound to one committed document incarnation (frozen pi
    /// `Session.watchDoc`). The acquisition revision is the committed value at
    /// the moment of acquisition; every later commit delivers its exact frame.
    [[nodiscard]] support::AsyncResult<std::optional<DocumentWatch>> watch_doc(
            const DocumentDefinition& definition, DocumentAddress address);

    /// Whether the Session has failed (frozen pi `SessionImpl.failure`).
    [[nodiscard]] bool failed() const noexcept;

    /// The error that failed the Session, if one did (frozen pi
    /// `SessionImpl.failure`).
    [[nodiscard]] const std::optional<std::string>& failure_message() const noexcept;

    /// How the Session ended (frozen pi `SessionEnd`). A Session that is still
    /// open reports neither a failure nor a close.
    [[nodiscard]] SessionEnd closed() const noexcept;

private:
    struct Impl;

    friend class Transaction;

    explicit Session(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};

} // namespace cch::durable
