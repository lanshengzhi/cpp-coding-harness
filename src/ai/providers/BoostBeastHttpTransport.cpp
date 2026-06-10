#include "BoostBeastHttpTransport.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>

#include <openssl/ssl.h>

#include <sstream>

namespace cch::ai::providers {
namespace {

struct ParsedUrl {
    std::string host;
    std::string port{"443"};
    std::string target{"/"};
};

util::Result<ParsedUrl> parse_https_url(const std::string& url) {
    constexpr std::string_view scheme = "https://";
    if (url.rfind(std::string(scheme), 0) != 0) {
        return util::Result<ParsedUrl>::failure("BoostBeastHttpTransport only supports https URLs");
    }
    auto rest = url.substr(scheme.size());
    auto slash = rest.find('/');
    auto authority = slash == std::string::npos ? rest : rest.substr(0, slash);
    ParsedUrl parsed;
    parsed.target = slash == std::string::npos ? "/" : rest.substr(slash);
    if (authority.empty()) {
        return util::Result<ParsedUrl>::failure("https URL is missing host");
    }
    auto colon = authority.rfind(':');
    if (colon != std::string::npos) {
        parsed.host = authority.substr(0, colon);
        parsed.port = authority.substr(colon + 1);
    } else {
        parsed.host = authority;
    }
    if (parsed.host.empty() || parsed.port.empty()) {
        return util::Result<ParsedUrl>::failure("https URL has invalid host or port");
    }
    return util::Result<ParsedUrl>::success(std::move(parsed));
}

} // namespace

util::Result<HttpResponse> BoostBeastHttpTransport::send(const HttpRequest& request) {
    namespace asio = boost::asio;
    namespace beast = boost::beast;
    namespace http = boost::beast::http;
    namespace ssl = boost::asio::ssl;
    using tcp = boost::asio::ip::tcp;

    auto parsed = parse_https_url(request.url);
    if (!parsed) {
        return util::Result<HttpResponse>::failure(parsed.error());
    }

    try {
        asio::io_context ioc;
        ssl::context ctx(ssl::context::tls_client);
        boost::system::error_code ec;
        ctx.set_default_verify_paths(ec);
        if (ec) {
            return util::Result<HttpResponse>::failure("CA loading failure: " + ec.message());
        }

        beast::ssl_stream<beast::tcp_stream> stream(ioc, ctx);
        if (!SSL_set_tlsext_host_name(stream.native_handle(), parsed.value().host.c_str())) {
            return util::Result<HttpResponse>::failure("TLS SNI setup failed");
        }
        stream.set_verify_mode(ssl::verify_peer);
        stream.set_verify_callback(ssl::host_name_verification(parsed.value().host));

        tcp::resolver resolver(ioc);
        beast::get_lowest_layer(stream).expires_after(request.timeout);
        auto const results = resolver.resolve(parsed.value().host, parsed.value().port);
        beast::get_lowest_layer(stream).connect(results);
        stream.handshake(ssl::stream_base::client);

        http::request<http::string_body> req{http::verb::post, parsed.value().target, 11};
        req.set(http::field::host, parsed.value().host);
        req.set(http::field::user_agent, "cpp-coding-harness/0.1");
        for (const auto& [key, value] : request.headers) {
            req.set(key, value);
        }
        req.body() = request.body;
        req.prepare_payload();

        http::write(stream, req);
        beast::flat_buffer buffer;
        http::response<http::string_body> res;
        http::read(stream, buffer, res);

        boost::system::error_code shutdown_ec;
        stream.shutdown(shutdown_ec);
        if (shutdown_ec == asio::error::eof) {
            shutdown_ec = {};
        }
        if (shutdown_ec) {
            return util::Result<HttpResponse>::failure("TLS shutdown failure: " + shutdown_ec.message());
        }

        HttpResponse response;
        response.status_code = static_cast<int>(res.result_int());
        response.body = res.body();
        for (const auto& field : res.base()) {
            response.headers[std::string(field.name_string())] = std::string(field.value());
        }
        return util::Result<HttpResponse>::success(std::move(response));
    } catch (const std::exception& e) {
        return util::Result<HttpResponse>::failure(e.what());
    }
}

} // namespace cch::ai::providers
