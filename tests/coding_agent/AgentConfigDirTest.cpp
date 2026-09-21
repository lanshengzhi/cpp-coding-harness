#include <cch/coding_agent/AgentConfigDir.hpp>
#include "support/AgentRootFixture.hpp"
#include "support/EnvVarGuard.hpp"
#include "support/TempWorkspace.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <optional>
#include <string>

using namespace cch;

TEST_CASE("agent_config_dir honors XDG_CONFIG_HOME", "[coding_agent][agent-config-dir][issue754][spec]") {
    const tests::EnvVarGuard xdg{"XDG_CONFIG_HOME", std::string{"/tmp/cch-test-xdg"}};
    CHECK(coding_agent::agent_config_dir() == std::filesystem::path{"/tmp/cch-test-xdg/pike/agent"});
}

TEST_CASE("derived user state files live inside the product agent config directory",
        "[coding_agent][agent-config-dir][issue754][spec]") {
    const tests::EnvVarGuard xdg{"XDG_CONFIG_HOME", std::string{"/tmp/cch-test-xdg"}};
    const std::filesystem::path root{"/tmp/cch-test-xdg/pike/agent"};
    CHECK(coding_agent::auth_file_path() == root / "auth.json");
    CHECK(coding_agent::settings_file_path() == root / "settings.json");
    CHECK(coding_agent::models_file_path() == root / "models.json");
    CHECK(coding_agent::trust_store_file_path() == root / "trust.json");
    CHECK(coding_agent::sessions_root_path() == root / "sessions");
    CHECK(coding_agent::themes_root_path() == root / "themes");
}

TEST_CASE(
        "agent resource root calculations do not create the root", "[coding_agent][agent-config-dir][issue754][spec]") {
    tests::TempWorkspace temp;
    const auto xdg = temp.path() / "not-created-xdg-root";
    const tests::EnvVarGuard xdg_guard{"XDG_CONFIG_HOME", xdg.string()};

    CHECK(coding_agent::agent_config_dir() == tests::agent_root_under_xdg(xdg));
    CHECK(coding_agent::sessions_root_path() == tests::agent_root_under_xdg(xdg) / "sessions");
    CHECK_FALSE(std::filesystem::exists(xdg));
}

TEST_CASE("state paths are empty when no user-level root is available",
        "[coding_agent][agent-config-dir][issue754][spec]") {
    const tests::EnvVarGuard no_xdg{"XDG_CONFIG_HOME", std::nullopt};
    const tests::EnvVarGuard no_home{"HOME", std::nullopt};
    CHECK(coding_agent::agent_config_dir().empty());
    CHECK(coding_agent::sessions_root_path().empty());
}

TEST_CASE("agent_config_dir defaults to the product home layout", "[coding_agent][agent-config-dir][issue754][spec]") {
    const tests::EnvVarGuard no_xdg{"XDG_CONFIG_HOME", std::nullopt};
    const tests::EnvVarGuard home{"HOME", std::string{"/tmp/test-home"}};
    const auto root = tests::agent_root_under_home("/tmp/test-home");
    CHECK(coding_agent::agent_config_dir() == root);
    CHECK(coding_agent::settings_file_path() == root / "settings.json");
    CHECK(coding_agent::models_file_path() == root / "models.json");
    CHECK(coding_agent::auth_file_path() == root / "auth.json");
    CHECK(coding_agent::trust_store_file_path() == root / "trust.json");
}

TEST_CASE("the agent config directory ignores every environment override",
        "[coding_agent][agent-config-dir][issue754][spec]") {
    const tests::EnvVarGuard no_xdg{"XDG_CONFIG_HOME", std::nullopt};
    const tests::EnvVarGuard home{"HOME", std::string{"/tmp/override-home"}};
    const tests::EnvVarGuard product_override{"PIKE_CONFIG_DIR", std::string{"/tmp/pike-config-dir"}};
    const tests::EnvVarGuard agent_override{"PIKE_CODING_AGENT_DIR", std::string{"/tmp/pike-coding-agent-dir"}};
    const tests::EnvVarGuard pi_override{"PI_CODING_AGENT_DIR", std::string{"/tmp/old-pi-agent-dir"}};
    const tests::EnvVarGuard legacy_override{"CCH_CODING_AGENT_DIR", std::string{"/tmp/legacy-agent-dir"}};
    CHECK(coding_agent::agent_config_dir() == tests::agent_root_under_home("/tmp/override-home"));
}

TEST_CASE("fresh installs do not fall back to an existing pi tree",
        "[coding_agent][agent-config-dir][issue754][diverge]") {
    tests::TempWorkspace temp;
    std::filesystem::create_directories(temp.path() / ".pi" / "agent");
    const tests::EnvVarGuard no_xdg{"XDG_CONFIG_HOME", std::nullopt};
    const tests::EnvVarGuard no_pi_override{"PI_CODING_AGENT_DIR", std::nullopt};
    const tests::EnvVarGuard home{"HOME", temp.path().string()};

    const auto root = tests::agent_root_under_home(temp.path());
    CHECK(coding_agent::agent_config_dir() == root);
    CHECK_FALSE(std::filesystem::exists(coding_agent::settings_file_path()));
}
