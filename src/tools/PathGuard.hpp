#pragma once

#include "../../include/cch/util/Error.hpp"

#include <filesystem>
#include <string>

namespace cch::tools {

class PathGuard {
public:
    PathGuard() = default;
    explicit PathGuard(std::filesystem::path workspace)
        : root_(std::filesystem::weakly_canonical(std::move(workspace))) {}

    static util::Expected<PathGuard> create(const std::filesystem::path& workspace) {
        std::error_code ec;
        if (!std::filesystem::exists(workspace, ec) || !std::filesystem::is_directory(workspace, ec)) {
            return std::unexpected(workspace_error("workspace does not exist or is not a directory"));
        }
        return PathGuard(workspace);
    }

    [[nodiscard]] util::Expected<std::filesystem::path> resolve_existing_file(const std::string& requested) const {
        auto target = lexical_workspace_path(requested);
        if (!target) {
            return std::unexpected(target.error());
        }
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
        return canonical;
    }

    [[nodiscard]] util::Expected<std::filesystem::path> resolve_for_write(const std::string& requested, bool create_parents) const {
        auto target = lexical_workspace_path(requested);
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

    [[nodiscard]] const std::filesystem::path& root() const { return root_; }

private:
    [[nodiscard]] static util::Error workspace_error(std::string message) {
        return util::make_error(util::ErrorCode::Workspace, message, message);
    }

    [[nodiscard]] util::Expected<std::filesystem::path> lexical_workspace_path(const std::string& requested) const {
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

    [[nodiscard]] bool inside(const std::filesystem::path& path) const { return inside_lexically(std::filesystem::weakly_canonical(path)); }

    [[nodiscard]] bool inside_lexically(const std::filesystem::path& path) const {
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

    [[nodiscard]] bool has_symlink_component(const std::filesystem::path& lexical_parent) const {
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

    std::filesystem::path root_{std::filesystem::current_path()};
};

} // namespace cch::tools
