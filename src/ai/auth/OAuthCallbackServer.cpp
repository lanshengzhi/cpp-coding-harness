#include "OAuthCallbackServer.hpp"

#include "OauthPage.hpp"
#include "Pkce.hpp"
#include "support/ExpectedMacros.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/experimental/channel.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/system/error_code.hpp>
#include <boost/system/system_error.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cch::ai::auth {
namespace {

using WaitChannel = boost::asio::experimental::channel<
    void(boost::system::error_code, std::optional<std::string>)>;

constexpr std::string_view kCallbackPath = "/auth/callback";

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
    response.keep_alive(false);
    response.body() = std::move(body);
    response.prepare_payload();
    return response;
}

} // namespace

struct OAuthCallbackServer::Impl {
    Impl(
        boost::asio::any_io_executor executor,
        OAuthCallbackServerOptions options)
        // acceptor and wait_channel must reference the member: the parameter
        // has already been moved from by the time they are initialized.
        : executor(std::move(executor)),
          options(std::move(options)),
          acceptor(this->executor),
          wait_channel(this->executor, 1) {}

    boost::asio::any_io_executor executor;
    OAuthCallbackServerOptions options;
    boost::asio::ip::tcp::acceptor acceptor;
    WaitChannel wait_channel;
    bool closed{false};
    bool degraded{false};
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
    std::optional<std::string> code;
    try {
        code = co_await impl_->wait_channel.async_receive(
            boost::asio::use_awaitable);
    } catch (const boost::system::system_error&) {
        co_return std::optional<std::string>{};
    }
    co_return code;
}

void OAuthCallbackServer::cancel_wait() {
    impl_->wait_channel.try_send(boost::system::error_code{}, std::nullopt);
}

void OAuthCallbackServer::close() {
    if (impl_->closed) {
        return;
    }
    impl_->closed = true;
    boost::system::error_code error;
    impl_->acceptor.close(error);
}

boost::asio::awaitable<support::Expected<std::shared_ptr<OAuthCallbackServer>>>
OAuthCallbackServer::start(OAuthCallbackServerOptions options) {
    namespace asio = boost::asio;
    namespace beast = boost::beast;
    namespace http = boost::beast::http;
    using tcp = asio::ip::tcp;

    auto executor = co_await asio::this_coro::executor;
    auto impl = std::make_shared<Impl>(executor, std::move(options));

    boost::system::error_code error;
    tcp::resolver resolver(executor);
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
        impl->wait_channel.try_send(boost::system::error_code{}, std::nullopt);
        co_return std::make_shared<OAuthCallbackServer>(impl);
    }

    const auto expected_state = impl->options.state;
    co_spawn(
        executor,
        [impl, expected_state]() -> asio::awaitable<void> {
            while (!impl->closed) {
                tcp::socket socket(impl->executor);
                boost::system::error_code accept_error;
                co_await impl->acceptor.async_accept(
                    socket,
                    asio::redirect_error(asio::use_awaitable, accept_error));
                if (accept_error) {
                    break;
                }
                co_spawn(
                    impl->executor,
                    [impl, expected_state, socket = std::move(socket)]()
                        mutable -> asio::awaitable<void> {
                        namespace http = boost::beast::http;
                        auto response = html_response(
                            500,
                            oauth_error_html(
                                "Internal error while processing OAuth callback."));
                        try {
                            beast::tcp_stream stream(std::move(socket));
                            beast::flat_buffer buffer;
                            http::request<http::string_body> request;
                            co_await http::async_read(
                                stream,
                                buffer,
                                request,
                                asio::use_awaitable);
                            const auto target = parse_target(request.target());
                            if (target.path != kCallbackPath) {
                                response = html_response(
                                    404,
                                    oauth_error_html("Callback route not found."));
                            } else if (target.state != expected_state) {
                                response = html_response(
                                    400,
                                    oauth_error_html("State mismatch."));
                            } else if (target.code.empty()) {
                                response = html_response(
                                    400,
                                    oauth_error_html("Missing authorization code."));
                            } else {
                                response = html_response(
                                    200,
                                    oauth_success_html(
                                        "OpenAI authentication completed. You can close this window."));
                                impl->wait_channel.try_send(
                                    boost::system::error_code{}, target.code);
                            }
                            co_await http::async_write(
                                stream,
                                response,
                                asio::use_awaitable);
                        } catch (const boost::system::system_error&) {
                            // Best-effort session; a valid callback still
                            // settles the wait before the write fails.
                        } catch (...) {
                            // pi returns the frozen internal-error page (500)
                            // for any handler failure; best-effort here.
                        }
                    },
                    asio::detached);
            }
        },
        asio::detached);

    co_return std::make_shared<OAuthCallbackServer>(impl);
}

} // namespace cch::ai::auth
