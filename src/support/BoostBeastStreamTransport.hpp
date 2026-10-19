#pragma once

#include "support/StreamTransport.hpp"

namespace cch::support {

class BoostBeastStreamTransport final : public StreamTransport {
public:
    [[nodiscard]] boost::asio::awaitable<Expected<StreamResponse>> async_stream(
            const StreamRequest& request, BodyChunkHandler on_body_chunk) override;
};

} // namespace cch::support
