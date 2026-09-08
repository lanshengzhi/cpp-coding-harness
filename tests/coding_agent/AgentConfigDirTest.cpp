#include <cch/coding_agent/AgentConfigDir.hpp>
#include "support/EnvVarGuard.hpp"
#include "support/TempWorkspace.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <optional>
#include <string>

TEST_CASE("agent_config_dir honors the PIKE_CODING_AGENT_DIR override",
        "[coding_agent][agent-config-dir][issue337][spec]") {
    const cch::tests::EnvVarGuard override_dir{"PIKE_CODING_AGENT_DIR", std::string{"/tmp/cch-test-agent-dir"}};
    CHECK(cch::coding_agent::agent_config_dir() == std::filesystem::path{"/tmp/cch-test-agent-dir"});
}

TEST_CASE("derived user state files live inside the product agent config directory",
        "[coding_agent][agent-config-dir][issue337][spec]") {
    const cch::tests::EnvVarGuard override_dir{"PIKE_CODING_AGENT_DIR", std::string{"/tmp/cch-test-agent-dir"}};
    CHECK(cch::coding_agent::auth_file_path() == std::filesystem::path{"/tmp/cch-test-agent-dir/auth.json"});
    CHECK(cch::coding_agent::settings_file_path() == std::filesystem::path{"/tmp/cch-test-agent-dir/settings.json"});
    CHECK(cch::coding_agent::models_file_path() == std::filesystem::path{"/tmp/cch-test-agent-dir/models.json"});
    CHECK(cch::coding_agent::trust_store_file_path() == std::filesystem::path{"/tmp/cch-test-agent-dir/trust.json"});
    CHECK(cch::coding_agent::sessions_root_path() == std::filesystem::path{"/tmp/cch-test-agent-dir/sessions"});
    CHECK(cch::coding_agent::themes_root_path() == std::filesystem::path{"/tmp/cch-test-agent-dir/themes"});
}

TEST_CASE(
        "agent resource root calculations do not create the root", "[coding_agent][agent-config-dir][issue337][spec]") {
    cch::tests::TempWorkspace temp;
    const auto agent_root = temp.path() / "not-created-agent-root";
    const cch::tests::EnvVarGuard override_dir{"PIKE_CODING_AGENT_DIR", agent_root.string()};

    CHECK(cch::coding_agent::sessions_root_path() == agent_root / "sessions");
    CHECK(cch::coding_agent::themes_root_path() == agent_root / "themes");
    CHECK_FALSE(std::filesystem::exists(agent_root));
}

TEST_CASE(
        "sessions_root_path is empty when no user-level root is available", "[coding_agent][agent-config-dir][spec]") {
    const cch::tests::EnvVarGuard no_override{"PIKE_CODING_AGENT_DIR", std::nullopt};
    const cch::tests::EnvVarGuard no_home{"HOME", std::nullopt};
    CHECK(cch::coding_agent::agent_config_dir().empty());
    CHECK(cch::coding_agent::sessions_root_path().empty());
}

TEST_CASE("agent_config_dir defaults directly to the product home layout",
        "[coding_agent][agent-config-dir][issue337][spec]") {
    const cch::tests::EnvVarGuard no_override{"PIKE_CODING_AGENT_DIR", std::nullopt};
    const cch::tests::EnvVarGuard old_override{"PI_CODING_AGENT_DIR", std::string{"/tmp/old-pi-agent-dir"}};
    const cch::tests::EnvVarGuard home{"HOME", std::string{"/tmp/test-home"}};
    CHECK(cch::coding_agent::agent_config_dir() == "/tmp/test-home/.pike/agent");
    CHECK(cch::coding_agent::settings_file_path() == "/tmp/test-home/.pike/agent/settings.json");
    CHECK(cch::coding_agent::models_file_path() == "/tmp/test-home/.pike/agent/models.json");
    CHECK(cch::coding_agent::auth_file_path() == "/tmp/test-home/.pike/agent/auth.json");
    CHECK(cch::coding_agent::trust_store_file_path() == "/tmp/test-home/.pike/agent/trust.json");
}

TEST_CASE("legacy pi and harness agent directory inputs are ignored",
        "[coding_agent][agent-config-dir][issue337][spec]") {
    const cch::tests::EnvVarGuard no_product_override{"PIKE_CODING_AGENT_DIR", std::nullopt};
    const cch::tests::EnvVarGuard old_pi_override{"PI_CODING_AGENT_DIR", std::string{"/tmp/old-pi-agent-dir"}};
    const cch::tests::EnvVarGuard legacy_override{"CCH_CODING_AGENT_DIR", std::string{"/tmp/legacy-agent-dir"}};
    const cch::tests::EnvVarGuard home{"HOME", std::string{"/tmp/pi-home"}};
    CHECK(cch::coding_agent::agent_config_dir() == "/tmp/pi-home/.pike/agent");
}

TEST_CASE(
        "fresh installs do not fall back to an existing pi tree", "[coding_agent][agent-config-dir][issue626][spec]") {
    cch::tests::TempWorkspace temp;
    std::filesystem::create_directories(temp.path() / ".pi" / "agent");
    const cch::tests::EnvVarGuard no_product_override{"PIKE_CODING_AGENT_DIR", std::nullopt};
    const cch::tests::EnvVarGuard no_pi_override{"PI_CODING_AGENT_DIR", std::nullopt};
    const cch::tests::EnvVarGuard home{"HOME", temp.path().string()};

    CHECK(cch::coding_agent::agent_config_dir() == temp.path() / ".pike" / "agent");
    CHECK(cch::coding_agent::settings_file_path() == temp.path() / ".pike" / "agent" / "settings.json");
    CHECK_FALSE(std::filesystem::exists(cch::coding_agent::settings_file_path()));
}
