#pragma once

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/execution.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>

namespace cch::ai {

/// Concrete executor spelling for the cch_ai Beast client transports and the
/// loopback OAuth callback server (issue #638, ADR 0054). Keying Beast stream
/// machinery on the concrete `io_context::executor_type` — the executor the
/// Runtime actually runs (RuntimeRoot's loop) — instead of the
/// `beast::tcp_stream` default (`any_io_executor`) collapses the
/// type-erased-executor instantiations in the distribution binary.
using TransportExecutor = boost::asio::io_context::executor_type;
using TransportTcpStream = boost::beast::basic_stream<boost::asio::ip::tcp, TransportExecutor>;
using TransportTlsStream = boost::beast::ssl_stream<TransportTcpStream>;
using TransportResolver = boost::asio::ip::basic_resolver<boost::asio::ip::tcp, TransportExecutor>;
using TransportTimer = boost::asio::basic_waitable_timer<std::chrono::steady_clock,
        boost::asio::wait_traits<std::chrono::steady_clock>,
        TransportExecutor>;

/// The concrete executor behind a transport coroutine's ambient executor.
/// The transport contracts (WebSocketTransport, StreamTransport, OAuth
/// flows) require a single-threaded `io_context` executor; the cast asserts
/// that contract and never converts.
[[nodiscard]] inline TransportExecutor transport_executor(const boost::asio::any_io_executor& ambient) {
    auto& context = boost::asio::query(ambient, boost::asio::execution::context);
    return static_cast<boost::asio::io_context&>(context).get_executor();
}

} // namespace cch::ai
