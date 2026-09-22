#include "OAuthCallbackServer.hpp"

#include "OauthPage.hpp"
#include "Pkce.hpp"
#include "ai/TransportExecutor.hpp"
#include "support/ExpectedMacros.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/experimental/channel.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/system/error_code.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cch::ai::auth {
namespace {

using WaitResult = support::Expected<std::optional<std::string>>;
using WaitChannel = boost::asio::experimental::channel<void(boost::system::error_code, WaitResult)>;

struct ParsedTarget {
    std::string path{};
    std::string code{};
    std::string state{};
};

/// Parse a request target like `/auth/callback?code=X&state=Y` with
/// URLSearchParams-equivalent query decoding.
[[nodiscard]] ParsedTarget parse_target(std::string_view target) {
    ParsedTarget parsed;
    const auto query_start = target.find('?');
    parsed.path = std::string{
        target.substr(0, query_start == std::string_view::npos ? target.size() : query_start)};
    if (query_start == std::string_view::npos) {
        return parsed;
    }
    const auto pairs = parse_query_pairs(target.substr(query_start + 1));
    if (const auto found = pairs.find("code"); found != pairs.end()) {
        parsed.code = found->second;
    }
    if (const auto found = pairs.find("state"); found != pairs.end()) {
        parsed.state = found->second;
    }
    return parsed;
}

[[nodiscard]] boost::beast::http::response<boost::beast::http::string_body>
html_response(int status, std::string body) {
    boost::beast::http::response<boost::beast::http::string_body> response{
        static_cast<boost::beast::http::status>(status),
        /*version=*/11};
    response.set(boost::beast::http::field::content_type, "text/html; charset=utf-8");
    response.set(boost::beast::http::field::cache_control, "no-store");
    response.keep_alive(false);
    response.body() = std::move(body);
    response.prepare_payload();
    return response;
}

} // namespace

struct OAuthCallbackServer::Impl {
    Impl(TransportExecutor executor, OAuthCallbackServerOptions options)
        // acceptor and wait_channel must reference the member: the parameter
        // has already been moved from by the time they are initialized.
        : executor(std::move(executor)), options(std::move(options)), acceptor(this->executor),
          wait_channel(this->executor, 1) {}

    TransportExecutor executor;
    OAuthCallbackServerOptions options;
    boost::asio::basic_socket_acceptor<boost::asio::ip::tcp, TransportExecutor> acceptor;
    WaitChannel wait_channel;
    bool closed{false};
    bool degraded{false};
    bool claimed{false};
    bool wait_cancelled{false};
};

OAuthCallbackServer::OAuthCallbackServer(std::shared_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

OAuthCallbackServer::~OAuthCallbackServer() {
    close();
}

std::uint16_t OAuthCallbackServer::bound_port() const {
    if (impl_->degraded || !impl_->acceptor.is_open()) {
        return impl_->options.port;
    }
    boost::system::error_code error;
    const auto endpoint = impl_->acceptor.local_endpoint(error);
    if (error) {
        return impl_->options.port;
    }
    return endpoint.port();
}

boost::asio::awaitable<support::Expected<std::optional<std::string>>>
OAuthCallbackServer::wait_for_code() {
    WaitResult result{std::optional<std::string>{}};
    boost::system::error_code receive_error;
    result = co_await impl_->wait_channel.async_receive(
            boost::asio::redirect_error(boost::asio::use_awaitable, receive_error));
    if (receive_error) {
        co_return std::optional<std::string>{};
    }
    co_return result;
}

void OAuthCallbackServer::cancel_wait() {
    const auto impl = impl_;
    boost::asio::dispatch(impl->executor, [impl] {
        if (impl->closed || impl->claimed || impl->wait_cancelled) {
            return;
        }
        impl->wait_cancelled = true;
        impl->wait_channel.try_send(boost::system::error_code{}, WaitResult{std::optional<std::string>{}});
    });
}

void OAuthCallbackServer::close() {
    const auto impl = impl_;
    boost::asio::dispatch(impl->executor, [impl] {
        if (impl->closed) {
            return;
        }
        impl->closed = true;
        boost::system::error_code error;
        impl->acceptor.close(error);
    });
}

boost::asio::awaitable<support::Expected<std::shared_ptr<OAuthCallbackServer>>>
OAuthCallbackServer::start(OAuthCallbackServerOptions options) {
    namespace asio = boost::asio;
    namespace beast = boost::beast;
    namespace http = boost::beast::http;
    using tcp = asio::ip::tcp;

    const auto executor = transport_executor(co_await asio::this_coro::executor);
    auto impl = std::make_shared<Impl>(executor, std::move(options));

    boost::system::error_code error;
    TransportResolver resolver(executor);
    const auto results = resolver.resolve(
        impl->options.host,
        std::to_string(impl->options.port),
        error);
    if (!error) {
        impl->acceptor.open(results.begin()->endpoint().protocol(), error);
    }
    if (!error) {
        impl->acceptor.set_option(tcp::acceptor::reuse_address(true), error);
    }
    if (!error) {
        impl->acceptor.bind(*results.begin(), error);
    }
    if (!error) {
        impl->acceptor.listen(tcp::socket::max_listen_connections, error);
    }
    if (error) {
        // Listen errors degrade to manual input only (pi settleWait(null)).
        impl->degraded = true;
        impl->wait_cancelled = true;
        impl->wait_channel.try_send(boost::system::error_code{}, WaitResult{std::optional<std::string>{}});
        co_return std::make_shared<OAuthCallbackServer>(impl);
    }

    const auto expected_state = impl->options.state;
    const auto callback_path = impl->options.path;
    co_spawn(
            executor,
            [impl, expected_state, callback_path]() -> asio::awaitable<void> {
                while (!impl->closed) {
                    boost::asio::basic_stream_socket<tcp, TransportExecutor> socket(impl->executor);
                    boost::system::error_code accept_error;
                    co_await impl->acceptor.async_accept(
                            socket, asio::redirect_error(asio::use_awaitable, accept_error));
                    if (accept_error) {
                        break;
                    }
                    co_spawn(
                            impl->executor,
                            [impl, expected_state, callback_path, socket = std::move(socket)]() mutable
                                    -> asio::awaitable<void> {
                                namespace http = boost::beast::http;
                                auto response = html_response(
                                        500, oauth_error_html("Internal error while processing OAuth callback."));
                                TransportTcpStream stream(std::move(socket));
                                beast::flat_buffer buffer;
                                http::request<http::string_body> request;
                                boost::system::error_code handler_error;
                                co_await http::async_read(stream,
                                        buffer,
                                        request,
                                        asio::redirect_error(asio::use_awaitable, handler_error));
                                if (!handler_error) {
                                    const auto target = parse_target(request.target());
                                    if (request.method() != http::verb::get || target.path != callback_path) {
                                        response = html_response(404, oauth_error_html("Callback route not found."));
                                    } else if (impl->wait_cancelled || impl->claimed) {
                                        response = html_response(
                                                409, oauth_error_html("This OAuth callback has already been used."));
                                    } else if (impl->options.validate_state && target.state != expected_state) {
                                        response = html_response(400, oauth_error_html("State mismatch."));
                                    } else if (target.code.empty()) {
                                        response = html_response(400, oauth_error_html("Missing authorization code."));
                                    } else {
                                        impl->claimed = true;
                                        if (impl->options.callback_handler) {
                                            auto callback_result = co_await impl->options.callback_handler(target.code);
                                            if (callback_result) {
                                                response = html_response(
                                                        200, oauth_success_html(impl->options.success_message));
                                                impl->wait_channel.try_send(boost::system::error_code{},
                                                        WaitResult{std::optional<std::string>{
                                                                std::move(*callback_result)}});
                                            } else {
                                                auto callback_error = std::move(callback_result.error());
                                                std::string details = callback_error.message;
                                                if (!callback_error.detail.empty()) {
                                                    details += ": ";
                                                    details += callback_error.detail;
                                                }
                                                response = html_response(502,
                                                        oauth_error_html(
                                                                impl->options.exchange_error_message, details));
                                                impl->wait_channel.try_send(boost::system::error_code{},
                                                        WaitResult{std::unexpected(std::move(callback_error))});
                                            }
                                        } else {
                                            response = html_response(
                                                    200, oauth_success_html(impl->options.success_message));
                                            impl->wait_channel.try_send(boost::system::error_code{},
                                                    WaitResult{std::optional<std::string>{std::move(target.code)}});
                                        }
                                    }
                                    co_await http::async_write(
                                            stream, response, asio::redirect_error(asio::use_awaitable, handler_error));
                                }
                                // Best-effort session: any read/write failure still
                                // leaves the 500 page and a valid callback settles the
                                // wait before the write fails.
                            },
                            asio::detached);
                }
            },
            asio::detached);

    co_return std::make_shared<OAuthCallbackServer>(impl);
}

} // namespace cch::ai::auth
