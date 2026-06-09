#pragma once

#include <regex>
#include <string>

namespace cch::util {

inline std::string redact_text(std::string text) {
    const std::regex openai_key(R"(sk-[A-Za-z0-9_-]{8,})");
    text = std::regex_replace(text, openai_key, "[REDACTED]");

    const std::regex aws_key(R"(AKIA[0-9A-Z]{16})");
    text = std::regex_replace(text, aws_key, "[REDACTED]");

    const std::regex assignment(R"(((?:api[_-]?key|token|secret|password)\s*[:=]\s*)([^\s]+))", std::regex_constants::icase);
    text = std::regex_replace(text, assignment, "$1[REDACTED]");
    return text;
}

} // namespace cch::util
