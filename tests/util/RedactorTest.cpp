#include "util/Redactor.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace cch;

TEST_CASE("normalized_secret_key strips non-alphanumerics and uppercases", "[util][redactor][issue66]") {
    CHECK(util::normalized_secret_key("Api-Key_1") == "APIKEY1");
    CHECK(util::normalized_secret_key("openai api key") == "OPENAIAPIKEY");
    CHECK(util::normalized_secret_key("") == "");
}

TEST_CASE("looks_secret_key matches secret-shaped keys and exempts token counters", "[util][redactor][issue66]") {
    CHECK(util::looks_secret_key("api_key"));
    CHECK(util::looks_secret_key("APIKEY"));
    CHECK(util::looks_secret_key("auth_token"));
    CHECK(util::looks_secret_key("client_secret"));
    CHECK(util::looks_secret_key("password"));
    CHECK(util::looks_secret_key("Authorization"));
    CHECK(util::looks_secret_key("OPENAI_ORG"));

    CHECK_FALSE(util::looks_secret_key("username"));
    CHECK_FALSE(util::looks_secret_key("monkey"));
    CHECK_FALSE(util::looks_secret_key("total_tokens"));
    CHECK_FALSE(util::looks_secret_key("tokens_before"));
}

TEST_CASE("redact_text redacts colon-separated secret assignments", "[util][redactor][issue66]") {
    CHECK(util::redact_text(R"({"api_key": "sk_live_1234567890"})") == R"({"api_key": "[REDACTED]"})");
    CHECK(util::redact_text("password: hunter2") == "password: [REDACTED]");
}

TEST_CASE("redact_text redacts equals-separated secret assignments", "[util][redactor][issue66]") {
    CHECK(util::redact_text("PASSWORD=hunter2") == "PASSWORD=[REDACTED]");
    CHECK(util::redact_text("export API_TOKEN = abc123") == "export API_TOKEN = [REDACTED]");
}

TEST_CASE("redact_text leaves non-secret assignments untouched", "[util][redactor][issue66]") {
    const std::string input = R"({"name": "value", "count": 3})";
    CHECK(util::redact_text(input) == input);
    CHECK(util::redact_text("total_tokens: 42") == "total_tokens: 42");
}

TEST_CASE("redact_text redacts quoted values including escaped quotes", "[util][redactor][issue66]") {
    CHECK(util::redact_text(R"(token = "ab\"cd" rest)") == R"(token = "[REDACTED]" rest)");
    CHECK(util::redact_text("secret='plain'") == "secret='[REDACTED]'");
}

TEST_CASE("redact_text keeps authorization values with spaces together", "[util][redactor][issue66]") {
    CHECK(util::redact_text("Authorization: Bearer abc def\nnext") == "Authorization: [REDACTED]\nnext");
}

TEST_CASE("redact_text stops unquoted non-authorization values at whitespace", "[util][redactor][issue66]") {
    CHECK(util::redact_text("api_key=abc def") == "api_key=[REDACTED] def");
}

TEST_CASE("redact_text redacts sk- tokens with long enough suffixes", "[util][redactor][issue66]") {
    CHECK(util::redact_text("key sk-abcdefghij here") == "key [REDACTED] here");
    CHECK(util::redact_text("sk-abcdefgh,next") == "[REDACTED],next");
}

TEST_CASE("redact_text leaves short sk- tokens untouched", "[util][redactor][issue66]") {
    CHECK(util::redact_text("code sk-abc end") == "code sk-abc end");
}

TEST_CASE("redact_text redacts AKIA tokens of uppercase digits only", "[util][redactor][issue66]") {
    CHECK(util::redact_text("aws AKIAIOSFODNN7EXAMPLE end") == "aws [REDACTED] end");
    CHECK(util::redact_text("aws AKIAiosfodnn7example end") == "aws AKIAiosfodnn7example end");
    CHECK(util::redact_text("aws AKIA1234 end") == "aws AKIA1234 end");
}

TEST_CASE("redact_text redacts multiple secrets in one text", "[util][redactor][issue66]") {
    CHECK(util::redact_text("a=sk-12345678 and password: x") == "a=[REDACTED] and password: [REDACTED]");
}

TEST_CASE("redact_text is idempotent", "[util][redactor][issue66]") {
    const std::string input = R"({"api_key": "abc", "note": "sk-12345678"})";
    const auto once = util::redact_text(input);
    CHECK(util::redact_text(once) == once);
}
