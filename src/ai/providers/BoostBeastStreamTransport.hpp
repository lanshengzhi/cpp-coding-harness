#pragma once

#include "../../../include/cch/ai/providers/StreamTransport.hpp"

namespace cch::ai::providers {

class BoostBeastStreamTransport final : public StreamTransport {
public:
    [[nodiscard]] boost::asio::awaitable<util::Expected<StreamResponse>> async_stream(
        const StreamRequest& request,
        BodyChunkHandler on_body_chunk) override;
};

} // namespace cch::ai::providers
