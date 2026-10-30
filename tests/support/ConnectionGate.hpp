#pragma once

#include "support/ReleaseGate.hpp"

namespace cch::tests {

/// One held connection attempt; opening also admits later reconnects inline.
/// wait() has one pending caller. Call open() on the same serialized executor
/// as wait(), or while that executor is not being pumped. The shared owner
/// must outlive the waiting coroutine; ReleaseGate supplies the latched wake.
class ConnectionGate final {
public:
    void open() {
        if (!open_) {
            open_ = true;
            gate_.release();
        }
    }

    [[nodiscard]] boost::asio::awaitable<void> wait() {
        if (!open_) co_await gate_.wait();
    }

private:
    bool open_{false};
    ReleaseGate gate_;
};

} // namespace cch::tests
