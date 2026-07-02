#include "SessionJournal.hpp"

#include <array>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>
#include <system_error>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace cch::harness::session {
namespace {

[[nodiscard]] util::Error session_error(std::string message, std::string detail = {}) {
    return util::make_error(util::ErrorCode::Session, std::move(message), std::move(detail));
}

[[nodiscard]] bool parent_path_contains_symlink(const std::filesystem::path& path) {
    auto parent = path.parent_path();
    if (parent.empty()) {
        return false;
    }
    std::error_code ec;
    std::filesystem::path cursor;
    for (const auto& part : parent) {
        if (part == "/" || part == "." || part.empty()) {
            continue;
        }
        if (part == "..") {
            return true;
        }
        cursor /= part;
        auto status = std::filesystem::symlink_status(cursor, ec);
        if (ec) {
            return status.type() != std::filesystem::file_type::not_found;
        }
        if (std::filesystem::is_symlink(status)) {
            return true;
        }
    }
    return false;
}

#if defined(__unix__) || defined(__APPLE__)
[[nodiscard]] int open_session_path(const std::filesystem::path& path, int flags, int mode = 0) {
    int final_flags = flags;
#ifdef O_NOFOLLOW
    final_flags |= O_NOFOLLOW;
#endif
    if (mode != 0) {
        return ::open(path.c_str(), final_flags, mode);
    }
    return ::open(path.c_str(), final_flags);
}

[[nodiscard]] util::Expected<std::string> read_file_contents(int fd) {
    std::string contents;
    std::array<char, 8192> buffer{};
    for (;;) {
        ssize_t n = ::read(fd, buffer.data(), buffer.size());
        if (n < 0) {
            return std::unexpected(session_error("could not read session file", std::strerror(errno)));
        }
        if (n == 0) {
            break;
        }
        contents.append(buffer.data(), static_cast<std::size_t>(n));
    }
    return contents;
}

[[nodiscard]] util::ExpectedVoid set_fd_private_permissions(int fd) {
    struct stat st {};
    if (::fstat(fd, &st) != 0) {
        return std::unexpected(session_error("could not inspect session permissions", std::strerror(errno)));
    }
    if ((st.st_mode & (S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH)) != 0) {
        return std::unexpected(session_error(
            "session file is readable by group/others", "refusing to load sensitive transcript"));
    }
    if (::fchmod(fd, S_IRUSR | S_IWUSR) != 0) {
        return std::unexpected(session_error("could not set owner-only session permissions", std::strerror(errno)));
    }
    return {};
}
#endif

[[maybe_unused]] bool has_public_read(std::filesystem::perms mode) {
    using std::filesystem::perms;
    return (mode & perms::others_read) != perms::none || (mode & perms::group_read) != perms::none;
}

} // namespace

util::Expected<SessionJournal> SessionJournal::create_new(
    const std::filesystem::path& path, std::string_view header_line) {
    auto validation = validate_session_path_for_open(path, false);
    if (!validation) {
        return std::unexpected(validation.error());
    }

    std::error_code ec;
    if (!path.parent_path().empty()) {
        if (parent_path_contains_symlink(path)) {
            return std::unexpected(session_error(
                "session parent path contains a symlink",
                "refusing to create session directory through a symlink"));
        }
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) {
            return std::unexpected(session_error("could not create session directory", ec.message()));
        }
    }

    if (std::filesystem::exists(path, ec)) {
        return std::unexpected(session_error("session file already exists", "use --resume to append"));
    }

    auto content = std::string{header_line} + '\n';
    if (auto written = write_new_file_exclusive(path, content); !written) {
        return std::unexpected(written.error());
    }

    if (auto perms = ensure_private_permissions(path, false); !perms) {
        return std::unexpected(perms.error());
    }

    SessionJournal journal;
    journal.path_ = path;
    return journal;
}

util::Expected<SessionJournal> SessionJournal::open_existing(const std::filesystem::path& path) {
    auto validation = validate_session_path_for_open(path, true);
    if (!validation) {
        return std::unexpected(validation.error());
    }

    if (auto perms = ensure_private_permissions(path, true); !perms) {
        return std::unexpected(perms.error());
    }

    SessionJournal journal;
    journal.path_ = path;
    return journal;
}

util::ExpectedVoid SessionJournal::append_line(std::string_view line) const {
#if defined(__unix__) || defined(__APPLE__)
    int flags = O_WRONLY | O_APPEND | O_CREAT;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int fd = ::open(path_.c_str(), flags, S_IRUSR | S_IWUSR);
    if (fd == -1) {
        return std::unexpected(session_error("could not append to session file", std::strerror(errno)));
    }
    auto fd_guard = std::unique_ptr<int, void (*)(int*)>(
        new int(fd), [](int* p) { if (p && *p != -1) ::close(*p); delete p; });

    const char* data = line.data();
    std::size_t remaining = line.size();
    while (remaining > 0) {
        ssize_t written = ::write(fd, data, remaining);
        if (written < 0) {
            const auto message = std::string(std::strerror(errno));
            return std::unexpected(session_error("could not write session entry", message));
        }
        data += written;
        remaining -= static_cast<std::size_t>(written);
    }

    if (::fsync(fd) != 0) {
        const auto message = std::string(std::strerror(errno));
        return std::unexpected(session_error("could not persist session entry", message));
    }
    if (::close(fd) != 0) {
        return std::unexpected(session_error("could not close session file", std::strerror(errno)));
    }
    fd_guard.release();
    return {};
#else
    std::ofstream output(path_, std::ios::binary | std::ios::app);
    if (!output) {
        return std::unexpected(session_error("could not append to session file"));
    }
    output.write(line.data(), static_cast<std::streamsize>(line.size()));
    output.flush();
    output.close();
    if (!output) {
        return std::unexpected(session_error("could not persist session entry"));
    }
    return {};
#endif
}

util::Expected<std::vector<std::string>> SessionJournal::read_lines() const {
#if defined(__unix__) || defined(__APPLE__)
    int fd = open_session_path(path_, O_RDONLY);
    if (fd == -1) {
        return std::unexpected(session_error("could not open session file", std::strerror(errno)));
    }
    auto fd_guard = std::unique_ptr<int, void (*)(int*)>(
        new int(fd), [](int* p) { if (p && *p != -1) ::close(*p); delete p; });

    auto contents = read_file_contents(fd);
    if (!contents) {
        return std::unexpected(contents.error());
    }
#else
    if (auto perms = ensure_private_permissions(path_, true); !perms) {
        return std::unexpected(perms.error());
    }
    std::ifstream input(path_, std::ios::binary);
    if (!input) {
        return std::unexpected(session_error("could not open session file"));
    }
    std::string contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
#endif

    std::vector<std::string> lines;
    std::string line;
    std::istringstream stream(*contents);
    while (std::getline(stream, line)) {
        lines.push_back(std::move(line));
    }

#if defined(__unix__) || defined(__APPLE__)
    // read_file_contents returns full content; stream parsing errors are caught above.
#else
    if (input.bad()) {
        return std::unexpected(session_error("could not read complete session file"));
    }
#endif

    return lines;
}

util::ExpectedVoid SessionJournal::validate_session_path_for_open(
    const std::filesystem::path& path, bool must_exist) {
    std::error_code ec;
    if (path.empty()) {
        return std::unexpected(session_error("session path is required"));
    }
    if (must_exist && !std::filesystem::exists(path, ec)) {
        return std::unexpected(session_error("session file does not exist"));
    }
    if (std::filesystem::exists(path, ec) &&
        std::filesystem::is_symlink(std::filesystem::symlink_status(path, ec))) {
        return std::unexpected(session_error("refusing to follow symlink session path"));
    }
    return {};
}

util::ExpectedVoid SessionJournal::ensure_private_permissions(
    const std::filesystem::path& path, bool existing) {
#if defined(__unix__) || defined(__APPLE__)
    int fd = open_session_path(path, O_RDONLY);
    if (fd == -1) {
        return std::unexpected(session_error(
            "could not open session file for permission check", std::strerror(errno)));
    }
    auto fd_guard = std::unique_ptr<int, void (*)(int*)>(
        new int(fd), [](int* p) { if (p && *p != -1) ::close(*p); delete p; });

    if (auto perms = set_fd_private_permissions(fd); !perms) {
        if (!existing) {
            auto detail = perms.error().detail.empty()
                ? "could not set owner-only session permissions"
                : perms.error().detail;
            return std::unexpected(session_error("could not set owner-only session permissions", detail));
        }
        return std::unexpected(perms.error());
    }
    return {};
#else
    std::error_code ec;
    auto status = std::filesystem::status(path, ec);
    if (ec) {
        return std::unexpected(session_error("could not inspect session permissions", ec.message()));
    }
    if (existing && has_public_read(status.permissions())) {
        return std::unexpected(session_error(
            "session file is readable by group/others", "refusing to load sensitive transcript"));
    }
    (void)existing;
    return {};
#endif
}

util::ExpectedVoid SessionJournal::write_new_file_exclusive(
    const std::filesystem::path& path, std::string_view content) {
#if defined(__unix__) || defined(__APPLE__)
    int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int fd = ::open(path.c_str(), flags, S_IRUSR | S_IWUSR);
    if (fd == -1) {
        return std::unexpected(session_error("could not create session file", std::strerror(errno)));
    }

    const char* data = content.data();
    std::size_t remaining = content.size();
    while (remaining > 0) {
        ssize_t written = ::write(fd, data, remaining);
        if (written < 0) {
            const auto message = std::string(std::strerror(errno));
            ::close(fd);
            std::error_code ec;
            std::filesystem::remove(path, ec);
            return std::unexpected(session_error("could not write session header", message));
        }
        data += written;
        remaining -= static_cast<std::size_t>(written);
    }
    if (::fsync(fd) != 0) {
        const auto message = std::string(std::strerror(errno));
        ::close(fd);
        std::error_code ec;
        std::filesystem::remove(path, ec);
        return std::unexpected(session_error("could not flush session header", message));
    }
    if (::close(fd) != 0) {
        const auto message = std::string(std::strerror(errno));
        std::error_code ec;
        std::filesystem::remove(path, ec);
        return std::unexpected(session_error("could not close session file", message));
    }
    return {};
#else
    std::error_code ec;
    if (std::filesystem::exists(path, ec) ||
        std::filesystem::is_symlink(std::filesystem::symlink_status(path, ec))) {
        return std::unexpected(session_error("session file already exists", "use resume to append"));
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return std::unexpected(session_error("could not create session file"));
    }
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    output.flush();
    output.close();
    if (!output) {
        return std::unexpected(session_error("could not write session header"));
    }
    return {};
#endif
}

} // namespace cch::harness::session
