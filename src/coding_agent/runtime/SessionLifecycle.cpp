#include "SessionLifecycle.hpp"

#include "../../../include/cch/harness/session/JsonlSessionStore.hpp"

#include <cerrno>
#include <cstring>
#include <iterator>
#include <memory>
#include <string>
#include <system_error>
#include <utility>

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

namespace cch::coding_agent::runtime {
namespace {

bool same_workspace(const std::filesystem::path& first, const std::filesystem::path& second) {
    std::error_code first_ec;
    std::error_code second_ec;
    auto first_canonical = std::filesystem::weakly_canonical(first, first_ec);
    auto second_canonical = std::filesystem::weakly_canonical(second, second_ec);
    if (first_ec || second_ec) {
        return first.lexically_normal() == second.lexically_normal();
    }
    return first_canonical == second_canonical;
}

[[nodiscard]] util::Error session_error(std::string message, std::string detail = {}) {
    return util::make_error(util::ErrorCode::Session, std::move(message), std::move(detail));
}

[[nodiscard]] std::string underlying_reason(const util::Error& error) {
    if (error.detail.empty() || error.detail == error.message) {
        return error.message;
    }
    return error.message + ": " + error.detail;
}

[[nodiscard]] util::Error publication_error(
    const std::filesystem::path& attempted_target,
    const util::Error& underlying) {
    const auto display_target = attempted_target.empty()
        ? std::string{"<unresolved agent config sessions root>"}
        : attempted_target.string();
    return session_error(
        "could not publish session",
        "attempted target: " + display_target + "; " + underlying_reason(underlying));
}

#if defined(_WIN32)
[[nodiscard]] util::ExpectedVoid reject_windows_reparse_components(
    const std::filesystem::path& path) {
    std::filesystem::path cursor = path.root_path();
    for (const auto& component : path.relative_path()) {
        if (component.empty() || component == ".") {
            continue;
        }
        if (component == "..") {
            return std::unexpected(session_error(
                "default session directory contains parent traversal",
                "refusing to create a default session directory through '..'"));
        }
        cursor /= component;
        const DWORD attributes = ::GetFileAttributesW(cursor.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            const auto code = ::GetLastError();
            if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
                continue;
            }
            return std::unexpected(session_error(
                "could not inspect default session directory",
                cursor.string() + ": Windows error " + std::to_string(code)));
        }
        if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            return std::unexpected(session_error(
                "default session directory contains a symlink or junction",
                "refusing to publish through reparse point: " + cursor.string()));
        }
    }
    return {};
}
#endif

[[nodiscard]] util::ExpectedVoid prepare_default_directory(
    const std::filesystem::path& path) {
#if defined(__unix__) || defined(__APPLE__)
    int open_flags = O_RDONLY | O_DIRECTORY;
#ifdef O_CLOEXEC
    open_flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    open_flags |= O_NOFOLLOW;
#endif

    const auto start = path.is_absolute() ? path.root_path() : std::filesystem::path{"."};
    int current = ::open(start.c_str(), open_flags);
    if (current == -1) {
        return std::unexpected(session_error(
            "could not open default session directory root",
            start.string() + ": " + std::strerror(errno)));
    }

    const auto relative = path.is_absolute() ? path.relative_path() : path;
    auto component = relative.begin();
    while (component != relative.end()) {
        if (*component == "." || component->empty()) {
            ++component;
            continue;
        }
        if (*component == "..") {
            ::close(current);
            return std::unexpected(session_error(
                "default session directory contains parent traversal",
                "refusing to create a default session directory through '..'"));
        }

        const auto next_component = std::next(component);
        const bool final_component = next_component == relative.end();
        const mode_t create_mode = final_component ? S_IRWXU : (S_IRWXU | S_IRWXG | S_IRWXO);
        if (::mkdirat(current, component->c_str(), create_mode) != 0 && errno != EEXIST) {
            const auto reason = std::string{std::strerror(errno)};
            ::close(current);
            return std::unexpected(session_error(
                "could not create default session directory",
                path.string() + ": " + reason));
        }

        const int next = ::openat(current, component->c_str(), open_flags);
        if (next == -1) {
            const auto reason = std::string{std::strerror(errno)};
            ::close(current);
            return std::unexpected(session_error(
                "default session path contains a symlink or non-directory",
                component->string() + ": " + reason));
        }
        ::close(current);
        current = next;
        component = next_component;
    }

    if (::fchmod(current, S_IRWXU) != 0) {
        const auto reason = std::string{std::strerror(errno)};
        ::close(current);
        return std::unexpected(session_error(
            "could not make default session directory private",
            path.string() + ": " + reason));
    }
    if (::close(current) != 0) {
        return std::unexpected(session_error(
            "could not close default session directory",
            path.string() + ": " + std::strerror(errno)));
    }
#elif defined(_WIN32)
    if (auto safe = reject_windows_reparse_components(path); !safe) {
        return safe;
    }
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (ec) {
        return std::unexpected(session_error(
            "could not create default session directory",
            path.string() + ": " + ec.message()));
    }
    if (auto safe = reject_windows_reparse_components(path); !safe) {
        return safe;
    }
    std::filesystem::permissions(
        path,
        std::filesystem::perms::owner_all,
        std::filesystem::perm_options::replace,
        ec);
    if (ec) {
        return std::unexpected(session_error(
            "could not make default session directory private",
            path.string() + ": " + ec.message()));
    }
#else
    std::filesystem::path cursor = path.root_path();
    for (const auto& component : path.relative_path()) {
        if (component.empty() || component == ".") {
            continue;
        }
        if (component == "..") {
            return std::unexpected(session_error(
                "default session directory contains parent traversal",
                "refusing to create a default session directory through '..'"));
        }
        cursor /= component;
        std::error_code inspect_ec;
        const auto status = std::filesystem::symlink_status(cursor, inspect_ec);
        if (!inspect_ec && std::filesystem::is_symlink(status)) {
            return std::unexpected(session_error(
                "default session directory contains a symlink",
                "refusing to publish through symlink: " + cursor.string()));
        }
    }

    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (ec) {
        return std::unexpected(session_error(
            "could not create default session directory",
            path.string() + ": " + ec.message()));
    }
    std::filesystem::permissions(
        path,
        std::filesystem::perms::owner_all,
        std::filesystem::perm_options::replace,
        ec);
    if (ec) {
        return std::unexpected(session_error(
            "could not make default session directory private",
            path.string() + ": " + ec.message()));
    }
#endif
    return {};
}

[[nodiscard]] util::ExpectedVoid validate_automatic_target(
    const session_paths::AutomaticSessionTarget& target) {
    if (target.sessions_root.empty()) {
        return std::unexpected(session_error(
            "automatic session storage is unavailable",
            "Agent Config Directory sessions root could not be resolved for workspace " +
                target.workspace.string()));
    }
    if (!target.sessions_root.is_absolute()) {
        return std::unexpected(session_error(
            "automatic session storage root must be absolute",
            "refusing relative Agent Config Directory sessions root: " +
                target.sessions_root.string()));
    }
    const auto expected_directory =
        target.sessions_root / session_paths::encode_workspace_key(target.workspace);
    const auto expected_path =
        expected_directory / session_paths::automatic_session_filename(target.identity);
    if (target.workspace_directory != expected_directory || target.session_path != expected_path) {
        return std::unexpected(session_error(
            "automatic session target does not match path policy",
            "derive the target through make_automatic_session_target"));
    }
    return {};
}

} // namespace

util::Expected<PreparedResumeTarget> prepare_resume_target(
    std::filesystem::path resume_path,
    std::filesystem::path explicit_workspace,
    bool workspace_explicit) {
    auto resumed = harness::session::resume_session(resume_path);
    if (!resumed) {
        return std::unexpected(resumed.error());
    }

    std::filesystem::path workspace = explicit_workspace;
    if (!resumed->metadata.workspace.empty()) {
        if (workspace_explicit && !same_workspace(workspace, resumed->metadata.workspace)) {
            return std::unexpected(util::make_error(
                util::ErrorCode::Session,
                "resume workspace does not match session metadata",
                "resume workspace does not match session metadata; omit --workspace to use " +
                    resumed->metadata.workspace.string() + " or start a new session"));
        }
        if (!workspace_explicit) {
            workspace = resumed->metadata.workspace;
        }
    }

    return PreparedResumeTarget{
        .resume_path = std::move(resume_path),
        .workspace = std::move(workspace),
        .resume = std::move(*resumed),
    };
}

util::Expected<OpenSession> publish_new_session(
    std::filesystem::path session_path,
    std::filesystem::path workspace,
    harness::session::SessionMetadata metadata) {
    OpenSession session;
    session.workspace = std::move(workspace);
    session.metadata = std::move(metadata);

    auto created = harness::session::JsonlSessionStore::create_new(session_path, session.metadata);
    if (!created) {
        return std::unexpected(publication_error(session_path, created.error()));
    }
    session.store = std::make_unique<harness::session::JsonlSessionStore>(std::move(*created));
    return session;
}

util::Expected<OpenSession> publish_automatic_session(
    const session_paths::AutomaticSessionTarget& target,
    std::string provider,
    std::string model) {
    if (auto valid = validate_automatic_target(target); !valid) {
        return std::unexpected(publication_error(target.session_path, valid.error()));
    }
    if (auto prepared = prepare_default_directory(target.sessions_root); !prepared) {
        return std::unexpected(publication_error(target.session_path, prepared.error()));
    }
    if (auto prepared = prepare_default_directory(target.workspace_directory); !prepared) {
        return std::unexpected(publication_error(target.session_path, prepared.error()));
    }

    harness::session::SessionMetadata metadata{
        target.identity.session_id,
        target.identity.created_at,
        target.workspace,
        std::move(provider),
        std::move(model),
    };
    return publish_new_session(target.session_path, target.workspace, std::move(metadata));
}

util::Expected<OpenSession> publish_resume_session(
    const PreparedResumeTarget& target) {
    OpenSession session;
    session.workspace = target.workspace;
    session.metadata = target.resume.metadata;
    session.history = target.resume.history;
    session.context_model = target.resume.model;
    session.context_thinking_level = target.resume.thinking_level;
    session.topology = target.resume.topology;
    session.stored_provider = session.metadata.provider;
    session.stored_model = session.metadata.model;

    auto opened = harness::session::JsonlSessionStore::open_existing(target.resume_path);
    if (!opened) {
        return std::unexpected(opened.error());
    }
    session.store = std::make_unique<harness::session::JsonlSessionStore>(std::move(*opened));
    return session;
}

util::Expected<OpenSession> open_session(SessionOpenRequest request) {
    if (!request.resume_path.empty()) {
        auto prepared = prepare_resume_target(
            std::move(request.resume_path),
            std::move(request.workspace),
            request.workspace_explicit);
        if (!prepared) {
            return std::unexpected(prepared.error());
        }
        return publish_resume_session(*prepared);
    }

    harness::session::SessionMetadata metadata{
        std::move(request.session_id),
        std::move(request.created_at),
        request.workspace,
        std::move(request.provider),
        std::move(request.model),
    };
    return publish_new_session(
        std::move(request.session_path),
        std::move(request.workspace),
        std::move(metadata));
}

} // namespace cch::coding_agent::runtime
