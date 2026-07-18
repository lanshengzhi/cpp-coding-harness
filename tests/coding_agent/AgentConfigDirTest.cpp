#include "../../third_party/catch2/catch_test_macros.hpp"

#include "../../include/cch/coding_agent/AgentConfigDir.hpp"

#include <cstdlib>
#include <filesystem>

namespace {

/// RAII guard for CCH_CODING_AGENT_DIR so tests never leak environment state.
class AgentDirEnvOverride {
public:
    explicit AgentDirEnvOverride(const std::filesystem::path& dir) {
        setenv("CCH_CODING_AGENT_DIR", dir.string().c_str(), 1);
    }
    ~AgentDirEnvOverride() {
        unsetenv("CCH_CODING_AGENT_DIR");
    }
    AgentDirEnvOverride(const AgentDirEnvOverride&) = delete;
    AgentDirEnvOverride& operator=(const AgentDirEnvOverride&) = delete;
};

} // namespace

TEST_CASE("agent_config_dir honors the CCH_CODING_AGENT_DIR override", "[coding_agent][agent-config-dir]") {
    const AgentDirEnvOverride override_dir{"/tmp/cch-test-agent-dir"};
    CHECK(cch::coding_agent::agent_config_dir() == std::filesystem::path{"/tmp/cch-test-agent-dir"});
}

TEST_CASE("derived user state files live inside the agent config directory", "[coding_agent][agent-config-dir]") {
    const AgentDirEnvOverride override_dir{"/tmp/cch-test-agent-dir"};
    CHECK(cch::coding_agent::auth_file_path() == std::filesystem::path{"/tmp/cch-test-agent-dir/auth.json"});
    CHECK(cch::coding_agent::settings_file_path() == std::filesystem::path{"/tmp/cch-test-agent-dir/settings.json"});
    CHECK(cch::coding_agent::trust_store_file_path() == std::filesystem::path{"/tmp/cch-test-agent-dir/trust.json"});
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
