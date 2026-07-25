#include "../../third_party/catch2/catch_test_macros.hpp"

#include "coding_agent/SessionPathPolicy.hpp"
#include "coding_agent/runtime/SessionLifecycle.hpp"

#include "../../include/cch/harness/session/JsonlSessionStore.hpp"
#include "../support/EnvVarGuard.hpp"
#include "../support/TempWorkspace.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
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

#if defined(__unix__) || defined(__APPLE__)
mode_t permission_bits(const std::filesystem::path& path) {
    struct stat status {};
    REQUIRE(::stat(path.c_str(), &status) == 0);
    return status.st_mode & 0777;
}
#endif

} // namespace

TEST_CASE("session directory override values resolve against the final workspace", "[coding_agent][session-path-policy]") {
    tests::TempWorkspace temp;
    const auto workspace = temp.path() / "workspace";
    const auto home = temp.path() / "home";

    // Absolute values remain absolute and normalize interior separators.
    CHECK(session_paths::resolve_session_dir_value("/data/sessions", workspace, home) ==
          std::filesystem::path{"/data/sessions"});
    CHECK(session_paths::resolve_session_dir_value("/data/./sessions/", workspace, home) ==
          std::filesystem::path{"/data/sessions"});

    // Relative values resolve against the final workspace, not the process cwd.
    CHECK(session_paths::resolve_session_dir_value("sessions", workspace, home) == workspace / "sessions");
    CHECK(session_paths::resolve_session_dir_value("nested/dir", workspace, home) == workspace / "nested" / "dir");
    CHECK(session_paths::resolve_session_dir_value("./sessions", workspace, home) == workspace / "sessions");
    CHECK(session_paths::resolve_session_dir_value("../shared", workspace, home) ==
          workspace.parent_path() / "shared");

    // A leading home marker expands against the supplied home directory.
    CHECK(session_paths::resolve_session_dir_value("~", workspace, home) == home);
    CHECK(session_paths::resolve_session_dir_value("~/sessions", workspace, home) == home / "sessions");

    // pi expands only "~" and "~/"; other tilde forms stay literal relative values.
    CHECK(session_paths::resolve_session_dir_value("~other/sessions", workspace, home) ==
          workspace / "~other" / "sessions");

    // Home expansion without a resolvable home fails explicitly.
    CHECK_FALSE(session_paths::resolve_session_dir_value("~", workspace, {}).has_value());
    CHECK_FALSE(session_paths::resolve_session_dir_value("~/sessions", workspace, {}).has_value());

    // Resolution is pure: it never creates filesystem state.
    CHECK_FALSE(std::filesystem::exists(workspace));
    CHECK_FALSE(std::filesystem::exists(home));
}

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
    tests::EnvVarGuard config_dir{"CCH_CODING_AGENT_DIR"};
    config_dir.set((temp.path() / "agent").string());
    const auto sessions_root = temp.path() / "agent" / "sessions";
    const auto workspace = temp.path() / "workspace";
    std::filesystem::create_directory(workspace);

    CHECK_FALSE(std::filesystem::exists(sessions_root));
    auto published = runtime::publish_session(
        runtime::AutomaticPublication{
            .workspace = workspace,
            .directory_override = std::nullopt,
        },
        "fake",
        "fake-model");
    REQUIRE(published);

    // The published header identity recomposes the exact persisted path.
    const auto workspace_directory = sessions_root / session_paths::encode_workspace_key(workspace);
    const session_paths::AutomaticSessionIdentity identity{
        published->metadata.session_id,
        published->metadata.created_at,
    };
    const auto session_path = workspace_directory / session_paths::automatic_session_filename(identity);
    CHECK(published->metadata.workspace == workspace);
    CHECK(published->store->path() == session_path);

    auto loaded = harness::session::JsonlSessionStore::load(session_path);
    REQUIRE(loaded);
    CHECK(loaded->metadata.session_id == published->metadata.session_id);
    CHECK(loaded->metadata.created_at == published->metadata.created_at);
#if defined(__unix__) || defined(__APPLE__)
    CHECK(permission_bits(sessions_root) == 0700);
    CHECK(permission_bits(workspace_directory) == 0700);
    CHECK(permission_bits(session_path) == 0600);
#endif
}

TEST_CASE("automatic publication makes default directories and file private", "[coding_agent][session-path-policy][publication]") {
#if defined(__unix__) || defined(__APPLE__)
    tests::TempWorkspace temp;
    tests::EnvVarGuard config_dir{"CCH_CODING_AGENT_DIR"};
    config_dir.set((temp.path() / "agent").string());
    const auto sessions_root = temp.path() / "agent" / "sessions";
    const auto workspace = temp.path() / "workspace";
    std::filesystem::create_directory(workspace);
    const auto workspace_directory = sessions_root / session_paths::encode_workspace_key(workspace);
    std::filesystem::create_directories(workspace_directory);
    ::chmod(sessions_root.c_str(), 0755);
    ::chmod(workspace_directory.c_str(), 0755);

    auto published = runtime::publish_session(
        runtime::AutomaticPublication{
            .workspace = workspace,
            .directory_override = std::nullopt,
        },
        "fake",
        "fake-model");

    REQUIRE(published);
    REQUIRE(published->store->path());
    CHECK(permission_bits(sessions_root) == 0700);
    CHECK(permission_bits(workspace_directory) == 0700);
    CHECK(permission_bits(*published->store->path()) == 0600);
#else
    SUCCEED("POSIX permission assertions are not available on this platform");
#endif
}

TEST_CASE("custom automatic session target calculation is side effect free", "[coding_agent][session-path-policy]") {
    tests::TempWorkspace temp;
    const auto directory = temp.path() / "not-created" / "custom-sessions";
    const auto workspace = std::filesystem::path{"/resolved/workspace"};
    const session_paths::AutomaticSessionIdentity identity{
        "123e4567-e89b-42d3-a456-426614174000",
        "2026-07-18T01:02:03.456Z",
    };

    const auto target = session_paths::make_custom_automatic_session_target(directory, workspace, identity);

    CHECK_FALSE(std::filesystem::exists(temp.path() / "not-created"));
    CHECK(target.custom_directory);
    CHECK(target.sessions_root == directory);
    CHECK(target.workspace == workspace);
    // A CLI directory override replaces the whole automatic directory: the
    // file lands directly inside it without a workspace-key component.
    CHECK(target.workspace_directory == directory);
    CHECK(target.session_path == directory /
          "2026-07-18T01-02-03-456Z_123e4567-e89b-42d3-a456-426614174000.jsonl");
    CHECK(target.identity.session_id == identity.session_id);
    CHECK(target.identity.created_at == identity.created_at);
}

TEST_CASE("custom automatic publication creates a missing override directory privately", "[coding_agent][session-path-policy][publication]") {
#if defined(__unix__) || defined(__APPLE__)
    tests::TempWorkspace temp;
    const auto directory = temp.path() / "missing" / "custom-sessions";
    const auto workspace = temp.path() / "workspace";
    std::filesystem::create_directory(workspace);

    auto published = runtime::publish_session(
        runtime::AutomaticPublication{
            .workspace = workspace,
            .directory_override = directory,
        },
        "fake",
        "fake-model");

    REQUIRE(published);
    REQUIRE(published->store->path());
    CHECK(published->store->path()->parent_path() == directory);
    CHECK(published->metadata.workspace == workspace);
    CHECK(permission_bits(directory) == 0700);
    CHECK(permission_bits(*published->store->path()) == 0600);
    auto loaded = harness::session::JsonlSessionStore::load(*published->store->path());
    REQUIRE(loaded);
    CHECK(loaded->metadata.session_id == published->metadata.session_id);
#else
    SUCCEED("POSIX permission assertions are not available on this platform");
#endif
}

TEST_CASE("custom automatic publication preserves an existing override directory mode", "[coding_agent][session-path-policy][publication]") {
#if defined(__unix__) || defined(__APPLE__)
    tests::TempWorkspace temp;
    const auto directory = temp.path() / "custom-sessions";
    const auto workspace = temp.path() / "workspace";
    std::filesystem::create_directory(directory);
    std::filesystem::create_directory(workspace);
    ::chmod(directory.c_str(), 0755);

    auto published = runtime::publish_session(
        runtime::AutomaticPublication{
            .workspace = workspace,
            .directory_override = directory,
        },
        "fake",
        "fake-model");

    REQUIRE(published);
    REQUIRE(published->store->path());
    CHECK(permission_bits(directory) == 0755);
    CHECK(permission_bits(*published->store->path()) == 0600);
#else
    SUCCEED("POSIX permission assertions are not available on this platform");
#endif
}

TEST_CASE("custom automatic publication rejects symbolic link override directories", "[coding_agent][session-path-policy][publication]") {
#if defined(__unix__) || defined(__APPLE__)
    tests::TempWorkspace temp;
    const auto real_directory = temp.path() / "real-sessions";
    const auto linked_directory = temp.path() / "linked-sessions";
    const auto workspace = temp.path() / "workspace";
    std::filesystem::create_directory(real_directory);
    std::filesystem::create_directory(workspace);
    REQUIRE(::symlink(real_directory.c_str(), linked_directory.c_str()) == 0);

    auto published = runtime::publish_session(
        runtime::AutomaticPublication{
            .workspace = workspace,
            .directory_override = linked_directory,
        },
        "fake",
        "fake-model");

    REQUIRE_FALSE(published);
    CHECK(published.error().detail.find(linked_directory.string()) != std::string::npos);
    CHECK(published.error().detail.find("symlink") != std::string::npos);
    CHECK(std::filesystem::is_empty(real_directory));
#else
    SUCCEED("symbolic-link publication assertion is not available on this platform");
#endif
}

TEST_CASE("custom automatic publication failures include attempted target and reason", "[coding_agent][session-path-policy][publication]") {
    tests::TempWorkspace temp;
    const auto directory = temp.path() / "custom-sessions";
    const auto workspace = temp.path() / "workspace";
    std::filesystem::create_directory(workspace);
    {
        std::ofstream blocker(directory);
        blocker << "not a directory";
    }

    auto published = runtime::publish_session(
        runtime::AutomaticPublication{
            .workspace = workspace,
            .directory_override = directory,
        },
        "fake",
        "fake-model");

    REQUIRE_FALSE(published);
    CHECK(published.error().detail.find(directory.string()) != std::string::npos);
    CHECK(published.error().detail.find("directory") != std::string::npos);
}

TEST_CASE("custom automatic publication rejects a relative override directory", "[coding_agent][session-path-policy][publication]") {
    auto published = runtime::publish_session(
        runtime::AutomaticPublication{
            .workspace = std::filesystem::path{"/resolved/workspace"},
            .directory_override = std::filesystem::path{"relative-sessions"},
        },
        "fake",
        "fake-model");

    REQUIRE_FALSE(published);
    CHECK(published.error().detail.find("absolute") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists("relative-sessions"));
}

TEST_CASE("explicit publication preserves custom directory mode while making file private", "[coding_agent][session-path-policy][publication]") {
#if defined(__unix__) || defined(__APPLE__)
    tests::TempWorkspace temp;
    const auto custom_directory = temp.path() / "custom";
    std::filesystem::create_directory(custom_directory);
    ::chmod(custom_directory.c_str(), 0755);
    const auto path = custom_directory / "explicit.jsonl";

    auto published = runtime::publish_session(
        runtime::ExplicitNewPublication{
            .session_path = path,
            .workspace = temp.path(),
        },
        "fake",
        "fake-model");

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
    tests::EnvVarGuard config_dir{"CCH_CODING_AGENT_DIR"};
    const auto config_root = temp.path() / "cfg";
    std::filesystem::create_directory(config_root);
    config_dir.set(config_root.string());
    const auto sessions_root = config_root / "sessions";
    const auto real_root = temp.path() / "real-sessions";
    const auto workspace = temp.path() / "workspace";
    std::filesystem::create_directory(real_root);
    std::filesystem::create_directory(workspace);
    REQUIRE(::symlink(real_root.c_str(), sessions_root.c_str()) == 0);

    auto published = runtime::publish_session(
        runtime::AutomaticPublication{
            .workspace = workspace,
            .directory_override = std::nullopt,
        },
        "fake",
        "fake-model");

    REQUIRE_FALSE(published);
    CHECK(published.error().detail.find(sessions_root.string()) != std::string::npos);
    CHECK(published.error().detail.find("symlink") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(real_root / session_paths::encode_workspace_key(workspace)));
#else
    SUCCEED("symbolic-link publication assertion is not available on this platform");
#endif
}

TEST_CASE("automatic publication failures include attempted target and reason", "[coding_agent][session-path-policy][publication]") {
    tests::TempWorkspace temp;
    tests::EnvVarGuard config_dir{"CCH_CODING_AGENT_DIR"};
    const auto config_root = temp.path() / "cfg";
    std::filesystem::create_directory(config_root);
    config_dir.set(config_root.string());
    const auto sessions_root = config_root / "sessions";
    const auto workspace = temp.path() / "workspace";
    std::filesystem::create_directory(workspace);
    {
        std::ofstream blocker(sessions_root);
        blocker << "not a directory";
    }

    auto published = runtime::publish_session(
        runtime::AutomaticPublication{
            .workspace = workspace,
            .directory_override = std::nullopt,
        },
        "fake",
        "fake-model");

    REQUIRE_FALSE(published);
    CHECK(published.error().detail.find(sessions_root.string()) != std::string::npos);
    CHECK(published.error().detail.find("directory") != std::string::npos);
}

TEST_CASE("automatic publication rejects a relative sessions root", "[coding_agent][session-path-policy][publication]") {
    tests::TempWorkspace temp;
    tests::EnvVarGuard config_dir{"CCH_CODING_AGENT_DIR"};
    config_dir.set("relative-agent");

    auto published = runtime::publish_session(
        runtime::AutomaticPublication{
            .workspace = temp.path(),
            .directory_override = std::nullopt,
        },
        "fake",
        "fake-model");

    REQUIRE_FALSE(published);
    const auto expected_root = (std::filesystem::path{"relative-agent"} / "sessions").string();
    CHECK(published.error().detail.find(expected_root) != std::string::npos);
    CHECK(published.error().detail.find("must be absolute") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists("relative-agent"));
}

TEST_CASE("automatic publication fails when the user sessions root is unresolved", "[coding_agent][session-path-policy][publication]") {
    tests::TempWorkspace temp;
    tests::EnvVarGuard config_dir{"CCH_CODING_AGENT_DIR"};
    tests::EnvVarGuard home{"HOME"};
    tests::EnvVarGuard user_profile{"USERPROFILE"};
    config_dir.set("");
    home.set("");
    user_profile.set("");

    auto published = runtime::publish_session(
        runtime::AutomaticPublication{
            .workspace = temp.path(),
            .directory_override = std::nullopt,
        },
        "fake",
        "fake-model");

    REQUIRE_FALSE(published);
    CHECK_FALSE(std::filesystem::exists(temp.path() / "sessions"));
    CHECK(published.error().detail.find("<unresolved agent config sessions root>") != std::string::npos);
    CHECK(published.error().detail.find("could not be resolved") != std::string::npos);
}
