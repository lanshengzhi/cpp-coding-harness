#pragma once

#include <cch/ai/ModelStream.hpp>
#include <cch/util/Error.hpp>

#include "ai/AsyncResultBridge.hpp"

#include <boost/asio/bind_executor.hpp>
#include <boost/asio/co_spawn.hpp>

#include <exception>
#include <utility>

namespace cch::ai::detail {

/// Wraps a one-shot awaitable producer into a move-only `ModelStream`
/// (ADR 0040 / #455). `make_awaitable` is invoked exactly once at consumption
/// with the event sink and must return a fresh `boost::asio::awaitable` whose
/// terminal `util::Expected<AssistantMessage>` is the stream's one terminal
/// outcome. The producer reads the initiating executor captured by `consume`
/// (AsyncResultBridge) so the wrapped coroutine runs in the consuming
/// serialized domain; event order, retry boundaries, cooperative cancellation,
/// and the terminal-error contract remain the producer's responsibility.
template <typename AwaitableFactory>
[[nodiscard]] ModelStream make_model_stream(AwaitableFactory make_awaitable) {
    // The coroutine producer owns the factory for the stream's whole lifetime:
    // a coroutine lambda's frame may reference its closure, so the closure
    // must not be destroyed while the co_spawn'd coroutine can still resume.
    auto shared = std::make_shared<AwaitableFactory>(std::move(make_awaitable));
    return ModelStream{ModelStreamProducer{
        [shared](AssistantEventSink sink, ModelStreamCompletion completion) mutable noexcept {
            const auto executor = t_initiating_executor;
            try {
                boost::asio::co_spawn(
                    executor,
                    (*shared)(std::move(sink)),
                    boost::asio::bind_executor(
                        executor,
                        [shared,
                         completion = std::move(completion)](
                            std::exception_ptr eptr,
                            util::Expected<AssistantMessage> result) mutable noexcept {
                            if (eptr) {
                                completion(std::unexpected(util::make_error(
                                    util::ErrorCode::Stream,
                                    "model stream failed")));
                            } else {
                                completion(std::move(result));
                            }
                        }));
            } catch (...) {
                completion(std::unexpected(util::make_error(
                    util::ErrorCode::Stream,
                    "model stream initiation failed")));
            }
        }}};
}

} // namespace cch::ai::detail
