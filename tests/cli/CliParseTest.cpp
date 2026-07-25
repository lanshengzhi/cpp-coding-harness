#include "../../third_party/catch2/catch_test_macros.hpp"

#include "cli/CliParse.hpp"
#include "harness/UniqueFd.hpp"

#include <filesystem>
#include <string>
#include <variant>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <unistd.h>
#endif

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

TEST_CASE("parse_args defaults to no turn cap and records an explicit --max-turns", "[cli][parse][issue68]") {
    std::vector<std::string> args{"cpp-harness", "--fake", "hello"};
    auto argv = argv_from_strings(args);
    auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
    REQUIRE(parsed);
    CHECK_FALSE(parsed->max_turns.has_value());

    std::vector<std::string> capped_args{"cpp-harness", "--fake", "--max-turns", "12", "hello"};
    auto capped_argv = argv_from_strings(capped_args);
    auto capped_parsed = cch::cli::parse_args(static_cast<int>(capped_argv.size()), capped_argv.data());
    REQUIRE(capped_parsed);
    REQUIRE(capped_parsed->max_turns.has_value());
    CHECK(*capped_parsed->max_turns == 12);
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

TEST_CASE("parse_args maps --no-session to the in-memory target", "[cli][parse][session-target]") {
    std::vector<std::string> args{"cpp-harness", "--fake", "--no-session", "hello"};
    auto argv = argv_from_strings(args);
    auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
    REQUIRE(parsed);
    CHECK(std::holds_alternative<cch::coding_agent::InMemorySessionTarget>(parsed->session_target));
}

TEST_CASE("parse_args rejects --no-session with an explicit create target", "[cli][parse][session-target]") {
    std::vector<std::string> args{"cpp-harness", "--fake", "--no-session", "--session", "new.jsonl", "hello"};
    auto argv = argv_from_strings(args);
    auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
    REQUIRE_FALSE(parsed);
    CHECK(parsed.error().message.find("--no-session cannot be combined with --session") != std::string::npos);
}

TEST_CASE("parse_args rejects --no-session with an explicit resume target", "[cli][parse][session-target]") {
    std::vector<std::string> args{"cpp-harness", "--fake", "--no-session", "--resume", "old.jsonl", "hello"};
    auto argv = argv_from_strings(args);
    auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
    REQUIRE_FALSE(parsed);
    CHECK(parsed.error().message.find("--no-session cannot be combined with --resume") != std::string::npos);
}

TEST_CASE("parse_args help text advertises --no-session", "[cli][parse][session-target]") {
    std::vector<std::string> args{"cpp-harness", "--help"};
    auto argv = argv_from_strings(args);
    auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
    REQUIRE(parsed);
    REQUIRE(parsed->help);
    CHECK(parsed->help_text.find("--no-session") != std::string::npos);
}

TEST_CASE("parse_args captures --session-dir as the automatic-directory override", "[cli][parse][session-dir]") {
    std::vector<std::string> args{"cpp-harness", "--fake", "--session-dir", "/data/sessions", "hello"};
    auto argv = argv_from_strings(args);
    auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
    REQUIRE(parsed);
    REQUIRE(parsed->session_dir.has_value());
    CHECK(*parsed->session_dir == "/data/sessions");
    // The automatic-directory override leaves the normalized default target alone.
    CHECK(std::holds_alternative<cch::coding_agent::DefaultPersistedSessionTarget>(parsed->session_target));
}

TEST_CASE("parse_args defaults --session-dir to absent", "[cli][parse][session-dir]") {
    std::vector<std::string> args{"cpp-harness", "--fake", "hello"};
    auto argv = argv_from_strings(args);
    auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
    REQUIRE(parsed);
    CHECK_FALSE(parsed->session_dir.has_value());
}

TEST_CASE("parse_args accepts --session-dir alongside explicit and in-memory targets", "[cli][parse][session-dir]") {
    {
        std::vector<std::string> args{"cpp-harness", "--fake", "--session-dir", "/data", "--session", "explicit.jsonl", "hello"};
        auto argv = argv_from_strings(args);
        auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
        REQUIRE(parsed);
        REQUIRE(parsed->session_dir.has_value());
        const auto* target = std::get_if<cch::coding_agent::ExplicitNewSessionTarget>(&parsed->session_target);
        REQUIRE(target != nullptr);
        CHECK(target->path == "explicit.jsonl");
    }
    {
        std::vector<std::string> args{"cpp-harness", "--fake", "--session-dir", "/data", "--resume", "explicit.jsonl", "hello"};
        auto argv = argv_from_strings(args);
        auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
        REQUIRE(parsed);
        CHECK(std::holds_alternative<cch::coding_agent::ExplicitResumeSessionTarget>(parsed->session_target));
    }
    {
        std::vector<std::string> args{"cpp-harness", "--fake", "--session-dir", "/data", "--no-session", "hello"};
        auto argv = argv_from_strings(args);
        auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
        REQUIRE(parsed);
        CHECK(std::holds_alternative<cch::coding_agent::InMemorySessionTarget>(parsed->session_target));
    }
}

TEST_CASE("parse_args help text advertises the session-directory override precedence", "[cli][parse][session-dir]") {
    std::vector<std::string> args{"cpp-harness", "--help"};
    auto argv = argv_from_strings(args);
    auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
    REQUIRE(parsed);
    REQUIRE(parsed->help);
    CHECK(parsed->help_text.find("--session-dir") != std::string::npos);
    CHECK(parsed->help_text.find("CCH_CODING_AGENT_SESSION_DIR") != std::string::npos);
    CHECK(parsed->help_text.find("sessionDir") != std::string::npos);
}

TEST_CASE("parse_args still requires prompt for json mode", "[cli][parse]") {
    std::vector<std::string> args{"cpp-harness", "--fake", "--mode", "json"};
    auto argv = argv_from_strings(args);
    auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
    REQUIRE_FALSE(parsed);
    CHECK(parsed.error().message.find("prompt is required") != std::string::npos);
}

#if defined(__unix__) || defined(__APPLE__)
TEST_CASE("parse_args reports a diagnostic when the working directory is unavailable", "[cli][parse][issue67]") {
    const cch::harness::UniqueFd saved_cwd(::open(".", O_RDONLY | O_DIRECTORY));
    REQUIRE(saved_cwd);

    auto dir_template = (std::filesystem::temp_directory_path() / "cch-cli-deleted-cwd-XXXXXX").string();
    std::vector<char> dir_buffer(dir_template.begin(), dir_template.end());
    dir_buffer.push_back('\0');
    const char* deleted_dir = ::mkdtemp(dir_buffer.data());
    REQUIRE(deleted_dir != nullptr);
    REQUIRE(::chdir(deleted_dir) == 0);
    REQUIRE(::rmdir(deleted_dir) == 0);

    std::vector<std::string> args{"cpp-harness", "--fake", "hello"};
    auto argv = argv_from_strings(args);
    auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());

    REQUIRE(::fchdir(saved_cwd.get()) == 0);

    REQUIRE_FALSE(parsed);
    CHECK(parsed.error().message.find("working directory") != std::string::npos);
}
#endif
