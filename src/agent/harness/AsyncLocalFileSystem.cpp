#include <cch/agent/harness/LocalFileSystem.hpp>

#include "AsyncFileSystemOperations.hpp"
#include "WorkspaceFileSystem.hpp"

#include <cstddef>
#include <memory>
#include <utility>

namespace cch::harness {

struct AsyncLocalFileSystem::Impl final {
    Impl(std::shared_ptr<RuntimeTarget> runtime_target, std::shared_ptr<WorkspaceFileSystem> filesystem)
        : runtime_target(std::move(runtime_target)), filesystem(std::move(filesystem)) {}

    std::shared_ptr<RuntimeTarget> runtime_target;
    std::shared_ptr<WorkspaceFileSystem> filesystem;
};

AsyncLocalFileSystem::AsyncLocalFileSystem(
        std::shared_ptr<RuntimeTarget> runtime_target, std::filesystem::path workspace)
    : impl_(std::make_shared<Impl>(
              std::move(runtime_target), std::make_shared<WorkspaceFileSystem>(std::move(workspace)))) {}

AsyncLocalFileSystem::AsyncLocalFileSystem(AsyncLocalFileSystem&&) noexcept = default;
AsyncLocalFileSystem& AsyncLocalFileSystem::operator=(AsyncLocalFileSystem&&) noexcept = default;
AsyncLocalFileSystem::~AsyncLocalFileSystem() = default;

const std::filesystem::path& AsyncLocalFileSystem::workspace() const { return impl_->filesystem->root(); }

support::AsyncResult<std::string, FileError> AsyncLocalFileSystem::absolutePath(
        std::string path, std::stop_token stop_token) {
    return filesystem_detail::submit_filesystem_operation<std::string>(impl_->runtime_target,
            impl_->filesystem,
            path.size(),
            stop_token,
            path,
            [path = std::move(path)](const WorkspaceFileSystem& fs) { return fs.absolutePath(path); });
}

support::AsyncResult<std::string, FileError> AsyncLocalFileSystem::joinPath(
        std::vector<std::string> parts, std::stop_token stop_token) {
    std::size_t byte_charge{0};
    for (const auto& part : parts) {
        byte_charge = filesystem_detail::saturating_add(byte_charge, part.size());
    }
    return filesystem_detail::submit_filesystem_operation<std::string>(impl_->runtime_target,
            impl_->filesystem,
            byte_charge,
            stop_token,
            std::nullopt,
            [parts = std::move(parts)](const WorkspaceFileSystem& fs) { return fs.joinPath(parts); });
}

support::AsyncResult<std::string, FileError> AsyncLocalFileSystem::readTextFile(
        std::string path, std::stop_token stop_token) {
    return filesystem_detail::submit_filesystem_operation<std::string>(impl_->runtime_target,
            impl_->filesystem,
            filesystem_detail::file_read_charge(path.size()),
            stop_token,
            path,
            [path = std::move(path), stop_token](
                    const WorkspaceFileSystem& fs) { return fs.readTextFile(path, stop_token); });
}

support::AsyncResult<std::vector<std::string>, FileError> AsyncLocalFileSystem::readTextLines(
        std::string path, std::optional<int> maxLines, std::stop_token stop_token) {
    return filesystem_detail::submit_filesystem_operation<std::vector<std::string>>(impl_->runtime_target,
            impl_->filesystem,
            filesystem_detail::text_lines_charge(path.size()),
            stop_token,
            path,
            [path = std::move(path), maxLines, stop_token](
                    const WorkspaceFileSystem& fs) { return fs.readTextLines(path, maxLines, stop_token); });
}

support::AsyncResult<BinaryData, FileError> AsyncLocalFileSystem::readBinaryFile(
        std::string path, std::stop_token stop_token) {
    return filesystem_detail::submit_filesystem_operation<BinaryData>(impl_->runtime_target,
            impl_->filesystem,
            filesystem_detail::file_read_charge(path.size()),
            stop_token,
            path,
            [path = std::move(path), stop_token](
                    const WorkspaceFileSystem& fs) { return fs.readBinaryFile(path, stop_token); });
}

support::AsyncResult<void, FileError> AsyncLocalFileSystem::writeFile(
        std::string path, WriteContent content, std::stop_token stop_token) {
    const auto content_size = std::visit([](const auto& value) { return value.size(); }, content);
    const auto byte_charge = filesystem_detail::saturating_add(path.size(), content_size);
    return filesystem_detail::submit_filesystem_operation<void>(impl_->runtime_target,
            impl_->filesystem,
            byte_charge,
            stop_token,
            path,
            [path = std::move(path), content = std::move(content), stop_token](
                    const WorkspaceFileSystem& fs) { return fs.writeFile(path, content, stop_token); });
}

support::AsyncResult<void, FileError> AsyncLocalFileSystem::appendFile(
        std::string path, WriteContent content, std::stop_token stop_token) {
    const auto content_size = std::visit([](const auto& value) { return value.size(); }, content);
    const auto byte_charge = filesystem_detail::saturating_add(path.size(), content_size);
    return filesystem_detail::submit_filesystem_operation<void>(impl_->runtime_target,
            impl_->filesystem,
            byte_charge,
            stop_token,
            path,
            [path = std::move(path), content = std::move(content), stop_token](
                    const WorkspaceFileSystem& fs) { return fs.appendFile(path, content, stop_token); });
}

support::AsyncResult<FileInfo, FileError> AsyncLocalFileSystem::fileInfo(std::string path, std::stop_token stop_token) {
    return filesystem_detail::submit_filesystem_operation<FileInfo>(impl_->runtime_target,
            impl_->filesystem,
            path.size(),
            stop_token,
            path,
            [path = std::move(path)](const WorkspaceFileSystem& fs) { return fs.fileInfo(path); });
}

support::AsyncResult<std::vector<FileInfo>, FileError> AsyncLocalFileSystem::listDir(
        std::string path, std::stop_token stop_token) {
    return filesystem_detail::submit_filesystem_operation<std::vector<FileInfo>>(impl_->runtime_target,
            impl_->filesystem,
            filesystem_detail::directory_list_charge(path.size()),
            stop_token,
            path,
            [path = std::move(path), stop_token](
                    const WorkspaceFileSystem& fs) { return fs.listDir(path, stop_token); });
}

support::AsyncResult<std::string, FileError> AsyncLocalFileSystem::canonicalPath(
        std::string path, std::stop_token stop_token) {
    return filesystem_detail::submit_filesystem_operation<std::string>(impl_->runtime_target,
            impl_->filesystem,
            path.size(),
            stop_token,
            path,
            [path = std::move(path)](const WorkspaceFileSystem& fs) { return fs.canonicalPath(path); });
}

support::AsyncResult<bool, FileError> AsyncLocalFileSystem::exists(std::string path, std::stop_token stop_token) {
    return filesystem_detail::submit_filesystem_operation<bool>(impl_->runtime_target,
            impl_->filesystem,
            path.size(),
            stop_token,
            path,
            [path = std::move(path)](const WorkspaceFileSystem& fs) { return fs.exists(path); });
}

support::AsyncResult<void, FileError> AsyncLocalFileSystem::createDir(
        std::string path, bool recursive, std::stop_token stop_token) {
    return filesystem_detail::submit_filesystem_operation<void>(impl_->runtime_target,
            impl_->filesystem,
            path.size(),
            stop_token,
            path,
            [path = std::move(path), recursive](
                    const WorkspaceFileSystem& fs) { return fs.createDir(path, recursive); });
}

support::AsyncResult<void, FileError> AsyncLocalFileSystem::remove(
        std::string path, bool recursive, std::stop_token stop_token) {
    return filesystem_detail::submit_filesystem_operation<void>(impl_->runtime_target,
            impl_->filesystem,
            path.size(),
            stop_token,
            path,
            [path = std::move(path), recursive, stop_token](
                    const WorkspaceFileSystem& fs) { return fs.remove(path, recursive, stop_token); });
}

support::AsyncResult<std::string, FileError> AsyncLocalFileSystem::createTempDir(
        std::optional<std::string> prefix, std::stop_token stop_token) {
    const auto byte_charge = prefix ? prefix->size() : std::size_t{0};
    return filesystem_detail::submit_filesystem_operation<std::string>(impl_->runtime_target,
            impl_->filesystem,
            byte_charge,
            stop_token,
            std::nullopt,
            [prefix = std::move(prefix)](const WorkspaceFileSystem& fs) { return fs.createTempDir(prefix); });
}

support::AsyncResult<std::string, FileError> AsyncLocalFileSystem::createTempFile(
        std::optional<std::string> prefix, std::optional<std::string> suffix, std::stop_token stop_token) {
    const auto prefix_size = prefix ? prefix->size() : std::size_t{0};
    const auto suffix_size = suffix ? suffix->size() : std::size_t{0};
    const auto byte_charge = filesystem_detail::saturating_add(prefix_size, suffix_size);
    return filesystem_detail::submit_filesystem_operation<std::string>(impl_->runtime_target,
            impl_->filesystem,
            byte_charge,
            stop_token,
            std::nullopt,
            [prefix = std::move(prefix), suffix = std::move(suffix)](
                    const WorkspaceFileSystem& fs) { return fs.createTempFile(prefix, suffix); });
}

support::AsyncResult<void, FileError> AsyncLocalFileSystem::cleanup() {
    if (!impl_->runtime_target) {
        return support::AsyncResult<void, FileError>{std::expected<void, FileError>{}};
    }
    return filesystem_detail::submit_filesystem_operation<void>(
            impl_->runtime_target,
            impl_->filesystem,
            0,
            std::stop_token{},
            std::nullopt,
            [](const WorkspaceFileSystem& fs) {
                fs.cleanup_temporary_resources();
                return std::expected<void, FileError>{};
            },
            AdmissionLane::Reserved);
}

} // namespace cch::harness
