#include "../../third_party/catch2/catch_test_macros.hpp"

#include "coding_agent/SessionPathPolicy.hpp"
#include "coding_agent/runtime/SessionLifecycle.hpp"

#include "../../include/cch/harness/session/JsonlSessionStore.hpp"
#include "../support/TempWorkspace.hpp"

#include <filesystem>
#include <fstream>
#include <regex>
#include <string>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace cch;
namespace session_paths = cch::coding_agent::session_paths;
namespace runtime = cch::coding_agent::runtime;

namespace {

harness::session::SessionMetadata metadata_for(const std::filesystem::path& workspace) {
    return {"explicit-session-id", "2026-07-18T00:00:00.000Z", workspace, "fake", "fake-model"};
}

#if defined(__unix__) || defined(__APPLE__)
mode_t permission_bits(const std::filesystem::path& path) {
    struct stat status {};
    REQUIRE(::stat(path.c_str(), &status) == 0);
    return status.st_mode & 0777;
}
#endif

} // namespace

TEST_CASE("workspace session keys use pi readable encoding", "[coding_agent][session-path-policy]") {
    struct Example {
        const char* input;
        const char* expected;
    };
    const Example examples[] = {
        {"/home/alice/project", "--home-alice-project--"},
        {R"(C:\Users\Alice\project)", "--C--Users-Alice-project--"},
        {R"(\server/share\project)", "--server-share-project--"},
        {R"(\\server\share)", "---server-share--"},
        {"/", "----"},
        {"/path//with:separators", "--path--with-separators--"},
        {"/path with spaces", "--path with spaces--"},
    };

    for (const auto& example : examples) {
        CHECK(session_paths::encode_workspace_key(std::filesystem::path{example.input}) == example.expected);
    }
}

TEST_CASE("automatic session identity is a UUID correlated with one UTC timestamp", "[coding_agent][session-path-policy]") {
    const auto identity = session_paths::generate_automatic_session_identity();
    const std::regex uuid_v4{
        R"(^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$)"};
    const std::regex utc_timestamp{
        R"(^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{3}Z$)"};

    CHECK(std::regex_match(identity.session_id, uuid_v4));
    CHECK(std::regex_match(identity.created_at, utc_timestamp));

    auto safe_timestamp = identity.created_at;
    for (auto& character : safe_timestamp) {
        if (character == ':' || character == '.') {
            character = '-';
        }
    }
    CHECK(session_paths::automatic_session_filename(identity) ==
          safe_timestamp + "_" + identity.session_id + ".jsonl");
}

TEST_CASE("automatic session target calculation is side effect free", "[coding_agent][session-path-policy]") {
    tests::TempWorkspace temp;
    const auto sessions_root = temp.path() / "not-created" / "sessions";
    const auto workspace = std::filesystem::path{"/resolved/workspace"};
    const session_paths::AutomaticSessionIdentity identity{
        "123e4567-e89b-42d3-a456-426614174000",
        "2026-07-18T01:02:03.456Z",
    };

    const auto target = session_paths::make_automatic_session_target(sessions_root, workspace, identity);

    CHECK_FALSE(std::filesystem::exists(sessions_root));
    CHECK(target.workspace == workspace);
    CHECK(target.workspace_directory == sessions_root / "--resolved-workspace--");
    CHECK(target.session_path == target.workspace_directory /
          "2026-07-18T01-02-03-456Z_123e4567-e89b-42d3-a456-426614174000.jsonl");
    CHECK(target.identity.session_id == identity.session_id);
    CHECK(target.identity.created_at == identity.created_at);
    CHECK(target.workspace_directory.filename().string().find("123e4567") == std::string::npos);
}

TEST_CASE("automatic session publication correlates path header and identity", "[coding_agent][session-path-policy][publication]") {
    tests::TempWorkspace temp;
    const auto sessions_root = temp.path() / "agent" / "sessions";
    const auto workspace = temp.path() / "workspace";
    std::filesystem::create_directory(workspace);
    const auto identity = session_paths::generate_automatic_session_identity();
    const auto target = session_paths::make_automatic_session_target(sessions_root, workspace, identity);

    CHECK_FALSE(std::filesystem::exists(sessions_root));
    auto published = runtime::publish_automatic_session(target, "fake", "fake-model");
    REQUIRE(published);
    CHECK(published->metadata.session_id == identity.session_id);
    CHECK(published->metadata.created_at == identity.created_at);
    CHECK(published->metadata.workspace == workspace);
    CHECK(published->store->path() == target.session_path);

    auto loaded = harness::session::JsonlSessionStore::load(target.session_path);
    REQUIRE(loaded);
    CHECK(loaded->metadata.session_id == identity.session_id);
    CHECK(loaded->metadata.created_at == identity.created_at);
    CHECK(target.session_path.filename() == session_paths::automatic_session_filename(identity));
#if defined(__unix__) || defined(__APPLE__)
    CHECK(permission_bits(sessions_root) == 0700);
    CHECK(permission_bits(target.workspace_directory) == 0700);
    CHECK(permission_bits(target.session_path) == 0600);
#endif
}

TEST_CASE("automatic publication makes default directories and file private", "[coding_agent][session-path-policy][publication]") {
#if defined(__unix__) || defined(__APPLE__)
    tests::TempWorkspace temp;
    const auto sessions_root = temp.path() / "sessions";
    const auto workspace = temp.path() / "workspace";
    std::filesystem::create_directory(workspace);
    std::filesystem::create_directory(sessions_root);
    ::chmod(sessions_root.c_str(), 0755);

    const auto target = session_paths::make_automatic_session_target(
        sessions_root, workspace, session_paths::generate_automatic_session_identity());
    std::filesystem::create_directory(target.workspace_directory);
    ::chmod(target.workspace_directory.c_str(), 0755);

    auto published = runtime::publish_automatic_session(target, "fake", "fake-model");
    REQUIRE(published);
    CHECK(permission_bits(sessions_root) == 0700);
    CHECK(permission_bits(target.workspace_directory) == 0700);
    CHECK(permission_bits(target.session_path) == 0600);
#else
    SUCCEED("POSIX permission assertions are not available on this platform");
#endif
}

TEST_CASE("explicit publication preserves custom directory mode while making file private", "[coding_agent][session-path-policy][publication]") {
#if defined(__unix__) || defined(__APPLE__)
    tests::TempWorkspace temp;
    const auto custom_directory = temp.path() / "custom";
    std::filesystem::create_directory(custom_directory);
    ::chmod(custom_directory.c_str(), 0755);
    const auto path = custom_directory / "explicit.jsonl";

    auto published = runtime::publish_new_session(path, temp.path(), metadata_for(temp.path()));
    REQUIRE(published);
    CHECK(permission_bits(custom_directory) == 0755);
    CHECK(permission_bits(path) == 0600);
#else
    SUCCEED("POSIX permission assertions are not available on this platform");
#endif
}

TEST_CASE("automatic publication rejects symbolic link directories", "[coding_agent][session-path-policy][publication]") {
#if defined(__unix__) || defined(__APPLE__)
    tests::TempWorkspace temp;
    const auto real_root = temp.path() / "real-sessions";
    const auto linked_root = temp.path() / "linked-sessions";
    const auto workspace = temp.path() / "workspace";
    std::filesystem::create_directory(real_root);
    std::filesystem::create_directory(workspace);
    REQUIRE(::symlink(real_root.c_str(), linked_root.c_str()) == 0);

    const auto target = session_paths::make_automatic_session_target(
        linked_root, workspace, session_paths::generate_automatic_session_identity());
    auto published = runtime::publish_automatic_session(target, "fake", "fake-model");

    REQUIRE_FALSE(published);
    CHECK(published.error().detail.find(target.session_path.string()) != std::string::npos);
    CHECK(published.error().detail.find("symlink") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(real_root / session_paths::encode_workspace_key(workspace)));
#else
    SUCCEED("symbolic-link publication assertion is not available on this platform");
#endif
}

TEST_CASE("automatic publication retains exclusive file creation", "[coding_agent][session-path-policy][publication]") {
    tests::TempWorkspace temp;
    const auto workspace = temp.path() / "workspace";
    std::filesystem::create_directory(workspace);
    const auto target = session_paths::make_automatic_session_target(
        temp.path() / "sessions", workspace, session_paths::generate_automatic_session_identity());

    auto first = runtime::publish_automatic_session(target, "fake", "fake-model");
    REQUIRE(first);
    auto second = runtime::publish_automatic_session(target, "fake", "fake-model");

    REQUIRE_FALSE(second);
    CHECK(second.error().detail.find(target.session_path.string()) != std::string::npos);
    CHECK(second.error().detail.find("already exists") != std::string::npos);
}

TEST_CASE("automatic publication rejects a symbolic link final target", "[coding_agent][session-path-policy][publication]") {
#if defined(__unix__) || defined(__APPLE__)
    tests::TempWorkspace temp;
    const auto workspace = temp.path() / "workspace";
    std::filesystem::create_directory(workspace);
    const auto target = session_paths::make_automatic_session_target(
        temp.path() / "sessions", workspace, session_paths::generate_automatic_session_identity());
    std::filesystem::create_directories(target.workspace_directory);
    const auto outside = temp.path() / "outside.jsonl";
    {
        std::ofstream file(outside);
        file << "outside";
    }
    REQUIRE(::symlink(outside.c_str(), target.session_path.c_str()) == 0);

    auto published = runtime::publish_automatic_session(target, "fake", "fake-model");

    REQUIRE_FALSE(published);
    CHECK(published.error().detail.find(target.session_path.string()) != std::string::npos);
    CHECK(published.error().detail.find("symlink") != std::string::npos);
#else
    SUCCEED("symbolic-link publication assertion is not available on this platform");
#endif
}

TEST_CASE("automatic publication failures include attempted target and reason", "[coding_agent][session-path-policy][publication]") {
    tests::TempWorkspace temp;
    const auto sessions_root = temp.path() / "sessions";
    const auto workspace = temp.path() / "workspace";
    std::filesystem::create_directory(workspace);
    {
        std::ofstream blocker(sessions_root);
        blocker << "not a directory";
    }
    const auto target = session_paths::make_automatic_session_target(
        sessions_root, workspace, session_paths::generate_automatic_session_identity());

    auto published = runtime::publish_automatic_session(target, "fake", "fake-model");

    REQUIRE_FALSE(published);
    CHECK(published.error().detail.find(target.session_path.string()) != std::string::npos);
    CHECK(published.error().detail.find("directory") != std::string::npos);
}

TEST_CASE("automatic publication rejects a relative sessions root", "[coding_agent][session-path-policy][publication]") {
    tests::TempWorkspace temp;
    const auto target = session_paths::make_automatic_session_target(
        "relative-agent/sessions",
        temp.path(),
        session_paths::generate_automatic_session_identity());

    CHECK(target.workspace_directory.empty());
    CHECK(target.session_path.empty());
    auto published = runtime::publish_automatic_session(target, "fake", "fake-model");

    REQUIRE_FALSE(published);
    CHECK(published.error().detail.find("relative-agent/sessions") != std::string::npos);
    CHECK(published.error().detail.find("must be absolute") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists("relative-agent"));
}

TEST_CASE("automatic publication fails when the user sessions root is unresolved", "[coding_agent][session-path-policy][publication]") {
    tests::TempWorkspace temp;
    const auto identity = session_paths::generate_automatic_session_identity();
    const auto target = session_paths::make_automatic_session_target({}, temp.path(), identity);

    auto published = runtime::publish_automatic_session(target, "fake", "fake-model");

    REQUIRE_FALSE(published);
    CHECK_FALSE(std::filesystem::exists(temp.path() / "sessions"));
    CHECK(published.error().detail.find("<unresolved agent config sessions root>") != std::string::npos);
    CHECK(published.error().detail.find("could not be resolved") != std::string::npos);
}
