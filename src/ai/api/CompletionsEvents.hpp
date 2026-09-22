#pragma once

#include <cch/ai/Message.hpp>
#include <cch/ai/Model.hpp>
#include <cch/ai/StreamEvent.hpp>
#include <cch/support/Error.hpp>
#include <cch/support/JsonValue.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace cch::ai::api {

struct CompletionsProviderError {
    std::optional<std::string> code{std::nullopt};
    std::optional<std::string> message{std::nullopt};
    std::optional<std::uint64_t> suggested_backoff_ms{std::nullopt};
};

struct CompletionsProcessOutcome {
    std::optional<CompletionsProviderError> provider_error{std::nullopt};
};

/// Private Chat Completions event processor. It owns only decoded stream
/// state; SSE framing, request retries, cancellation, and terminal event
/// commitment remain in OpenAICompletionsAdapter and StreamExecutionEngine.
class CompletionsEventProcessor final {
public:
    explicit CompletionsEventProcessor(Model model);
    CompletionsEventProcessor(CompletionsEventProcessor&&) noexcept;
    CompletionsEventProcessor& operator=(CompletionsEventProcessor&&) noexcept;
    ~CompletionsEventProcessor();
    CompletionsEventProcessor(const CompletionsEventProcessor&) = delete;
    CompletionsEventProcessor& operator=(const CompletionsEventProcessor&) = delete;

    [[nodiscard]] support::Expected<CompletionsProcessOutcome> process(
            support::JsonValue::object_t event,
            AssistantMessage& assistant,
            AssistantEventSink& sink);
    [[nodiscard]] support::ExpectedVoid finish(AssistantMessage& assistant);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cch::ai::api
