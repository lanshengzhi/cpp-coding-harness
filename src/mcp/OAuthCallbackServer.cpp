#include <cch/mcp/OAuthCallbackServer.hpp>

#include "support/AsyncResultBridge.hpp"
#include <cch/mcp/Pkce.hpp>

#include "support/ExpectedMacros.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/experimental/channel.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/execution.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::mcp {
namespace {

using OAuthExecutor = boost::asio::io_context::executor_type;
using OAuthTcpStream = boost::beast::basic_stream<boost::asio::ip::tcp, OAuthExecutor>;
using OAuthResolver = boost::asio::ip::basic_resolver<boost::asio::ip::tcp, OAuthExecutor>;

[[nodiscard]] OAuthExecutor oauth_executor(const boost::asio::any_io_executor& ambient) {
    auto& context = boost::asio::query(ambient, boost::asio::execution::context);
    return static_cast<boost::asio::io_context&>(context).get_executor();
}

constexpr std::string_view kLogoSvg = "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 800 800\" "
                                      "aria-hidden=\"true\"><path fill=\"#fff\" fill-rule=\"evenodd\" "
                                      "d=\"M165.29 165.29 H517.36 V400 H400 V517.36 H282.65 V634.72 H165.29 Z "
                                      "M282.65 282.65 V400 H400 V282.65 Z\"/><path fill=\"#fff\" "
                                      "d=\"M517.36 400 H634.72 V634.72 H517.36 Z\"/></svg>";

[[nodiscard]] std::string escape_html(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (const char character : value) {
        switch (character) {
        case '&':
            escaped += "&amp;";
            break;
        case '<':
            escaped += "&lt;";
            break;
        case '>':
            escaped += "&gt;";
            break;
        case '"':
            escaped += "&quot;";
            break;
        case '\'':
            escaped += "&#39;";
            break;
        default:
            escaped.push_back(character);
            break;
        }
    }
    return escaped;
}

[[nodiscard]] std::string render_page(std::string_view title,
        std::string_view heading,
        std::string_view message,
        std::optional<std::string_view> details) {
    const std::string escaped_title = escape_html(title);
    const std::string escaped_heading = escape_html(heading);
    const std::string escaped_message = escape_html(message);
    const std::string escaped_details = details ? escape_html(*details) : std::string{};

    // Byte-identical to pi's renderPage template at baseline 83114817.
    std::string page = R"(<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1" />
  <title>)";
    page += escaped_title;
    page += R"(</title>
  <style>
    :root {
      --text: #fafafa;
      --text-dim: #a1a1aa;
      --page-bg: #09090b;
      --font-sans: ui-sans-serif, system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, "Helvetica Neue", Arial, "Noto Sans", sans-serif, "Apple Color Emoji", "Segoe UI Emoji", "Segoe UI Symbol", "Noto Color Emoji";
      --font-mono: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, "Liberation Mono", "Courier New", monospace;
    }
    * { box-sizing: border-box; }
    html { color-scheme: dark; }
    body {
      margin: 0;
      min-height: 100vh;
      display: flex;
      align-items: center;
      justify-content: center;
      padding: 24px;
      background: var(--page-bg);
      color: var(--text);
      font-family: var(--font-sans);
      text-align: center;
    }
    main {
      width: 100%;
      max-width: 560px;
      display: flex;
      flex-direction: column;
      align-items: center;
      justify-content: center;
    }
    .logo {
      width: 72px;
      height: 72px;
      display: block;
      margin-bottom: 24px;
    }
    h1 {
      margin: 0 0 10px;
      font-size: 28px;
      line-height: 1.15;
      font-weight: 650;
      color: var(--text);
    }
    p {
      margin: 0;
      line-height: 1.7;
      color: var(--text-dim);
      font-size: 15px;
    }
    .details {
      margin-top: 16px;
      font-family: var(--font-mono);
      font-size: 13px;
      color: var(--text-dim);
      white-space: pre-wrap;
      word-break: break-word;
    }
  </style>
</head>
<body>
  <main>
    <div class="logo">)";
    page += std::string{kLogoSvg};
    page += R"(</div>
    <h1>)";
    page += escaped_heading;
    page += R"(</h1>
    <p>)";
    page += escaped_message;
    page += R"(</p>
    )";
    if (details) {
        page += "<div class=\"details\">" + escaped_details + "</div>";
    }
    page += R"(
  </main>
</body>
</html>)";
    return page;
}

[[nodiscard]] std::string oauth_success_html(std::string_view message) {
    return render_page("Authentication successful", "Authentication successful", message, std::nullopt);
}

[[nodiscard]] std::string oauth_error_html(
        std::string_view message, std::optional<std::string_view> details = std::nullopt) {
    return render_page("Authentication failed", "Authentication failed", message, details);
}

using WaitResult = support::Expected<OAuthCallbackResult>;
using WaitChannel = boost::asio::experimental::channel<void(boost::system::error_code, WaitResult)>;

struct ParsedTarget {
    std::string path{};
    std::string code{};
    std::string state{};
    std::string client_id{};
    std::string iss{};
};

/// Parse a request target like `/auth/callback?code=X&state=Y` with
/// URLSearchParams-equivalent query decoding.
[[nodiscard]] ParsedTarget parse_target(std::string_view target) {
    ParsedTarget parsed;
    const auto query_start = target.find('?');
    parsed.path = std::string{target.substr(0, query_start == std::string_view::npos ? target.size() : query_start)};
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
    if (const auto found = pairs.find("client_id"); found != pairs.end()) {
        parsed.client_id = found->second;
    }
    if (const auto found = pairs.find("iss"); found != pairs.end()) {
        parsed.iss = found->second;
    }
    return parsed;
}

[[nodiscard]] boost::beast::http::response<boost::beast::http::string_body> html_response(
        int status, std::string body) {
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
    Impl(OAuthExecutor executor, OAuthCallbackServerOptions options)
        // acceptor and wait_channel must reference the member: the parameter
        // has already been moved from by the time they are initialized.
        : executor(std::move(executor)), options(std::move(options)), acceptor(this->executor),
          wait_channel(this->executor, 1) {}

    OAuthExecutor executor;
    OAuthCallbackServerOptions options;
    boost::asio::basic_socket_acceptor<boost::asio::ip::tcp, OAuthExecutor> acceptor;
    WaitChannel wait_channel;
    bool closed{false};
    bool degraded{false};
    bool claimed{false};
    bool wait_cancelled{false};
};

OAuthCallbackServer::OAuthCallbackServer(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}

OAuthCallbackServer::~OAuthCallbackServer() { close(); }

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

support::AsyncResult<OAuthCallbackResult> OAuthCallbackServer::wait_for_code() {
    auto impl = impl_;
    return support::detail::make_async_result([impl]() -> boost::asio::awaitable<support::Expected<OAuthCallbackResult>> {
        WaitResult result{OAuthCallbackResult{}};
        boost::system::error_code receive_error;
        result = co_await impl->wait_channel.async_receive(
                boost::asio::redirect_error(boost::asio::use_awaitable, receive_error));
        if (receive_error) {
            co_return OAuthCallbackResult{};
        }
        co_return result;
    });
}

void OAuthCallbackServer::cancel_wait() {
    const auto impl = impl_;
    boost::asio::dispatch(impl->executor, [impl] {
        if (impl->closed || impl->claimed || impl->wait_cancelled) {
            return;
        }
        impl->wait_cancelled = true;
        impl->wait_channel.try_send(boost::system::error_code{}, WaitResult{OAuthCallbackResult{}});
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

namespace {

boost::asio::awaitable<support::Expected<std::shared_ptr<OAuthCallbackServer>>> start_callback_server(
        OAuthCallbackServerOptions options) {
    namespace asio = boost::asio;
    namespace beast = boost::beast;
    namespace http = boost::beast::http;
    using tcp = asio::ip::tcp;

    const auto executor = oauth_executor(co_await asio::this_coro::executor);
    auto impl = std::make_shared<OAuthCallbackServer::Impl>(executor, std::move(options));

    boost::system::error_code error;
    OAuthResolver resolver(executor);
    const auto results = resolver.resolve(impl->options.host, std::to_string(impl->options.port), error);
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
        impl->wait_channel.try_send(boost::system::error_code{}, WaitResult{OAuthCallbackResult{}});
        co_return std::make_shared<OAuthCallbackServer>(impl);
    }

    const auto expected_state = impl->options.state;
    const auto callback_path = impl->options.path;
    const auto extra_paths = impl->options.extra_paths;
    co_spawn(
            executor,
            [impl, expected_state, callback_path, extra_paths]() -> asio::awaitable<void> {
                while (!impl->closed) {
                    boost::asio::basic_stream_socket<tcp, OAuthExecutor> socket(impl->executor);
                    boost::system::error_code accept_error;
                    co_await impl->acceptor.async_accept(
                            socket, asio::redirect_error(asio::use_awaitable, accept_error));
                    if (accept_error) {
                        break;
                    }
                    co_spawn(
                            impl->executor,
                            [impl, expected_state, callback_path, extra_paths, socket = std::move(socket)]() mutable
                                    -> asio::awaitable<void> {
                                namespace http = boost::beast::http;
                                auto response = html_response(
                                        500, oauth_error_html("Internal error while processing OAuth callback."));
                                OAuthTcpStream stream(std::move(socket));
                                beast::flat_buffer buffer;
                                http::request<http::string_body> request;
                                boost::system::error_code handler_error;
                                co_await http::async_read(stream,
                                        buffer,
                                        request,
                                        asio::redirect_error(asio::use_awaitable, handler_error));
                                if (!handler_error) {
                                    const auto target = parse_target(request.target());
                                    const bool route_matches =
                                            target.path == callback_path ||
                                            std::find(extra_paths.begin(), extra_paths.end(), target.path) !=
                                                    extra_paths.end();
                                    if (request.method() != http::verb::get || !route_matches) {
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
                                            auto callback_result = co_await support::detail::await_async_result(
                                                    impl->options.callback_handler(target.code));
                                            if (callback_result) {
                                                response = html_response(
                                                        200, oauth_success_html(impl->options.success_message));
                                                OAuthCallbackResult settled;
                                                settled.value = std::move(*callback_result);
                                                settled.client_id =
                                                        target.client_id.empty()
                                                                ? std::optional<std::string>{}
                                                                : std::optional<std::string>{target.client_id};
                                                settled.iss = target.iss.empty()
                                                                      ? std::optional<std::string>{}
                                                                      : std::optional<std::string>{target.iss};
                                                impl->wait_channel.try_send(
                                                        boost::system::error_code{}, WaitResult{std::move(settled)});
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
                                            OAuthCallbackResult settled;
                                            settled.value = std::move(target.code);
                                            settled.client_id = target.client_id.empty()
                                                                        ? std::optional<std::string>{}
                                                                        : std::optional<std::string>{target.client_id};
                                            settled.iss = target.iss.empty() ? std::optional<std::string>{}
                                                                             : std::optional<std::string>{target.iss};
                                            impl->wait_channel.try_send(
                                                    boost::system::error_code{}, WaitResult{std::move(settled)});
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

} // namespace

support::AsyncResult<std::shared_ptr<OAuthCallbackServer>> OAuthCallbackServer::start(
        OAuthCallbackServerOptions options) {
    return support::detail::make_async_result(
            [options = std::move(options)]() mutable
                    -> boost::asio::awaitable<support::Expected<std::shared_ptr<OAuthCallbackServer>>> {
                co_return co_await start_callback_server(std::move(options));
            });
}

} // namespace cch::mcp
