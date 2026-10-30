// Path normalization and resolution: the single pi `resolveToCwd` seam
// (ADR 0057), the pi-shaped path queries built on it, and the root and
// error helpers every filesystem operation reports through.
#include "coding_agent/environment/WorkspaceFileSystem.hpp"

#include "coding_agent/environment/WorkspaceFileSystemErrors.hpp"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace cch::harness {
WorkspaceFileSystem::WorkspaceFileSystem() : temporary_state_(std::make_shared<TemporaryState>()) {}

WorkspaceFileSystem::WorkspaceFileSystem(std::filesystem::path workspace)
    : root_(canonicalized(std::move(workspace))), temporary_state_(std::make_shared<TemporaryState>()) {}

support::Expected<WorkspaceFileSystem> WorkspaceFileSystem::create(const std::filesystem::path& workspace) {
    std::error_code ec;
    if (!std::filesystem::exists(workspace, ec) || !std::filesystem::is_directory(workspace, ec)) {
        return std::unexpected(workspace_error("workspace does not exist or is not a directory"));
    }
    return WorkspaceFileSystem(workspace);
}

namespace {

/// pi `utils/paths.ts` UNICODE_SPACES (verbatim set): no-break and narrow
/// no-break style spaces normalize to an ASCII space before resolution.
[[nodiscard]] std::string normalize_unicode_spaces(std::string_view input) {
    std::string out;
    out.reserve(input.size());
    for (std::size_t i = 0; i < input.size();) {
        const auto byte = static_cast<unsigned char>(input[i]);
        if (byte == 0xC2 && i + 1 < input.size() && static_cast<unsigned char>(input[i + 1]) == 0xA0) {
            out += ' ';
            i += 2;
            continue;
        }
        if (byte == 0xE2 && i + 2 < input.size()) {
            const auto b1 = static_cast<unsigned char>(input[i + 1]);
            const auto b2 = static_cast<unsigned char>(input[i + 2]);
            if ((b1 == 0x80 && ((b2 >= 0x80 && b2 <= 0x8A) || b2 == 0xAF)) || (b1 == 0x81 && b2 == 0x9F)) {
                out += ' ';
                i += 3;
                continue;
            }
        }
        if (byte == 0xE3 && i + 2 < input.size() && static_cast<unsigned char>(input[i + 1]) == 0x80 &&
                static_cast<unsigned char>(input[i + 2]) == 0x80) {
            out += ' ';
            i += 3;
            continue;
        }
        out += input[i++];
    }
    return out;
}

struct DecodedCodePoint {
    std::uint32_t value;
    std::size_t length;
};

[[nodiscard]] DecodedCodePoint decode_utf8_first(std::string_view text) {
    const auto first = static_cast<unsigned char>(text.front());
    if (first < 0x80) {
        return {first, 1};
    }
    std::size_t length = 0;
    std::uint32_t value = 0;
    if ((first & 0xE0) == 0xC0) {
        length = 2;
        value = first & 0x1F;
    } else if ((first & 0xF0) == 0xE0) {
        length = 3;
        value = first & 0x0F;
    } else if ((first & 0xF8) == 0xF0) {
        length = 4;
        value = first & 0x07;
    } else {
        return {first, 1};
    }
    if (text.size() < length) {
        return {first, 1};
    }
    for (std::size_t index = 1; index < length; ++index) {
        const auto byte = static_cast<unsigned char>(text[index]);
        if ((byte & 0xC0) != 0x80) {
            return {first, 1};
        }
        value = (value << 6) | (byte & 0x3F);
    }
    const bool overlong =
            (length == 2 && value < 0x80) || (length == 3 && value < 0x800) || (length == 4 && value < 0x10000);
    if (overlong || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) {
        return {first, 1};
    }
    return {value, length};
}

[[nodiscard]] DecodedCodePoint decode_utf8_last(std::string_view text) {
    const auto last = static_cast<unsigned char>(text.back());
    if (last < 0x80) {
        return {last, 1};
    }
    std::size_t start = text.size() - 1;
    for (int back = 0; back < 3 && start > 0; ++back) {
        --start;
        if ((static_cast<unsigned char>(text[start]) & 0xC0) != 0x80) {
            break;
        }
    }
    const auto decoded = decode_utf8_first(text.substr(start));
    if (start + decoded.length == text.size()) {
        return decoded;
    }
    return {last, 1};
}

/// ECMAScript `String.prototype.trim()` whitespace set, which is what
/// `resolvePath(..., { trim: true })` applies.
[[nodiscard]] bool is_trim_whitespace(std::uint32_t code_point) {
    return (code_point >= 0x0009 && code_point <= 0x000D) || code_point == 0x0020 || code_point == 0x00A0 ||
           code_point == 0x1680 || (code_point >= 0x2000 && code_point <= 0x200A) || code_point == 0x2028 ||
           code_point == 0x2029 || code_point == 0x202F || code_point == 0x205F || code_point == 0x3000 ||
           code_point == 0xFEFF;
}

[[nodiscard]] std::string_view trim_whitespace(std::string_view text) {
    while (!text.empty() && is_trim_whitespace(decode_utf8_first(text).value)) {
        text.remove_prefix(decode_utf8_first(text).length);
    }
    while (!text.empty() && is_trim_whitespace(decode_utf8_last(text).value)) {
        text.remove_suffix(decode_utf8_last(text).length);
    }
    return text;
}

[[nodiscard]] std::optional<unsigned> hex_value(char ch) {
    if (ch >= '0' && ch <= '9') {
        return static_cast<unsigned>(ch - '0');
    }
    if (ch >= 'a' && ch <= 'f') {
        return static_cast<unsigned>(ch - 'a' + 10);
    }
    if (ch >= 'A' && ch <= 'F') {
        return static_cast<unsigned>(ch - 'A' + 10);
    }
    return std::nullopt;
}

[[nodiscard]] support::Error path_resolution_error(std::string message) {
    return support::make_error(support::ErrorCode::Workspace, message, message);
}

/// pi `normalizePath` converts `file://` through Node `fileURLToPath` on
/// POSIX: no or `localhost` authority, an absolute path, and percent-decoded
/// bytes.
[[nodiscard]] support::Expected<std::string> file_url_to_path(std::string_view text) {
    auto rest = text.substr(std::string_view{"file://"}.size());
    const auto slash = rest.find('/');
    const auto authority = rest.substr(0, slash);
    if (!authority.empty() && authority != "localhost") {
        return std::unexpected(path_resolution_error("file URL host must be empty or localhost on POSIX"));
    }
    if (slash == std::string_view::npos) {
        return std::unexpected(path_resolution_error("file URL does not name an absolute path"));
    }
    rest = rest.substr(slash);
    std::string decoded;
    decoded.reserve(rest.size());
    for (std::size_t index = 0; index < rest.size(); ++index) {
        if (rest[index] != '%') {
            decoded += rest[index];
            continue;
        }
        if (index + 2 >= rest.size()) {
            return std::unexpected(path_resolution_error("file URL contains a malformed percent escape"));
        }
        const auto high = hex_value(rest[index + 1]);
        const auto low = hex_value(rest[index + 2]);
        if (!high || !low) {
            return std::unexpected(path_resolution_error("file URL contains a malformed percent escape"));
        }
        decoded += static_cast<char>((*high << 4) | *low);
        index += 2;
    }
    return decoded;
}
} // namespace

support::Expected<std::filesystem::path> WorkspaceFileSystem::resolve_path(
        const std::string& requested, const PathResolutionOptions& options) const {
    if (requested.empty()) {
        if (options.allow_empty) {
            return root_;
        }
        return std::unexpected(workspace_error("path is required; use a workspace-relative path or an absolute path"));
    }
    std::string_view text = requested;
    if (options.trim) {
        text = trim_whitespace(text);
    }
    if (text.empty()) {
        if (options.allow_empty) {
            return root_;
        }
        return std::unexpected(workspace_error("path is required; use a workspace-relative path or an absolute path"));
    }
    if (text.find('\0') != std::string_view::npos) {
        return std::unexpected(workspace_error("NUL bytes are not allowed in paths"));
    }
    // pi resolveToCwd semantics (ADR 0057) and the resource-flag modifier set
    // (`resolveCliPaths` + `resolvePath(..., { trim: true })`, #1114) share
    // this core: preprocessing in pi `normalizePath` order (unicode spaces,
    // "@" prefix, `~` against $HOME, POSIX `file://` conversion), then
    // absolute paths are honored after lexical normalization regardless of
    // containment; relative paths resolve against the workspace root with
    // ".." handled by normalization rather than rejection. The open-time
    // no-follow symlink guards still apply to every read.
    std::string preprocessed(text);
    const bool is_initial_absolute = std::filesystem::path(preprocessed).is_absolute();
    if (options.normalize_unicode_spaces && !is_initial_absolute) {
        preprocessed = normalize_unicode_spaces(preprocessed);
    }
    if (options.strip_at_prefix && preprocessed.starts_with('@')) {
        preprocessed.erase(0, 1);
    }
    if (options.expand_tilde && (preprocessed == "~" || preprocessed.starts_with("~/"))) {
        if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
            preprocessed = home + preprocessed.substr(1);
        }
    }
    if (options.convert_file_url && preprocessed.starts_with("file://")) {
        auto converted = file_url_to_path(preprocessed);
        if (!converted) {
            return std::unexpected(converted.error());
        }
        preprocessed = std::move(*converted);
    }
    if (preprocessed.empty()) {
        // Node `path.resolve(cwd, "")` is the base directory.
        return root_;
    }
    const std::filesystem::path queried(preprocessed);
    auto target = queried.is_absolute() ? queried.lexically_normal() : (root_ / queried).lexically_normal();
    if (target != root_ && target.filename().empty()) {
        target = target.parent_path();
    }
    return target;
}

support::Expected<std::filesystem::path> WorkspaceFileSystem::resolve_to_cwd(const std::string& requested) const {
    return resolve_path(requested, PathResolutionOptions{});
}

std::expected<std::string, FileError> WorkspaceFileSystem::resolveResourcePath(const std::string& path) const {
    // pi `resolveCliPaths` only resolves local inputs, then the resource
    // loader applies `resolvePath(p, cwd, { trim: true })`: JavaScript trim,
    // `~` expansion, POSIX `file://` conversion, literal leading `@`, no
    // unicode-space normalization, and an empty value resolving to the root.
    const PathResolutionOptions options{
            .trim = true,
            .expand_tilde = true,
            .strip_at_prefix = false,
            .normalize_unicode_spaces = false,
            .convert_file_url = true,
            .allow_empty = true,
    };
    auto resolved = resolve_path(path, options);
    if (!resolved) {
        return std::unexpected(util_error_to_file_error(resolved.error(), path));
    }
    return resolved->string();
}

std::expected<std::string, FileError> WorkspaceFileSystem::absolutePath(const std::string& path) const {
    auto resolved = resolve_to_cwd(path);
    if (!resolved) {
        return std::unexpected(util_error_to_file_error(resolved.error(), path));
    }
    return resolved->string();
}

std::expected<std::string, FileError> WorkspaceFileSystem::joinPath(const std::vector<std::string>& parts) const {
    std::filesystem::path result = root_;
    for (const auto& part : parts) {
        if (part.find('\0') != std::string::npos) {
            return std::unexpected(FileError{
                    .code = FileErrorCode::Invalid,
                    .message = "NUL bytes are not allowed in paths",
                    .path = std::nullopt,
            });
        }
        result /= part;
    }
    // No containment (ADR 0057): segments join against the workspace root
    // and normalize lexically; results landing outside the root are
    // returned, not rejected.
    return result.lexically_normal().string();
}

std::expected<std::string, FileError> WorkspaceFileSystem::canonicalPath(const std::string& path) const {
    auto resolved = resolve_to_cwd(path);
    if (!resolved) {
        return std::unexpected(util_error_to_file_error(resolved.error(), path));
    }
    std::error_code ec;
    auto canonical = std::filesystem::canonical(*resolved, ec);
    if (ec) {
        // An unsearchable parent surfaces as a real EACCES here; report it
        // honestly instead of claiming the path is missing (issue #702).
        if (ec == std::errc::permission_denied || ec == std::errc::operation_not_permitted) {
            return std::unexpected(permission_denied_error(path));
        }
        return std::unexpected(
                FileError{FileErrorCode::NotFound, "could not canonicalize: " + path, std::string{path}});
    }
    return canonical.string();
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

support::Error WorkspaceFileSystem::workspace_error(std::string message) {
    return support::make_error(support::ErrorCode::Workspace, message, message);
}

FileError WorkspaceFileSystem::util_error_to_file_error(const support::Error& error, const std::string& path) {
    FileErrorCode code = FileErrorCode::Unknown;
    switch (error.code) {
    case support::ErrorCode::Workspace:
        code = FileErrorCode::PermissionDenied;
        break;
    case support::ErrorCode::Validation:
        code = FileErrorCode::Invalid;
        break;
    case support::ErrorCode::Cancelled:
        code = FileErrorCode::Aborted;
        break;
    case support::ErrorCode::ResourceLimit:
        code = FileErrorCode::ResourceLimit;
        break;
    default:
        break;
    }
    return FileError{code, error.message, std::string{path}};
}

} // namespace cch::harness
