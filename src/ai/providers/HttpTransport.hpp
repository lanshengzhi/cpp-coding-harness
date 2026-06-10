#pragma once

#include "../../util/Result.hpp"

#include <chrono>
#include <map>
#include <string>

namespace cch::ai::providers {

struct HttpRequest {
    std::string method{"POST"};
    std::string url;
    std::map<std::string, std::string> headers;
    std::string body;
    std::chrono::milliseconds timeout{30000};
};

struct HttpResponse {
    int status_code{0};
    std::map<std::string, std::string> headers;
    std::string body;
};

class HttpTransport {
public:
    virtual ~HttpTransport() = default;
    [[nodiscard]] virtual util::Result<HttpResponse> send(const HttpRequest& request) = 0;
};

} // namespace cch::ai::providers
