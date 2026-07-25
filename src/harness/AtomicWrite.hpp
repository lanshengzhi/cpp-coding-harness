#pragma once

#include "../../include/cch/util/Error.hpp"
#include "PosixWrite.hpp"
#include "UniqueFd.hpp"

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>

#if defined(__unix__) || defined(__APPLE__)
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace cch::harness {

inline std::filesystem::path atomic_temp_path(const std::filesystem::path& target, int suffix) {
    return target.parent_path() / ("." + target.filename().string() + ".tmp-" + std::to_string(suffix));
}

inline util::Error write_error(std::string message) {
    return util::make_error(util::ErrorCode::Workspace, message, message);
}

inline util::ExpectedVoid write_atomic_file(const std::filesystem::path& target, const std::string& content) {
    std::error_code ec;
    std::filesystem::path temp;
#if defined(__unix__) || defined(__APPLE__)
    mode_t mode = S_IRUSR | S_IWUSR;
    struct stat existing_status {};
    if (::lstat(target.c_str(), &existing_status) == 0) {
        mode = existing_status.st_mode & 0777;
    }
#endif

    for (int suffix = 0; suffix < 100; ++suffix) {
        auto candidate = atomic_temp_path(target, suffix);
        auto status = std::filesystem::symlink_status(candidate, ec);
        if (!ec && std::filesystem::is_symlink(status)) {
            continue;
        }
        if (!ec && std::filesystem::exists(status)) {
            continue;
        }
        if (ec && status.type() != std::filesystem::file_type::not_found) {
            return std::unexpected(write_error("could not inspect temporary file: " + ec.message()));
        }
        temp = candidate;
        break;
    }
    if (temp.empty()) {
        return std::unexpected(write_error("could not allocate temporary file for atomic write"));
    }

#if defined(__unix__) || defined(__APPLE__)
    auto parent = target.parent_path();
    if (parent.empty()) {
        parent = ".";
    }
    int dir_flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC;
#ifdef O_NOFOLLOW
    dir_flags |= O_NOFOLLOW;
#endif
    const UniqueFd dir_fd(::open(parent.c_str(), dir_flags));
    if (!dir_fd) {
        return std::unexpected(write_error("could not open target parent directory: " + std::string(std::strerror(errno))));
    }

    auto temp_filename = temp.filename().string();
    int flags = O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    UniqueFd file_fd(::openat(dir_fd.get(), temp_filename.c_str(), flags, mode));
    if (!file_fd) {
        return std::unexpected(write_error("could not create temporary file: " + std::string(std::strerror(errno))));
    }
    if (auto persisted = write_all_fsync(file_fd.get(), content); !persisted) {
        const auto message = std::string(std::strerror(persisted.error().error_number));
        ::unlinkat(dir_fd.get(), temp_filename.c_str(), 0);
        if (persisted.error().kind == PosixWriteErrorKind::Write) {
            return std::unexpected(write_error("could not write temporary file: " + message));
        }
        return std::unexpected(write_error("could not flush temporary file: " + message));
    }
    if (file_fd.close() != 0) {
        const auto message = std::string(std::strerror(errno));
        ::unlinkat(dir_fd.get(), temp_filename.c_str(), 0);
        return std::unexpected(write_error("could not close temporary file: " + message));
    }

    auto target_filename = target.filename().string();
    if (::renameat(dir_fd.get(), temp_filename.c_str(), dir_fd.get(), target_filename.c_str()) != 0) {
        const auto message = std::string(std::strerror(errno));
        ::unlinkat(dir_fd.get(), temp_filename.c_str(), 0);
        return std::unexpected(write_error("could not replace target atomically: " + message));
    }
#else
    std::ofstream output(temp, std::ios::binary | std::ios::trunc);
    if (!output) {
        return std::unexpected(write_error("could not open temporary file for writing"));
    }
    output << content;
    output.flush();
    output.close();
    if (!output) {
        std::filesystem::remove(temp, ec);
        return std::unexpected(write_error("could not write temporary file"));
    }

    std::filesystem::rename(temp, target, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        return std::unexpected(write_error("could not replace target atomically: " + ec.message()));
    }
#endif
    return {};
}

} // namespace cch::harness
