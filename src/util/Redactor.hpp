#pragma once

#include <boost/json.hpp>

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

inline boost::json::value redact_json_value(const boost::json::value& value);

inline boost::json::object redact_json_object(const boost::json::object& object) {
    boost::json::object redacted;
    for (const auto& [key, child] : object) {
        const std::string key_string(key);
        if (looks_secret_key(key_string)) {
            redacted[key_string] = "[REDACTED]";
        } else {
            redacted[key_string] = redact_json_value(child);
        }
    }
    return redacted;
}

inline boost::json::array redact_json_array(const boost::json::array& array) {
    boost::json::array redacted;
    for (const auto& child : array) {
        redacted.push_back(redact_json_value(child));
    }
    return redacted;
}

inline boost::json::value redact_json_value(const boost::json::value& value) {
    if (value.is_object()) {
        return redact_json_object(value.as_object());
    }
    if (value.is_array()) {
        return redact_json_array(value.as_array());
    }
    if (value.is_string()) {
        return boost::json::value(redact_text(std::string(value.as_string())));
    }
    return value;
}

inline std::string redact_json_text(const std::string& text) {
    boost::system::error_code ec;
    auto parsed = boost::json::parse(text, ec);
    if (ec) {
        return redact_text(text);
    }
    return boost::json::serialize(redact_json_value(parsed));
}

} // namespace cch::util
