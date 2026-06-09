#include "../../third_party/catch2/catch_test_macros.hpp"

#include "../../src/llm/BoostBeastHttpTransport.hpp"

using namespace cch;

TEST_CASE("BoostBeast transport rejects unsupported URLs before network", "[llm][u6]") {
    llm::BoostBeastHttpTransport transport;
    llm::HttpRequest request;
    request.url = "http://example.com/v1/chat/completions";

    auto response = transport.send(request);

    REQUIRE_FALSE(response.ok());
    CHECK(response.error().find("https") != std::string::npos);
}

TEST_CASE("BoostBeast transport rejects missing host before network", "[llm][u6]") {
    llm::BoostBeastHttpTransport transport;
    llm::HttpRequest request;
    request.url = "https:///v1/chat/completions";

    auto response = transport.send(request);

    REQUIRE_FALSE(response.ok());
    CHECK(response.error().find("host") != std::string::npos);
}
