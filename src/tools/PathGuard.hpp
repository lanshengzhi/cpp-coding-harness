#pragma once

#include "../util/Result.hpp"

#include <filesystem>
#include <string>

namespace cch::tools {

class PathGuard {
public:
    PathGuard() = default;
    explicit PathGuard(std::filesystem::path workspace)
        : root_(std::filesystem::weakly_canonical(std::move(workspace))) {}

    static util::Result<PathGuard> create(const std::filesystem::path& workspace) {
        std::error_code ec;
        if (!std::filesystem::exists(workspace, ec) || !std::filesystem::is_directory(workspace, ec)) {
            return util::Result<PathGuard>::failure("workspace does not exist or is not a directory");
        }
        return util::Result<PathGuard>::success(PathGuard(workspace));
    }

    [[nodiscard]] util::Result<std::filesystem::path> resolve_existing_file(const std::string& requested) const {
        auto target = lexical_workspace_path(requested);
        if (!target) {
            return util::Result<std::filesystem::path>::failure(target.error());
        }
        std::error_code ec;
        auto canonical = std::filesystem::canonical(target.value(), ec);
        if (ec) {
            return util::Result<std::filesystem::path>::failure("path does not exist inside workspace: " + requested);
        }
        if (!inside(canonical)) {
            return util::Result<std::filesystem::path>::failure("path escapes workspace: " + requested);
        }
        if (!std::filesystem::is_regular_file(canonical, ec)) {
            return util::Result<std::filesystem::path>::failure("path is not a regular file: " + requested);
        }
        return util::Result<std::filesystem::path>::success(canonical);
    }

    [[nodiscard]] util::Result<std::filesystem::path> resolve_for_write(const std::string& requested, bool create_parents) const {
        auto target = lexical_workspace_path(requested);
        if (!target) {
            return util::Result<std::filesystem::path>::failure(target.error());
        }
        std::error_code ec;
        auto parent = target.value().parent_path();
        if (parent.empty()) {
            parent = root_;
        }
        if (!std::filesystem::exists(parent, ec)) {
            if (!create_parents) {
                return util::Result<std::filesystem::path>::failure("parent directory does not exist: " + requested);
            }
            std::filesystem::create_directories(parent, ec);
            if (ec) {
                return util::Result<std::filesystem::path>::failure("could not create parent directory: " + ec.message());
            }
        }
        auto parent_canonical = std::filesystem::canonical(parent, ec);
        if (ec || !inside(parent_canonical)) {
            return util::Result<std::filesystem::path>::failure("parent path escapes workspace: " + requested);
        }
        if (has_symlink_component(parent_canonical)) {
            return util::Result<std::filesystem::path>::failure("parent path contains a symlink: " + requested);
        }
        if (std::filesystem::exists(target.value(), ec)) {
            auto status = std::filesystem::symlink_status(target.value(), ec);
            if (ec) {
                return util::Result<std::filesystem::path>::failure("could not inspect target: " + requested);
            }
            if (std::filesystem::is_symlink(status)) {
                return util::Result<std::filesystem::path>::failure("refusing to write through final symlink: " + requested);
            }
            if (!std::filesystem::is_regular_file(status)) {
                return util::Result<std::filesystem::path>::failure("target is not a regular file: " + requested);
            }
        }
        return util::Result<std::filesystem::path>::success(target.value());
    }

    [[nodiscard]] const std::filesystem::path& root() const { return root_; }

private:
    [[nodiscard]] util::Result<std::filesystem::path> lexical_workspace_path(const std::string& requested) const {
        if (requested.empty()) {
            return util::Result<std::filesystem::path>::failure("path is required");
        }
        std::filesystem::path relative(requested);
        if (relative.is_absolute()) {
            return util::Result<std::filesystem::path>::failure("absolute paths are not allowed: " + requested);
        }
        auto normalized = relative.lexically_normal();
        for (const auto& part : normalized) {
            if (part == "..") {
                return util::Result<std::filesystem::path>::failure("path escapes workspace: " + requested);
            }
        }
        auto target = (root_ / normalized).lexically_normal();
        if (!inside_lexically(target)) {
            return util::Result<std::filesystem::path>::failure("path escapes workspace: " + requested);
        }
        return util::Result<std::filesystem::path>::success(target);
    }

    [[nodiscard]] bool inside(const std::filesystem::path& path) const { return inside_lexically(std::filesystem::weakly_canonical(path)); }

    [[nodiscard]] bool inside_lexically(const std::filesystem::path& path) const {
        auto rel = std::filesystem::relative(path, root_);
        return rel.empty() || (rel.native().rfind("..", 0) != 0 && !rel.is_absolute());
    }

    [[nodiscard]] bool has_symlink_component(const std::filesystem::path& canonical_parent) const {
        std::error_code ec;
        auto rel = std::filesystem::relative(canonical_parent, root_, ec);
        if (ec) {
            return true;
        }
        std::filesystem::path cursor = root_;
        for (const auto& part : rel) {
            cursor /= part;
            if (std::filesystem::is_symlink(std::filesystem::symlink_status(cursor, ec))) {
                return true;
            }
        }
        return false;
    }

    std::filesystem::path root_{std::filesystem::current_path()};
};

} // namespace cch::tools
