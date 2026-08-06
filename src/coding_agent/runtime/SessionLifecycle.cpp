#include "SessionLifecycle.hpp"

#include "../../../include/cch/coding_agent/AgentConfigDir.hpp"
#include "../../../include/cch/harness/session/JsonlSessionStore.hpp"
#include "../SessionPathPolicy.hpp"
#include "util/UniqueFd.hpp"
#include "harness/session/InMemorySessionStore.hpp"

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
                "session directory contains parent traversal",
                "refusing to create a session directory through '..'"));
        }
        cursor /= component;
        const DWORD attributes = ::GetFileAttributesW(cursor.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            const auto code = ::GetLastError();
            if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
                continue;
            }
            return std::unexpected(session_error(
                "could not inspect session directory",
                cursor.string() + ": Windows error " + std::to_string(code)));
        }
        if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            return std::unexpected(session_error(
                "session directory contains a symlink or junction",
                "refusing to publish through reparse point: " + cursor.string()));
        }
    }
    return {};
}
#endif

/// Prepare one session directory chain. When tighten_existing is true the
/// final directory is always made owner-only (harness-owned default roots);
/// when false an existing final directory keeps its mode (custom override
/// directories are never chmodded) and only a newly created one is made
/// private.
[[nodiscard]] util::ExpectedVoid prepare_session_directory(
    const std::filesystem::path& path,
    bool tighten_existing) {
#if defined(__unix__) || defined(__APPLE__)
    int open_flags = O_RDONLY | O_DIRECTORY;
#ifdef O_CLOEXEC
    open_flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    open_flags |= O_NOFOLLOW;
#endif

    const auto start = path.is_absolute() ? path.root_path() : std::filesystem::path{"."};
    util::UniqueFd current(::open(start.c_str(), open_flags));
    if (!current) {
        return std::unexpected(session_error(
            "could not open session directory root",
            start.string() + ": " + std::strerror(errno)));
    }

    bool final_created = false;
    const auto relative = path.is_absolute() ? path.relative_path() : path;
    auto component = relative.begin();
    while (component != relative.end()) {
        if (*component == "." || component->empty()) {
            ++component;
            continue;
        }
        if (*component == "..") {
            return std::unexpected(session_error(
                "session directory contains parent traversal",
                "refusing to create a session directory through '..'"));
        }

        const auto next_component = std::next(component);
        const bool final_component = next_component == relative.end();
        const mode_t create_mode = final_component ? S_IRWXU : (S_IRWXU | S_IRWXG | S_IRWXO);
        if (::mkdirat(current.get(), component->c_str(), create_mode) != 0) {
            if (errno != EEXIST) {
                const auto reason = std::string{std::strerror(errno)};
                return std::unexpected(session_error(
                    "could not create session directory",
                    path.string() + ": " + reason));
            }
        } else if (final_component) {
            final_created = true;
        }

        util::UniqueFd next(::openat(current.get(), component->c_str(), open_flags));
        if (!next) {
            const auto reason = std::string{std::strerror(errno)};
            return std::unexpected(session_error(
                "session directory path contains a symlink or non-directory",
                component->string() + ": " + reason));
        }
        current = std::move(next);
        component = next_component;
    }

    if (tighten_existing || final_created) {
        if (::fchmod(current.get(), S_IRWXU) != 0) {
            const auto reason = std::string{std::strerror(errno)};
            return std::unexpected(session_error(
                "could not make session directory private",
                path.string() + ": " + reason));
        }
    }
    if (current.close() != 0) {
        return std::unexpected(session_error(
            "could not close session directory",
            path.string() + ": " + std::strerror(errno)));
    }
#elif defined(_WIN32)
    if (auto safe = reject_windows_reparse_components(path); !safe) {
        return safe;
    }
    std::error_code pre_ec;
    const bool existed_before = std::filesystem::exists(path, pre_ec);
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (ec) {
        return std::unexpected(session_error(
            "could not create session directory",
            path.string() + ": " + ec.message()));
    }
    if (auto safe = reject_windows_reparse_components(path); !safe) {
        return safe;
    }
    if (tighten_existing || !existed_before) {
        std::filesystem::permissions(
            path,
            std::filesystem::perms::owner_all,
            std::filesystem::perm_options::replace,
            ec);
        if (ec) {
            return std::unexpected(session_error(
                "could not make session directory private",
                path.string() + ": " + ec.message()));
        }
    }
#else
    std::filesystem::path cursor = path.root_path();
    bool existed_before = true;
    for (const auto& component : path.relative_path()) {
        if (component.empty() || component == ".") {
            continue;
        }
        if (component == "..") {
            return std::unexpected(session_error(
                "session directory contains parent traversal",
                "refusing to create a session directory through '..'"));
        }
        cursor /= component;
        std::error_code inspect_ec;
        const auto status = std::filesystem::symlink_status(cursor, inspect_ec);
        if (!inspect_ec && std::filesystem::is_symlink(status)) {
            return std::unexpected(session_error(
                "session directory contains a symlink",
                "refusing to publish through symlink: " + cursor.string()));
        }
        existed_before = existed_before && !inspect_ec && std::filesystem::exists(status);
    }

    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (ec) {
        return std::unexpected(session_error(
            "could not create session directory",
            path.string() + ": " + ec.message()));
    }
    if (tighten_existing || !existed_before) {
        std::filesystem::permissions(
            path,
            std::filesystem::perms::owner_all,
            std::filesystem::perm_options::replace,
            ec);
        if (ec) {
            return std::unexpected(session_error(
                "could not make session directory private",
                path.string() + ": " + ec.message()));
        }
    }
#endif
    return {};
}

/// Harness-owned default sessions roots and workspace-keyed directories are
/// always created or tightened to owner-only.
[[nodiscard]] util::ExpectedVoid prepare_default_directory(
    const std::filesystem::path& path) {
    return prepare_session_directory(path, true);
}

/// Custom override directories are created privately when missing; an
/// existing custom directory's mode is left untouched.
[[nodiscard]] util::ExpectedVoid prepare_custom_directory(
    const std::filesystem::path& path) {
    return prepare_session_directory(path, false);
}

[[nodiscard]] util::Expected<OpenSession> publish_new_jsonl_session(
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

[[nodiscard]] harness::session::SessionMetadata new_session_metadata(
    const std::filesystem::path& workspace,
    const session_paths::AutomaticSessionIdentity& identity,
    std::string provider,
    std::string model) {
    return harness::session::SessionMetadata{
        .session_id = identity.session_id,
        .created_at = identity.created_at,
        .workspace = workspace,
        .provider = std::move(provider),
        .model = std::move(model),
    };
}

} // namespace

util::Expected<OpenSession> publish_session(
    NewSessionPublication target,
    std::string provider,
    std::string model) {
    if (const auto* in_memory = std::get_if<InMemoryPublication>(&target)) {
        OpenSession session;
        session.workspace = in_memory->workspace;
        session.metadata = new_session_metadata(
            in_memory->workspace,
            session_paths::generate_automatic_session_identity(),
            std::move(provider),
            std::move(model));
        session.store = std::make_unique<harness::session::InMemorySessionStore>();
        return session;
    }

    const auto identity = session_paths::generate_automatic_session_identity();
    std::filesystem::path session_path;
    std::filesystem::path workspace;

    if (const auto* automatic = std::get_if<AutomaticPublication>(&target)) {
        workspace = automatic->workspace;
        const auto composed = automatic->directory_override
            ? session_paths::make_custom_automatic_session_target(
                  *automatic->directory_override,
                  automatic->workspace,
                  identity)
            : session_paths::make_automatic_session_target(
                  coding_agent::sessions_root_path(),
                  automatic->workspace,
                  identity);

        if (composed.sessions_root.empty()) {
            return std::unexpected(publication_error(
                composed.session_path,
                session_error(
                    "automatic session storage is unavailable",
                    "Agent Config Directory sessions root could not be resolved for workspace " +
                        composed.workspace.string())));
        }
        if (!composed.sessions_root.is_absolute()) {
            return std::unexpected(publication_error(
                composed.session_path,
                session_error(
                    "automatic session storage root must be absolute",
                    composed.custom_directory
                        ? "refusing relative session directory override: " +
                              composed.sessions_root.string()
                        : "refusing relative Agent Config Directory sessions root: " +
                              composed.sessions_root.string())));
        }

        if (composed.custom_directory) {
            // A CLI directory override replaces the whole automatic directory;
            // existing custom directories are created when missing but never
            // chmodded, matching the explicit-path ownership rule.
            if (auto prepared = prepare_custom_directory(composed.workspace_directory); !prepared) {
                return std::unexpected(publication_error(composed.session_path, prepared.error()));
            }
        } else {
            if (auto prepared = prepare_default_directory(composed.sessions_root); !prepared) {
                return std::unexpected(publication_error(composed.session_path, prepared.error()));
            }
            if (auto prepared = prepare_default_directory(composed.workspace_directory); !prepared) {
                return std::unexpected(publication_error(composed.session_path, prepared.error()));
            }
        }
        session_path = composed.session_path;
    } else {
        const auto& explicit_new = std::get<ExplicitNewPublication>(target);
        session_path = explicit_new.session_path;
        workspace = explicit_new.workspace;
    }

    return publish_new_jsonl_session(
        std::move(session_path),
        std::move(workspace),
        new_session_metadata(workspace, identity, std::move(provider), std::move(model)));
}

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
                "omit --workspace to use " + resumed->metadata.workspace.string() +
                    " or start a new session"));
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

util::Expected<OpenSession> publish_resume_session(
    const PreparedResumeTarget& target) {
    OpenSession session;
    session.workspace = target.workspace;
    session.metadata = target.resume.metadata;
    session.history = target.resume.history;
    session.context_model = target.resume.model;
    // A resumed `thinking_level_change` entry wins over the settings default
    // and DEFAULT_THINKING_LEVEL (pi sdk.ts `hasThinkingEntry`); without an
    // entry the runtime falls back to the settings default.
    session.context_thinking_level =
        target.resume.has_thinking_level_entry
            ? std::optional<std::string>{target.resume.thinking_level}
            : std::nullopt;
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

} // namespace cch::coding_agent::runtime
