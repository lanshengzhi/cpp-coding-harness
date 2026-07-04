#pragma once

#include "../util/Error.hpp"

#include <map>
#include <optional>
#include <string>

namespace cch::coding_agent {

struct AuthEntry {
    std::string type;
    std::string key;
};

/// Loads user authentication entries from ~/.cpp-harness/agent/auth.json.
/// The file is expected to be a JSON object mapping provider names to objects
/// with "type" and "key" fields. Missing or unreadable files yield an empty map
/// rather than an error.
class AuthLoader {
public:
    [[nodiscard]] static util::Expected<std::map<std::string, AuthEntry>> load(const std::string& path);
    [[nodiscard]] static std::string default_path();
};

} // namespace cch::coding_agent
