#pragma once

#include "HttpTransport.hpp"

namespace cch::llm {

class BoostBeastHttpTransport final : public HttpTransport {
public:
    [[nodiscard]] util::Result<HttpResponse> send(const HttpRequest& request) override;
};

} // namespace cch::llm
