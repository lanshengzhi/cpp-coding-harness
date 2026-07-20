#include "../../third_party/catch2/catch_test_macros.hpp"

#include "../../include/cch/coding_agent/AgentConfigDir.hpp"
#include "../support/EnvVarGuard.hpp"
#include "../support/TempWorkspace.hpp"

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>

TEST_CASE("agent_config_dir honors the CCH_CODING_AGENT_DIR override", "[coding_agent][agent-config-dir]") {
    const cch::tests::EnvVarGuard override_dir{"CCH_CODING_AGENT_DIR", std::string{"/tmp/cch-test-agent-dir"}};
    CHECK(cch::coding_agent::agent_config_dir() == std::filesystem::path{"/tmp/cch-test-agent-dir"});
}

TEST_CASE("derived user state files live inside the agent config directory", "[coding_agent][agent-config-dir]") {
    const cch::tests::EnvVarGuard override_dir{"CCH_CODING_AGENT_DIR", std::string{"/tmp/cch-test-agent-dir"}};
    CHECK(cch::coding_agent::auth_file_path() == std::filesystem::path{"/tmp/cch-test-agent-dir/auth.json"});
    CHECK(cch::coding_agent::settings_file_path() == std::filesystem::path{"/tmp/cch-test-agent-dir/settings.json"});
    CHECK(cch::coding_agent::trust_store_file_path() == std::filesystem::path{"/tmp/cch-test-agent-dir/trust.json"});
    CHECK(cch::coding_agent::sessions_root_path() == std::filesystem::path{"/tmp/cch-test-agent-dir/sessions"});
}

TEST_CASE("sessions_root_path calculation does not create the root", "[coding_agent][agent-config-dir]") {
    cch::tests::TempWorkspace temp;
    const auto agent_root = temp.path() / "not-created-agent-root";
    const cch::tests::EnvVarGuard override_dir{"CCH_CODING_AGENT_DIR", agent_root.string()};

    CHECK(cch::coding_agent::sessions_root_path() == agent_root / "sessions");
    CHECK_FALSE(std::filesystem::exists(agent_root));
}

TEST_CASE("sessions_root_path is empty when no user-level root is available", "[coding_agent][agent-config-dir]") {
#if defined(__unix__) || defined(__APPLE__)
    const cch::tests::EnvVarGuard no_override{"CCH_CODING_AGENT_DIR", std::nullopt};
    const cch::tests::EnvVarGuard no_home{"HOME", std::nullopt};
    CHECK(cch::coding_agent::agent_config_dir().empty());
    CHECK(cch::coding_agent::sessions_root_path().empty());
#else
    SUCCEED("unavailable home-directory test skipped on this platform");
#endif
}

TEST_CASE("settings_file_path resolves under HOME with pi-mirrored layout", "[coding_agent][agent-config-dir]") {
#if defined(__unix__) || defined(__APPLE__)
    unsetenv("CCH_CODING_AGENT_DIR");
    const auto previous = std::getenv("HOME");
    setenv("HOME", "/tmp/test-home", 1);
    CHECK(cch::coding_agent::settings_file_path() == "/tmp/test-home/.cpp-harness/agent/settings.json");
    CHECK(cch::coding_agent::auth_file_path() == "/tmp/test-home/.cpp-harness/agent/auth.json");
    CHECK(cch::coding_agent::trust_store_file_path() == "/tmp/test-home/.cpp-harness/agent/trust.json");
    if (previous != nullptr) {
        setenv("HOME", previous, 1);
    } else {
        unsetenv("HOME");
    }
#else
    SUCCEED("HOME-based agent config dir test skipped on this platform");
#endif
}

TEST_CASE("agent_config_dir mirrors pi layout under the home directory", "[coding_agent][agent-config-dir]") {
    // Without the override, resolution falls back to <home>/.cpp-harness/agent,
    // mirroring pi's ~/.pi/agent. HOME is expected to be set in test environments.
    const auto dir = cch::coding_agent::agent_config_dir();
    REQUIRE_FALSE(dir.empty());
    CHECK(dir.filename() == "agent");
    CHECK(dir.parent_path().filename() == ".cpp-harness");
}
