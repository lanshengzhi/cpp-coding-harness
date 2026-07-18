#include "../../include/cch/coding_agent/AgentConfigDir.hpp"

#include <cstdlib>

namespace cch::coding_agent {
namespace {

/// pi resolves the home directory through `os.homedir()`; the C++ equivalent
/// reads platform conventions directly. Environment access stays at this edge
/// so path composition in the public functions remains trivially auditable.
[[nodiscard]] std::filesystem::path home_dir() {
#if defined(_WIN32)
    if (const char* profile = std::getenv("USERPROFILE"); profile != nullptr && profile[0] != '\0') {
        return profile;
    }
#else
    if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
        return home;
    }
#endif
    return {};
}

[[nodiscard]] std::filesystem::path in_agent_config_dir(const char* file_name) {
    const auto dir = agent_config_dir();
    // An empty base must yield an empty path rather than a relative file name
    // that could accidentally resolve inside the current working directory.
    return dir.empty() ? std::filesystem::path{} : dir / file_name;
}

} // namespace

std::filesystem::path agent_config_dir() {
    if (const char* override_dir = std::getenv("CCH_CODING_AGENT_DIR");
        override_dir != nullptr && override_dir[0] != '\0') {
        return override_dir;
    }
    const auto home = home_dir();
    return home.empty() ? std::filesystem::path{} : home / ".cpp-harness" / "agent";
}

std::filesystem::path sessions_root_path() {
    return in_agent_config_dir("sessions");
}

std::filesystem::path auth_file_path() {
    return in_agent_config_dir("auth.json");
}

std::filesystem::path settings_file_path() {
    return in_agent_config_dir("settings.json");
}

std::filesystem::path trust_store_file_path() {
    return in_agent_config_dir("trust.json");
}

} // namespace cch::coding_agent
