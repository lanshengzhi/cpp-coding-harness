#include "SessionPathPolicy.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <random>
#include <sstream>
#include <utility>

namespace cch::coding_agent::session_paths {
namespace {

[[nodiscard]] std::tm utc_time(std::time_t value) {
    std::tm result{};
#if defined(_WIN32)
    gmtime_s(&result, &value);
#else
    gmtime_r(&value, &result);
#endif
    return result;
}

[[nodiscard]] std::string make_uuid_v4() {
    std::array<std::uint8_t, 16> bytes{};
    std::random_device random;
    std::uniform_int_distribution<unsigned int> byte_distribution(0, 255);
    for (auto& byte : bytes) {
        byte = static_cast<std::uint8_t>(byte_distribution(random));
    }

    bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0fU) | 0x40U);
    bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3fU) | 0x80U);

    std::ostringstream output;
    output << std::hex << std::nouppercase << std::setfill('0');
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) {
            output << '-';
        }
        output << std::setw(2) << static_cast<unsigned int>(bytes[index]);
    }
    return output.str();
}

[[nodiscard]] std::string make_utc_timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto since_epoch = now.time_since_epoch();
    const auto whole_seconds = std::chrono::floor<std::chrono::seconds>(since_epoch);
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(since_epoch - whole_seconds);
    const auto time = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::time_point{whole_seconds});
    const auto utc = utc_time(time);

    std::ostringstream output;
    output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S")
           << '.' << std::setw(3) << std::setfill('0') << milliseconds.count() << 'Z';
    return output.str();
}

} // namespace

std::string encode_workspace_key(const std::filesystem::path& resolved_workspace) {
    auto encoded = resolved_workspace.string();
    if (!encoded.empty() && (encoded.front() == '/' || encoded.front() == '\\')) {
        encoded.erase(encoded.begin());
    }
    for (auto& character : encoded) {
        if (character == '/' || character == '\\' || character == ':') {
            character = '-';
        }
    }
    return "--" + encoded + "--";
}

AutomaticSessionIdentity generate_automatic_session_identity() {
    return AutomaticSessionIdentity{
        .session_id = make_uuid_v4(),
        .created_at = make_utc_timestamp(),
    };
}

std::string automatic_session_filename(const AutomaticSessionIdentity& identity) {
    auto file_timestamp = identity.created_at;
    for (auto& character : file_timestamp) {
        if (character == ':' || character == '.') {
            character = '-';
        }
    }
    return file_timestamp + '_' + identity.session_id + ".jsonl";
}

AutomaticSessionTarget make_automatic_session_target(
    std::filesystem::path sessions_root,
    std::filesystem::path resolved_workspace,
    AutomaticSessionIdentity identity) {
    AutomaticSessionTarget target{
        .sessions_root = std::move(sessions_root),
        .workspace = std::move(resolved_workspace),
        .workspace_directory = {},
        .session_path = {},
        .identity = std::move(identity),
    };
    if (target.sessions_root.empty() || !target.sessions_root.is_absolute()) {
        return target;
    }
    target.workspace_directory = target.sessions_root / encode_workspace_key(target.workspace);
    target.session_path = target.workspace_directory / automatic_session_filename(target.identity);
    return target;
}

} // namespace cch::coding_agent::session_paths
