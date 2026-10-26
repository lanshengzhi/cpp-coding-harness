#pragma once

#include <cch/ai/Models.hpp>
#include <cch/durable/Records.hpp>
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

/// Stream options selected by the host; no credentials are stored in Session
/// state. Production callers supply `ai::Models::stream` through the AI-owned
/// factory; tests use the same ModelStream contract.
struct HarnessSettings {
    ai::SimpleStreamOptions stream{};
};

struct RootOptions {
    ai::Model model{};
    std::optional<ai::ThinkingLevel> thinking_level{std::nullopt};
};

/// Admission of one foreground input. This bounded workflow uses the durable
/// generation task as its receipt identity; queued/deduplicated Submission
/// storage and inbox policies are not implemented by this handle.
struct Submission {
    TaskId id{};
    ConversationId conversation_id{};
    EntryId entry{};
};

struct SettledSubmission {
    TaskId id{};
    ConversationId conversation_id{};
    EntryId entry{};
    std::optional<EntryId> answer{std::nullopt};
    std::string status{};
    std::optional<std::string> error{std::nullopt};
};

} // namespace cch::durable
