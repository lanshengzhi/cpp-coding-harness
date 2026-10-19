#pragma once

#include <cch/ai/Context.hpp>
#include <cch/ai/Message.hpp>
#include <cch/ai/Model.hpp>
#include <cch/ai/StreamEvent.hpp>
#include "PreparedStreamOptions.hpp"
#include "ai/providers/StreamEmitter.hpp"
#include "support/StreamTransport.hpp"
#include <cch/support/Error.hpp>

#include <boost/asio/awaitable.hpp>

namespace cch::ai::api {

/// Executes a streaming request using the OpenAI Chat Completions protocol
/// over the given transport. This entry owns the whole request preparation:
/// authentication assembled from the gateway key and the model's own header
/// set, the caller's `SimpleStreamOptions` resolved against the Model and
/// AiContext (clamped generation fields, cache retention, session identity),
/// and the resolved request header list.
///
/// Borrowed transport, model, context, emitter and request_started must outlive the returned awaitable.
[[nodiscard]] boost::asio::awaitable<support::Expected<AssistantMessage>> execute_completions_stream(
        support::StreamTransport& transport,
        std::string base_url,
        const Model& model,
        const AiContext& context,
        RequestAuth auth,
        SimpleStreamOptions options,
        providers::StreamEmitter& emitter,
        bool& request_started);

/// Builds an OpenAI Chat Completions HTTP request from the model, context,
/// and prepared request state.
[[nodiscard]] support::Expected<support::StreamRequest> build_stream_request(
        std::string_view base_url, const Model& model, const AiContext& context, const PreparedStreamOptions& options);

} // namespace cch::ai::api
