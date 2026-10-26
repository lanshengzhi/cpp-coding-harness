#pragma once

#include <cch/ai/Models.hpp>
#include <cch/durable/Records.hpp>
#include <cch/durable/Session.hpp>
#include <cch/durable/Task.hpp>
#include <cch/support/AsyncResult.hpp>
#include <cch/support/JsonValue.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace cch::durable {

/// The callable part of a frozen pi `ToolRegistration`. The returned blocks
/// become the model-visible tool result; cancellation reaches the physical tool.
using ToolExecuteHook = std::move_only_function<support::AsyncResult<std::vector<ai::Content>>(
        support::JsonValue, std::stop_token)>;

struct ToolRegistration {
    ai::Tool declaration{};
    ToolExecuteHook execute{};
};

/// A constant prompt section (frozen pi `section`). Dynamic renderers and
/// wrapping hooks are separate registry capabilities.
struct PromptSection {
    std::string key{};
    std::string text{};
    bool tag{true};
};

/// Hook handlers registered by an extension for a task kind (frozen pi `HookRegistration`).
struct TaskHookRegistration {
    std::string task{};
    std::map<std::string, TaskHook> handlers{};
};

/// Build a `TaskHookRegistration` for a single named hook handler (frozen pi `hook`).
[[nodiscard]] TaskHookRegistration task_hook(std::string task_kind, std::string hook_name, TaskHook handler);
[[nodiscard]] TaskHookRegistration task_hook(const TaskDefinition& task, std::string hook_name, TaskHook handler);
[[nodiscard]] TaskHookRegistration task_hook(
        const TaskDefinition& task, std::string hook_name, std::move_only_function<support::ExpectedVoid()> handler);

struct Extension {
    std::string name{};
    std::vector<ToolRegistration> tools{};
    std::vector<PromptSection> sections{};
    std::vector<TaskDefinition> tasks{};
    std::vector<TaskHookRegistration> hooks{};
};

/// Live registry for the durable Harness. Install, replace-in-place, uninstall,
/// and request snapshots are serialized internally. Existing registrations stay
/// alive for a request or phase even when another extension is installed.
class Registry {
public:
    Registry();
    Registry(Registry&&) noexcept;
    Registry& operator=(Registry&&) noexcept;
    ~Registry();
    Registry(const Registry&) = delete;
    Registry& operator=(const Registry&) = delete;

    [[nodiscard]] support::ExpectedVoid install(Extension extension);
    void uninstall(std::string_view extension_name);

private:
    friend class Harness;
    friend class WorkflowRuntime;

    struct Snapshot {
        /// Shared immutable registrations keep move-only execute and task/hook
        /// operations alive for every request or phase that captured this snapshot.
        std::vector<std::shared_ptr<ToolRegistration>> tools{};
        std::vector<PromptSection> sections{};
        std::vector<std::shared_ptr<TaskDefinition>> tasks{};
        std::vector<std::shared_ptr<TaskHookRegistration>> hooks{};
    };
    [[nodiscard]] Snapshot snapshot() const;
    [[nodiscard]] std::uint64_t subscribe(std::move_only_function<void()> listener);
    void unsubscribe(std::uint64_t id);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// Queue placement mode: how many queued user items of one kind a boundary
/// places (frozen pi `QueueMode`).
enum class QueueMode {
    /// Place every selected queued item.
    All,
    /// Place only the first selected queued item.
    OneAtATime,
};

/// Stream options selected by the host; no credentials are stored in Session
/// state. Production callers supply `ai::Models::stream` through the AI-owned
/// factory; tests use the same ModelStream contract.
struct HarnessSettings {
    ai::SimpleStreamOptions stream{};
    /// How many queued steers join the run at the postTools boundary (frozen
    /// `Settings.steeringMode`; the frozen default is one-at-a-time).
    QueueMode steering_mode{QueueMode::OneAtATime};
    /// How many queued follow-ups are placed at the final boundary (frozen
    /// `Settings.followUpMode`; the frozen default is one-at-a-time).
    QueueMode follow_up_mode{QueueMode::OneAtATime};
};

struct RootOptions {
    ai::Model model{};
    std::optional<ai::ThinkingLevel> thinking_level{std::nullopt};
};

/// What admission does with an input that reaches a busy conversation
/// (frozen pi `SubmissionDraft.whenBusy`).
enum class WhenBusy {
    /// Place the input at the next postTools boundary of the running run.
    Steer,
    /// Hold the input for the final boundary; it starts the next run.
    FollowUp,
    /// Reject the admission with `ConversationBusy`.
    Reject,
};

/// The entry an input is placed as, instead of a plain `pi.user` (frozen pi
/// `InputEntry`): an application kind — a non-empty string outside the
/// reserved `pi.` prefix — and optional transcript data. The entry's model
/// message is still the input's user message.
struct InputEntry {
    std::string kind{};
    std::optional<chord::Json> data{std::nullopt};
};

/// A passive entry write admitted as a submission (frozen pi `EntryDraft`
/// write body). The frozen `edits` context-override field is not part of the
/// C++ record model: a write carrying `edits` cannot be expressed through this
/// boundary and stays frozen-only.
struct WriteEntry {
    std::string kind{};
    std::optional<chord::Json> data{std::nullopt};
    /// Model messages the entry carries. Presence counts against display-only
    /// even when the array is empty (frozen `isDisplayOnly` present-counts
    /// rule); a display-only write has no `model` and no `head`.
    std::optional<std::vector<chord::Json>> model{std::nullopt};
    /// Context head selection: an existing entry, or `SelfEntryHead` to
    /// select the appended entry itself (frozen `EntryDraft.head`, `"self"`).
    std::optional<EntryHeadVariant> head{std::nullopt};
};

/// One user input admission (frozen pi `SubmissionDraft` input body):
/// the user-message content, how busy admission treats it, and the entry it
/// is placed as.
struct InputSubmissionDraft {
    /// The user-message content placed as the entry's model message.
    chord::Json content{};
    /// Busy-admission mode; absent queues a follow-up.
    std::optional<WhenBusy> when_busy{std::nullopt};
    /// The entry to place the input as; absent places a plain `pi.user`.
    std::optional<InputEntry> entry{std::nullopt};
};

/// One passive entry write admission (frozen pi `SubmissionDraft` write body).
struct WriteSubmissionDraft {
    WriteEntry entry{};
};

/// The variant body of one admission: user input or passive write (frozen pi
/// `SubmissionDraft` union).
using SubmissionDraftBody = std::variant<InputSubmissionDraft, WriteSubmissionDraft>;

/// One admission request: user input that may start a run, or a passive entry
/// write (frozen pi `SubmissionDraft`). A known `request_id` returns the
/// existing submission of the same type without writing.
struct SubmissionDraft {
    /// Host-provided deduplication key, scoped to the conversation (frozen
    /// `SubmissionDraft.requestId`).
    std::optional<std::string> request_id{std::nullopt};
    SubmissionDraftBody body{};
};

/// The settled durable outcome of one submission (frozen pi
/// `SettledSubmissionRecord`): the settled `SubmissionRecord` with status
/// `done` or `unanswered`.
struct SettledSubmission {
    SubmissionId id{};
    ConversationId conversation_id{};
    std::optional<std::string> request_id{std::nullopt};
    /// The submission discriminator: `"input"` or `"write"`.
    std::string type{};
    /// `"done"` or `"unanswered"`.
    std::string status{};
    /// The transcript entry the submission was placed at, when placed.
    std::optional<EntryId> entry{std::nullopt};
    /// The answer entry of a done input submission.
    std::optional<EntryId> answer{std::nullopt};
    /// The terminal reason of an unanswered submission.
    std::optional<std::string> reason{std::nullopt};
    /// Optional structured diagnostic data of an unanswered submission.
    std::optional<chord::Json> detail{std::nullopt};
};

/// Terminal settlement statuses (frozen pi `SubmissionSettlement`).
[[nodiscard]] constexpr bool submission_settled(std::string_view status) {
    return status == "done" || status == "unanswered";
}

/// The durable waiter's view of one submission record (frozen pi
/// `Submissions.load`).
[[nodiscard]] inline SettledSubmission settled_submission(const SubmissionRecord& record) {
    return SettledSubmission{.id = record.id,
            .conversation_id = record.conversation_id,
            .request_id = record.request_id,
            .type = record.type,
            .status = record.status,
            .entry = record.entry,
            .answer = record.answer,
            .reason = record.reason,
            .detail = record.detail};
}

/// Awaitable durable handle for one admitted submission (frozen pi
/// `Submission`). The admitting Harness must outlive the handle and every
/// operation on it.
class Submission {
public:
    /// Read the latest committed submission record (frozen `Submission.status`).
    [[nodiscard]] support::AsyncResult<SubmissionRecord> status() const;

    /// Await the settled record (frozen `Submission.wait`). Cancellation
    /// rejects only this wait with a "stop waiting" cancellation error; the
    /// submission stays placed until it settles.
    [[nodiscard]] support::AsyncResult<SettledSubmission> wait(std::stop_token stop = std::stop_token{}) const;

    /// Withdraw this submission when queued (frozen `Submission.abort`):
    /// `"aborted"`, `"already_placed"` or `"settled"`.
    [[nodiscard]] support::AsyncResult<std::string> abort() const;

    SubmissionId id{};
    ConversationId conversation_id{};

    /// The admitting Harness's waiter registry; the name is public so the
    /// definition can live in the Harness Owner, the members stay private to
    /// it.
    class Registry;

private:
    friend class Harness;

    Submission(SubmissionId submission_id, ConversationId conversation_id, Registry& registry) noexcept
        : id(submission_id), conversation_id(conversation_id), registry_(&registry) {}

    Registry* registry_; // must outlive every operation (the admitting Harness)
};

} // namespace cch::durable
