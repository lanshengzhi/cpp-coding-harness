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

TEST_CASE("parse_args leaves model selection empty when model flags omitted", "[cli][parse]") {
    std::vector<std::string> args{"cpp-harness", "--fake", "hello"};
    auto argv = argv_from_strings(args);
    auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
    REQUIRE(parsed);
    CHECK_FALSE(parsed->model.has_value());
    CHECK_FALSE(parsed->provider.has_value());
    CHECK(parsed->models.empty());
    CHECK_FALSE(parsed->api_key.has_value());
}

TEST_CASE("parse_args records the pi CLI model selection surface", "[cli][parse]") {
    std::vector<std::string> args{
        "cpp-harness",
        "--fake",
        "--provider",
        "deepseek",
        "--model",
        "deepseek-v4-flash",
        "--models",
        "deepseek-v4-flash,deepseek-r1:high",
        "--api-key",
        "sk-demo",
        "hello",
    };
    auto argv = argv_from_strings(args);
    auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
    REQUIRE(parsed);
    REQUIRE(parsed->provider.has_value());
    REQUIRE(parsed->model.has_value());
    REQUIRE(parsed->api_key.has_value());
    CHECK(*parsed->provider == "deepseek");
    CHECK(*parsed->model == "deepseek-v4-flash");
    CHECK(*parsed->api_key == "sk-demo");
    REQUIRE(parsed->models.size() == 2);
    CHECK(parsed->models[0] == "deepseek-v4-flash");
    CHECK(parsed->models[1] == "deepseek-r1:high");
}

TEST_CASE("parse_args trims --models patterns and tolerates empty entries", "[cli][parse]") {
    std::vector<std::string> args{
        "cpp-harness", "--models", " sonnet ,, haiku ", "hello"};
    auto argv = argv_from_strings(args);
    auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
    REQUIRE(parsed);
    REQUIRE(parsed->models.size() == 2);
    CHECK(parsed->models[0] == "sonnet");
    CHECK(parsed->models[1] == "haiku");
}

TEST_CASE("parse_args rejects --api-key without an explicit model", "[cli][parse]") {
    std::vector<std::string> args{"cpp-harness", "--api-key", "sk-demo", "hello"};
    auto argv = argv_from_strings(args);
    auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
    REQUIRE_FALSE(parsed);
    CHECK(
        parsed.error().message.find("--api-key requires a model") != std::string::npos);
}

TEST_CASE("parse_args accepts --api-key with each explicit-model form", "[cli][parse]") {
    {
        std::vector<std::string> args{"cpp-harness", "--model", "gpt-5.5", "--api-key", "k", "hello"};
        auto argv = argv_from_strings(args);
        auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
        REQUIRE(parsed);
    }
    {
        std::vector<std::string> args{"cpp-harness", "--provider", "deepseek", "--model", "deepseek-v4-flash", "--api-key", "k", "hello"};
        auto argv = argv_from_strings(args);
        auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
        REQUIRE(parsed);
    }
    {
        std::vector<std::string> args{"cpp-harness", "--models", "gpt-5.5", "--api-key", "k", "hello"};
        auto argv = argv_from_strings(args);
        auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
        REQUIRE(parsed);
    }
}

TEST_CASE("parse_args rejects the removed --base-url flag", "[cli][parse]") {
    std::vector<std::string> args{"cpp-harness", "--fake", "--base-url", "https://x", "hello"};
    auto argv = argv_from_strings(args);
    auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
    REQUIRE_FALSE(parsed);
    CHECK(parsed.error().message.find("unknown option: --base-url") != std::string::npos);
}

TEST_CASE("parse_args rejects the removed --api-key-env flag", "[cli][parse]") {
    std::vector<std::string> args{"cpp-harness", "--fake", "--api-key-env", "OPENAI_API_KEY", "hello"};
    auto argv = argv_from_strings(args);
    auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
    REQUIRE_FALSE(parsed);
    CHECK(parsed.error().message.find("unknown option: --api-key-env") != std::string::npos);
}

TEST_CASE("parse_args rejects the removed --auth flag", "[cli][parse]") {
    std::vector<std::string> args{"cpp-harness", "--fake", "--auth", "openai", "hello"};
    auto argv = argv_from_strings(args);
    auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
    REQUIRE_FALSE(parsed);
    CHECK(parsed.error().message.find("unknown option: --auth") != std::string::npos);
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

TEST_CASE("parse_args rejects the removed repl option", "[cli][parse][issue64]") {
    std::vector<std::string> args{"cpp-harness", "--fake", "--repl"};
    auto argv = argv_from_strings(args);
    auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
    REQUIRE_FALSE(parsed);
    CHECK(parsed.error().message.find("unknown option: --repl") != std::string::npos);
}

TEST_CASE("parse_args exposes no temporary TUI selector", "[cli][parse][issue64]") {
    std::vector<std::string> args{"cpp-harness", "--fake", "--tui"};
    auto argv = argv_from_strings(args);
    auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
    REQUIRE_FALSE(parsed);
    CHECK(parsed.error().message.find("unknown option: --tui") != std::string::npos);

    std::vector<std::string> mode_args{"cpp-harness", "--fake", "--mode", "tui"};
    auto mode_argv = argv_from_strings(mode_args);
    parsed = cch::cli::parse_args(static_cast<int>(mode_argv.size()), mode_argv.data());
    REQUIRE_FALSE(parsed);
    CHECK(parsed.error().message.find("unsupported --mode: tui") != std::string::npos);
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

TEST_CASE("parse_args records print intent without requiring positional input", "[cli][parse][issue64]") {
    std::vector<std::string> args{"cpp-harness", "--fake", "--print"};
    auto argv = argv_from_strings(args);
    auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
    REQUIRE(parsed);
    CHECK(parsed->print);
    CHECK(parsed->prompt.empty());
}

TEST_CASE("parse_args retains positional file arguments separately from prompt text", "[cli][parse][issue63]") {
    std::vector<std::string> args{
        "cpp-harness",
        "--fake",
        "@first.png",
        "describe",
        "@second.webp",
        "these",
    };
    auto argv = argv_from_strings(args);
    auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());

    REQUIRE(parsed);
    CHECK(parsed->prompt == "describe these");
    REQUIRE(parsed->file_arguments.size() == 2);
    CHECK(parsed->file_arguments[0] == "first.png");
    CHECK(parsed->file_arguments[1] == "second.webp");
}

TEST_CASE("parse_args treats a lone positional file as initial input", "[cli][parse][issue63]") {
    std::vector<std::string> args{"cpp-harness", "--fake", "@only.gif"};
    auto argv = argv_from_strings(args);
    auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());

    REQUIRE(parsed);
    CHECK(parsed->prompt.empty());
    REQUIRE(parsed->file_arguments.size() == 1);
    CHECK(parsed->file_arguments[0] == "only.gif");
}

TEST_CASE("parse_args accepts a lone positional file in json mode", "[cli][parse][issue63]") {
    std::vector<std::string> args{
        "cpp-harness", "--fake", "--mode", "json", "@image.webp"};
    auto argv = argv_from_strings(args);
    auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());

    REQUIRE(parsed);
    CHECK(parsed->output_mode == cch::cli::OutputMode::Json);
    CHECK(parsed->prompt.empty());
    REQUIRE(parsed->file_arguments.size() == 1);
    CHECK(parsed->file_arguments[0] == "image.webp");
}

TEST_CASE("parse_args rejects positional files in rpc mode", "[cli][parse][issue63]") {
    std::vector<std::string> args{"cpp-harness", "--fake", "--mode", "rpc", "@image.png"};
    auto argv = argv_from_strings(args);
    auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());

    REQUIRE_FALSE(parsed);
    CHECK(parsed.error().message.find("positional prompt is not allowed") != std::string::npos);
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
    CHECK(parsed->help_text.find("PI_CODING_AGENT_SESSION_DIR") != std::string::npos);
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
