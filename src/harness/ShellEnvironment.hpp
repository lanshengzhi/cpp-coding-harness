#pragma once

#include <algorithm>
#include <cctype>
#include <map>
#include <string>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
extern char** environ;
#endif

namespace cch::harness {

/// Shared secret-name policy for local Shell launches: explicit provider
/// credential names plus the project heuristic set. Anything matched here is
/// withheld from child processes.
[[nodiscard]] inline bool secret_env_name(
    std::string name,
    const std::vector<std::string>& explicit_secret_names = {}) {
    for (const auto& explicit_name : explicit_secret_names) {
        if (name == explicit_name) {
            return true;
        }
    }
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    return name.find("API_KEY") != std::string::npos ||
           name.find("TOKEN") != std::string::npos ||
           name.find("SECRET") != std::string::npos ||
           name.find("PASSWORD") != std::string::npos ||
           name.find("CREDENTIAL") != std::string::npos ||
           name.find("PRIVATE_KEY") != std::string::npos ||
           name.find("AUTH") != std::string::npos ||
           name.find("JWT") != std::string::npos ||
           name.find("CERTIFICATE") != std::string::npos ||
           name.find("PASSPHRASE") != std::string::npos ||
           name.find("OPENAI") != std::string::npos;
}

/// The project's filtered process environment (an intentional security
/// divergence from pi's unfiltered environment): the ambient environment with
/// every secret-named variable removed.
[[nodiscard]] inline std::map<std::string, std::string> sanitized_environment(
    const std::vector<std::string>& explicit_secret_names = {}) {
    std::map<std::string, std::string> env;
#if defined(__unix__) || defined(__APPLE__)
    for (char** current = environ; current != nullptr && *current != nullptr; ++current) {
        std::string entry(*current);
        auto split = entry.find('=');
        if (split == std::string::npos) {
            continue;
        }
        auto key = entry.substr(0, split);
        if (!secret_env_name(key, explicit_secret_names)) {
            env[key] = entry.substr(split + 1);
        }
    }
#else
    (void)explicit_secret_names;
#endif
    return env;
}

} // namespace cch::harness
