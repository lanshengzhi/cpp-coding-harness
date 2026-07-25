#include "WorkspaceFileSystem.hpp"

#include "AtomicWrite.hpp"

#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>
#include <utility>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace cch::harness {

WorkspaceFileSystem::WorkspaceFileSystem(std::filesystem::path workspace)
    : root_(canonicalized(std::move(workspace))) {}

util::Expected<WorkspaceFileSystem> WorkspaceFileSystem::create(const std::filesystem::path& workspace) {
    std::error_code ec;
    if (!std::filesystem::exists(workspace, ec) || !std::filesystem::is_directory(workspace, ec)) {
        return std::unexpected(workspace_error("workspace does not exist or is not a directory"));
    }
    return WorkspaceFileSystem(workspace);
}

util::Expected<std::filesystem::path> WorkspaceFileSystem::resolve_addressed_path(
    const std::string& requested) const {
    if (requested.empty()) {
        return std::unexpected(workspace_error("path is required"));
    }
    std::filesystem::path relative(requested);
    if (relative.is_absolute()) {
        return std::unexpected(workspace_error("absolute paths are not allowed: " + requested));
    }
    auto normalized = relative.lexically_normal();
    for (const auto& part : normalized) {
        if (part == "..") {
            return std::unexpected(workspace_error("path escapes workspace: " + requested));
        }
    }
    auto target = (root_ / normalized).lexically_normal();
    if (!inside_lexically(target)) {
        return std::unexpected(workspace_error("path escapes workspace: " + requested));
    }
    return target;
}

util::Expected<std::string> WorkspaceFileSystem::read_existing_file(const std::string& requested) const {
    auto target = resolve_addressed_path(requested);
    if (!target) {
        return std::unexpected(target.error());
    }

#if defined(__unix__) || defined(__APPLE__)
    auto parent_guard = open_parent_directory(*target, false);
    if (!parent_guard) {
        return std::unexpected(parent_guard.error());
    }

    auto filename = target->filename().string();
    UniqueFd fd(::openat(parent_guard->get(), filename.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
    if (!fd) {
        if (errno == ELOOP) {
            return std::unexpected(workspace_error("refusing to read through symlink: " + requested));
        }
        return std::unexpected(workspace_error("could not open file for reading: " + requested));
    }

    struct stat st {};
    if (::fstat(fd.get(), &st) != 0 || !S_ISREG(st.st_mode)) {
        return std::unexpected(workspace_error("path is not a regular file: " + requested));
    }

    std::string content;
    char buffer[4096];
    ssize_t n = 0;
    while ((n = ::read(fd.get(), buffer, sizeof(buffer))) > 0) {
        content.append(buffer, static_cast<std::size_t>(n));
    }
    if (n < 0) {
        return std::unexpected(workspace_error("could not read file: " + requested));
    }
    return content;
#else
    std::error_code ec;
    auto canonical = std::filesystem::canonical(*target, ec);
    if (ec) {
        return std::unexpected(workspace_error("path does not exist inside workspace: " + requested));
    }
    if (!inside(canonical)) {
        return std::unexpected(workspace_error("path escapes workspace: " + requested));
    }
    if (!std::filesystem::is_regular_file(canonical, ec)) {
        return std::unexpected(workspace_error("path is not a regular file: " + requested));
    }
    std::ifstream input(canonical, std::ios::binary);
    if (!input) {
        return std::unexpected(workspace_error("could not open file for reading: " + requested));
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
#endif
}

util::Expected<std::size_t> WorkspaceFileSystem::write_file(
    const std::string& requested,
    const std::string& content,
    bool create_parents) const {
    auto target = resolve_addressed_path(requested);
    if (!target) {
        return std::unexpected(target.error());
    }

#if defined(__unix__) || defined(__APPLE__)
    auto parent = target->parent_path();
    if (parent.empty()) {
        parent = root_;
    }
    std::error_code ec;
    if (!std::filesystem::exists(parent, ec)) {
        if (!create_parents) {
            return std::unexpected(workspace_error("parent directory does not exist: " + requested));
        }
        auto created = create_parent_directories(*target);
        if (!created) {
            return std::unexpected(created.error());
        }
    }
    auto parent_guard = open_parent_directory(*target, create_parents);
    if (!parent_guard) {
        return std::unexpected(parent_guard.error());
    }
    auto target_status = std::filesystem::symlink_status(*target, ec);
    if (ec && target_status.type() != std::filesystem::file_type::not_found) {
        return std::unexpected(workspace_error("could not inspect target: " + requested));
    }
    if (std::filesystem::is_symlink(target_status)) {
        return std::unexpected(workspace_error("refusing to write through final symlink: " + requested));
    }
    if (std::filesystem::exists(target_status) && !std::filesystem::is_regular_file(target_status)) {
        return std::unexpected(workspace_error("target is not a regular file: " + requested));
    }
#else
    auto resolved = resolve_for_write(requested, create_parents);
    if (!resolved) {
        return std::unexpected(resolved.error());
    }
    target = *resolved;
#endif
    auto written = write_atomic_file(*target, content);
    if (!written) {
        return std::unexpected(written.error());
    }
    return content.size();
}

util::Error WorkspaceFileSystem::workspace_error(std::string message) {
    return util::make_error(util::ErrorCode::Workspace, message, message);
}

FileError WorkspaceFileSystem::util_error_to_file_error(const util::Error& error, const std::string& path) {
    FileErrorCode code = FileErrorCode::Unknown;
    switch (error.code) {
    case util::ErrorCode::Workspace:
        code = FileErrorCode::PermissionDenied;
        break;
    case util::ErrorCode::Validation:
        code = FileErrorCode::Invalid;
        break;
    case util::ErrorCode::Cancelled:
        code = FileErrorCode::Aborted;
        break;
    default:
        break;
    }
    return FileError{code, error.message, std::string{path}};
}

bool WorkspaceFileSystem::inside(const std::filesystem::path& path) const {
    std::error_code ec;
    auto canonical = std::filesystem::weakly_canonical(path, ec);
    if (ec) {
        return false;
    }
    return inside_lexically(canonical);
}

bool WorkspaceFileSystem::inside_lexically(const std::filesystem::path& path) const {
    auto rel = path.lexically_normal().lexically_relative(root_);
    if (rel.empty() || rel == ".") {
        return true;
    }
    if (rel.is_absolute()) {
        return false;
    }
    auto first = rel.begin();
    return first == rel.end() || *first != "..";
}

bool WorkspaceFileSystem::has_symlink_component(const std::filesystem::path& lexical_parent) const {
    auto rel = lexical_parent.lexically_normal().lexically_relative(root_);
    if (rel.empty() || rel == ".") {
        return false;
    }
    std::error_code ec;
    std::filesystem::path cursor = root_;
    for (const auto& part : rel) {
        if (part == "." || part.empty()) {
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
        if (!std::filesystem::exists(status)) {
            return false;
        }
        if (std::filesystem::is_symlink(status)) {
            return true;
        }
    }
    return false;
}

util::Expected<std::filesystem::path> WorkspaceFileSystem::resolve_for_write(
    const std::string& requested,
    bool create_parents) const {
    auto target = resolve_addressed_path(requested);
    if (!target) {
        return std::unexpected(target.error());
    }
    std::error_code ec;
    auto parent = target->parent_path();
    if (parent.empty()) {
        parent = root_;
    }
    if (!std::filesystem::exists(parent, ec)) {
        if (!create_parents) {
            return std::unexpected(workspace_error("parent directory does not exist: " + requested));
        }
        if (has_symlink_component(parent)) {
            return std::unexpected(workspace_error("parent path contains a symlink: " + requested));
        }
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            return std::unexpected(workspace_error("could not create parent directory: " + ec.message()));
        }
    }
    if (has_symlink_component(parent)) {
        return std::unexpected(workspace_error("parent path contains a symlink: " + requested));
    }
    auto parent_canonical = std::filesystem::canonical(parent, ec);
    if (ec || !inside(parent_canonical)) {
        return std::unexpected(workspace_error("parent path escapes workspace: " + requested));
    }
    auto status = std::filesystem::symlink_status(*target, ec);
    if (ec && status.type() != std::filesystem::file_type::not_found) {
        return std::unexpected(workspace_error("could not inspect target: " + requested));
    }
    if (std::filesystem::is_symlink(status)) {
        return std::unexpected(workspace_error("refusing to write through final symlink: " + requested));
    }
    if (std::filesystem::exists(status) && !std::filesystem::is_regular_file(status)) {
        return std::unexpected(workspace_error("target is not a regular file: " + requested));
    }
    return *target;
}

std::filesystem::path WorkspaceFileSystem::canonicalized(std::filesystem::path workspace) {
    std::error_code ec;
    auto canonical = std::filesystem::weakly_canonical(workspace, ec);
    if (ec) {
        return workspace;
    }
    return canonical;
}

std::filesystem::path WorkspaceFileSystem::default_root() {
    std::error_code ec;
    auto cwd = std::filesystem::current_path(ec);
    if (ec) {
        return std::filesystem::path{"."};
    }
    return cwd;
}

} // namespace cch::harness
