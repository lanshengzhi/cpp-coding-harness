#include "support/LocalMcpHttpServer.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>

#include <array>
#include <atomic>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace cch::tests {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = boost::beast::http;
namespace ssl = boost::asio::ssl;
using tcp = asio::ip::tcp;
using TlsStream = beast::ssl_stream<tcp::socket>;

/// The committed test credentials (issue #638), reused rather than
/// regenerated: `test-server.pem` and `test-server-key.pem` secure this
/// fixture (SAN: IP 127.0.0.1, DNS localhost) and `test-ca.pem` is the trust
/// anchor the client-side transport is given. The regeneration commands and
/// the reason the credentials are committed live in
/// `tests/ai/providers/tls/README.md`.
constexpr std::string_view kTlsFixtureDir{"/tests/ai/providers/tls/"};

/// The size of one flood frame, so a flooding reply reaches a client's
/// retention bound in a bounded number of writes.
constexpr std::size_t kFloodFrameBytes{64 * 1024};

[[nodiscard]] std::string tls_fixture_path(std::string_view name) {
    return std::string{CCH_SOURCE_DIR} + std::string{kTlsFixtureDir} + std::string{name};
}

[[nodiscard]] std::string lowercase(std::string_view name) {
    std::string lowered;
    lowered.reserve(name.size());
    for (const char ch : name) {
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return lowered;
}

/// The live state the connections share with the accept loop and with the test
/// thread, guarded by its own mutex: the request log, the live sessions to
/// release on stop, and the stop flag itself.
struct ServerState {
    mutable std::mutex mutex{};
    std::vector<RecordedHttpRequest> received{};
    std::vector<std::weak_ptr<TlsStream>> live{};
    std::atomic<bool> stopping{false};
    std::atomic<std::size_t> accepted{0};
};

/// Read one request, keeping its head exactly as the client wrote it. The head
/// is read byte-first so a test can assert on the real wire bytes, and the
/// parsed form then comes from feeding those same bytes to a parser.
[[nodiscard]] asio::awaitable<std::optional<RecordedHttpRequest>> read_request(TlsStream& stream) {
    std::array<char, 4096> chunk{};
    std::string raw;
    boost::system::error_code error;
    while (raw.find("\r\n\r\n") == std::string::npos) {
        const auto received =
                co_await stream.async_read_some(asio::buffer(chunk), asio::redirect_error(asio::use_awaitable, error));
        if (error) {
            co_return std::nullopt;
        }
        raw.append(chunk.data(), received);
    }

    http::request_parser<http::string_body> parser;
    const auto consumed = parser.put(asio::buffer(raw), error);
    if (error && error != http::error::need_more) {
        co_return std::nullopt;
    }
    if (consumed < raw.size()) {
        (void) parser.put(asio::buffer(raw.data() + consumed, raw.size() - consumed), error);
        if (error && error != http::error::need_more) {
            co_return std::nullopt;
        }
    }
    while (!parser.is_done()) {
        const auto received =
                co_await stream.async_read_some(asio::buffer(chunk), asio::redirect_error(asio::use_awaitable, error));
        if (error) {
            co_return std::nullopt;
        }
        (void) parser.put(asio::buffer(chunk, received), error);
        if (error && error != http::error::need_more) {
            co_return std::nullopt;
        }
    }

    RecordedHttpRequest recorded;
    recorded.method = std::string(parser.get().method_string());
    recorded.target = std::string(parser.get().target());
    recorded.raw_head = raw.substr(0, raw.find("\r\n\r\n") + 4);
    for (const auto& field : parser.get().base()) {
        recorded.headers[lowercase(field.name_string())] = std::string(field.value());
    }
    recorded.body = parser.get().body();
    co_return recorded;
}

/// Write one chunked-transfer chunk. `false` means the client is gone.
[[nodiscard]] asio::awaitable<bool> write_chunk(TlsStream& stream, std::string_view payload) {
    std::string chunk;
    chunk.reserve(payload.size() + 16);
    // A chunked-transfer size is hexadecimal, not decimal.
    chunk.append(std::format("{:x}\r\n", payload.size()));
    chunk.append(payload);
    chunk.append("\r\n");
    boost::system::error_code error;
    co_await asio::async_write(stream, asio::buffer(chunk), asio::redirect_error(asio::use_awaitable, error));
    co_return !error;
}

/// The exact response head this fixture puts on the wire.
///
/// The head is written as text rather than through Beast's serializer because
/// a serializer owns the whole message framing: serializing a chunked message
/// would write the terminating chunk itself and leave this fixture no way to
/// hold an event stream open, truncate one, or flood one. A fixture whose
/// subject is the bytes on the wire writes those bytes.
[[nodiscard]] std::string response_head(int status_code, std::string_view content_type, std::string_view framing) {
    const auto status = http::status(status_code);
    return std::string("HTTP/1.1 ") + std::to_string(status_code) + " " + std::string(http::obsolete_reason(status)) +
           "\r\nContent-Type: " + std::string(content_type) + "\r\nCache-Control: no-store\r\n" +
           std::string(framing) + "\r\n\r\n";
}

/// Write raw body bytes into a response whose length already declares them.
/// `false` means the client is gone.
[[nodiscard]] asio::awaitable<bool> write_bytes(TlsStream& stream, std::string_view payload) {
    boost::system::error_code error;
    co_await asio::async_write(stream, asio::buffer(payload), asio::redirect_error(asio::use_awaitable, error));
    co_return !error;
}

} // namespace

std::string RecordedHttpRequest::header(std::string_view name) const {
    const auto found = headers.find(lowercase(name));
    return found == headers.end() ? std::string{} : found->second;
}

bool RecordedHttpRequest::has_header(std::string_view name) const { return headers.count(lowercase(name)) != 0; }

struct LocalMcpHttpServer::Impl {
    LocalMcpHttpServer::Handler handler;
    LocalMcpHttpServerOptions options;
    std::shared_ptr<ssl::context> context;
    std::shared_ptr<ServerState> state{std::make_shared<ServerState>()};
    std::shared_ptr<tcp::acceptor> acceptor;
    asio::io_context io;
    std::string setup_error{};
    std::uint16_t port{0};
    /// The lifetime backstop. It is held here rather than kept alive only by
    /// its own handler so `release()` can cancel it; an armed timer is pending
    /// work, so an uncancelled one holds the server thread's `io.run()` open
    /// for the whole backstop after `stop()`.
    std::shared_ptr<asio::steady_timer> lifetime;
    std::thread thread;

    /// Stop accepting and release the live connections. Safe to call from the
    /// server thread, so the lifetime backstop and `stop()` share it.
    void release() {
        state->stopping.store(true);
        boost::system::error_code error;
        if (acceptor) {
            acceptor->close(error);
        }
        if (lifetime) {
            lifetime->cancel();
        }
        std::vector<std::shared_ptr<TlsStream>> live;
        {
            const std::lock_guard<std::mutex> lock{state->mutex};
            for (const auto& weak : state->live) {
                if (auto stream = weak.lock(); stream) {
                    live.push_back(std::move(stream));
                }
            }
            state->live.clear();
        }
        for (const auto& stream : live) {
            boost::system::error_code cancel_error;
            beast::get_lowest_layer(*stream).cancel(cancel_error);
            beast::get_lowest_layer(*stream).close(cancel_error);
        }
    }

    /// One connection: handshake, read the request, answer it, and release.
    [[nodiscard]] asio::awaitable<void> serve(tcp::socket peer) {
        auto stream = std::make_shared<TlsStream>(std::move(peer), *context);
        {
            const std::lock_guard<std::mutex> lock{state->mutex};
            state->live.push_back(stream);
        }
        boost::system::error_code error;
        co_await stream->async_handshake(ssl::stream_base::server, asio::redirect_error(asio::use_awaitable, error));
        if (error) {
            co_return;
        }
        auto request = co_await read_request(*stream);
        if (!request) {
            co_return;
        }
        const auto reply = handler(*request);
        if (!reply) {
            co_return; // this fixture does not answer that request
        }
        {
            const std::lock_guard<std::mutex> lock{state->mutex};
            state->received.push_back(*request);
        }
        if (reply->as_event_stream) {
            co_await write_event_stream(*stream, *reply);
        } else {
            co_await write_message_body(*stream, *reply);
        }
        if (!state->stopping.load()) {
            boost::system::error_code shutdown_error;
            co_await stream->async_shutdown(asio::redirect_error(asio::use_awaitable, shutdown_error));
        }
    }

    /// The whole reply as one message body, declared with its length — length
    /// plus any flood — so a truncation can break it mid-body and a flooding
    /// reply is a legitimately enormous body rather than a broken one.
    [[nodiscard]] asio::awaitable<void> write_message_body(TlsStream& stream, const McpServerReply& reply) {
        const auto head = response_head(reply.status_code,
                reply.content_type,
                "Content-Length: " + std::to_string(reply.body.size() + reply.flood_bytes));
        boost::system::error_code error;
        if (!(co_await write_bytes(stream, head))) {
            co_return;
        }
        if (!reply.body.empty()) {
            co_await asio::async_write(
                    stream, asio::buffer(reply.body), asio::redirect_error(asio::use_awaitable, error));
            if (error) {
                co_return;
            }
        }
        if (reply.truncate_after_bytes.has_value()) {
            co_return; // the connection closes with the declared length unmet
        }
        co_await write_filler(stream, reply.flood_bytes, false);
    }

    /// The reply as a chunked `text/event-stream`, so the client assembles
    /// frames as they arrive instead of reading one whole body at once.
    [[nodiscard]] asio::awaitable<void> write_event_stream(TlsStream& stream, const McpServerReply& reply) {
        if (!(co_await write_bytes(
                    stream, response_head(reply.status_code, reply.content_type, "Transfer-Encoding: chunked")))) {
            co_return;
        }

        const auto payloads =
                reply.event_payloads.empty() ? std::vector<std::string>{reply.body} : reply.event_payloads;
        std::size_t written = 0;
        for (std::size_t index = 0; index < payloads.size(); ++index) {
            const bool last = index + 1 == payloads.size();
            const auto terminator = last && reply.end_stream_mid_frame ? "\n" : "\n\n";
            const std::string frame = "data: " + payloads[index] + terminator;
            if (reply.truncate_after_bytes.has_value() && written + frame.size() > *reply.truncate_after_bytes) {
                co_return; // the connection closes with the response incomplete
            }
            if (!co_await write_chunk(stream, frame)) {
                co_return;
            }
            written += frame.size();
        }
        if (reply.truncate_after_bytes.has_value()) {
            co_return;
        }
        if (reply.flood_bytes != 0) {
            co_await write_filler(stream, reply.flood_bytes, true);
            co_return; // a flooding stream never ends, by definition
        }
        (void) co_await write_chunk(stream, {});
    }

    /// Keep the connection busy past the point the client stops reading. The
    /// loop ends when the client closes, when the server is stopping, or when
    /// the requested filler has been written.
    [[nodiscard]] asio::awaitable<void> write_filler(TlsStream& stream, std::size_t flood_bytes, bool chunked) {
        const std::string payload = chunked ? "data: " + std::string(kFloodFrameBytes - 7, 'a') + "\n\n"
                                            : std::string(kFloodFrameBytes, 'a');
        for (std::size_t written = 0; written < flood_bytes; written += kFloodFrameBytes) {
            if (state->stopping.load()) {
                co_return;
            }
            const bool written_ok = chunked ? co_await write_chunk(stream, payload)
                                            : co_await write_bytes(stream, payload);
            if (!written_ok) {
                co_return;
            }
        }
    }

    [[nodiscard]] asio::awaitable<void> accept_loop() {
        for (;;) {
            tcp::socket peer{acceptor->get_executor()};
            boost::system::error_code error;
            co_await acceptor->async_accept(peer, asio::redirect_error(asio::use_awaitable, error));
            if (error) {
                co_return;
            }
            if (state->accepted.fetch_add(1) == 0 && options.abort_first_handshake) {
                boost::system::error_code close_error;
                peer.close(close_error);
                continue;
            }
            asio::co_spawn(io, serve(std::move(peer)), asio::detached);
        }
    }
};

LocalMcpHttpServer::LocalMcpHttpServer(Handler handler, LocalMcpHttpServerOptions options)
    : impl_(std::make_unique<Impl>()) {
    impl_->handler = std::move(handler);
    impl_->options = options;

    boost::system::error_code error;
    impl_->context = std::make_shared<ssl::context>(ssl::context::tls_server);
    impl_->context->use_certificate_chain_file(tls_fixture_path("test-server.pem"), error);
    if (!error) {
        impl_->context->use_private_key_file(tls_fixture_path("test-server-key.pem"), ssl::context::pem, error);
    }
    if (error) {
        impl_->setup_error = "the local MCP HTTP server could not load its test credentials: " + error.message();
        return;
    }
    impl_->acceptor = std::make_shared<tcp::acceptor>(impl_->io);
    impl_->acceptor->open(tcp::v4(), error);
    impl_->acceptor->set_option(tcp::acceptor::reuse_address(true), error);
    impl_->acceptor->bind({tcp::v4(), 0}, error);
    impl_->acceptor->listen(tcp::acceptor::max_listen_connections, error);
    if (error) {
        impl_->setup_error = "the local MCP HTTP server could not bind a loopback endpoint: " + error.message();
        return;
    }
    impl_->port = impl_->acceptor->local_endpoint(error).port();

    auto* impl = impl_.get();
    impl_->thread = std::thread([impl] {
        asio::co_spawn(impl->io, impl->accept_loop(), asio::detached);
        // The backstop keeps itself alive through its own handler: a fixture
        // whose case never finishes must not outlive its shard.
        impl->lifetime = std::make_shared<asio::steady_timer>(impl->io, impl->options.lifetime);
        const auto backstop = impl->lifetime;
        backstop->async_wait([impl, backstop](boost::system::error_code) { impl->release(); });
        impl->io.run();
    });
}

LocalMcpHttpServer::~LocalMcpHttpServer() { stop(); }

bool LocalMcpHttpServer::ready() const noexcept { return impl_->setup_error.empty(); }

const std::string& LocalMcpHttpServer::setup_error() const noexcept { return impl_->setup_error; }

std::uint16_t LocalMcpHttpServer::port() const noexcept { return impl_->port; }

std::string LocalMcpHttpServer::url(std::string path) const {
    return "https://127.0.0.1:" + std::to_string(impl_->port) + path;
}

std::vector<RecordedHttpRequest> LocalMcpHttpServer::requests() const {
    const std::lock_guard<std::mutex> lock{impl_->state->mutex};
    return impl_->state->received;
}

void LocalMcpHttpServer::stop() {
    if (impl_ == nullptr || impl_->thread.joinable() == false) {
        return;
    }
    impl_->release();
    impl_->thread.join();
}

} // namespace cch::tests
