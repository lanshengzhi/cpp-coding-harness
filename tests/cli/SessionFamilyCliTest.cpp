#include <catch2/catch_test_macros.hpp>

#include "support/CliRunFixture.hpp"
#include "support/EnvVarGuard.hpp"
#include "support/TempWorkspace.hpp"
#include "coding_agent/SessionPathPolicy.hpp"
#include "util/Json.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using cch::tests::CliRunResult;
using cch::tests::CliRunOptions;
using cch::tests::EnvVarGuard;
using cch::tests::TempWorkspace;
using cch::tests::run_cli;

const std::string kSessionIdValidationMessage =
    "Error: Session id must be non-empty, contain only alphanumeric "
    "characters, '-', '_', and '.', and start and end with an alphanumeric "
    "character";

struct SessionFixture {
    TempWorkspace workspace;
    std::filesystem::path agent_dir;
    std::vector<std::pair<std::string, std::optional<std::string>>> env;

    explicit SessionFixture(std::string name = "agent") : agent_dir(workspace.path() / name) {
        env.emplace_back("PI_CODING_AGENT_DIR", agent_dir.string());
    }

    /// Run the CLI in the fixture workspace with the isolated agent dir.
    CliRunResult run(
        std::vector<std::string> args,
        std::string stdin_text = {},
        std::optional<std::filesystem::path> cwd = std::nullopt) const {
        return run_cli(CliRunOptions{
            .args = std::move(args),
            .cwd = cwd.value_or(workspace.path()),
            .env = env,
            .stdin_text = std::move(stdin_text),
        });
    }
};

/// Every `.jsonl` file under the fixture agent dir, sorted.
std::vector<std::filesystem::path> session_files(const std::filesystem::path& root) {
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    if (!std::filesystem::exists(root, ec)) {
        return files;
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root, ec)) {
        if (entry.is_regular_file(ec) && entry.path().extension() == ".jsonl") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

/// The session id from a session file's header line.
std::string header_session_id(const std::filesystem::path& path) {
    const auto content = read_file(path);
    const auto newline = content.find('\n');
    REQUIRE(newline != std::string::npos);
    auto parsed = cch::util::read_json(content.substr(0, newline));
    REQUIRE(parsed.has_value());
    const auto& object = parsed->get<cch::util::JsonValue::object_t>();
    return object.at("id").get<std::string>();
}

/// The `parentSession` field from a session file's header, when present.
std::optional<std::string> header_parent_session(const std::filesystem::path& path) {
    const auto content = read_file(path);
    const auto newline = content.find('\n');
    REQUIRE(newline != std::string::npos);
    auto parsed = cch::util::read_json(content.substr(0, newline));
    REQUIRE(parsed.has_value());
    const auto& object = parsed->get<cch::util::JsonValue::object_t>();
    const auto parent = object.find("parentSession");
    if (parent == object.end()) {
        return std::nullopt;
    }
    return parent->second.get<std::string>();
}

/// Count user messages in a session file.
std::size_t user_message_count(const std::filesystem::path& path) {
    std::size_t count = 0;
    std::ifstream input(path, std::ios::binary);
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty()) {
            continue;
        }
        auto parsed = cch::util::read_json(line);
        if (!parsed) {
            continue;
        }
        const auto* object = parsed->get_if<cch::util::JsonValue::object_t>();
        if (object == nullptr) {
            continue;
        }
        const auto type = object->find("type");
        if (type == object->end() || type->second.get<std::string>() != "message") {
            continue;
        }
        const auto& message = object->at("message").get<cch::util::JsonValue::object_t>();
        if (message.at("role").get<std::string>() == "user") {
            ++count;
        }
    }
    return count;
}

/// The session_info name entries in a session file.
std::vector<std::string> session_info_names(const std::filesystem::path& path) {
    std::vector<std::string> names;
    std::ifstream input(path, std::ios::binary);
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty()) {
            continue;
        }
        auto parsed = cch::util::read_json(line);
        if (!parsed) {
            continue;
        }
        const auto* object = parsed->get_if<cch::util::JsonValue::object_t>();
        if (object == nullptr) {
            continue;
        }
        const auto type = object->find("type");
        if (type == object->end() || type->second.get<std::string>() != "session_info") {
            continue;
        }
        const auto name = object->find("name");
        if (name != object->end()) {
            names.push_back(name->second.get<std::string>());
        }
    }
    return names;
}

} // namespace

TEST_CASE("session-family: --session with a path opens-or-creates at the target", "[cli][session-family]") {
    SessionFixture fixture;
    const auto session = fixture.workspace.path() / "target.jsonl";

    // Missing path: creates a new session at the exact path.
    auto created = fixture.run({"--session", session.string(), "first"});
    REQUIRE(created.exit_code == 0);
    CHECK(created.stdout_text == "fake: first\n");
    CHECK(std::filesystem::exists(session));

    // Existing path: opens (resumes) the session, appending the new message.
    auto resumed = fixture.run({"--session", session.string(), "second"});
    REQUIRE(resumed.exit_code == 0);
    CHECK(resumed.stdout_text == "fake: second\n");
    CHECK(user_message_count(session) == 2);
}

TEST_CASE("session-family: --session initializes an existing empty file as a new session", "[cli][session-family]") {
    SessionFixture fixture;
    const auto session = fixture.workspace.path() / "empty.jsonl";
    std::ofstream(session) << "";

    auto result = fixture.run({"--session", session.string(), "hello"});
    REQUIRE(result.exit_code == 0);
    CHECK(user_message_count(session) == 1);
    CHECK_FALSE(header_session_id(session).empty());
}

TEST_CASE("session-family: --session resolves a local session id exactly then by prefix", "[cli][session-family]") {
    SessionFixture fixture;

    auto first = fixture.run({"first-message"});
    REQUIRE(first.exit_code == 0);
    auto second = fixture.run({"second-message"});
    REQUIRE(second.exit_code == 0);

    const auto files = session_files(fixture.agent_dir / "sessions");
    REQUIRE(files.size() == 2);
    const auto first_id = header_session_id(files[0]);
    const auto second_id = header_session_id(files[1]);

    // Exact id resumes the owning session.
    auto exact = fixture.run({"--session", first_id, "again"});
    REQUIRE(exact.exit_code == 0);
    CHECK(user_message_count(files[0]) == 2);
    CHECK(user_message_count(files[1]) == 1);

    // A unique prefix resumes the owning session too (pi: startsWith).
    auto prefix = fixture.run({"--session", second_id.substr(0, 8), "again"});
    REQUIRE(prefix.exit_code == 0);
    CHECK(user_message_count(files[1]) == 2);
}

TEST_CASE("session-family: --session with an unknown id errors like pi", "[cli][session-family]") {
    SessionFixture fixture;
    auto result = fixture.run({"--session", "no-such-session", "hello"});

    REQUIRE(result.exit_code == 1);
    CHECK(result.stderr_text.find("No session found matching 'no-such-session'") != std::string::npos);
    CHECK(result.stdout_text.empty());
}

TEST_CASE("session-family: --session on an existing file resumes with the header cwd", "[cli][session-family]") {
    SessionFixture fixture;
    TempWorkspace original;
    TempWorkspace other;
    std::filesystem::create_directory(other.path());
    original.write("note.txt", "from-session-workspace");
    const auto session = original.path() / "cross-cwd.jsonl";

    auto created = run_cli(CliRunOptions{
        .args = {"--session", session.string(), "first"},
        .cwd = original.path(),
        .env = fixture.env,
    });
    REQUIRE(created.exit_code == 0);

    // --session opens the existing file from another launch directory and
    // the runtime cwd follows the session header cwd (pi SessionManager.open).
    auto resumed = fixture.run({"--session", session.string(), "read note.txt"}, "", other.path());
    REQUIRE(resumed.exit_code == 0);
    CHECK(resumed.stdout_text.find("from-session-workspace") != std::string::npos);
    CHECK(user_message_count(session) == 2);
}

TEST_CASE("session-family: --session with a global id prompts to fork and confirms", "[cli][session-family]") {
    SessionFixture fixture;
    TempWorkspace other;
    std::filesystem::create_directory(other.path());

    // A session in another project (automatic storage under the sessions root).
    auto other_run = run_cli(CliRunOptions{
        .args = {"only-message"},
        .cwd = other.path(),
        .env = fixture.env,
    });
    REQUIRE(other_run.exit_code == 0);
    const auto other_files = session_files(fixture.agent_dir / "sessions");
    REQUIRE(other_files.size() == 1);
    const auto other_id = header_session_id(other_files.front());

    // From the fixture workspace the id is global: confirm the fork prompt.
    // The notice and prompt land on stderr in print mode (pi's stdout
    // takeover), keeping stdout clean for the final text.
    auto forked = fixture.run({"--session", other_id, "forked"}, "y\n");
    REQUIRE(forked.exit_code == 0);
    CHECK(forked.stdout_text == "fake: forked\n");
    CHECK(forked.stderr_text.find("Session found in different project: ") != std::string::npos);
    CHECK(forked.stderr_text.find("Fork this session into current directory? [y/N] ") != std::string::npos);

    // The fork created a new session in the current project with the source
    // path recorded as parentSession and the copied history plus the fork
    // run's message.
    const auto files = session_files(fixture.agent_dir / "sessions");
    REQUIRE(files.size() == 2);
    const auto forked_file = files[0].parent_path() == other_files.front().parent_path()
        ? files[1] : files[0];
    CHECK(header_parent_session(forked_file) == other_files.front().string());
    CHECK(user_message_count(forked_file) == 2);
}

TEST_CASE("session-family: declining the cross-project fork prints Aborted and exits 0", "[cli][session-family]") {
    SessionFixture fixture;
    TempWorkspace other;
    std::filesystem::create_directory(other.path());

    auto other_run = run_cli(CliRunOptions{
        .args = {"only-message"},
        .cwd = other.path(),
        .env = fixture.env,
    });
    REQUIRE(other_run.exit_code == 0);
    const auto other_files = session_files(fixture.agent_dir / "sessions");
    REQUIRE(other_files.size() == 1);
    const auto other_id = header_session_id(other_files.front());

    auto declined = fixture.run({"--session", other_id, "forked"}, "n\n");
    REQUIRE(declined.exit_code == 0);
    // The notice, prompt, and Aborted line land on stderr in print mode.
    CHECK(declined.stderr_text.find("Session found in different project: ") != std::string::npos);
    CHECK(declined.stderr_text.find("Fork this session into current directory? [y/N] ") != std::string::npos);
    CHECK(declined.stderr_text.find("Aborted.") != std::string::npos);
    CHECK(declined.stdout_text.empty());

    // No new session appeared in the current project.
    const auto files = session_files(fixture.agent_dir / "sessions");
    CHECK(files.size() == 1);
}

TEST_CASE("session-family: --continue resumes the most recent session", "[cli][session-family]") {
    SessionFixture fixture;

    auto first = fixture.run({"first-message"});
    REQUIRE(first.exit_code == 0);
    auto second = fixture.run({"second-message"});
    REQUIRE(second.exit_code == 0);

    const auto files = session_files(fixture.agent_dir / "sessions");
    REQUIRE(files.size() == 2);

    // Force deterministic mtimes: files[1] is the most recent.
    const auto now = std::filesystem::file_time_type::clock::now();
    std::filesystem::last_write_time(files[1], now);
    std::filesystem::last_write_time(files[0], now - std::chrono::hours(1));

    auto continued = fixture.run({"--continue", "third-message"});
    REQUIRE(continued.exit_code == 0);
    CHECK(continued.stdout_text == "fake: third-message\n");
    CHECK(user_message_count(files[1]) == 2);
    CHECK(user_message_count(files[0]) == 1);
}

TEST_CASE("session-family: --continue creates a new session when none exists", "[cli][session-family]") {
    SessionFixture fixture;
    auto result = fixture.run({"--continue", "only-message"});
    REQUIRE(result.exit_code == 0);
    CHECK(result.stdout_text == "fake: only-message\n");
    const auto files = session_files(fixture.agent_dir / "sessions");
    REQUIRE(files.size() == 1);
    CHECK(user_message_count(files.front()) == 1);
}

TEST_CASE("session-family: --fork forks from a path with the full history", "[cli][session-family]") {
    SessionFixture fixture;
    const auto source = fixture.workspace.path() / "source.jsonl";
    auto created = fixture.run({"--session", source.string(), "first"});
    REQUIRE(created.exit_code == 0);

    auto forked = fixture.run({"--fork", source.string(), "forked"});
    REQUIRE(forked.exit_code == 0);
    CHECK(forked.stdout_text == "fake: forked\n");

    const auto files = session_files(fixture.agent_dir / "sessions");
    REQUIRE(files.size() == 1);
    CHECK(header_parent_session(files.front()) == std::filesystem::absolute(source).string());
    // The copied history keeps the source message plus the fork run's message.
    CHECK(user_message_count(files.front()) == 2);
}

TEST_CASE("session-family: --fork resolves a local id and never prompts cross-project", "[cli][session-family]") {
    SessionFixture fixture;
    TempWorkspace other;
    std::filesystem::create_directory(other.path());

    // A session in another project.
    auto other_run = run_cli(CliRunOptions{
        .args = {"only-message"},
        .cwd = other.path(),
        .env = fixture.env,
    });
    REQUIRE(other_run.exit_code == 0);
    const auto other_files = session_files(fixture.agent_dir / "sessions");
    REQUIRE(other_files.size() == 1);
    const auto other_id = header_session_id(other_files.front());

    // Forking a global id proceeds without any prompt (no stdin needed).
    auto forked = fixture.run({"--fork", other_id, "forked"}, "");
    REQUIRE(forked.exit_code == 0);
    CHECK(forked.stdout_text == "fake: forked\n");
    CHECK(forked.stdout_text.find("Fork this session") == std::string::npos);

    const auto files = session_files(fixture.agent_dir / "sessions");
    REQUIRE(files.size() == 2);
    const auto forked_file = files[0].parent_path() == other_files.front().parent_path()
        ? files[1] : files[0];
    CHECK(header_parent_session(forked_file) == other_files.front().string());
}

TEST_CASE("session-family: --fork rejects unknown targets", "[cli][session-family]") {
    SessionFixture fixture;
    auto result = fixture.run({"--fork", "no-such-session", "hello"});
    REQUIRE(result.exit_code == 1);
    CHECK(result.stderr_text.find("No session found matching 'no-such-session'") != std::string::npos);
}

TEST_CASE("session-family: --fork conflict checks match pi", "[cli][session-family]") {
    SessionFixture fixture;
    const auto session = fixture.workspace.path() / "source.jsonl";

    {
        auto result = fixture.run({"--fork", session.string(), "--session", session.string(), "x"});
        REQUIRE(result.exit_code == 1);
        CHECK(result.stderr_text.find("Error: --fork cannot be combined with --session") != std::string::npos);
    }
    {
        auto result = fixture.run({"--fork", session.string(), "--continue", "x"});
        REQUIRE(result.exit_code == 1);
        CHECK(result.stderr_text.find("Error: --fork cannot be combined with --continue") != std::string::npos);
    }
    {
        auto result = fixture.run({"--fork", session.string(), "--resume", session.string(), "x"});
        REQUIRE(result.exit_code == 1);
        CHECK(result.stderr_text.find("Error: --fork cannot be combined with --resume") != std::string::npos);
    }
    {
        auto result = fixture.run({"--fork", session.string(), "--no-session", "x"});
        REQUIRE(result.exit_code == 1);
        CHECK(result.stderr_text.find("Error: --fork cannot be combined with --no-session") != std::string::npos);
    }
}

TEST_CASE("session-family: --fork with an occupied --session-id target errors locally", "[cli][session-family]") {
    SessionFixture fixture;
    const auto source = fixture.workspace.path() / "source.jsonl";
    auto created = fixture.run({"--session", source.string(), "first"});
    REQUIRE(created.exit_code == 0);

    // Occupy the target id with a warn-create session.
    auto occupied = fixture.run({"--session-id", "target-id", "hello"});
    REQUIRE(occupied.exit_code == 0);

    auto result = fixture.run({"--fork", source.string(), "--session-id", "target-id", "x"});
    REQUIRE(result.exit_code == 1);
    CHECK(result.stderr_text.find("Session already exists with id 'target-id'") != std::string::npos);
}

TEST_CASE("session-family: --fork --session-id creates the fork with that id", "[cli][session-family]") {
    SessionFixture fixture;
    const auto source = fixture.workspace.path() / "source.jsonl";
    auto created = fixture.run({"--session", source.string(), "first"});
    REQUIRE(created.exit_code == 0);

    auto forked = fixture.run({"--fork", source.string(), "--session-id", "fork-id", "forked"});
    REQUIRE(forked.exit_code == 0);
    const auto files = session_files(fixture.agent_dir / "sessions");
    REQUIRE(files.size() == 1);
    CHECK(header_session_id(files.front()) == "fork-id");
}

TEST_CASE("session-family: --session-id conflict checks match pi", "[cli][session-family]") {
    SessionFixture fixture;
    const auto session = fixture.workspace.path() / "x.jsonl";

    {
        auto result = fixture.run({"--session-id", "sid", "--session", session.string(), "x"});
        REQUIRE(result.exit_code == 1);
        CHECK(result.stderr_text.find("Error: --session-id cannot be combined with --session") != std::string::npos);
    }
    {
        auto result = fixture.run({"--session-id", "sid", "--continue", "x"});
        REQUIRE(result.exit_code == 1);
        CHECK(result.stderr_text.find("Error: --session-id cannot be combined with --continue") != std::string::npos);
    }
    {
        auto result = fixture.run({"--session-id", "sid", "--resume", session.string(), "x"});
        REQUIRE(result.exit_code == 1);
        CHECK(result.stderr_text.find("Error: --session-id cannot be combined with --resume") != std::string::npos);
    }
}

TEST_CASE("session-family: --session-id validates the id format like pi", "[cli][session-family]") {
    SessionFixture fixture;
    for (const auto& invalid : {"", "bad id", "-leading", "trailing-", "a/b"}) {
        auto result = fixture.run({"--session-id", invalid, "hello"});
        REQUIRE(result.exit_code == 1);
        CHECK(result.stderr_text.find(kSessionIdValidationMessage) != std::string::npos);
    }
}

TEST_CASE("session-family: --session-id warns and creates when missing, resumes when present", "[cli][session-family]") {
    SessionFixture fixture;

    // Missing: warning + a new session carrying the id.
    auto created = fixture.run({"--session-id", "project-session", "first"});
    REQUIRE(created.exit_code == 0);
    CHECK(created.stdout_text == "fake: first\n");
    CHECK(created.stderr_text.find(
              "Warning: No project session found with id 'project-session'; "
              "creating a new session with that id.") != std::string::npos);
    const auto files = session_files(fixture.agent_dir / "sessions");
    REQUIRE(files.size() == 1);
    CHECK(header_session_id(files.front()) == "project-session");

    // Present: resumes without a warning.
    auto resumed = fixture.run({"--session-id", "project-session", "second"});
    REQUIRE(resumed.exit_code == 0);
    CHECK(resumed.stdout_text == "fake: second\n");
    CHECK(resumed.stderr_text.find("Warning:") == std::string::npos);
    CHECK(user_message_count(files.front()) == 2);
}

TEST_CASE("session-family: --name guard rejects empty and whitespace values", "[cli][session-family]") {
    SessionFixture fixture;
    for (const auto& empty : {"", "   ", "\t"}) {
        auto result = fixture.run({"--name", empty, "hello"});
        REQUIRE(result.exit_code == 1);
        CHECK(result.stderr_text.find("Error: --name requires a non-empty value") != std::string::npos);
        CHECK(session_files(fixture.agent_dir / "sessions").empty());
    }
}

TEST_CASE("session-family: --name appends a sanitized session_info entry", "[cli][session-family]") {
    SessionFixture fixture;
    auto result = fixture.run({"--name", "  My Session  ", "hello"});
    REQUIRE(result.exit_code == 0);
    const auto files = session_files(fixture.agent_dir / "sessions");
    REQUIRE(files.size() == 1);
    const auto names = session_info_names(files.front());
    REQUIRE(names.size() == 1);
    CHECK(names.front() == "My Session");
}

TEST_CASE("session-family: --name applies to resumed and forked sessions too", "[cli][session-family]") {
    SessionFixture fixture;
    const auto source = fixture.workspace.path() / "source.jsonl";
    auto created = fixture.run({"--session", source.string(), "first"});
    REQUIRE(created.exit_code == 0);

    auto resumed = fixture.run({"--session", source.string(), "--name", "Resumed Name", "second"});
    REQUIRE(resumed.exit_code == 0);
    CHECK(session_info_names(source) == std::vector<std::string>{"Resumed Name"});

    auto forked = fixture.run({"--fork", source.string(), "--name", "Fork Name", "forked"});
    REQUIRE(forked.exit_code == 0);
    const auto files = session_files(fixture.agent_dir / "sessions");
    REQUIRE(files.size() == 1);
    // The fork inherits the source's entries (including its session_info);
    // the new name lands last and wins getSessionName (pi forkFrom copies
    // every non-header entry, then appendSessionInfo appends).
    const auto names = session_info_names(files.front());
    REQUIRE(names.size() == 2);
    CHECK(names[0] == "Resumed Name");
    CHECK(names[1] == "Fork Name");
}

TEST_CASE("session-family: --no-session short-circuits silently over session flags", "[cli][session-family]") {
    SessionFixture fixture;
    const auto session = fixture.workspace.path() / "explicit.jsonl";

    // --no-session wins over --session/--resume/--continue with no conflict
    // error (pi precedence; the C++-today conflict errors are deleted).
    {
        auto result = fixture.run({"--no-session", "--session", session.string(), "hello"});
        REQUIRE(result.exit_code == 0);
        CHECK(result.stdout_text == "fake: hello\n");
        CHECK(result.stderr_text.find("cannot be combined") == std::string::npos);
        CHECK_FALSE(std::filesystem::exists(session));
        CHECK(session_files(fixture.agent_dir / "sessions").empty());
    }
    {
        // pi: --resume is the picker flag; --no-session wins silently before
        // the picker ever opens.
        auto result = fixture.run({"--no-session", "--resume", "hello"});
        REQUIRE(result.exit_code == 0);
        CHECK(result.stdout_text == "fake: hello\n");
        CHECK(result.stderr_text.find("cannot be combined") == std::string::npos);
        CHECK_FALSE(std::filesystem::exists(session));
        CHECK(session_files(fixture.agent_dir / "sessions").empty());
    }
    {
        auto result = fixture.run({"--no-session", "--continue", "hello"});
        REQUIRE(result.exit_code == 0);
        CHECK(result.stdout_text == "fake: hello\n");
        CHECK(result.stderr_text.find("cannot be combined") == std::string::npos);
        CHECK(session_files(fixture.agent_dir / "sessions").empty());
    }
}

TEST_CASE("session-family: --no-session --session-id keeps the id in memory", "[cli][session-family]") {
    SessionFixture fixture;
    auto result = fixture.run({"--no-session", "--session-id", "memory-id", "hello"});
    REQUIRE(result.exit_code == 0);
    CHECK(session_files(fixture.agent_dir / "sessions").empty());
}

TEST_CASE("session-family: --session wins over --resume without a conflict error", "[cli][session-family]") {
    SessionFixture fixture;
    const auto session = fixture.workspace.path() / "wins.jsonl";

    auto result = fixture.run({"--session", session.string(), "--resume", "hello"});
    REQUIRE(result.exit_code == 0);
    CHECK(result.stderr_text.find("use either --session or --resume") == std::string::npos);
    CHECK(std::filesystem::exists(session));
}

TEST_CASE("session-family: custom --session-dir filters local id search by cwd", "[cli][session-family]") {
    SessionFixture fixture;
    TempWorkspace other;
    std::filesystem::create_directory(other.path());
    const auto shared = fixture.workspace.path() / "shared-sessions";

    // A session created from the other workspace lives in the shared dir.
    auto other_run = run_cli(CliRunOptions{
        .args = {"--session-dir", shared.string(), "other-message"},
        .cwd = other.path(),
        .env = fixture.env,
    });
    REQUIRE(other_run.exit_code == 0);
    const auto other_files = session_files(shared);
    REQUIRE(other_files.size() == 1);
    const auto other_id = header_session_id(other_files.front());

    // From the fixture workspace the local search is cwd-filtered, so the
    // other workspace's session only matches globally: pi then prompts to
    // fork (declining here aborts). A cwd-unfiltered local search would have
    // opened it without any prompt.
    auto prompted = fixture.run(
        {"--session-dir", shared.string(), "--session", other_id, "x"},
        "n\n");
    REQUIRE(prompted.exit_code == 0);
    CHECK(prompted.stderr_text.find("Session found in different project: ") != std::string::npos);
    CHECK(prompted.stderr_text.find("Aborted.") != std::string::npos);

    // --session-id warn-create honors the same local cwd filter.
    auto created = fixture.run({"--session-dir", shared.string(), "--session-id", other_id, "hello"});
    REQUIRE(created.exit_code == 0);
    CHECK(created.stderr_text.find("creating a new session with that id") != std::string::npos);
    CHECK(session_files(shared).size() == 2);
}

TEST_CASE("session-family: custom --session-dir filters --continue by cwd", "[cli][session-family]") {
    SessionFixture fixture;
    TempWorkspace other;
    std::filesystem::create_directory(other.path());
    const auto shared = fixture.workspace.path() / "shared-sessions";

    auto other_run = run_cli(CliRunOptions{
        .args = {"--session-dir", shared.string(), "other-message"},
        .cwd = other.path(),
        .env = fixture.env,
    });
    REQUIRE(other_run.exit_code == 0);

    // The fixture workspace's --continue does not see the other cwd's
    // session; it creates a new one in the shared directory.
    auto continued = fixture.run({"--session-dir", shared.string(), "--continue", "mine"});
    REQUIRE(continued.exit_code == 0);
    CHECK(continued.stdout_text == "fake: mine\n");
    CHECK(session_files(shared).size() == 2);
}

TEST_CASE("session-family: --session-dir equal to the default directory skips the cwd filter", "[cli][session-family]") {
    SessionFixture fixture;

    auto first = fixture.run({"first-message"});
    REQUIRE(first.exit_code == 0);
    const auto default_dir = fixture.agent_dir / "sessions" /
        cch::coding_agent::session_paths::encode_workspace_key(
            std::filesystem::weakly_canonical(fixture.workspace.path()));

    // Passing the default directory explicitly behaves like no override:
    // --session by id still matches locally.
    const auto files = session_files(default_dir);
    REQUIRE(files.size() == 1);
    const auto id = header_session_id(files.front());
    auto result = fixture.run({"--session-dir", default_dir.string(), "--session", id, "again"});
    REQUIRE(result.exit_code == 0);
    CHECK(user_message_count(files.front()) == 2);
}

/// A scripted startup-TUI picker (pi `selectSession` host): records that it
/// ran and returns the fixed path (or nullopt for a cancelled picker).
[[nodiscard]] cch::cli::ResumePickerSink scripted_picker(
    std::optional<std::filesystem::path> picked,
    bool* called = nullptr) {
    return [picked, called](
               cch::coding_agent::tui::SessionListLoader current_loader,
               cch::coding_agent::tui::SessionListLoader all_loader)
        -> cch::util::Expected<std::optional<std::filesystem::path>> {
        if (called != nullptr) *called = true;
        // The picker host receives the effective session space loaders (pi
        // `SessionManager.list`/`listAll`); a selected run must see the
        // sessions the picker can offer.
        if (picked) {
            const auto current = current_loader();
            const auto all = all_loader();
            REQUIRE(!current.empty());
            REQUIRE(!all.empty());
        }
        return picked;
    };
}

TEST_CASE("session-family: --resume opens the picker and resumes the picked session", "[cli][session-family][issue417]") {
    SessionFixture fixture;
    auto created = fixture.run({"first"});
    REQUIRE(created.exit_code == 0);
    const auto files = session_files(fixture.agent_dir / "sessions");
    REQUIRE(files.size() == 1);

    bool called = false;
    auto resumed = run_cli(CliRunOptions{
        .args = {"--resume", "second"},
        .cwd = fixture.workspace.path(),
        .env = fixture.env,
        .resume_picker = scripted_picker(files.front(), &called),
    });

    REQUIRE(resumed.exit_code == 0);
    CHECK(called);
    // The picked session resumes (pi selectSession → SessionManager.open).
    CHECK(resumed.stdout_text == "fake: second\n");
    CHECK(user_message_count(files.front()) == 2);
}

TEST_CASE("session-family: --resume cancel prints pi's No session selected and exits 0", "[cli][session-family][issue417]") {
    SessionFixture fixture;
    auto created = fixture.run({"first"});
    REQUIRE(created.exit_code == 0);

    bool called = false;
    // Print frontend: the line lands on stderr (pi output-guard takeover).
    auto cancelled = run_cli(CliRunOptions{
        .args = {"--resume"},
        .cwd = fixture.workspace.path(),
        .env = fixture.env,
        .resume_picker = scripted_picker(std::nullopt, &called),
    });

    REQUIRE(cancelled.exit_code == 0);
    CHECK(called);
    CHECK(cancelled.stdout_text.empty());
    CHECK(cancelled.stderr_text.find("No session selected") != std::string::npos);
}

TEST_CASE("session-family: --no-session wins before the --resume picker opens", "[cli][session-family][issue417]") {
    SessionFixture fixture;
    bool called = false;
    auto result = run_cli(CliRunOptions{
        .args = {"--no-session", "--resume", "hello"},
        .cwd = fixture.workspace.path(),
        .env = fixture.env,
        .resume_picker = scripted_picker(std::nullopt, &called),
    });

    // pi createSessionManager: the in-memory short-circuit returns before
    // selectSession; the picker never runs.
    REQUIRE(result.exit_code == 0);
    CHECK_FALSE(called);
    CHECK(result.stdout_text == "fake: hello\n");
}

/// pi main.ts `getMissingSessionCwdIssue` over the assembled target: the
/// assembly runs chdir'd to the launch workspace (the session space is
/// workspace-keyed).
TEST_CASE("session-family: the boot missing-cwd issue resolves per target", "[cli][session-family][issue417]") {
    SessionFixture fixture;
    TempWorkspace storage;
    TempWorkspace continue_agent;
    TempWorkspace vanished_launch;
    // The direct assembly calls below resolve the sessions root from the
    // process environment; point it at the continue-session agent dir.
    EnvVarGuard agent_guard{"PI_CODING_AGENT_DIR"};
    agent_guard.set(continue_agent.path().string());

    // A resume-shaped session whose header cwd (the fixture workspace) is
    // removed while the file survives.
    const auto session = storage.path() / "vanished.jsonl";
    auto created = fixture.run({"--session", session.string(), "first"});
    REQUIRE(created.exit_code == 0);
    std::error_code ec;
    REQUIRE(std::filesystem::remove_all(fixture.workspace.path(), ec) > 0);
    REQUIRE_FALSE(ec);

    // An automatic-space session inside the storage workspace-keyed
    // directory whose header cwd (`vanished_launch`) is removed:
    // --continue resolves it as most recent without a cwd filter (pi
    // continueRecent: the filter engages only for a custom override).
    const auto storage_key =
        cch::coding_agent::session_paths::encode_workspace_key(
            std::filesystem::weakly_canonical(storage.path()));
    const auto storage_default_dir =
        continue_agent.path() / "sessions" / storage_key;
    std::filesystem::create_directories(storage_default_dir, ec);
    REQUIRE_FALSE(ec);
    const auto continue_session =
        storage_default_dir / "continue-session.jsonl";
    auto launched = run_cli(CliRunOptions{
        .args = {"--session", continue_session.string(), "from-elsewhere"},
        .cwd = vanished_launch.path(),
        .env = {{"PI_CODING_AGENT_DIR", continue_agent.path().string()}},
    });
    REQUIRE(launched.exit_code == 0);
    REQUIRE(std::filesystem::exists(continue_session));
    REQUIRE(std::filesystem::remove_all(vanished_launch.path(), ec) > 0);
    REQUIRE_FALSE(ec);

    // The assembly helpers resolve the workspace-keyed session space from
    // the process cwd (the shared CLI fixture guard restores it after).
    cch::tests::detail::CliRunCwdGuard cwd_guard{
        std::optional<std::filesystem::path>{storage.path()}};
    const auto assemble_with = [&](std::vector<std::string> args,
                                   cch::cli::ResumePickerSink picker = {}) {
        args.insert(args.begin(), "cpp-harness");
        std::vector<char*> argv;
        argv.reserve(args.size());
        for (auto& argument : args) {
            argv.push_back(argument.data());
        }
        auto config = cch::cli::parse_args(
            static_cast<int>(argv.size()), argv.data());
        REQUIRE(config);
        std::istringstream input;
        std::ostringstream output;
        std::ostringstream error;
        return cch::cli::assemble_session_target(
            *config, std::nullopt, input, output, error,
            std::move(picker));
    };

    // The resume-shaped targets report the stored header cwd (pi
    // `getMissingSessionCwdIssue`): the picked resume target...
    {
        // The picked path comes from the picker directly; the effective
        // space here is `storage` (empty of automatic sessions).
        auto assembly = assemble_with(
            {"--resume"},
            [session](cch::coding_agent::tui::SessionListLoader,
                      cch::coding_agent::tui::SessionListLoader)
                -> cch::util::Expected<
                    std::optional<std::filesystem::path>> {
                return session;
            });
        REQUIRE(assembly);
        auto issue = cch::cli::missing_session_cwd_issue(*assembly, storage.path());
        REQUIRE(issue);
        CHECK(issue->session_file == session);
        CHECK(issue->session_cwd == fixture.workspace.path());
        CHECK(issue->fallback_cwd == storage.path());
    }
    // ...the open-or-create target on the same file...
    {
        auto assembly = assemble_with({"--session", session.string()});
        REQUIRE(assembly);
        auto issue = cch::cli::missing_session_cwd_issue(*assembly, storage.path());
        REQUIRE(issue);
        CHECK(issue->session_cwd == fixture.workspace.path());
    }
    // ...and --continue's most recent session.
    {
        auto assembly = assemble_with({"--continue"});
        REQUIRE(assembly);
        auto issue = cch::cli::missing_session_cwd_issue(*assembly, storage.path());
        REQUIRE(issue);
        CHECK(issue->session_file == continue_session);
        CHECK(issue->session_cwd == vanished_launch.path());
    }
    // A fresh create target never has an issue (an empty header keeps the
    // launch cwd; pi's create writes the launch cwd).
    {
        auto assembly = assemble_with({"--session", "fresh.jsonl"});
        REQUIRE(assembly);
        CHECK_FALSE(cch::cli::missing_session_cwd_issue(*assembly, storage.path()));
    }
    // Forks always target the launch cwd and never have an issue.
    {
        auto assembly = assemble_with({"--fork", session.string()});
        REQUIRE(assembly);
        CHECK_FALSE(cch::cli::missing_session_cwd_issue(*assembly, storage.path()));
    }
    // An in-memory run never has an issue.
    {
        auto assembly = assemble_with({"--no-session"});
        REQUIRE(assembly);
        CHECK_FALSE(cch::cli::missing_session_cwd_issue(*assembly, storage.path()));
    }
}
