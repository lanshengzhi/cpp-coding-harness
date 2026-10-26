#pragma once

#include <cch/durable/Harness.hpp>

#include <span>

namespace cch::durable {

/// Harness-owned workflow state. Harness shutdown joins its task invocations
/// before destroying this object; Session, Storage, registry and the clock
/// outlive every operation and task callback below.
class WorkflowRuntime {
public:
    WorkflowRuntime(Session& session, Storage& storage, HarnessOptions& options,
            std::shared_ptr<std::move_only_function<std::int64_t()>> clock);
    [[nodiscard]] TaskDefinition definition();
    [[nodiscard]] support::AsyncResult<ConversationId> root(RootOptions options);
    [[nodiscard]] support::AsyncResult<Submission> submit(
            ConversationId conversation_id, std::string text, const TaskDefinition& definition);
    [[nodiscard]] static support::Expected<SettledSubmission> receipt(const TaskRecord& task);

private:
    struct PromptPlan {
        ai::AiContext context{};
        ai::SystemMessage system{};
    };
    [[nodiscard]] static PromptPlan plan_prompt(const Registry::Snapshot& snapshot,
            std::span<const ai::Tool> shown_tools,
            std::span<const ai::SystemMessageSection> shown_sections,
            std::int64_t timestamp);
    [[nodiscard]] ai::ToolResultMessage execute_tool(const ai::ToolCallContent& call,
            const Registry::Snapshot& snapshot, std::stop_token stop_token);
    [[nodiscard]] support::AsyncResult<void> generate(const TaskRecord& task, TaskRuntime& runtime);
    [[nodiscard]] support::ExpectedVoid run(const TaskRecord& task, TaskRuntime& runtime);
    [[nodiscard]] std::int64_t now() const;

    Session& session_; // outlives every operation (Harness ownership)
    Storage& storage_; // outlives every operation (Harness::open contract)
    Registry empty_registry_;
    Registry& registry_; // outlives the Harness
    ai::ModelStreamFactory stream_;
    HarnessSettings settings_;
    std::shared_ptr<std::move_only_function<std::int64_t()>> clock_;
    ai::Model model_{};
    std::optional<ai::ThinkingLevel> thinking_level_{std::nullopt};
    // debt: the workflow's replay cache covers one open Harness. #1082 must
    // rebuild model context and prompt deltas from committed entries on reopen.
    std::vector<ai::MessageVariant> messages_;
    std::vector<ai::Tool> shown_tools_;
    std::vector<ai::SystemMessageSection> shown_sections_;
};

} // namespace cch::durable
