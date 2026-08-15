#include <cch/coding_agent/AgentConfigDir.hpp>
#include "../support/EnvVarGuard.hpp"
#include "../support/TempWorkspace.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <optional>
#include <string>

TEST_CASE("agent_config_dir honors the PI_CODING_AGENT_DIR override", "[coding_agent][agent-config-dir][issue337]") {
    const cch::tests::EnvVarGuard override_dir{"PI_CODING_AGENT_DIR", std::string{"/tmp/cch-test-agent-dir"}};
    CHECK(cch::coding_agent::agent_config_dir() == std::filesystem::path{"/tmp/cch-test-agent-dir"});
}

TEST_CASE(
    "derived user state files live inside the pi agent config directory",
    "[coding_agent][agent-config-dir][issue337]") {
    const cch::tests::EnvVarGuard override_dir{"PI_CODING_AGENT_DIR", std::string{"/tmp/cch-test-agent-dir"}};
    CHECK(cch::coding_agent::auth_file_path() == std::filesystem::path{"/tmp/cch-test-agent-dir/auth.json"});
    CHECK(cch::coding_agent::settings_file_path() == std::filesystem::path{"/tmp/cch-test-agent-dir/settings.json"});
    CHECK(cch::coding_agent::models_file_path() == std::filesystem::path{"/tmp/cch-test-agent-dir/models.json"});
    CHECK(cch::coding_agent::trust_store_file_path() == std::filesystem::path{"/tmp/cch-test-agent-dir/trust.json"});
    CHECK(cch::coding_agent::sessions_root_path() == std::filesystem::path{"/tmp/cch-test-agent-dir/sessions"});
    CHECK(cch::coding_agent::themes_root_path() == std::filesystem::path{"/tmp/cch-test-agent-dir/themes"});
}

TEST_CASE("agent resource root calculations do not create the root", "[coding_agent][agent-config-dir][issue337]") {
    cch::tests::TempWorkspace temp;
    const auto agent_root = temp.path() / "not-created-agent-root";
    const cch::tests::EnvVarGuard override_dir{"PI_CODING_AGENT_DIR", agent_root.string()};

    CHECK(cch::coding_agent::sessions_root_path() == agent_root / "sessions");
    CHECK(cch::coding_agent::themes_root_path() == agent_root / "themes");
    CHECK_FALSE(std::filesystem::exists(agent_root));
}

TEST_CASE("sessions_root_path is empty when no user-level root is available", "[coding_agent][agent-config-dir]") {
#if defined(__unix__) || defined(__APPLE__)
    const cch::tests::EnvVarGuard no_override{"PI_CODING_AGENT_DIR", std::nullopt};
    const cch::tests::EnvVarGuard no_home{"HOME", std::nullopt};
    CHECK(cch::coding_agent::agent_config_dir().empty());
    CHECK(cch::coding_agent::sessions_root_path().empty());
#else
    SUCCEED("unavailable home-directory test skipped on this platform");
#endif
}

TEST_CASE("agent_config_dir defaults directly to the pi home layout", "[coding_agent][agent-config-dir][issue337]") {
#if defined(__unix__) || defined(__APPLE__)
    const cch::tests::EnvVarGuard no_override{"PI_CODING_AGENT_DIR", std::nullopt};
    const cch::tests::EnvVarGuard home{"HOME", std::string{"/tmp/test-home"}};
    CHECK(cch::coding_agent::agent_config_dir() == "/tmp/test-home/.pi/agent");
    CHECK(cch::coding_agent::settings_file_path() == "/tmp/test-home/.pi/agent/settings.json");
    CHECK(cch::coding_agent::models_file_path() == "/tmp/test-home/.pi/agent/models.json");
    CHECK(cch::coding_agent::auth_file_path() == "/tmp/test-home/.pi/agent/auth.json");
    CHECK(cch::coding_agent::trust_store_file_path() == "/tmp/test-home/.pi/agent/trust.json");
#else
    SUCCEED("HOME-based agent config dir test skipped on this platform");
#endif
}

TEST_CASE("legacy harness agent directory inputs are ignored", "[coding_agent][agent-config-dir][issue337]") {
#if defined(__unix__) || defined(__APPLE__)
    const cch::tests::EnvVarGuard no_pi_override{"PI_CODING_AGENT_DIR", std::nullopt};
    const cch::tests::EnvVarGuard legacy_override{"CCH_CODING_AGENT_DIR", std::string{"/tmp/legacy-agent-dir"}};
    const cch::tests::EnvVarGuard home{"HOME", std::string{"/tmp/pi-home"}};
    CHECK(cch::coding_agent::agent_config_dir() == "/tmp/pi-home/.pi/agent");
#else
    SUCCEED("legacy environment assertion is not available on this platform");
#endif
}
