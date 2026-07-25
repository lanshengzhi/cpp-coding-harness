#include "WorkspaceFileSystem.hpp"

#include <cerrno>
#include <fstream>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/stat.h>
#endif

namespace cch::harness {

std::expected<std::string, FileError> WorkspaceFileSystem::createTempDir(
    std::optional<std::string> prefix) const {
    auto tmp_area = root_ / ".cch-tmp";
    std::error_code ec;
    std::filesystem::create_directories(tmp_area, ec);

    std::string pfx = prefix.value_or("tmp-");
    for (int i = 0; i < 1000; ++i) {
        auto candidate = tmp_area / (pfx + std::to_string(i));
        auto status = std::filesystem::symlink_status(candidate, ec);
        if (!ec && std::filesystem::exists(status)) {
            continue;
        }
        if (::mkdir(candidate.c_str(), 0700) == 0) {
            return candidate.string();
        }
        if (errno == EEXIST) {
            continue;
        }
        break;
    }
    return std::unexpected(FileError{
        FileErrorCode::Unknown,
        "could not create temp directory",
        std::nullopt});
}

std::expected<std::string, FileError> WorkspaceFileSystem::createTempFile(
    std::optional<std::string> prefix,
    std::optional<std::string> suffix) const {
    auto tmp_area = root_ / ".cch-tmp";
    std::error_code ec;
    std::filesystem::create_directories(tmp_area, ec);

    std::string pfx = prefix.value_or("");
    std::string sfx = suffix.value_or("");
    for (int i = 0; i < 1000; ++i) {
        auto candidate = tmp_area / (pfx + std::to_string(i) + sfx);
        auto status = std::filesystem::symlink_status(candidate, ec);
        if (!ec && std::filesystem::exists(status)) {
            continue;
        }
        std::ofstream out(candidate, std::ios::binary);
        if (out) {
            out.close();
            return candidate.string();
        }
        break;
    }
    return std::unexpected(FileError{
        FileErrorCode::Unknown,
        "could not create temp file",
        std::nullopt});
}

} // namespace cch::harness
