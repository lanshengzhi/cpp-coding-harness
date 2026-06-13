#pragma once

#include <algorithm>
#include <cctype>
#include <regex>
#include <string>

namespace cch::util {

inline bool looks_secret_key(std::string key) {
    std::string normalized;
    normalized.reserve(key.size());
    for (unsigned char ch : key) {
        if (std::isalnum(ch)) {
            normalized.push_back(static_cast<char>(std::toupper(ch)));
        }
    }
    return normalized.find("APIKEY") != std::string::npos || normalized.find("TOKEN") != std::string::npos ||
           normalized.find("SECRET") != std::string::npos || normalized.find("PASSWORD") != std::string::npos ||
           normalized.find("OPENAI") != std::string::npos;
}

inline std::string redact_text(std::string text) {
    const std::regex openai_key(R"(sk-[A-Za-z0-9_-]{8,})");
    text = std::regex_replace(text, openai_key, "[REDACTED]");

    const std::regex aws_key(R"(AKIA[0-9A-Z]{16})");
    text = std::regex_replace(text, aws_key, "[REDACTED]");

    const std::regex quoted_assignment(R"redact(("[^"]*(?:api[_-]?key|token|secret|password|openai)[^"]*"\s*:\s*")([^"]*)("))redact", std::regex_constants::icase);
    text = std::regex_replace(text, quoted_assignment, "$1[REDACTED]$3");

    const std::regex assignment(R"(((?:[A-Za-z0-9_-]*(?:api[_-]?key|token|secret|password|openai)[A-Za-z0-9_-]*)\s*[:=]\s*)([^\s,}]+))", std::regex_constants::icase);
    text = std::regex_replace(text, assignment, "$1[REDACTED]");
    return text;
}

inline std::string redact_json_text(const std::string& text) {
    return redact_text(text);
}

} // namespace cch::util
