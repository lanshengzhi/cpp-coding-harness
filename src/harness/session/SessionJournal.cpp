#include "SessionJournal.hpp"
#include "SessionJournalTestHooks.hpp"

#include "harness/PosixWrite.hpp"
#include "support/UniqueFd.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <mutex>
#include <optional>
#include <sstream>
#include <system_error>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace cch::harness::session {
namespace {

[[nodiscard]] support::Error session_error(std::string message, std::string detail = {}) {
    return support::make_error(support::ErrorCode::Session, std::move(message), std::move(detail));
}

[[nodiscard]] support::Expected<support::UniqueFd> open_parent_directory(
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
    support::UniqueFd current(::open(start.c_str(), open_flags));
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
        support::UniqueFd next(::openat(current.get(), component.c_str(), open_flags));
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

[[nodiscard]] support::Expected<support::UniqueFd> open_session_path(
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
    support::UniqueFd descriptor(::openat(parent->get(), path.filename().c_str(), final_flags, mode));
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

[[nodiscard]] support::Expected<std::string> read_file_contents(int fd) {
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

[[nodiscard]] support::ExpectedVoid set_fd_private_permissions(int fd) {
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

support::Expected<SessionJournal> SessionJournal::create_new(
    const std::filesystem::path& path, std::string_view header_line) {
    auto validation = validate_session_path_for_open(path, false);
    if (!validation) {
        return std::unexpected(validation.error());
    }

    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
        return std::unexpected(session_error("session file already exists", "use --resume to append"));
    }

    auto content = std::string{header_line} + '\n';
    if (auto written = write_new_file_exclusive(path, content); !written) {
        return std::unexpected(written.error());
    }

    if (auto perms = ensure_private_permissions(path, false); !perms) {
        remove_session_file(path);
        return std::unexpected(perms.error());
    }

    SessionJournal journal;
    journal.path_ = path;
    return journal;
}

support::Expected<SessionJournal> SessionJournal::open_existing(const std::filesystem::path& path) {
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

support::ExpectedVoid SessionJournal::append_line(std::string_view line) const {
    apply_append_test_hooks(path_);
    if (consume_injected_append_failure(path_)) {
        return std::unexpected(session_error(
            "could not persist session entry", "injected append failure"));
    }
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
}

support::Expected<std::vector<std::string>> SessionJournal::read_lines() const {
    auto opened = open_session_path(path_, O_RDONLY);
    if (!opened) {
        return std::unexpected(opened.error());
    }

    auto read_contents = read_file_contents(opened->get());
    if (!read_contents) {
        return std::unexpected(read_contents.error());
    }
    std::string contents = std::move(*read_contents);

    std::vector<std::string> lines;
    std::string line;
    std::istringstream stream(contents);
    while (std::getline(stream, line)) {
        lines.push_back(std::move(line));
    }

    return lines;
}

support::ExpectedVoid SessionJournal::validate_session_path_for_open(
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

support::ExpectedVoid SessionJournal::ensure_private_permissions(
    const std::filesystem::path& path, bool existing) {
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
}

support::ExpectedVoid SessionJournal::write_new_file_exclusive(
    const std::filesystem::path& path, std::string_view content) {
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
}

} // namespace cch::harness::session
