#include "../../third_party/catch2/catch_test_macros.hpp"

#include "cli/CliParse.hpp"

#include <string>
#include <variant>
#include <vector>

namespace {

std::vector<char*> argv_from_strings(std::vector<std::string>& args) {
    std::vector<char*> argv;
    argv.reserve(args.size());
    for (auto& arg : args) {
        argv.push_back(arg.data());
    }
    return argv;
}

} // namespace

TEST_CASE("parse_args leaves provider overrides empty when model flags omitted", "[cli][parse]") {
    std::vector<std::string> args{"cpp-harness", "--fake", "hello"};
    auto argv = argv_from_strings(args);
    auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
    REQUIRE(parsed);
    CHECK_FALSE(parsed->provider_overrides.model.has_value());
    CHECK_FALSE(parsed->provider_overrides.base_url.has_value());
    CHECK_FALSE(parsed->provider_overrides.api_key_env.has_value());
}

TEST_CASE("parse_args records explicit provider overrides", "[cli][parse]") {
    std::vector<std::string> args{
        "cpp-harness",
        "--fake",
        "--model",
        "demo-model",
        "--base-url",
        "https://demo.example/v1",
        "--api-key-env",
        "DEMO_KEY",
        "hello",
    };
    auto argv = argv_from_strings(args);
    auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
    REQUIRE(parsed);
    REQUIRE(parsed->provider_overrides.model.has_value());
    REQUIRE(parsed->provider_overrides.base_url.has_value());
    REQUIRE(parsed->provider_overrides.api_key_env.has_value());
    CHECK(*parsed->provider_overrides.model == "demo-model");
    CHECK(*parsed->provider_overrides.base_url == "https://demo.example/v1");
    CHECK(*parsed->provider_overrides.api_key_env == "DEMO_KEY");
}

TEST_CASE("parse_args rejects json mode with repl", "[cli][parse]") {
    std::vector<std::string> args{"cpp-harness", "--fake", "--mode", "json", "--repl"};
    auto argv = argv_from_strings(args);
    auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
    REQUIRE_FALSE(parsed);
    CHECK(parsed.error().message.find("--mode json cannot be combined with --repl") != std::string::npos);
}

TEST_CASE("parse_args rejects rpc mode with positional prompt", "[cli][parse]") {
    std::vector<std::string> args{"cpp-harness", "--fake", "--mode", "rpc", "hello"};
    auto argv = argv_from_strings(args);
    auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
    REQUIRE_FALSE(parsed);
    CHECK(parsed.error().message.find("positional prompt is not allowed") != std::string::npos);
}

TEST_CASE("parse_args normalizes unknown options", "[cli][parse]") {
    std::vector<std::string> args{"cpp-harness", "--fake", "--not-a-flag", "hello"};
    auto argv = argv_from_strings(args);
    auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
    REQUIRE_FALSE(parsed);
    CHECK(parsed.error().message.find("unknown option: --not-a-flag") != std::string::npos);
}

TEST_CASE("parse_args maps approve flags to project trust override", "[cli][parse]") {
    {
        std::vector<std::string> args{"cpp-harness", "--fake", "--approve", "hello"};
        auto argv = argv_from_strings(args);
        auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
        REQUIRE(parsed);
        REQUIRE(parsed->project_trust_override.has_value());
        CHECK(*parsed->project_trust_override);
    }
    {
        std::vector<std::string> args{"cpp-harness", "--fake", "--no-approve", "hello"};
        auto argv = argv_from_strings(args);
        auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
        REQUIRE(parsed);
        REQUIRE(parsed->project_trust_override.has_value());
        CHECK_FALSE(*parsed->project_trust_override);
    }
}

TEST_CASE("parse_args treats prompt-template as one repeatable path", "[cli][parse]") {
    std::vector<std::string> args{
        "cpp-harness",
        "--fake",
        "--prompt-template",
        "custom.md",
        "--prompt-template",
        "more.md",
        "/custom",
        "Ada",
    };
    auto argv = argv_from_strings(args);
    auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
    REQUIRE(parsed);
    REQUIRE(parsed->prompt_template_paths.size() == 2);
    CHECK(parsed->prompt_template_paths[0] == "custom.md");
    CHECK(parsed->prompt_template_paths[1] == "more.md");
    CHECK(parsed->prompt == "/custom Ada");
}

TEST_CASE("parse_args defaults output mode to text", "[cli][parse]") {
    std::vector<std::string> args{"cpp-harness", "--fake", "hello"};
    auto argv = argv_from_strings(args);
    auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
    REQUIRE(parsed);
    CHECK(parsed->output_mode == cch::cli::OutputMode::Text);
}

TEST_CASE("parse_args defaults empty text-mode prompt to repl", "[cli][parse]") {
    std::vector<std::string> args{"cpp-harness", "--fake"};
    auto argv = argv_from_strings(args);
    auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
    REQUIRE(parsed);
    CHECK(parsed->repl);
    CHECK(parsed->prompt.empty());
}

TEST_CASE("parse_args represents an omitted session target as default persisted creation", "[cli][parse][session-target]") {
    std::vector<std::string> args{"cpp-harness", "--fake", "hello"};
    auto argv = argv_from_strings(args);
    auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
    REQUIRE(parsed);
    CHECK(std::holds_alternative<cch::coding_agent::DefaultPersistedSessionTarget>(parsed->session_target));
}

TEST_CASE("parse_args maps --session to an explicit new-session target", "[cli][parse][session-target]") {
    std::vector<std::string> args{"cpp-harness", "--fake", "--session", "new.jsonl", "hello"};
    auto argv = argv_from_strings(args);
    auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
    REQUIRE(parsed);
    const auto* target = std::get_if<cch::coding_agent::ExplicitNewSessionTarget>(&parsed->session_target);
    REQUIRE(target != nullptr);
    CHECK(target->path == "new.jsonl");
}

TEST_CASE("parse_args maps --resume to an explicit resume target", "[cli][parse][session-target]") {
    std::vector<std::string> args{"cpp-harness", "--fake", "--resume", "old.jsonl", "hello"};
    auto argv = argv_from_strings(args);
    auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
    REQUIRE(parsed);
    const auto* target = std::get_if<cch::coding_agent::ExplicitResumeSessionTarget>(&parsed->session_target);
    REQUIRE(target != nullptr);
    CHECK(target->path == "old.jsonl");
}

TEST_CASE("parse_args still requires prompt for json mode", "[cli][parse]") {
    std::vector<std::string> args{"cpp-harness", "--fake", "--mode", "json"};
    auto argv = argv_from_strings(args);
    auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
    REQUIRE_FALSE(parsed);
    CHECK(parsed.error().message.find("prompt is required") != std::string::npos);
}
