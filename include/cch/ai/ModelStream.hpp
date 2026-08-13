#pragma once

#include <cch/ai/StreamEvent.hpp>
#include <cch/support/AsyncResult.hpp>
#include <cch/support/Error.hpp>

#include <expected>
#include <functional>
#include <utility>

namespace cch::ai {

/// Terminal delivery callback for one model stream: exactly one
/// `std::expected<AssistantMessage, Error>`.
using ModelStreamCompletion =
    cch::support::AsyncCompletion<AssistantMessage, cch::support::Error>;

/// Lazy producer of one model stream. Invoked exactly once at consumption with
/// the event sink and the terminal completion; after it returns it owns
/// everything it needs. Initiation is `noexcept` and the producer completes
/// exactly once through `completion`, so the stream's event ordering, retry
/// boundaries, cooperative cancellation, and terminal outcome stay the
/// responsibility of the producing capability Owner.
using ModelStreamProducer = std::move_only_function<
    void(AssistantEventSink, ModelStreamCompletion) noexcept>;

/// Move-only, single-consumption model stream (ADR 0040 §ModelStream).
///
/// One AI-owned value represents one model-stream operation: the producer
/// delivers stream events in order through an `AssistantEventSink` and reaches
/// exactly one terminal `std::expected<AssistantMessage, Error>`. A stream is
/// consumed exactly once, either by move-only callback `start` or by move-only
/// `run` + `co_await`. Duplicate, moved-from, or reused consumption, a producer
/// that completes twice, and an empty producer all call `std::terminate` in
/// Debug and Release.
///
/// `ModelStream` owns no execution machinery and names no third-party
/// execution type: the Agent consumes it through standard C++ `co_await`/
/// callback shapes, while `ai::Models` keeps the executor capture and the
/// Boost.Asio bridge private.
class ModelStream {
public:
    using completion_type = ModelStreamCompletion;
    using producer_type = ModelStreamProducer;

    /// Pending stream: owns the producer. The producer must be non-empty.
    explicit ModelStream(producer_type producer);

    // A moved-from stream is reset to an empty producer, so consuming a
    // moved-from stream is distinguishable from a valid one and terminates.
    ModelStream(ModelStream&&) noexcept;
    ModelStream& operator=(ModelStream&&) noexcept;
    ~ModelStream();
    ModelStream(const ModelStream&) = delete;
    ModelStream& operator=(const ModelStream&) = delete;

    /// Consume by move-only callback: `sink` receives stream events in order,
    /// `completion` receives exactly one terminal outcome.
    void start(AssistantEventSink sink, completion_type completion) noexcept;

    /// Consume by move-only `co_await` with an event sink. Returns a move-only
    /// `AsyncResult<AssistantMessage>` whose `co_await` yields the terminal
    /// `std::expected<AssistantMessage, Error>`; ready/pending and fatal-misuse
    /// semantics come from `AsyncResult`.
    [[nodiscard]] cch::support::AsyncResult<AssistantMessage> run(
        AssistantEventSink sink) &&;

private:
    producer_type producer_;
};

} // namespace cch::ai
