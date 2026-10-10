#pragma once

#include "support/Json.hpp"

#include <cch/support/Error.hpp>

#include <array>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>

namespace cch::tests {

[[nodiscard]] inline std::string quote_evidence_argument(std::string_view value) {
    std::string result{"'"};
    for (const char character : value) {
        if (character == '\'')
            result += "'\\''";
        else
            result += character;
    }
    result += '\'';
    return result;
}

/// Read from the strict named bundle. Historical readers are deliberately separate.
/// The offline reader returns the bytes it verified, not a path to reopen. No pi
/// checkout or capture dependency is involved in C++ replay. Python is the same
/// configure-selected system tool already required by the architecture gate.
[[nodiscard]] inline support::Expected<support::JsonValue> read_pi_tui_evidence(
        std::string_view artifact, std::string_view baseline = "pi-v1.0.4") {
    const auto runner = std::filesystem::path{CCH_SOURCE_DIR} / "scripts/tui/evidence.py";
    const std::string command = quote_evidence_argument(CCH_PYTHON3) + " " + quote_evidence_argument(runner.string()) +
                                " read --baseline " + quote_evidence_argument(baseline) + " --artifact " +
                                quote_evidence_argument(artifact);
    std::FILE* pipe = ::popen(command.c_str(), "r");
    if (!pipe) {
        return std::unexpected(
                support::make_error(support::ErrorCode::Unknown, "Failed to start named TUI evidence reader"));
    }
    std::string payload;
    std::array<char, 4096> buffer{};
    while (const auto count = std::fread(buffer.data(), 1, buffer.size(), pipe)) {
        payload.append(buffer.data(), count);
    }
    const bool read_failed = std::ferror(pipe) != 0;
    const int status = ::pclose(pipe);
    if (read_failed || status != 0) {
        return std::unexpected(support::make_error(support::ErrorCode::Unknown,
                "Named TUI evidence verification failed: " + std::string(baseline) + "/" + std::string(artifact)));
    }
    return support::read_json(payload);
}

} // namespace cch::tests
