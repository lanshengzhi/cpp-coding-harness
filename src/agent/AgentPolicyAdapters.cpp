#include <cch/agent/AgentContext.hpp>

#include <stop_token>
#include <utility>

namespace cch::agent {

BeforeToolCallHook adapt_sync_before_tool_call(
    SyncBeforeToolCallPolicy policy) {
    return [policy = std::move(policy)](
               BeforeToolCallContext context,
               std::stop_token) mutable {
        return support::AsyncResult<BeforeToolCallResult>{policy(std::move(context))};
    };
}

BeforeToolCallHook adapt_sync_before_tool_call(
    CancellableSyncBeforeToolCallPolicy policy) {
    return [policy = std::move(policy)](
               BeforeToolCallContext context,
               std::stop_token stop_token) mutable {
        return support::AsyncResult<BeforeToolCallResult>{
            policy(std::move(context), stop_token)};
    };
}

AfterToolCallHook adapt_sync_after_tool_call(
    SyncAfterToolCallPolicy policy) {
    return [policy = std::move(policy)](
               AfterToolCallContext context,
               std::stop_token) mutable {
        return support::AsyncResult<AfterToolCallResult>{policy(std::move(context))};
    };
}

AfterToolCallHook adapt_sync_after_tool_call(
    CancellableSyncAfterToolCallPolicy policy) {
    return [policy = std::move(policy)](
               AfterToolCallContext context,
               std::stop_token stop_token) mutable {
        return support::AsyncResult<AfterToolCallResult>{
            policy(std::move(context), stop_token)};
    };
}

TransformContextHook adapt_sync_transform_context(
    SyncTransformContextPolicy policy) {
    return [policy = std::move(policy)](
               std::vector<ai::MessageVariant> messages,
               std::stop_token) mutable {
        return support::AsyncResult<std::vector<ai::MessageVariant>>{
            policy(std::move(messages))};
    };
}

TransformContextHook adapt_sync_transform_context(
    CancellableSyncTransformContextPolicy policy) {
    return [policy = std::move(policy)](
               std::vector<ai::MessageVariant> messages,
               std::stop_token stop_token) mutable {
        return support::AsyncResult<std::vector<ai::MessageVariant>>{
            policy(std::move(messages), stop_token)};
    };
}

ConvertToLlmHook adapt_sync_convert_to_llm(
    SyncConvertToLlmPolicy policy) {
    return [policy = std::move(policy)](
               std::vector<ai::MessageVariant> messages) mutable {
        return support::AsyncResult<std::vector<ai::MessageVariant>>{
            policy(std::move(messages))};
    };
}

PrepareNextTurnHook adapt_sync_prepare_next_turn(
    SyncPrepareNextTurnPolicy policy) {
    return [policy = std::move(policy)](
               PrepareNextTurnContext context) mutable {
        return support::AsyncResult<std::optional<AgentLoopTurnUpdate>>{
            policy(std::move(context))};
    };
}

ShouldStopAfterTurnHook adapt_sync_should_stop_after_turn(
    SyncShouldStopAfterTurnPolicy policy) {
    return [policy = std::move(policy)](
               PrepareNextTurnContext context) mutable {
        return support::AsyncResult<bool>{policy(std::move(context))};
    };
}

ValidateTurnUpdateHook adapt_sync_validate_turn_update(
    SyncValidateTurnUpdatePolicy policy) {
    return [policy = std::move(policy)](
               AgentLoopTurnUpdate update) mutable {
        return support::AsyncResult<void>{policy(std::move(update))};
    };
}

} // namespace cch::agent
