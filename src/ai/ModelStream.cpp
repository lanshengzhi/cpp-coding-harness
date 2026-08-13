#include <cch/ai/ModelStream.hpp>

#include <atomic>
#include <memory>
#include <utility>

namespace cch::ai {

ModelStream::ModelStream(producer_type producer)
    : producer_(std::move(producer)) {}

ModelStream::ModelStream(ModelStream&&) noexcept = default;
ModelStream& ModelStream::operator=(ModelStream&&) noexcept = default;
ModelStream::~ModelStream() = default;

void ModelStream::start(
    AssistantEventSink sink,
    completion_type completion) noexcept {
    auto producer = std::exchange(producer_, producer_type{});
    if (!producer) {
        std::terminate(); // consumed, moved-from, or empty producer
    }
    // Enforce the producer's at-most-once contract on the callback path as
    // well (mirrors AsyncResult::start).
    auto delivered = std::make_shared<std::atomic<bool>>(false);
    completion_type guarded =
        [flag = delivered, done = std::move(completion)](
            std::expected<AssistantMessage, cch::support::Error> outcome) mutable noexcept {
            if (flag->exchange(true, std::memory_order_acq_rel)) {
                std::terminate(); // duplicate completion: at-most-once violation
            }
            done(std::move(outcome));
        };
    std::move(producer)(std::move(sink), std::move(guarded));
}

cch::support::AsyncResult<AssistantMessage> ModelStream::run(
    AssistantEventSink sink) && {
    auto producer = std::exchange(producer_, producer_type{});
    if (!producer) {
        std::terminate(); // consumed, moved-from, or empty producer
    }
    return cch::support::AsyncResult<AssistantMessage>(
        cch::support::AsyncProducer<AssistantMessage, cch::support::Error>{
            [producer = std::move(producer), sink = std::move(sink)](
                ModelStreamCompletion done) mutable noexcept {
                std::move(producer)(std::move(sink), std::move(done));
            }});
}

} // namespace cch::ai
