#include <catch2/catch_test_macros.hpp>

#include "cli/CliParse.hpp"
#include "support/UniqueFd.hpp"

#include <filesystem>
#include <string>
#include <variant>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

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
    std::vector<std::string> args{"cpp-harness", "hello"};
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
    std::vector<std::string> args{"cpp-harness", "--base-url", "https://x", "hello"};
    auto argv = argv_from_strings(args);
    auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
    REQUIRE_FALSE(parsed);
    CHECK(parsed.error().message.find("unknown option: --base-url") != std::string::npos);
}

TEST_CASE("parse_args rejects the removed --api-key-env flag", "[cli][parse]") {
    std::vector<std::string> args{"cpp-harness", "--api-key-env", "OPENAI_API_KEY", "hello"};
    auto argv = argv_from_strings(args);
    auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
    REQUIRE_FALSE(parsed);
    CHECK(parsed.error().message.find("unknown option: --api-key-env") != std::string::npos);
}

TEST_CASE("parse_args rejects the removed --auth flag", "[cli][parse]") {
    std::vector<std::string> args{"cpp-harness", "--auth", "openai", "hello"};
    auto argv = argv_from_strings(args);
    auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
    REQUIRE_FALSE(parsed);
    CHECK(parsed.error().message.find("unknown option: --auth") != std::string::npos);
}

TEST_CASE("parse_args rejects the deleted C++-only flags as unknown", "[cli][parse]") {
    const std::vector<std::string> deleted{
        "--fake",
        "--enable-bash",
        "--max-turns",
        "--workspace",
    };
    for (const auto& flag : deleted) {
        std::vector<std::string> args{"cpp-harness", flag, "hello"};
        if (flag == "--max-turns") {
            args = {"cpp-harness", flag, "12", "hello"};
        } else if (flag == "--workspace") {
            args = {"cpp-harness", flag, "/tmp", "hello"};
        } else if (flag == "--enable-bash") {
            args = {"cpp-harness", flag, "hello"};
        }
        auto argv = argv_from_strings(args);
        auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
        REQUIRE_FALSE(parsed);
        CHECK(parsed.error().message.find("unknown option: " + flag) != std::string::npos);
    }
}

TEST_CASE("parse_args rejects the removed repl option", "[cli][parse][issue64]") {
    std::vector<std::string> args{"cpp-harness", "--repl"};
    auto argv = argv_from_strings(args);
    auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
    REQUIRE_FALSE(parsed);
    CHECK(parsed.error().message.find("unknown option: --repl") != std::string::npos);
}

TEST_CASE("parse_args exposes no temporary TUI selector", "[cli][parse][issue64]") {
    std::vector<std::string> args{"cpp-harness", "--tui"};
    auto argv = argv_from_strings(args);
    auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
    REQUIRE_FALSE(parsed);
    CHECK(parsed.error().message.find("unknown option: --tui") != std::string::npos);

    std::vector<std::string> mode_args{"cpp-harness", "--mode", "tui"};
    auto mode_argv = argv_from_strings(mode_args);
    parsed = cch::cli::parse_args(static_cast<int>(mode_argv.size()), mode_argv.data());
    REQUIRE_FALSE(parsed);
    CHECK(parsed.error().message.find("unsupported --mode: tui") != std::string::npos);
}

TEST_CASE("parse_args rejects the removed json and rpc modes explicitly", "[cli][parse]") {
    {
        std::vector<std::string> args{"cpp-harness", "--mode", "json", "hello"};
        auto argv = argv_from_strings(args);
        auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
        REQUIRE_FALSE(parsed);
        CHECK(parsed.error().message.find("--mode json was removed") != std::string::npos);
    }
    {
        std::vector<std::string> args{"cpp-harness", "--mode", "rpc", "hello"};
        auto argv = argv_from_strings(args);
        auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
        REQUIRE_FALSE(parsed);
        CHECK(parsed.error().message.find("--mode rpc was removed") != std::string::npos);
    }
}

TEST_CASE("parse_args rejects a removed mode even when it is accepted-but-unused", "[cli][parse]") {
    // Never accepted-but-ignored: the mode rejection fires with no prompt and
    // with --print, in both spellings.
    {
        std::vector<std::string> args{"cpp-harness", "--mode", "json"};
        auto argv = argv_from_strings(args);
        auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
        REQUIRE_FALSE(parsed);
        CHECK(parsed.error().message.find("--mode json was removed") != std::string::npos);
    }
    {
        std::vector<std::string> args{"cpp-harness", "--print", "--mode", "rpc"};
        auto argv = argv_from_strings(args);
        auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
        REQUIRE_FALSE(parsed);
        CHECK(parsed.error().message.find("--mode rpc was removed") != std::string::npos);
    }
}

TEST_CASE("parse_args keeps --mode text as the pi-default spelling", "[cli][parse]") {
    std::vector<std::string> args{"cpp-harness", "--mode", "text", "hello"};
    auto argv = argv_from_strings(args);
    auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
    REQUIRE(parsed);
    REQUIRE(parsed->messages.size() == 1);
    CHECK(parsed->messages[0] == "hello");
}

TEST_CASE("parse_args normalizes unknown options", "[cli][parse]") {
    std::vector<std::string> args{"cpp-harness", "--not-a-flag", "hello"};
    auto argv = argv_from_strings(args);
    auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
    REQUIRE_FALSE(parsed);
    CHECK(parsed.error().message.find("unknown option: --not-a-flag") != std::string::npos);
}

TEST_CASE("parse_args maps approve flags to project trust override", "[cli][parse]") {
    {
        std::vector<std::string> args{"cpp-harness", "--approve", "hello"};
        auto argv = argv_from_strings(args);
        auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
        REQUIRE(parsed);
        REQUIRE(parsed->project_trust_override.has_value());
        CHECK(*parsed->project_trust_override);
    }
    {
        std::vector<std::string> args{"cpp-harness", "--no-approve", "hello"};
        auto argv = argv_from_strings(args);
        auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
        REQUIRE(parsed);
        REQUIRE(parsed->project_trust_override.has_value());
        CHECK_FALSE(*parsed->project_trust_override);
    }
}

TEST_CASE("parse_args accepts pi's shorts for approve and no-approve", "[cli][parse]") {
    {
        std::vector<std::string> args{"cpp-harness", "-a", "hello"};
        auto argv = argv_from_strings(args);
        auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
        REQUIRE(parsed);
        REQUIRE(parsed->project_trust_override.has_value());
        CHECK(*parsed->project_trust_override);
    }
    {
        std::vector<std::string> args{"cpp-harness", "-na", "hello"};
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
    // pi keeps every positional as its own message: the first merges into
    // the initial prompt, the rest prompt sequentially in print mode.
    REQUIRE(parsed->messages.size() == 2);
    CHECK(parsed->messages[0] == "/custom");
    CHECK(parsed->messages[1] == "Ada");
}

TEST_CASE("parse_args accepts the plain text default", "[cli][parse]") {
    std::vector<std::string> args{"cpp-harness", "hello"};
    auto argv = argv_from_strings(args);
    auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
    REQUIRE(parsed);
    REQUIRE(parsed->messages.size() == 1);
    CHECK(parsed->messages[0] == "hello");
}

TEST_CASE("parse_args records print intent without requiring positional input", "[cli][parse][issue64]") {
    std::vector<std::string> args{"cpp-harness", "--print"};
    auto argv = argv_from_strings(args);
    auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
    REQUIRE(parsed);
    CHECK(parsed->print);
    CHECK(parsed->messages.empty());
}

TEST_CASE("parse_args retains positional file arguments separately from prompt text", "[cli][parse][issue63]") {
    std::vector<std::string> args{
        "cpp-harness",
        "@first.png",
        "describe",
        "@second.webp",
        "these",
    };
    auto argv = argv_from_strings(args);
    auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());

    REQUIRE(parsed);
    REQUIRE(parsed->messages.size() == 2);
    CHECK(parsed->messages[0] == "describe");
    CHECK(parsed->messages[1] == "these");
    REQUIRE(parsed->file_arguments.size() == 2);
    CHECK(parsed->file_arguments[0] == "first.png");
    CHECK(parsed->file_arguments[1] == "second.webp");
}

TEST_CASE("parse_args treats a lone positional file as initial input", "[cli][parse][issue63]") {
    std::vector<std::string> args{"cpp-harness", "@only.gif"};
    auto argv = argv_from_strings(args);
    auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());

    REQUIRE(parsed);
    CHECK(parsed->messages.empty());
    REQUIRE(parsed->file_arguments.size() == 1);
    CHECK(parsed->file_arguments[0] == "only.gif");
}

TEST_CASE("parse_args rejects json mode with a lone positional file", "[cli][parse][issue63]") {
    std::vector<std::string> args{
        "cpp-harness", "--mode", "json", "@image.webp"};
    auto argv = argv_from_strings(args);
    auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());

    REQUIRE_FALSE(parsed);
    CHECK(parsed.error().message.find("--mode json was removed") != std::string::npos);
}

TEST_CASE("parse_args records an omitted session family as no flags", "[cli][parse][session-target]") {
    std::vector<std::string> args{"cpp-harness", "hello"};
    auto argv = argv_from_strings(args);
    auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
    REQUIRE(parsed);
    CHECK_FALSE(parsed->session_value.has_value());
    CHECK_FALSE(parsed->resume);
    CHECK_FALSE(parsed->continue_session);
    CHECK_FALSE(parsed->no_session_flag);
    CHECK_FALSE(parsed->session_id.has_value());
    CHECK_FALSE(parsed->fork.has_value());
    CHECK_FALSE(parsed->name.has_value());
}

TEST_CASE("parse_args records --session and --resume as raw flags", "[cli][parse][session-target]") {
    {
        std::vector<std::string> args{"cpp-harness", "--session", "new.jsonl", "hello"};
        auto argv = argv_from_strings(args);
        auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
        REQUIRE(parsed);
        REQUIRE(parsed->session_value.has_value());
        CHECK(*parsed->session_value == "new.jsonl");
        CHECK_FALSE(parsed->resume);
    }
    {
        // pi: --resume is a pure boolean flag; the following token is a
        // positional message, never a path (the picker selects the session).
        std::vector<std::string> args{"cpp-harness", "--resume", "old.jsonl", "hello"};
        auto argv = argv_from_strings(args);
        auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
        REQUIRE(parsed);
        CHECK(parsed->resume);
        CHECK_FALSE(parsed->session_value.has_value());
        REQUIRE(parsed->messages.size() == 2);
        CHECK(parsed->messages[0] == "old.jsonl");
        CHECK(parsed->messages[1] == "hello");
    }
    {
        std::vector<std::string> args{"cpp-harness", "-r", "old.jsonl", "hello"};
        auto argv = argv_from_strings(args);
        auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
        REQUIRE(parsed);
        CHECK(parsed->resume);
        REQUIRE(parsed->messages.size() == 2);
        CHECK(parsed->messages[0] == "old.jsonl");
    }
}

TEST_CASE("parse_args records --no-session as a raw flag without conflict errors", "[cli][parse][session-target]") {
    // pi precedence: --no-session short-circuits silently; the C++-today
    // conflict errors are deleted, and --session/--resume coexist at parse.
    {
        std::vector<std::string> args{"cpp-harness", "--no-session", "hello"};
        auto argv = argv_from_strings(args);
        auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
        REQUIRE(parsed);
        CHECK(parsed->no_session_flag);
    }
    {
        std::vector<std::string> args{"cpp-harness", "--no-session", "--session", "new.jsonl", "hello"};
        auto argv = argv_from_strings(args);
        auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
        REQUIRE(parsed);
        CHECK(parsed->no_session_flag);
        REQUIRE(parsed->session_value.has_value());
    }
    {
        std::vector<std::string> args{"cpp-harness", "--no-session", "--resume", "old.jsonl", "hello"};
        auto argv = argv_from_strings(args);
        auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
        REQUIRE(parsed);
        CHECK(parsed->no_session_flag);
        CHECK(parsed->resume);
    }
    {
        // pi: --session wins over --resume without a parse-time exclusion.
        std::vector<std::string> args{"cpp-harness", "--session", "a.jsonl", "--resume", "b.jsonl", "hello"};
        auto argv = argv_from_strings(args);
        auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
        REQUIRE(parsed);
        REQUIRE(parsed->session_value.has_value());
        CHECK(parsed->resume);
        REQUIRE(parsed->messages.size() == 2);
        CHECK(parsed->messages[0] == "b.jsonl");
        CHECK(parsed->messages[1] == "hello");
    }
}

TEST_CASE("parse_args carries the raw pi session-family flags", "[cli][parse][session-target]") {
    std::vector<std::string> args{
        "cpp-harness",
        "--continue",
        "--session-id",
        "project-session",
        "--fork",
        "old.jsonl",
        "--name",
        "my session",
        "hello",
    };
    auto argv = argv_from_strings(args);
    auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
    REQUIRE(parsed);
    CHECK(parsed->continue_session);
    REQUIRE(parsed->session_id.has_value());
    CHECK(*parsed->session_id == "project-session");
    REQUIRE(parsed->fork.has_value());
    CHECK(*parsed->fork == "old.jsonl");
    REQUIRE(parsed->name.has_value());
    CHECK(*parsed->name == "my session");
    REQUIRE(parsed->messages.size() == 1);
    CHECK(parsed->messages[0] == "hello");
}

TEST_CASE("parse_args accepts pi's session-family shorts", "[cli][parse][session-target]") {
    {
        std::vector<std::string> args{"cpp-harness", "-c", "hello"};
        auto argv = argv_from_strings(args);
        auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
        REQUIRE(parsed);
        CHECK(parsed->continue_session);
    }
    {
        std::vector<std::string> args{"cpp-harness", "-n", "named", "hello"};
        auto argv = argv_from_strings(args);
        auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
        REQUIRE(parsed);
        REQUIRE(parsed->name.has_value());
        CHECK(*parsed->name == "named");
        REQUIRE(parsed->messages.size() == 1);
        CHECK(parsed->messages[0] == "hello");
    }
}

TEST_CASE("parse_args help text advertises the pi-aligned surface", "[cli][parse][session-target]") {
    std::vector<std::string> args{"cpp-harness", "--help"};
    auto argv = argv_from_strings(args);
    auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
    REQUIRE(parsed);
    REQUIRE(parsed->help);
    CHECK(parsed->help_text.find("--no-session") != std::string::npos);
    CHECK(parsed->help_text.find("--session-id") != std::string::npos);
    CHECK(parsed->help_text.find("--continue, -c") != std::string::npos);
    CHECK(parsed->help_text.find("--resume, -r") != std::string::npos);
    CHECK(parsed->help_text.find("--name, -n") != std::string::npos);
    CHECK(parsed->help_text.find("--no-approve, -na") != std::string::npos);
    CHECK(parsed->help_text.find("--no-skills, -ns") != std::string::npos);
    CHECK(parsed->help_text.find("--no-prompt-templates, -np") != std::string::npos);
    CHECK(parsed->help_text.find("--no-context-files, -nc") != std::string::npos);
    CHECK(parsed->help_text.find("--list-models [search]") != std::string::npos);
    CHECK(parsed->help_text.find("--theme") != std::string::npos);
    CHECK(parsed->help_text.find("--no-themes") != std::string::npos);
    CHECK(parsed->help_text.find("--skill") != std::string::npos);
    CHECK(parsed->help_text.find("--system-prompt") != std::string::npos);
    CHECK(parsed->help_text.find("--append-system-prompt") != std::string::npos);
    CHECK(parsed->help_text.find("--thinking") != std::string::npos);
    CHECK(parsed->help_text.find("--version, -v") != std::string::npos);
}

TEST_CASE("parse_args help omits the deleted C++-only flags", "[cli][parse]") {
    std::vector<std::string> args{"cpp-harness", "--help"};
    auto argv = argv_from_strings(args);
    auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
    REQUIRE(parsed);
    REQUIRE(parsed->help);
    CHECK(parsed->help_text.find("--fake") == std::string::npos);
    CHECK(parsed->help_text.find("--enable-bash") == std::string::npos);
    CHECK(parsed->help_text.find("--max-turns") == std::string::npos);
    CHECK(parsed->help_text.find("--workspace") == std::string::npos);
    CHECK(parsed->help_text.find("--async") == std::string::npos);
    CHECK(parsed->help_text.find("--repl") == std::string::npos);
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

TEST_CASE("parse_args captures --session-dir as the automatic-directory override", "[cli][parse][session-dir]") {
    std::vector<std::string> args{"cpp-harness", "--session-dir", "/data/sessions", "hello"};
    auto argv = argv_from_strings(args);
    auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
    REQUIRE(parsed);
    REQUIRE(parsed->session_dir.has_value());
    CHECK(*parsed->session_dir == "/data/sessions");
    // The automatic-directory override rides alongside the raw session flags.
    CHECK_FALSE(parsed->session_value.has_value());
    CHECK_FALSE(parsed->resume);
    CHECK_FALSE(parsed->no_session_flag);
}

TEST_CASE("parse_args defaults --session-dir to absent", "[cli][parse][session-dir]") {
    std::vector<std::string> args{"cpp-harness", "hello"};
    auto argv = argv_from_strings(args);
    auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
    REQUIRE(parsed);
    CHECK_FALSE(parsed->session_dir.has_value());
}

TEST_CASE("parse_args accepts --session-dir alongside explicit and in-memory targets", "[cli][parse][session-dir]") {
    {
        std::vector<std::string> args{"cpp-harness", "--session-dir", "/data", "--session", "explicit.jsonl", "hello"};
        auto argv = argv_from_strings(args);
        auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
        REQUIRE(parsed);
        REQUIRE(parsed->session_dir.has_value());
        REQUIRE(parsed->session_value.has_value());
        CHECK(*parsed->session_value == "explicit.jsonl");
    }
    {
        std::vector<std::string> args{"cpp-harness", "--session-dir", "/data", "--resume", "explicit.jsonl", "hello"};
        auto argv = argv_from_strings(args);
        auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
        REQUIRE(parsed);
        REQUIRE(parsed->session_dir.has_value());
        CHECK(parsed->resume);
        REQUIRE(parsed->messages.size() == 2);
        CHECK(parsed->messages[0] == "explicit.jsonl");
        CHECK(parsed->messages[1] == "hello");
    }
    {
        std::vector<std::string> args{"cpp-harness", "--session-dir", "/data", "--no-session", "hello"};
        auto argv = argv_from_strings(args);
        auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
        REQUIRE(parsed);
        REQUIRE(parsed->session_dir.has_value());
        CHECK(parsed->no_session_flag);
    }
}

TEST_CASE("parse_args records the pi prompt/theme/skill flags", "[cli][parse]") {
    std::vector<std::string> args{
        "cpp-harness",
        "--system-prompt",
        "custom system prompt",
        "--append-system-prompt",
        "first append",
        "--append-system-prompt",
        "second append",
        "--thinking",
        "high",
        "--skill",
        "user-skill",
        "--skill",
        "project-skill",
        "--theme",
        "dark.json",
        "--theme",
        "custom-themes",
        "--no-context-files",
        "--no-themes",
        "--no-skills",
        "--no-prompt-templates",
        "hello",
    };
    auto argv = argv_from_strings(args);
    auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
    REQUIRE(parsed);
    REQUIRE(parsed->system_prompt.has_value());
    CHECK(*parsed->system_prompt == "custom system prompt");
    REQUIRE(parsed->append_system_prompt.size() == 2);
    CHECK(parsed->append_system_prompt[0] == "first append");
    CHECK(parsed->append_system_prompt[1] == "second append");
    REQUIRE(parsed->thinking.has_value());
    CHECK(*parsed->thinking == "high");
    REQUIRE(parsed->skills.size() == 2);
    CHECK(parsed->skills[0] == "user-skill");
    CHECK(parsed->skills[1] == "project-skill");
    REQUIRE(parsed->themes.size() == 2);
    CHECK(parsed->themes[0] == "dark.json");
    CHECK(parsed->themes[1] == "custom-themes");
    CHECK(parsed->no_context_files);
    CHECK(parsed->no_themes);
    CHECK(parsed->no_skills);
    CHECK(parsed->no_prompt_templates);
    REQUIRE(parsed->messages.size() == 1);
    CHECK(parsed->messages[0] == "hello");
}

TEST_CASE("parse_args accepts pi's multi-character shorts for the no-* flags", "[cli][parse]") {
    {
        std::vector<std::string> args{"cpp-harness", "-ns", "hello"};
        auto argv = argv_from_strings(args);
        auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
        REQUIRE(parsed);
        CHECK(parsed->no_skills);
        CHECK_FALSE(parsed->no_prompt_templates);
        CHECK_FALSE(parsed->no_context_files);
    }
    {
        std::vector<std::string> args{"cpp-harness", "-np", "hello"};
        auto argv = argv_from_strings(args);
        auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
        REQUIRE(parsed);
        CHECK(parsed->no_prompt_templates);
    }
    {
        std::vector<std::string> args{"cpp-harness", "-nc", "hello"};
        auto argv = argv_from_strings(args);
        auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
        REQUIRE(parsed);
        CHECK(parsed->no_context_files);
    }
}

TEST_CASE("parse_args accepts pi's -p and -v shorts", "[cli][parse]") {
    {
        std::vector<std::string> args{"cpp-harness", "-p", "hello"};
        auto argv = argv_from_strings(args);
        auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
        REQUIRE(parsed);
        CHECK(parsed->print);
        REQUIRE(parsed->messages.size() == 1);
        CHECK(parsed->messages[0] == "hello");
    }
    {
        std::vector<std::string> args{"cpp-harness", "-v"};
        auto argv = argv_from_strings(args);
        auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
        REQUIRE(parsed);
        CHECK(parsed->version);
    }
}

TEST_CASE("parse_args records --list-models with and without a search pattern", "[cli][parse]") {
    {
        std::vector<std::string> args{"cpp-harness", "--list-models"};
        auto argv = argv_from_strings(args);
        auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
        REQUIRE(parsed);
        REQUIRE(parsed->list_models.has_value());
        CHECK(parsed->list_models->empty());
    }
    {
        std::vector<std::string> args{"cpp-harness", "--list-models", "sonnet"};
        auto argv = argv_from_strings(args);
        auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
        REQUIRE(parsed);
        REQUIRE(parsed->list_models.has_value());
        CHECK(*parsed->list_models == "sonnet");
    }
}

TEST_CASE("parse_args does not treat an @file after --list-models as a search", "[cli][parse]") {
    std::vector<std::string> args{"cpp-harness", "--list-models", "@prompt.md"};
    auto argv = argv_from_strings(args);
    auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
    REQUIRE(parsed);
    REQUIRE(parsed->list_models.has_value());
    CHECK(parsed->list_models->empty());
    REQUIRE(parsed->file_arguments.size() == 1);
    CHECK(parsed->file_arguments[0] == "prompt.md");
}

TEST_CASE("parse_args keeps --list-models from swallowing later flags", "[cli][parse]") {
    std::vector<std::string> args{"cpp-harness", "--list-models", "@prompt.md", "--print"};
    auto argv = argv_from_strings(args);
    auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
    REQUIRE(parsed);
    REQUIRE(parsed->list_models.has_value());
    CHECK(parsed->list_models->empty());
    REQUIRE(parsed->file_arguments.size() == 1);
    CHECK(parsed->file_arguments[0] == "prompt.md");
    CHECK(parsed->print);
}

TEST_CASE("parse_args treats tokens after -- as positionals verbatim", "[cli][parse]") {
    std::vector<std::string> args{"cpp-harness", "--", "-p", "--print"};
    auto argv = argv_from_strings(args);
    auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
    REQUIRE(parsed);
    CHECK_FALSE(parsed->print);
    REQUIRE(parsed->messages.size() == 2);
    CHECK(parsed->messages[0] == "-p");
    CHECK(parsed->messages[1] == "--print");
}

TEST_CASE("parse_args exposes the CMake project version", "[cli][parse]") {
    CHECK_FALSE(cch::cli::project_version().empty());
}

TEST_CASE("parse_args reports a diagnostic when the working directory is unavailable", "[cli][parse][issue67]") {
    const cch::support::UniqueFd saved_cwd(::open(".", O_RDONLY | O_DIRECTORY));
    REQUIRE(saved_cwd);

    auto dir_template = (std::filesystem::temp_directory_path() / "cch-cli-deleted-cwd-XXXXXX").string();
    std::vector<char> dir_buffer(dir_template.begin(), dir_template.end());
    dir_buffer.push_back('\0');
    const char* deleted_dir = ::mkdtemp(dir_buffer.data());
    REQUIRE(deleted_dir != nullptr);
    REQUIRE(::chdir(deleted_dir) == 0);
    REQUIRE(::rmdir(deleted_dir) == 0);

    std::vector<std::string> args{"cpp-harness", "hello"};
    auto argv = argv_from_strings(args);
    auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());

    REQUIRE(::fchdir(saved_cwd.get()) == 0);

    REQUIRE_FALSE(parsed);
    CHECK(parsed.error().message.find("working directory") != std::string::npos);
}
