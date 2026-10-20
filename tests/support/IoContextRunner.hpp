#pragma once

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>

#include <thread>

namespace cch::tests {

/// One io_context serviced by its own worker thread, kept alive by a work
/// guard and stopped in the destructor. The terminal and stdin-buffer tests
/// hand it to a ProcessTerminal so its asynchronous reads make progress while
/// the test drives the other end of a pseudo-terminal.
class IoContextRunner final {
public:
    IoContextRunner() : work(boost::asio::make_work_guard(io)), thread([this] { io.run(); }) {}

    ~IoContextRunner() {
        work.reset();
        io.stop();
        thread.join();
    }

    IoContextRunner(const IoContextRunner&) = delete;
    IoContextRunner& operator=(const IoContextRunner&) = delete;

    boost::asio::io_context io;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work;
    std::jthread thread;
};

} // namespace cch::tests
