#pragma once

#include <cch/agent/harness/FileSystem.hpp>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>

namespace cch::tests {

/// Complete deterministic filesystem fake for contract tests. In its default
/// mode it preserves the small success behavior used by the filesystem
/// contract tests. Calling add_file/add_directory/add_symlink switches it to
/// a contained in-memory tree suitable for Project Resource tests.
class FakeAsyncFileSystem final : public harness::AsyncFileSystem {
public:
    explicit FakeAsyncFileSystem(std::filesystem::path workspace)
        : workspace_(std::move(workspace)) {}

    const std::filesystem::path& workspace() const override { return workspace_; }

    /// Add a regular file and any missing parent directories to the virtual
    /// tree. Paths are addressed relative to workspace, or may be absolute
    /// paths beneath workspace.
    void add_file(std::string path, std::string content) {
        configured_ = true;
        const auto key = key_for(path);
        if (!key) return;
        add_parent_directories(*key);
        nodes_[*key] = Node{.kind = harness::FileKind::File, .content = std::move(content), .target = {}};
    }

    void add_directory(std::string path) {
        configured_ = true;
        const auto key = key_for(path);
        if (!key) return;
        add_parent_directories(*key);
        nodes_[*key] = Node{.kind = harness::FileKind::Directory, .content = {}, .target = {}};
    }

    /// Add a symlink without following it for metadata/listing. Reads through
    /// the link are rejected, matching the Local Adapter's ordinary-read
    /// containment contract; canonicalPath resolves links for discovery.
    void add_symlink(std::string path, std::string target) {
        configured_ = true;
        const auto key = key_for(path);
        if (!key) return;
        add_parent_directories(*key);
        nodes_[*key] = Node{
                .kind = harness::FileKind::Symlink,
                .content = {},
                .target = std::move(target),
        };
    }

    /// Configure a path-specific outcome for one or more operations.
    void set_error(std::string path, harness::FileError error) {
        configured_ = true;
        path_errors_[key_for(path).value_or(std::move(path))] = std::move(error);
    }

    support::AsyncResult<std::string, harness::FileError> absolutePath(
        std::string path,
        std::stop_token stop_token) override {
        if (auto error = failure(stop_token)) return failed<std::string>(std::move(*error));
        return ready(std::move(path));
    }

    support::AsyncResult<std::string, harness::FileError> joinPath(
        std::vector<std::string> parts,
        std::stop_token stop_token) override {
        if (auto error = failure(stop_token)) return failed<std::string>(std::move(*error));
        std::filesystem::path result = workspace_;
        for (auto& part : parts)
            result /= std::move(part);
        return ready(result.lexically_normal().string());
    }

    support::AsyncResult<std::string, harness::FileError> readTextFile(
        std::string path,
        std::stop_token stop_token) override {
        if (auto error = failure(stop_token, path)) return failed<std::string>(std::move(*error));
        if (!configured_) return ready(std::move(path));
        if (auto error = path_error(path)) return failed<std::string>(std::move(*error));
        const auto key = key_for(path);
        if (!key) return failed<std::string>(permission_error(path));
        const auto node = nodes_.find(*key);
        if (node == nodes_.end()) return failed<std::string>(not_found(path));
        if (node->second.kind == harness::FileKind::Symlink) {
            return failed<std::string>(permission_error(path));
        }
        if (node->second.kind != harness::FileKind::File) {
            return failed<std::string>(harness::FileError{
                    .code = harness::FileErrorCode::IsDirectory,
                    .message = "path is a directory",
                    .path = std::move(path),
            });
        }
        return ready(node->second.content);
    }

    support::AsyncResult<std::vector<std::string>, harness::FileError> readTextLines(
            std::string path, std::optional<int> max_lines, std::stop_token stop_token) override {
        if (auto error = failure(stop_token, path)) {
            return failed<std::vector<std::string>>(std::move(*error));
        }
        auto text = readTextFile(path, stop_token);
        std::optional<std::expected<std::string, harness::FileError>> outcome;
        std::move(text).start([&outcome](std::expected<std::string, harness::FileError> value) noexcept {
            outcome.emplace(std::move(value));
        });
        if (!outcome || !*outcome) {
            return failed<std::vector<std::string>>(outcome ? std::move(outcome->error())
                                                            : harness::FileError{
                                                                      .code = harness::FileErrorCode::Unknown,
                                                                      .message = "fake read did not complete",
                                                                      .path = path,
                                                              });
        }
        std::vector<std::string> lines;
        std::istringstream input{**outcome};
        std::string line;
        while (std::getline(input, line) && (!max_lines || static_cast<int>(lines.size()) < *max_lines)) {
            lines.push_back(std::move(line));
        }
        return ready(std::move(lines));
    }

    support::AsyncResult<harness::BinaryData, harness::FileError> readBinaryFile(
            std::string path, std::stop_token stop_token) override {
        if (auto error = failure(stop_token)) return failed<harness::BinaryData>(std::move(*error));
        if (!configured_) return ready(harness::BinaryData{});
        auto text = readTextFile(path, stop_token);
        std::optional<std::expected<std::string, harness::FileError>> outcome;
        std::move(text).start([&outcome](std::expected<std::string, harness::FileError> value) noexcept {
            outcome.emplace(std::move(value));
        });
        if (!outcome || !*outcome) {
            return failed<harness::BinaryData>(outcome ? std::move(outcome->error())
                                                       : harness::FileError{
                                                                 .code = harness::FileErrorCode::Unknown,
                                                                 .message = "fake read did not complete",
                                                                 .path = path,
                                                         });
        }
        harness::BinaryData bytes;
        for (const char value : **outcome)
            bytes.push_back(static_cast<std::byte>(value));
        return ready(std::move(bytes));
    }

    support::AsyncResult<void, harness::FileError> writeFile(
            std::string path, harness::WriteContent content, std::stop_token stop_token) override {
        if (auto error = failure(stop_token, path)) return failed<void>(std::move(*error));
        if (configured_) {
            const auto key = key_for(path);
            if (!key) return failed<void>(permission_error(path));
            add_parent_directories(*key);
            nodes_[*key] = Node{
                    .kind = harness::FileKind::File,
                    .content = std::visit(
                            [](const auto& value) {
                                using Value = std::remove_cvref_t<decltype(value)>;
                                if constexpr (std::is_same_v<Value, std::string>) return value;
                                std::string text;
                                for (const auto byte : value)
                                    text.push_back(static_cast<char>(byte));
                                return text;
                            },
                            content),
                    .target = {},
            };
        }
        return ready();
    }

    support::AsyncResult<void, harness::FileError> appendFile(
            std::string path, harness::WriteContent content, std::stop_token stop_token) override {
        if (auto error = failure(stop_token, path)) return failed<void>(std::move(*error));
        if (configured_) {
            const auto key = key_for(path);
            if (!key) return failed<void>(permission_error(path));
            add_parent_directories(*key);
            auto& node = nodes_[*key];
            node.kind = harness::FileKind::File;
            if (const auto* text = std::get_if<std::string>(&content)) {
                node.content += *text;
            } else {
                for (const auto byte : std::get<harness::BinaryData>(content)) {
                    node.content.push_back(static_cast<char>(byte));
                }
            }
        }
        return ready();
    }

    support::AsyncResult<harness::FileInfo, harness::FileError> fileInfo(
        std::string path,
        std::stop_token stop_token) override {
        if (auto error = failure(stop_token, path)) return failed<harness::FileInfo>(std::move(*error));
        if (!configured_) {
            return ready(harness::FileInfo{
                    .name = std::move(path),
                    .path = workspace_.string(),
                    .kind = harness::FileKind::File,
            });
        }
        if (auto error = path_error(path)) return failed<harness::FileInfo>(std::move(*error));
        const auto key = key_for(path);
        if (!key) return failed<harness::FileInfo>(permission_error(path));
        const auto node = nodes_.find(*key);
        if (node == nodes_.end()) return failed<harness::FileInfo>(not_found(path));
        return ready(info_for(*key, node->second));
    }

    support::AsyncResult<std::vector<harness::FileInfo>, harness::FileError> listDir(
            std::string path, std::stop_token stop_token) override {
        if (auto error = failure(stop_token, path)) {
            return failed<std::vector<harness::FileInfo>>(std::move(*error));
        }
        if (!configured_) return ready(std::vector<harness::FileInfo>{});
        if (auto error = path_error(path)) {
            return failed<std::vector<harness::FileInfo>>(std::move(*error));
        }
        const auto key = key_for(path);
        if (!key) return failed<std::vector<harness::FileInfo>>(permission_error(path));
        const auto directory = nodes_.find(*key);
        if (directory == nodes_.end()) {
            return failed<std::vector<harness::FileInfo>>(not_found(path));
        }
        if (directory->second.kind != harness::FileKind::Directory) {
            return failed<std::vector<harness::FileInfo>>(harness::FileError{
                    .code = harness::FileErrorCode::NotDirectory,
                    .message = "path is not a directory",
                    .path = std::move(path),
            });
        }
        std::vector<harness::FileInfo> entries;
        const std::string prefix = *key == "." ? std::string{} : *key + "/";
        for (const auto& [child_key, node] : nodes_) {
            if (!child_key.starts_with(prefix) || child_key == *key) continue;
            const auto remainder = child_key.substr(prefix.size());
            if (remainder.find('/') != std::string::npos) continue;
            entries.push_back(info_for(child_key, node));
        }
        return ready(std::move(entries));
    }

    support::AsyncResult<std::string, harness::FileError> canonicalPath(
        std::string path,
        std::stop_token stop_token) override {
        if (auto error = failure(stop_token, path)) return failed<std::string>(std::move(*error));
        if (!configured_) return ready(std::move(path));
        auto key = key_for(path);
        if (!key) return failed<std::string>(permission_error(path));
        for (std::size_t depth = 0; depth < nodes_.size() + 1; ++depth) {
            const auto node = nodes_.find(*key);
            if (node == nodes_.end()) return failed<std::string>(not_found(path));
            if (node->second.kind != harness::FileKind::Symlink) {
                return ready(absolute_for_key(*key));
            }
            const auto target = std::filesystem::path{node->second.target};
            const auto parent = std::filesystem::path{*key}.parent_path();
            auto resolved = (parent / target).lexically_normal();
            const auto resolved_key = key_for(resolved.string());
            if (!resolved_key) return failed<std::string>(permission_error(path));
            *key = *resolved_key;
        }
        return failed<std::string>(harness::FileError{
                .code = harness::FileErrorCode::Unknown,
                .message = "symlink cycle",
                .path = std::move(path),
        });
    }

    support::AsyncResult<bool, harness::FileError> exists(std::string path, std::stop_token stop_token) override {
        if (auto error = failure(stop_token, path)) return failed<bool>(std::move(*error));
        if (!configured_) return ready(true);
        if (auto error = path_error(path)) {
            if (error->code == harness::FileErrorCode::NotFound) return ready(false);
            return failed<bool>(std::move(*error));
        }
        const auto key = key_for(path);
        if (!key) return failed<bool>(permission_error(path));
        return ready(nodes_.contains(*key));
    }

    support::AsyncResult<void, harness::FileError> createDir(
            std::string path, bool recursive, std::stop_token stop_token) override {
        if (auto error = failure(stop_token, path)) return failed<void>(std::move(*error));
        if (configured_) {
            const auto key = key_for(path);
            if (!key) return failed<void>(permission_error(path));
            if (recursive) add_parent_directories(*key);
            nodes_[*key] = Node{.kind = harness::FileKind::Directory, .content = {}, .target = {}};
        }
        return ready();
    }

    support::AsyncResult<void, harness::FileError> remove(
            std::string path, bool recursive, std::stop_token stop_token) override {
        if (auto error = failure(stop_token, path)) return failed<void>(std::move(*error));
        if (configured_) {
            const auto key = key_for(path);
            if (!key) return failed<void>(permission_error(path));
            if (!nodes_.contains(*key)) return failed<void>(not_found(path));
            if (!recursive) {
                for (const auto& [child, _] : nodes_) {
                    if (child.starts_with(*key + "/")) {
                        return failed<void>(harness::FileError{
                                .code = harness::FileErrorCode::Busy,
                                .message = "directory is not empty",
                                .path = std::move(path),
                        });
                    }
                }
            }
            for (auto it = nodes_.begin(); it != nodes_.end();) {
                if (it->first == *key || (recursive && it->first.starts_with(*key + "/"))) {
                    it = nodes_.erase(it);
                } else {
                    ++it;
                }
            }
        }
        return ready();
    }

    support::AsyncResult<std::string, harness::FileError> createTempDir(
        std::optional<std::string>,
        std::stop_token stop_token) override {
        if (auto error = failure(stop_token)) return failed<std::string>(std::move(*error));
        return ready(workspace_.string() + "/fake-temp-dir");
    }

    support::AsyncResult<std::string, harness::FileError> createTempFile(
        std::optional<std::string>,
        std::optional<std::string>,
        std::stop_token stop_token) override {
        if (auto error = failure(stop_token)) return failed<std::string>(std::move(*error));
        return ready(workspace_.string() + "/fake-temp-file");
    }

    support::AsyncResult<void, harness::FileError> cleanup() override {
        if (cleanup_error) return failed<void>(*cleanup_error);
        return ready();
    }

    std::optional<harness::FileError> next_error;
    std::optional<harness::FileError> cleanup_error;

private:
    struct Node {
        harness::FileKind kind{harness::FileKind::Directory};
        std::string content;
        std::string target;
    };

    [[nodiscard]] std::optional<std::string> key_for(const std::string& path) const {
        auto candidate = std::filesystem::path{path};
        const auto root = workspace_.lexically_normal();
        if (candidate.is_absolute()) {
            const auto absolute_candidate = candidate.lexically_normal();
            auto root_it = root.begin();
            auto candidate_it = absolute_candidate.begin();
            while (root_it != root.end() && candidate_it != candidate.end() && *root_it == *candidate_it) {
                ++root_it;
                ++candidate_it;
            }
            if (root_it != root.end()) return std::nullopt;
            candidate.clear();
            for (; candidate_it != absolute_candidate.end(); ++candidate_it) {
                candidate /= *candidate_it;
            }
        }
        candidate = candidate.lexically_normal();
        if (candidate.empty() || candidate == ".") return std::string{"."};
        for (const auto& component : candidate) {
            if (component == "..") return std::nullopt;
        }
        return candidate.string();
    }

    [[nodiscard]] std::string absolute_for_key(const std::string& key) const {
        if (key == ".") return workspace_.lexically_normal().string();
        return (workspace_ / key).lexically_normal().string();
    }

    void add_parent_directories(const std::string& key) {
        auto parent = std::filesystem::path{key}.parent_path();
        std::vector<std::filesystem::path> parents;
        while (!parent.empty() && parent != ".") {
            parents.push_back(parent);
            parent = parent.parent_path();
        }
        for (auto it = parents.rbegin(); it != parents.rend(); ++it) {
            nodes_.try_emplace(it->string(), Node{.kind = harness::FileKind::Directory, .content = {}, .target = {}});
        }
        nodes_.try_emplace(".", Node{.kind = harness::FileKind::Directory, .content = {}, .target = {}});
    }

    [[nodiscard]] harness::FileInfo info_for(const std::string& key, const Node& node) const {
        return harness::FileInfo{
                .name = std::filesystem::path{key}.filename().string(),
                .path = absolute_for_key(key),
                .kind = node.kind,
                .size = node.content.size(),
        };
    }

    [[nodiscard]] std::optional<harness::FileError> path_error(const std::string& path) const {
        const auto key = key_for(path);
        if (!key) return permission_error(path);
        if (const auto it = path_errors_.find(*key); it != path_errors_.end()) return it->second;
        return std::nullopt;
    }

    [[nodiscard]] static harness::FileError not_found(std::string path) {
        return harness::FileError{
                .code = harness::FileErrorCode::NotFound,
                .message = "path not found",
                .path = std::move(path),
        };
    }

    [[nodiscard]] static harness::FileError permission_error(std::string path) {
        return harness::FileError{
                .code = harness::FileErrorCode::PermissionDenied,
                .message = "path is outside fake workspace",
                .path = std::move(path),
        };
    }

    [[nodiscard]] std::optional<harness::FileError> failure(
            std::stop_token stop_token, std::optional<std::string> path = std::nullopt) const {
        if (stop_token.stop_requested()) {
            return harness::FileError{
                    .code = harness::FileErrorCode::Aborted,
                    .message = "Operation aborted",
                    .path = std::move(path),
            };
        }
        if (next_error) return next_error;
        return std::nullopt;
    }

    template <typename T> [[nodiscard]] static support::AsyncResult<T, harness::FileError> ready(T value) {
        return support::AsyncResult<T, harness::FileError>{std::expected<T, harness::FileError>{std::move(value)}};
    }

    [[nodiscard]] static support::AsyncResult<void, harness::FileError> ready() {
        return support::AsyncResult<void, harness::FileError>{std::expected<void, harness::FileError>{}};
    }

    template <typename T>
    [[nodiscard]] static support::AsyncResult<T, harness::FileError> failed(harness::FileError error) {
        return support::AsyncResult<T, harness::FileError>{std::unexpected(std::move(error))};
    }

    std::filesystem::path workspace_;
    bool configured_{false};
    std::map<std::string, Node> nodes_;
    std::map<std::string, harness::FileError> path_errors_;
};

} // namespace cch::tests
