#include "SessionJournal.hpp"
#include "SessionJournalTestHooks.hpp"

#include "harness/PosixWrite.hpp"
#include "util/UniqueFd.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <system_error>
#include <thread>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace cch::harness::session {
namespace {

[[nodiscard]] util::Error session_error(std::string message, std::string detail = {}) {
    return util::make_error(util::ErrorCode::Session, std::move(message), std::move(detail));
}

#if defined(__unix__) || defined(__APPLE__)
[[nodiscard]] util::Expected<util::UniqueFd> open_parent_directory(
    const std::filesystem::path& path,
    bool create_missing) {
    int open_flags = O_RDONLY | O_DIRECTORY;
#ifdef O_CLOEXEC
    open_flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    open_flags |= O_NOFOLLOW;
#endif

    const auto parent = path.parent_path().empty()
        ? std::filesystem::path{"."}
        : path.parent_path();
    const auto start = parent.is_absolute() ? parent.root_path() : std::filesystem::path{"."};
    util::UniqueFd current(::open(start.c_str(), open_flags));
    if (!current) {
        return std::unexpected(session_error(
            "could not open session directory root", std::strerror(errno)));
    }

    const auto relative = parent.is_absolute() ? parent.relative_path() : parent;
    for (const auto& component : relative) {
        if (component.empty() || component == ".") {
            continue;
        }
        if (component == "..") {
            return std::unexpected(session_error(
                "session parent path contains parent traversal",
                "refusing to open a session through '..'"));
        }
        if (create_missing &&
            ::mkdirat(current.get(), component.c_str(), S_IRWXU | S_IRWXG | S_IRWXO) != 0 &&
            errno != EEXIST) {
            const auto reason = std::string{std::strerror(errno)};
            return std::unexpected(session_error("could not create session directory", reason));
        }
        util::UniqueFd next(::openat(current.get(), component.c_str(), open_flags));
        if (!next) {
            const auto reason = std::string{std::strerror(errno)};
            return std::unexpected(session_error(
                "session parent path contains a symlink or non-directory",
                component.string() + ": " + reason));
        }
        current = std::move(next);
    }
    return current;
}

[[nodiscard]] util::Expected<util::UniqueFd> open_session_path(
    const std::filesystem::path& path,
    int flags,
    int mode = 0,
    bool create_parent_directories = false) {
    auto parent = open_parent_directory(path, create_parent_directories);
    if (!parent) {
        return std::unexpected(parent.error());
    }

    int final_flags = flags;
#ifdef O_CLOEXEC
    final_flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    final_flags |= O_NOFOLLOW;
#endif
    util::UniqueFd descriptor(::openat(parent->get(), path.filename().c_str(), final_flags, mode));
    if (!descriptor) {
        return std::unexpected(session_error("could not open session file", std::strerror(errno)));
    }
    return descriptor;
}

void remove_session_file(const std::filesystem::path& path) {
    auto parent = open_parent_directory(path, false);
    if (!parent) {
        return;
    }
    ::unlinkat(parent->get(), path.filename().c_str(), 0);
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
#elif defined(_WIN32)
[[nodiscard]] util::ExpectedVoid reject_windows_reparse_parents(
    const std::filesystem::path& path) {
    std::filesystem::path cursor = path.parent_path().root_path();
    for (const auto& component : path.parent_path().relative_path()) {
        if (component.empty() || component == ".") {
            continue;
        }
        if (component == "..") {
            return std::unexpected(session_error(
                "session parent path contains parent traversal"));
        }
        cursor /= component;
        const DWORD attributes = ::GetFileAttributesW(cursor.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            continue;
        }
        if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            return std::unexpected(session_error(
                "session parent path contains a symlink or junction",
                cursor.string()));
        }
    }
    return {};
}

[[nodiscard]] util::Expected<HANDLE> open_windows_session_file(
    const std::filesystem::path& path,
    DWORD access,
    DWORD creation,
    bool delete_on_unsafe = false) {
    if (auto safe = reject_windows_reparse_parents(path); !safe) {
        return std::unexpected(safe.error());
    }
    HANDLE file = ::CreateFileW(
        path.c_str(),
        access,
        FILE_SHARE_READ,
        nullptr,
        creation,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const auto code = ::GetLastError();
        if (creation == CREATE_NEW &&
            (code == ERROR_FILE_EXISTS || code == ERROR_ALREADY_EXISTS)) {
            return std::unexpected(session_error(
                "session file already exists", "use resume to append"));
        }
        return std::unexpected(session_error(
            "could not open session file",
            "Windows error " + std::to_string(code)));
    }

    FILE_ATTRIBUTE_TAG_INFO tag_info{};
    const bool final_is_reparse =
        !::GetFileInformationByHandleEx(file, FileAttributeTagInfo, &tag_info, sizeof(tag_info)) ||
        (tag_info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    auto parents_safe = reject_windows_reparse_parents(path);
    if (final_is_reparse || !parents_safe) {
        if (delete_on_unsafe) {
            FILE_DISPOSITION_INFO disposition{TRUE};
            ::SetFileInformationByHandle(
                file, FileDispositionInfo, &disposition, sizeof(disposition));
        }
        ::CloseHandle(file);
        return std::unexpected(session_error(
            "session path contains a symlink, junction, or reparse point",
            parents_safe ? path.string() : parents_safe.error().detail));
    }
    return file;
}
#endif

[[maybe_unused]] bool has_public_read(std::filesystem::perms mode) {
    using std::filesystem::perms;
    return (mode & perms::others_read) != perms::none || (mode & perms::group_read) != perms::none;
}

struct InjectedAppendFailure {
    std::filesystem::path path;
    std::size_t attempts_remaining{0};
};

std::mutex injected_append_failure_mutex;
std::optional<InjectedAppendFailure> injected_append_failure;

struct InjectedAppendDelay {
    std::filesystem::path path;
    std::chrono::milliseconds delay;
};

std::mutex injected_append_delay_mutex;
std::optional<InjectedAppendDelay> injected_append_delay;

struct RecordedAppendThreads {
    std::filesystem::path path;
    std::vector<std::thread::id> threads;
};

std::mutex recorded_append_threads_mutex;
std::optional<RecordedAppendThreads> recorded_append_threads;

/// Test-only append instrumentation (SessionJournalTestHooks.hpp): record
/// the executing thread, then simulate slow persistence. Runs from Runtime
/// worker threads, so all shared state is mutex-protected.
void apply_append_test_hooks(const std::filesystem::path& path) {
    {
        std::scoped_lock lock{recorded_append_threads_mutex};
        if (recorded_append_threads &&
            recorded_append_threads->path == path.lexically_normal()) {
            recorded_append_threads->threads.push_back(std::this_thread::get_id());
        }
    }
    std::optional<std::chrono::milliseconds> delay;
    {
        std::scoped_lock lock{injected_append_delay_mutex};
        if (injected_append_delay &&
            injected_append_delay->path == path.lexically_normal()) {
            delay = injected_append_delay->delay;
        }
    }
    if (delay) {
        std::this_thread::sleep_for(*delay);
    }
}

[[nodiscard]] bool consume_injected_append_failure(const std::filesystem::path& path) {
    std::scoped_lock lock{injected_append_failure_mutex};
    if (!injected_append_failure ||
        injected_append_failure->path != path.lexically_normal()) {
        return false;
    }
    if (--injected_append_failure->attempts_remaining != 0) {
        return false;
    }
    injected_append_failure.reset();
    return true;
}

} // namespace

namespace testing {

void fail_nth_append_for_test(const std::filesystem::path& path, std::size_t attempt) {
    std::scoped_lock lock{injected_append_failure_mutex};
    injected_append_failure = InjectedAppendFailure{
        .path = path.lexically_normal(),
        .attempts_remaining = attempt == 0 ? 1 : attempt,
    };
}

void delay_appends_for_test(
    const std::filesystem::path& path,
    std::chrono::milliseconds delay) {
    std::scoped_lock lock{injected_append_delay_mutex};
    injected_append_delay = InjectedAppendDelay{
        .path = path.lexically_normal(),
        .delay = delay,
    };
}

void clear_append_delay_for_test(const std::filesystem::path& path) {
    std::scoped_lock lock{injected_append_delay_mutex};
    if (injected_append_delay &&
        injected_append_delay->path == path.lexically_normal()) {
        injected_append_delay.reset();
    }
}

void record_append_threads_for_test(const std::filesystem::path& path) {
    std::scoped_lock lock{recorded_append_threads_mutex};
    recorded_append_threads = RecordedAppendThreads{
        .path = path.lexically_normal(),
        .threads = {},
    };
}

std::vector<std::thread::id> recorded_append_threads_for_test(
    const std::filesystem::path& path) {
    std::scoped_lock lock{recorded_append_threads_mutex};
    if (!recorded_append_threads ||
        recorded_append_threads->path != path.lexically_normal()) {
        return {};
    }
    auto threads = std::move(recorded_append_threads->threads);
    recorded_append_threads.reset();
    return threads;
}

} // namespace testing

util::Expected<SessionJournal> SessionJournal::create_new(
    const std::filesystem::path& path, std::string_view header_line) {
    auto validation = validate_session_path_for_open(path, false);
    if (!validation) {
        return std::unexpected(validation.error());
    }

    std::error_code ec;
#if defined(_WIN32)
    if (auto safe = reject_windows_reparse_parents(path); !safe) {
        return std::unexpected(safe.error());
    }
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) {
            return std::unexpected(session_error("could not create session directory", ec.message()));
        }
    }
    if (auto safe = reject_windows_reparse_parents(path); !safe) {
        return std::unexpected(safe.error());
    }
#elif !defined(__unix__) && !defined(__APPLE__)
    if (!path.parent_path().empty()) {
        std::filesystem::path cursor = path.parent_path().root_path();
        for (const auto& component : path.parent_path().relative_path()) {
            if (component.empty() || component == ".") {
                continue;
            }
            if (component == "..") {
                return std::unexpected(session_error(
                    "session parent path contains parent traversal"));
            }
            cursor /= component;
            const auto status = std::filesystem::symlink_status(cursor, ec);
            if (!ec && std::filesystem::is_symlink(status)) {
                return std::unexpected(session_error(
                    "session parent path contains a symlink",
                    "refusing to create session directory through a symlink"));
            }
            ec.clear();
        }
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) {
            return std::unexpected(session_error("could not create session directory", ec.message()));
        }
    }
#endif

    if (std::filesystem::exists(path, ec)) {
        return std::unexpected(session_error("session file already exists", "use --resume to append"));
    }

    auto content = std::string{header_line} + '\n';
    if (auto written = write_new_file_exclusive(path, content); !written) {
        return std::unexpected(written.error());
    }

    if (auto perms = ensure_private_permissions(path, false); !perms) {
#if defined(__unix__) || defined(__APPLE__)
        remove_session_file(path);
#else
        std::error_code remove_ec;
        std::filesystem::remove(path, remove_ec);
#endif
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
    apply_append_test_hooks(path_);
    if (consume_injected_append_failure(path_)) {
        return std::unexpected(session_error(
            "could not persist session entry", "injected append failure"));
    }
#if defined(__unix__) || defined(__APPLE__)
    auto opened = open_session_path(path_, O_WRONLY | O_APPEND);
    if (!opened) {
        return std::unexpected(session_error(
            "could not append to session file",
            opened.error().message + ": " + opened.error().detail));
    }

    if (auto persisted = write_all_fsync(opened->get(), line); !persisted) {
        const auto message = std::string(std::strerror(persisted.error().error_number));
        if (persisted.error().kind == PosixWriteErrorKind::Write) {
            return std::unexpected(session_error("could not write session entry", message));
        }
        return std::unexpected(session_error("could not persist session entry", message));
    }
    if (opened->close() != 0) {
        return std::unexpected(session_error("could not close session file", std::strerror(errno)));
    }
    return {};
#elif defined(_WIN32)
    auto opened = open_windows_session_file(path_, FILE_APPEND_DATA, OPEN_EXISTING);
    if (!opened) {
        return std::unexpected(session_error(
            "could not append to session file",
            opened.error().message + ": " + opened.error().detail));
    }
    HANDLE file = *opened;
    const char* data = line.data();
    std::size_t remaining = line.size();
    while (remaining > 0) {
        const auto chunk = static_cast<DWORD>(
            std::min<std::size_t>(remaining, static_cast<std::size_t>(MAXDWORD)));
        DWORD written = 0;
        if (!::WriteFile(file, data, chunk, &written, nullptr) || written == 0) {
            const auto code = ::GetLastError();
            ::CloseHandle(file);
            return std::unexpected(session_error(
                "could not write session entry",
                "Windows error " + std::to_string(code)));
        }
        data += written;
        remaining -= written;
    }
    if (!::FlushFileBuffers(file)) {
        const auto code = ::GetLastError();
        ::CloseHandle(file);
        return std::unexpected(session_error(
            "could not persist session entry",
            "Windows error " + std::to_string(code)));
    }
    if (!::CloseHandle(file)) {
        return std::unexpected(session_error(
            "could not close session file",
            "Windows error " + std::to_string(::GetLastError())));
    }
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
    auto opened = open_session_path(path_, O_RDONLY);
    if (!opened) {
        return std::unexpected(opened.error());
    }

    auto read_contents = read_file_contents(opened->get());
    if (!read_contents) {
        return std::unexpected(read_contents.error());
    }
    std::string contents = std::move(*read_contents);
#elif defined(_WIN32)
    auto opened = open_windows_session_file(path_, GENERIC_READ, OPEN_EXISTING);
    if (!opened) {
        return std::unexpected(opened.error());
    }
    HANDLE file = *opened;
    std::string contents;
    std::array<char, 8192> buffer{};
    for (;;) {
        DWORD read = 0;
        if (!::ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
            const auto code = ::GetLastError();
            ::CloseHandle(file);
            return std::unexpected(session_error(
                "could not read session file",
                "Windows error " + std::to_string(code)));
        }
        if (read == 0) {
            break;
        }
        contents.append(buffer.data(), read);
    }
    if (!::CloseHandle(file)) {
        return std::unexpected(session_error(
            "could not close session file",
            "Windows error " + std::to_string(::GetLastError())));
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
    std::istringstream stream(contents);
    while (std::getline(stream, line)) {
        lines.push_back(std::move(line));
    }

#if defined(__unix__) || defined(__APPLE__) || defined(_WIN32)
    // Platform reads return full content; stream parsing errors are caught above.
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
    auto opened = open_session_path(path, O_RDONLY);
    if (!opened) {
        return std::unexpected(session_error(
            "could not open session file for permission check",
            opened.error().message + ": " + opened.error().detail));
    }

    if (auto perms = set_fd_private_permissions(opened->get()); !perms) {
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
    auto opened = open_session_path(
        path,
        O_WRONLY | O_CREAT | O_EXCL,
        S_IRUSR | S_IWUSR,
        true);
    if (!opened) {
        return std::unexpected(session_error(
            "could not create session file",
            opened.error().message + ": " + opened.error().detail));
    }

    if (auto persisted = write_all_fsync(opened->get(), content); !persisted) {
        const auto message = std::string(std::strerror(persisted.error().error_number));
        remove_session_file(path);
        if (persisted.error().kind == PosixWriteErrorKind::Write) {
            return std::unexpected(session_error("could not write session header", message));
        }
        return std::unexpected(session_error("could not flush session header", message));
    }
    if (opened->close() != 0) {
        const auto message = std::string(std::strerror(errno));
        remove_session_file(path);
        return std::unexpected(session_error("could not close session file", message));
    }
    return {};
#elif defined(_WIN32)
    auto opened = open_windows_session_file(path, GENERIC_WRITE, CREATE_NEW, true);
    if (!opened) {
        return std::unexpected(opened.error());
    }
    HANDLE file = *opened;

    const char* data = content.data();
    std::size_t remaining = content.size();
    while (remaining > 0) {
        const auto chunk = static_cast<DWORD>(
            std::min<std::size_t>(remaining, static_cast<std::size_t>(MAXDWORD)));
        DWORD written = 0;
        if (!::WriteFile(file, data, chunk, &written, nullptr) || written == 0) {
            const auto code = ::GetLastError();
            ::CloseHandle(file);
            ::DeleteFileW(path.c_str());
            return std::unexpected(session_error(
                "could not write session header",
                "Windows error " + std::to_string(code)));
        }
        data += written;
        remaining -= written;
    }
    if (!::FlushFileBuffers(file)) {
        const auto code = ::GetLastError();
        ::CloseHandle(file);
        ::DeleteFileW(path.c_str());
        return std::unexpected(session_error(
            "could not flush session header",
            "Windows error " + std::to_string(code)));
    }
    if (!::CloseHandle(file)) {
        const auto code = ::GetLastError();
        ::DeleteFileW(path.c_str());
        return std::unexpected(session_error(
            "could not close session file",
            "Windows error " + std::to_string(code)));
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
