#pragma once

#include "support/Json.hpp"

#include <cch/support/Error.hpp>

#include <array>
#include <cstdio>
#include <cstdlib>
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

/// Read from the registered pi-v1.3.0 audit evidence family (pi `tui.ts` at
/// the pi-v1.3.0 baseline). The historical `pi-tui` registry above pins the
/// pi-v1.0.4 revision, whose components drew the fake cursor as raw
/// reverse-video bytes; the cursor-marker contract landed in pi-v1.3.0, so
/// the marker observations live in the audit families. The offline reader
/// returns the bytes it verified, not a path to reopen.
[[nodiscard]] inline support::Expected<support::JsonValue> read_pi_v130_audit_evidence(
        std::string_view family, std::string_view artifact) {
    const auto runner = std::filesystem::path{CCH_SOURCE_DIR} / "scripts/evidence/audit_evidence.py";
    const std::string command = quote_evidence_argument(CCH_PYTHON3) + " " + quote_evidence_argument(runner.string()) +
                                " read --family " + quote_evidence_argument(family) + " --artifact " +
                                quote_evidence_argument(artifact);
    std::FILE* pipe = ::popen(command.c_str(), "r");
    if (!pipe) {
        return std::unexpected(
                support::make_error(support::ErrorCode::Unknown, "Failed to start the audit evidence reader"));
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
                "Named pi-v1.3.0 audit evidence verification failed: " + std::string(family) + "/" +
                        std::string(artifact)));
    }
    return support::read_json(payload);
}

/// Read from a captured but not yet registered pi-v1.3.0 audit family (a
/// lane's own bundle before the serial merger applies the `register`
/// patch). The shared verifier's `check` validates the candidate manifest
/// and every pinned artifact byte (manifest digest, generator digest,
/// artifact SHA-256, envelope baseline/revision binding) fail-closed; only
/// after that verification succeeds does the reader return the artifact
/// bytes from the verified path. `verify`/`read` stay reserved for
/// registered families.
[[nodiscard]] inline support::Expected<support::JsonValue> read_pi_v130_audit_candidate(
        std::string_view family, std::string_view artifact) {
    const auto runner = std::filesystem::path{CCH_SOURCE_DIR} / "scripts/evidence/audit_evidence.py";
    const std::filesystem::path manifest =
            std::filesystem::path{CCH_SOURCE_DIR} / "fixtures/pi-v1.3.0" / family / "manifest.json";
    const std::string check_command = quote_evidence_argument(CCH_PYTHON3) + " " +
                                      quote_evidence_argument(runner.string()) + " check --manifest " +
                                      quote_evidence_argument(manifest.string());
    const int check_status = std::system(check_command.c_str());
    if (check_status != 0) {
        return std::unexpected(support::make_error(support::ErrorCode::Unknown,
                "Named pi-v1.3.0 audit candidate verification failed: " + std::string(family) + "/" +
                        std::string(artifact)));
    }
    const std::filesystem::path path = std::filesystem::path{CCH_SOURCE_DIR} / "fixtures/pi-v1.3.0" / family / artifact;
    std::FILE* input = std::fopen(path.string().c_str(), "rb");
    if (!input) {
        return std::unexpected(support::make_error(
                support::ErrorCode::Unknown, "audit candidate artifact is missing: " + path.string()));
    }
    std::string payload;
    std::array<char, 4096> buffer{};
    while (const auto count = std::fread(buffer.data(), 1, buffer.size(), input)) {
        payload.append(buffer.data(), count);
    }
    const bool read_failed = std::ferror(input) != 0;
    std::fclose(input);
    if (read_failed) {
        return std::unexpected(support::make_error(
                support::ErrorCode::Unknown, "audit candidate artifact could not be read: " + path.string()));
    }
    return support::read_json(payload);
}

} // namespace cch::tests
