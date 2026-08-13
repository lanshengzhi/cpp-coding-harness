#include "coding_agent/SessionDiscovery.hpp"

#include "support/TempWorkspace.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

using namespace cch;
namespace discovery = cch::coding_agent::session_discovery;

namespace {

void write_session_header(
    const std::filesystem::path& path,
    const std::string& id,
    const std::string& cwd) {
    std::ofstream output(path, std::ios::binary);
    output << "{\"type\":\"session\",\"version\":3,\"id\":\"" << id
           << "\",\"timestamp\":\"2026-08-08T00:00:00.000Z\",\"cwd\":\""
           << cwd << "\"}\n";
}

} // namespace

TEST_CASE("session id validation matches pi assertValidSessionId", "[coding_agent][session-discovery]") {
    CHECK_FALSE(discovery::invalid_session_id_reason("abc").has_value());
    CHECK_FALSE(discovery::invalid_session_id_reason("a1-B_c.d").has_value());
    CHECK_FALSE(discovery::invalid_session_id_reason("a").has_value());

    const auto expected =
        "Session id must be non-empty, contain only alphanumeric characters, "
        "'-', '_', and '.', and start and end with an alphanumeric character";
    CHECK(discovery::invalid_session_id_reason("") == expected);
    CHECK(discovery::invalid_session_id_reason(" abc") == expected);
    CHECK(discovery::invalid_session_id_reason("-abc") == expected);
    CHECK(discovery::invalid_session_id_reason("abc-") == expected);
    CHECK(discovery::invalid_session_id_reason("ab/c") == expected);
    CHECK(discovery::invalid_session_id_reason("ab c") == expected);
    CHECK(discovery::invalid_session_id_reason(".abc") == expected);
}

TEST_CASE("session directory listing reads headers and skips non-sessions", "[coding_agent][session-discovery]") {
    tests::TempWorkspace temp;
    const auto directory = temp.path() / "sessions";
    std::filesystem::create_directory(directory);

    write_session_header(directory / "one.jsonl", "id-one", temp.path().string());
    write_session_header(directory / "two.jsonl", "id-two", temp.path().string());
    {
        // Not a session header: skipped.
        std::ofstream not_session(directory / "other.jsonl");
        not_session << "{\"type\":\"header\",\"id\":\"x\"}\n";
    }
    {
        // Malformed JSON: skipped.
        std::ofstream malformed(directory / "broken.jsonl");
        malformed << "not json\n";
    }
    {
        // Not a .jsonl file: skipped.
        std::ofstream text(directory / "notes.txt");
        text << "hello";
    }

    const auto sessions = discovery::list_sessions_in_directory(directory, std::nullopt);
    REQUIRE(sessions.size() == 2);
    const auto ids = [&] {
        std::vector<std::string> result;
        for (const auto& session : sessions) {
            result.push_back(session.id);
        }
        return result;
    }();
    CHECK(std::find(ids.begin(), ids.end(), "id-one") != ids.end());
    CHECK(std::find(ids.begin(), ids.end(), "id-two") != ids.end());
    for (const auto& session : sessions) {
        CHECK(session.path.parent_path() == directory);
        CHECK(session.cwd == temp.path().string());
    }
}

TEST_CASE("session directory listing sorts newest first and cwd-filters", "[coding_agent][session-discovery]") {
    tests::TempWorkspace temp;
    const auto directory = temp.path() / "sessions";
    const auto other_cwd = temp.path() / "other-project";
    std::filesystem::create_directory(directory);

    const auto newest = directory / "newest.jsonl";
    const auto older = directory / "older.jsonl";
    const auto foreign = directory / "foreign.jsonl";
    write_session_header(newest, "id-new", temp.path().string());
    write_session_header(older, "id-old", temp.path().string());
    write_session_header(foreign, "id-foreign", other_cwd.string());

    const auto newer_time = std::filesystem::file_time_type::clock::now();
    const auto older_time = newer_time - std::chrono::hours(1);
    std::filesystem::last_write_time(newest, newer_time);
    std::filesystem::last_write_time(older, older_time);
    std::filesystem::last_write_time(foreign, older_time);

    const auto all = discovery::list_sessions_in_directory(directory, std::nullopt);
    REQUIRE(all.size() == 3);
    CHECK(all.front().id == "id-new");

    const auto filtered = discovery::list_sessions_in_directory(directory, temp.path());
    REQUIRE(filtered.size() == 2);
    CHECK(filtered.front().id == "id-new");
    CHECK(std::none_of(
        filtered.begin(), filtered.end(),
        [](const discovery::SessionInfoLite& session) {
            return session.id == "id-foreign";
        }));
}

TEST_CASE("missing and unreadable session directories list empty", "[coding_agent][session-discovery]") {
    tests::TempWorkspace temp;
    CHECK(discovery::list_sessions_in_directory(temp.path() / "missing", std::nullopt).empty());
    CHECK_FALSE(discovery::find_most_recent_session(temp.path() / "missing", std::nullopt).has_value());
}

TEST_CASE("find most recent session returns the newest cwd-matched file", "[coding_agent][session-discovery]") {
    tests::TempWorkspace temp;
    const auto directory = temp.path() / "sessions";
    const auto other_cwd = temp.path() / "other-project";
    std::filesystem::create_directory(directory);

    const auto candidate = directory / "candidate.jsonl";
    write_session_header(candidate, "id-candidate", temp.path().string());
    std::filesystem::last_write_time(
        candidate, std::filesystem::file_time_type::clock::now());

    auto most_recent = discovery::find_most_recent_session(directory, temp.path());
    REQUIRE(most_recent.has_value());
    CHECK(most_recent->id == "id-candidate");
    CHECK(most_recent->path == candidate);

    // An unmatched cwd yields nothing.
    CHECK_FALSE(discovery::find_most_recent_session(directory, other_cwd).has_value());
}

TEST_CASE("list all sessions scans per-project directories under the sessions root", "[coding_agent][session-discovery]") {
    tests::TempWorkspace temp;
    const auto sessions_root = temp.path() / "sessions";
    const auto project_a = sessions_root / "--project-a--";
    const auto project_b = sessions_root / "--project-b--";
    std::filesystem::create_directories(project_a);
    std::filesystem::create_directories(project_b);

    write_session_header(project_a / "a.jsonl", "id-a", temp.path().string());
    write_session_header(project_b / "b.jsonl", "id-b", temp.path().string());

    const auto all = discovery::list_all_sessions(sessions_root, std::nullopt);
    REQUIRE(all.size() == 2);
    const auto ids = [&] {
        std::vector<std::string> result;
        for (const auto& session : all) {
            result.push_back(session.id);
        }
        return result;
    }();
    CHECK(std::find(ids.begin(), ids.end(), "id-a") != ids.end());
    CHECK(std::find(ids.begin(), ids.end(), "id-b") != ids.end());

    // A custom session directory replaces the whole search space.
    const auto custom = temp.path() / "custom";
    std::filesystem::create_directory(custom);
    write_session_header(custom / "c.jsonl", "id-c", temp.path().string());
    const auto custom_all = discovery::list_all_sessions(sessions_root, custom);
    REQUIRE(custom_all.size() == 1);
    CHECK(custom_all.front().id == "id-c");
}

TEST_CASE("resolve session arg classifies path-like values against the workspace", "[coding_agent][session-discovery]") {
    tests::TempWorkspace temp;
    const auto workspace = temp.path() / "workspace";
    std::filesystem::create_directory(workspace);

    const auto path_relative = discovery::resolve_session_arg(
        "session.jsonl", workspace, workspace, std::nullopt, {}, std::nullopt);
    CHECK(path_relative.kind == discovery::SessionArgKind::Path);
    CHECK(path_relative.path == workspace / "session.jsonl");

    const auto path_slash = discovery::resolve_session_arg(
        "nested/session.jsonl", workspace, workspace, std::nullopt, {}, std::nullopt);
    CHECK(path_slash.kind == discovery::SessionArgKind::Path);
    CHECK(path_slash.path == workspace / "nested" / "session.jsonl");

    const auto path_absolute = discovery::resolve_session_arg(
        "/tmp/absolute.jsonl", workspace, workspace, std::nullopt, {}, std::nullopt);
    CHECK(path_absolute.kind == discovery::SessionArgKind::Path);
    CHECK(path_absolute.path == std::filesystem::path{"/tmp/absolute.jsonl"});

    const auto path_backslash = discovery::resolve_session_arg(
        R"(dir\session.jsonl)", workspace, workspace, std::nullopt, {}, std::nullopt);
    CHECK(path_backslash.kind == discovery::SessionArgKind::Path);

    // A bare name is an id, never a path.
    const auto bare = discovery::resolve_session_arg(
        "session", workspace, workspace, std::nullopt, {}, std::nullopt);
    CHECK(bare.kind == discovery::SessionArgKind::NotFound);
}

TEST_CASE("resolve session arg matches local exact then prefix, then global", "[coding_agent][session-discovery]") {
    tests::TempWorkspace temp;
    const auto workspace = temp.path() / "workspace";
    const auto local_dir = temp.path() / "local";
    const auto sessions_root = temp.path() / "sessions";
    std::filesystem::create_directory(workspace);
    std::filesystem::create_directory(local_dir);

    write_session_header(local_dir / "exact.jsonl", "session-abc", workspace.string());
    write_session_header(local_dir / "prefix.jsonl", "session-xyz", workspace.string());

    // Exact local match wins over the prefix match.
    const auto exact = discovery::resolve_session_arg(
        "session-abc", workspace, local_dir, std::nullopt, sessions_root, std::nullopt);
    CHECK(exact.kind == discovery::SessionArgKind::Local);
    CHECK(exact.path == local_dir / "exact.jsonl");

    const auto prefix = discovery::resolve_session_arg(
        "session-x", workspace, local_dir, std::nullopt, sessions_root, std::nullopt);
    CHECK(prefix.kind == discovery::SessionArgKind::Local);
    CHECK(prefix.path == local_dir / "prefix.jsonl");

    // No local match: global search under the sessions root.
    const auto project = sessions_root / "--other-project--";
    std::filesystem::create_directories(project);
    write_session_header(project / "global.jsonl", "global-42", temp.path() / "elsewhere");
    const auto global = discovery::resolve_session_arg(
        "global-42", workspace, local_dir, std::nullopt, sessions_root, std::nullopt);
    CHECK(global.kind == discovery::SessionArgKind::Global);
    CHECK(global.path == project / "global.jsonl");
    CHECK(global.cwd == (temp.path() / "elsewhere").string());

    const auto global_prefix = discovery::resolve_session_arg(
        "global-4", workspace, local_dir, std::nullopt, sessions_root, std::nullopt);
    CHECK(global_prefix.kind == discovery::SessionArgKind::Global);

    // No match anywhere.
    const auto missing = discovery::resolve_session_arg(
        "nothing-here", workspace, local_dir, std::nullopt, sessions_root, std::nullopt);
    CHECK(missing.kind == discovery::SessionArgKind::NotFound);
    CHECK(missing.arg == "nothing-here");
}

TEST_CASE("resolve session arg honors the engaged cwd filter", "[coding_agent][session-discovery]") {
    tests::TempWorkspace temp;
    const auto workspace = temp.path() / "workspace";
    const auto other_cwd = temp.path() / "other";
    const auto local_dir = temp.path() / "local";
    std::filesystem::create_directory(workspace);
    std::filesystem::create_directory(local_dir);

    write_session_header(local_dir / "mine.jsonl", "mine-1", workspace.string());
    write_session_header(local_dir / "theirs.jsonl", "theirs-1", other_cwd.string());

    const auto filtered = discovery::resolve_session_arg(
        "theirs-1", workspace, local_dir, workspace, {}, std::nullopt);
    CHECK(filtered.kind == discovery::SessionArgKind::NotFound);

    const auto match = discovery::resolve_session_arg(
        "mine-1", workspace, local_dir, workspace, {}, std::nullopt);
    CHECK(match.kind == discovery::SessionArgKind::Local);
}

TEST_CASE("build session info parses name, counts, first message, and activity", "[coding_agent][session-discovery][issue409]") {
    tests::TempWorkspace temp;
    const auto workspace = temp.path() / "workspace";
    std::filesystem::create_directory(workspace);
    const auto session_path = temp.path() / "session.jsonl";
    {
        std::ofstream output(session_path, std::ios::binary);
        output << "{\"type\":\"session\",\"version\":3,\"id\":\"forked-1\","
                  "\"timestamp\":\"2026-08-08T00:00:00.000Z\",\"cwd\":\""
               << workspace.string()
               << "\",\"parentSession\":\"/old/source.jsonl\"}\n";
        // session_info name (latest wins), a user message (string content),
        // an assistant message (text block), a blank user message (no text), a
        // tool-result message (no text), and a later name clear.
        output << "{\"type\":\"session_info\",\"id\":\"e1\",\"timestamp\":1,"
                  "\"name\":\"First name\"}\n";
        output << "{\"type\":\"message\",\"id\":\"m1\",\"timestamp\":2,"
                  "\"message\":{\"role\":\"user\",\"content\":\"hello world\"}}\n";
        output << "{\"type\":\"message\",\"id\":\"m2\",\"timestamp\":3,"
                  "\"message\":{\"role\":\"assistant\",\"content\":["
                  "{\"type\":\"text\",\"text\":\"assistant says\"}]}}\n";
        output << "{\"type\":\"message\",\"id\":\"m3\",\"timestamp\":4,"
                  "\"message\":{\"role\":\"user\",\"content\":\"   \"}}\n";
        output << "{\"type\":\"message\",\"id\":\"m4\",\"timestamp\":5,"
                  "\"message\":{\"role\":\"tool_result\",\"content\":\"nope\"}}\n";
        output << "{\"type\":\"session_info\",\"id\":\"e2\",\"timestamp\":6,"
                  "\"name\":\"Renamed\"}\n";
    }

    const auto info = discovery::build_session_info(session_path);
    REQUIRE(info.has_value());
    CHECK(info->id == "forked-1");
    CHECK(info->cwd == workspace.string());
    CHECK(info->name == "Renamed");
    REQUIRE(info->parent_session_path.has_value());
    CHECK(*info->parent_session_path == std::filesystem::path{"/old/source.jsonl"});
    CHECK(info->created == "2026-08-08T00:00:00.000Z");
    // Message count counts every message entry (blank/tool included); the
    // first message is the first user message with text; the all-messages
    // text joins user/assistant texts with spaces (pi does not trim, so the
    // blank user text is joined too).
    CHECK(info->message_count == 4);
    CHECK(info->first_message == "hello world");
    CHECK(info->all_messages_text == "hello world assistant says    ");

    // No name and no messages: first message defaults to "(no messages)".
    const auto bare = temp.path() / "bare.jsonl";
    {
        std::ofstream output(bare, std::ios::binary);
        output << "{\"type\":\"session\",\"version\":3,\"id\":\"bare-1\","
                  "\"timestamp\":\"2026-08-08T00:00:00.000Z\",\"cwd\":\""
               << workspace.string() << "\"}\n";
    }
    const auto bare_info = discovery::build_session_info(bare);
    REQUIRE(bare_info.has_value());
    CHECK_FALSE(bare_info->name.has_value());
    CHECK(bare_info->message_count == 0);
    CHECK(bare_info->first_message == "(no messages)");
    CHECK(bare_info->all_messages_text.empty());
}

TEST_CASE("build session info skips malformed lines and non-session files", "[coding_agent][session-discovery][issue409]") {
    tests::TempWorkspace temp;
    const auto workspace = temp.path() / "workspace";
    std::filesystem::create_directory(workspace);
    const auto session_path = temp.path() / "session.jsonl";
    {
        std::ofstream output(session_path, std::ios::binary);
        output << "{\"type\":\"session\",\"version\":3,\"id\":\"ok-1\","
                  "\"timestamp\":\"2026-08-08T00:00:00.000Z\",\"cwd\":\""
               << workspace.string() << "\"}\n";
        // A malformed middle line is skipped, not fatal (pi `continue`).
        output << "this is not json\n";
        output << "{\"type\":\"message\",\"id\":\"m1\",\"timestamp\":2,"
                  "\"message\":{\"role\":\"user\",\"content\":\"survived\"}}\n";
    }
    const auto info = discovery::build_session_info(session_path);
    REQUIRE(info.has_value());
    CHECK(info->id == "ok-1");
    CHECK(info->message_count == 1);
    CHECK(info->first_message == "survived");

    // No session header: nullopt.
    const auto not_session = temp.path() / "other.jsonl";
    {
        std::ofstream output(not_session, std::ios::binary);
        output << "{\"type\":\"header\",\"id\":\"x\"}\n";
    }
    CHECK_FALSE(discovery::build_session_info(not_session).has_value());
    CHECK_FALSE(discovery::build_session_info(temp.path() / "missing.jsonl").has_value());
}

TEST_CASE("list sessions info scans, cwd-filters, and sorts newest first", "[coding_agent][session-discovery][issue409]") {
    tests::TempWorkspace temp;
    const auto directory = temp.path() / "sessions";
    const auto workspace = temp.path() / "workspace";
    const auto other_cwd = temp.path() / "other";
    std::filesystem::create_directory(directory);
    std::filesystem::create_directory(workspace);
    std::filesystem::create_directory(other_cwd);

    const auto newer = directory / "newer.jsonl";
    const auto older = directory / "older.jsonl";
    const auto foreign = directory / "foreign.jsonl";
    write_session_header(newer, "id-new", workspace.string());
    write_session_header(older, "id-old", workspace.string());
    write_session_header(foreign, "id-foreign", other_cwd.string());
    // Distinct message activity timestamps drive the sort (pi `modified` = the
    // latest message activity, which beats the header timestamp and mtime).
    {
        std::ofstream output(newer, std::ios::app);
        output << "{\"type\":\"message\",\"id\":\"m1\",\"timestamp\":3000,"
                  "\"message\":{\"role\":\"user\",\"content\":\"newest\"}}\n";
    }
    {
        std::ofstream output(older, std::ios::app);
        output << "{\"type\":\"message\",\"id\":\"m1\",\"timestamp\":2000,"
                  "\"message\":{\"role\":\"user\",\"content\":\"older\"}}\n";
    }
    {
        std::ofstream output(foreign, std::ios::app);
        output << "{\"type\":\"message\",\"id\":\"m1\",\"timestamp\":1000,"
                  "\"message\":{\"role\":\"user\",\"content\":\"foreign\"}}\n";
    }
    const auto newer_time = std::filesystem::last_write_time(directory) +
        std::chrono::hours(2);
    const auto older_time = std::filesystem::last_write_time(directory);
    std::filesystem::last_write_time(newer, newer_time);
    std::filesystem::last_write_time(older, older_time);

    const auto all = discovery::list_sessions_info(directory, std::nullopt);
    REQUIRE(all.size() == 3);
    CHECK(all.front().id == "id-new");
    CHECK(all.back().id == "id-foreign");

    const auto filtered =
        discovery::list_sessions_info(directory, workspace);
    REQUIRE(filtered.size() == 2);
    CHECK(filtered.front().id == "id-new");
    CHECK(filtered.back().id == "id-old");

    CHECK(discovery::list_sessions_info(temp.path() / "missing", std::nullopt).empty());
}

TEST_CASE("list all sessions info scans projects and honors a custom directory", "[coding_agent][session-discovery][issue409]") {
    tests::TempWorkspace temp;
    const auto sessions_root = temp.path() / "root";
    const auto workspace = temp.path() / "workspace";
    std::filesystem::create_directory(workspace);
    const auto project_a = sessions_root / "--project-a--";
    const auto project_b = sessions_root / "--project-b--";
    std::filesystem::create_directories(project_a);
    std::filesystem::create_directories(project_b);

    write_session_header(project_a / "a.jsonl", "id-a", workspace.string());
    write_session_header(project_b / "b.jsonl", "id-b", workspace.string());

    const auto all = discovery::list_all_sessions_info(sessions_root, std::nullopt);
    REQUIRE(all.size() == 2);

    // Custom directory: only that directory, no cwd filter.
    const auto custom = temp.path() / "custom";
    std::filesystem::create_directory(custom);
    write_session_header(custom / "c.jsonl", "id-c", workspace.string());
    const auto custom_all =
        discovery::list_all_sessions_info(sessions_root, custom);
    REQUIRE(custom_all.size() == 1);
    CHECK(custom_all.front().id == "id-c");
}

