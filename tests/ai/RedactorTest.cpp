#include "ai/Redactor.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace cch;

TEST_CASE("normalized_secret_key strips non-alphanumerics and uppercases", "[ai][redactor][issue66]") {
    CHECK(ai::normalized_secret_key("Api-Key_1") == "APIKEY1");
    CHECK(ai::normalized_secret_key("openai api key") == "OPENAIAPIKEY");
    CHECK(ai::normalized_secret_key("") == "");
}

TEST_CASE("looks_secret_key matches secret-shaped keys and exempts token counters", "[ai][redactor][issue66]") {
    CHECK(ai::looks_secret_key("api_key"));
    CHECK(ai::looks_secret_key("APIKEY"));
    CHECK(ai::looks_secret_key("auth_token"));
    CHECK(ai::looks_secret_key("client_secret"));
    CHECK(ai::looks_secret_key("password"));
    CHECK(ai::looks_secret_key("Authorization"));
    CHECK(ai::looks_secret_key("OPENAI_ORG"));

    CHECK_FALSE(ai::looks_secret_key("username"));
    CHECK_FALSE(ai::looks_secret_key("monkey"));
    CHECK_FALSE(ai::looks_secret_key("total_tokens"));
    CHECK_FALSE(ai::looks_secret_key("tokens_before"));
}

TEST_CASE("redact_text redacts colon-separated secret assignments", "[ai][redactor][issue66]") {
    CHECK(ai::redact_text(R"({"api_key": "sk_live_1234567890"})") == R"({"api_key": "[REDACTED]"})");
    CHECK(ai::redact_text("password: hunter2") == "password: [REDACTED]");
}

TEST_CASE("redact_text redacts equals-separated secret assignments", "[ai][redactor][issue66]") {
    CHECK(ai::redact_text("PASSWORD=hunter2") == "PASSWORD=[REDACTED]");
    CHECK(ai::redact_text("export API_TOKEN = abc123") == "export API_TOKEN = [REDACTED]");
}

TEST_CASE("redact_text leaves non-secret assignments untouched", "[ai][redactor][issue66]") {
    const std::string input = R"({"name": "value", "count": 3})";
    CHECK(ai::redact_text(input) == input);
    CHECK(ai::redact_text("total_tokens: 42") == "total_tokens: 42");
}

TEST_CASE("redact_text redacts quoted values including escaped quotes", "[ai][redactor][issue66]") {
    CHECK(ai::redact_text(R"(token = "ab\"cd" rest)") == R"(token = "[REDACTED]" rest)");
    CHECK(ai::redact_text("secret='plain'") == "secret='[REDACTED]'");
}

TEST_CASE("redact_text keeps authorization values with spaces together", "[ai][redactor][issue66]") {
    CHECK(ai::redact_text("Authorization: Bearer abc def\nnext") == "Authorization: [REDACTED]\nnext");
}

TEST_CASE("redact_text stops unquoted non-authorization values at whitespace", "[ai][redactor][issue66]") {
    CHECK(ai::redact_text("api_key=abc def") == "api_key=[REDACTED] def");
}

TEST_CASE("redact_text redacts sk- tokens with long enough suffixes", "[ai][redactor][issue66]") {
    CHECK(ai::redact_text("key sk-abcdefghij here") == "key [REDACTED] here");
    CHECK(ai::redact_text("sk-abcdefgh,next") == "[REDACTED],next");
}

TEST_CASE("redact_text leaves short sk- tokens untouched", "[ai][redactor][issue66]") {
    CHECK(ai::redact_text("code sk-abc end") == "code sk-abc end");
}

TEST_CASE("redact_text redacts AKIA tokens of uppercase digits only", "[ai][redactor][issue66]") {
    CHECK(ai::redact_text("aws AKIAIOSFODNN7EXAMPLE end") == "aws [REDACTED] end");
    CHECK(ai::redact_text("aws AKIAiosfodnn7example end") == "aws AKIAiosfodnn7example end");
    CHECK(ai::redact_text("aws AKIA1234 end") == "aws AKIA1234 end");
}

TEST_CASE("redact_text redacts multiple secrets in one text", "[ai][redactor][issue66]") {
    CHECK(ai::redact_text("a=sk-12345678 and password: x") == "a=[REDACTED] and password: [REDACTED]");
}

TEST_CASE("redact_text is idempotent", "[ai][redactor][issue66]") {
    const std::string input = R"({"api_key": "abc", "note": "sk-12345678"})";
    const auto once = ai::redact_text(input);
    CHECK(ai::redact_text(once) == once);
}
