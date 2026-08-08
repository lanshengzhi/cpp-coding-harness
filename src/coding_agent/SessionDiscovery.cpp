#include "SessionDiscovery.hpp"

#include "SessionPathPolicy.hpp"
#include "util/Json.hpp"
#include <cch/coding_agent/AgentConfigDir.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>

namespace cch::coding_agent::session_discovery {
namespace {

/// Upper bound for the bounded first-line header read (pi
/// `SESSION_HEADER_READ_BUFFER_SIZE` is 4 KiB for discovery; the C++ headers
/// are small, but a long cwd or custom metadata field must not be truncated
/// into a false parse failure).
constexpr std::size_t kMaxHeaderLineBytes = 64 * 1024;

[[nodiscard]] bool header_cwd_matches(
    const std::string& header_cwd,
    const std::filesystem::path& workspace) {
    if (header_cwd.empty()) {
        return false;
    }
    std::error_code header_ec;
    std::error_code workspace_ec;
    const auto header_canonical =
        std::filesystem::weakly_canonical(header_cwd, header_ec);
    const auto workspace_canonical =
        std::filesystem::weakly_canonical(workspace, workspace_ec);
    if (header_ec || workspace_ec) {
        return std::filesystem::path{header_cwd}.lexically_normal() ==
               workspace.lexically_normal();
    }
    return header_canonical == workspace_canonical;
}

/// Read the first line of a session file with a bound; nullopt on unreadable
/// files and over-limit lines (pi skips files whose discovery header cannot
/// be read).
[[nodiscard]] std::optional<std::string> read_first_line_bounded(
    const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::nullopt;
    }
    std::string line;
    line.reserve(4096);
    while (line.size() < kMaxHeaderLineBytes) {
        const int character = input.get();
        if (character == std::char_traits<char>::eof()) {
            break;
        }
        if (character == '\n') {
            break;
        }
        line.push_back(static_cast<char>(character));
    }
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    return line;
}

/// Extract `type`, `id`, and `cwd` from a session header line; nullopt when
/// the line is not a session header (pi filters null discovery headers).
[[nodiscard]] std::optional<SessionInfoLite> read_header_facts(
    const std::filesystem::path& path) {
    const auto line = read_first_line_bounded(path);
    if (!line || line->empty()) {
        return std::nullopt;
    }
    auto parsed = util::read_json<util::JsonValue>(*line);
    if (!parsed) {
        return std::nullopt;
    }
    const auto* object = parsed->get_if<util::JsonValue::object_t>();
    if (object == nullptr) {
        return std::nullopt;
    }
    const auto type = object->find("type");
    if (type == object->end() ||
        !type->second.holds<std::string>() ||
        type->second.get_string() != "session") {
        return std::nullopt;
    }
    const auto id = object->find("id");
    if (id == object->end() || !id->second.holds<std::string>()) {
        return std::nullopt;
    }
    std::string cwd;
    if (const auto cwd_field = object->find("cwd");
        cwd_field != object->end() && cwd_field->second.holds<std::string>()) {
        cwd = cwd_field->second.get_string();
    }

    std::error_code ec;
    SessionInfoLite info;
    info.path = path;
    info.id = id->second.get_string();
    info.cwd = std::move(cwd);
    info.modified = std::filesystem::last_write_time(path, ec);
    if (ec) {
        info.modified = std::filesystem::file_time_type{};
    }
    return info;
}

[[nodiscard]] std::vector<SessionInfoLite> list_directory_sessions(
    const std::filesystem::path& directory,
    const std::optional<std::filesystem::path>& cwd_filter) {
    std::vector<SessionInfoLite> sessions;
    std::error_code ec;
    if (!std::filesystem::is_directory(directory, ec)) {
        return sessions;
    }
    for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
        if (ec) {
            break;
        }
        if (!entry.is_regular_file(ec) ||
            entry.path().extension() != ".jsonl") {
            continue;
        }
        auto info = read_header_facts(entry.path());
        if (!info) {
            continue;
        }
        if (cwd_filter && !header_cwd_matches(info->cwd, *cwd_filter)) {
            continue;
        }
        sessions.push_back(std::move(*info));
    }
    std::sort(
        sessions.begin(), sessions.end(),
        [](const SessionInfoLite& first, const SessionInfoLite& second) {
            return first.modified > second.modified;
        });
    return sessions;
}

/// pi `resolveSessionPath` id matching: an exact id match wins, then the
/// first prefix match in list order (newest first).
[[nodiscard]] std::optional<SessionInfoLite> find_id_match(
    const std::vector<SessionInfoLite>& sessions,
    const std::string& arg) {
    auto match = std::find_if(
        sessions.begin(), sessions.end(),
        [&](const SessionInfoLite& session) { return session.id == arg; });
    if (match == sessions.end()) {
        match = std::find_if(
            sessions.begin(), sessions.end(),
            [&](const SessionInfoLite& session) {
                return session.id.starts_with(arg);
            });
    }
    if (match == sessions.end()) {
        return std::nullopt;
    }
    return *match;
}

} // namespace

std::vector<SessionInfoLite> list_sessions_in_directory(
    const std::filesystem::path& directory,
    const std::optional<std::filesystem::path>& cwd_filter) {
    return list_directory_sessions(directory, cwd_filter);
}

std::vector<SessionInfoLite> list_all_sessions(
    const std::filesystem::path& sessions_root,
    const std::optional<std::filesystem::path>& custom_directory) {
    if (custom_directory) {
        return list_directory_sessions(*custom_directory, std::nullopt);
    }
    if (sessions_root.empty()) {
        return {};
    }
    std::vector<SessionInfoLite> sessions;
    std::error_code ec;
    if (!std::filesystem::is_directory(sessions_root, ec)) {
        return sessions;
    }
    for (const auto& entry : std::filesystem::directory_iterator(sessions_root, ec)) {
        if (ec) {
            break;
        }
        if (!entry.is_directory(ec)) {
            continue;
        }
        auto nested = list_directory_sessions(entry.path(), std::nullopt);
        sessions.insert(
            sessions.end(),
            std::make_move_iterator(nested.begin()),
            std::make_move_iterator(nested.end()));
    }
    std::sort(
        sessions.begin(), sessions.end(),
        [](const SessionInfoLite& first, const SessionInfoLite& second) {
            return first.modified > second.modified;
        });
    return sessions;
}

std::optional<SessionInfoLite> find_most_recent_session(
    const std::filesystem::path& directory,
    const std::optional<std::filesystem::path>& cwd_filter) {
    auto sessions = list_directory_sessions(directory, cwd_filter);
    if (sessions.empty()) {
        return std::nullopt;
    }
    return sessions.front();
}

std::optional<std::string> invalid_session_id_reason(const std::string& id) {
    const auto is_alnum = [](unsigned char character) {
        return std::isalnum(character) != 0;
    };
    if (id.empty() || !is_alnum(static_cast<unsigned char>(id.front())) ||
        !is_alnum(static_cast<unsigned char>(id.back()))) {
        return std::string{
            "Session id must be non-empty, contain only alphanumeric "
            "characters, '-', '_', and '.', and start and end with an "
            "alphanumeric character"};
    }
    for (const unsigned char character : id) {
        if (!is_alnum(character) && character != '-' && character != '_' &&
            character != '.') {
            return std::string{
                "Session id must be non-empty, contain only alphanumeric "
                "characters, '-', '_', and '.', and start and end with an "
                "alphanumeric character"};
        }
    }
    return std::nullopt;
}

ResolvedSessionArg resolve_session_arg(
    const std::string& arg,
    const std::filesystem::path& workspace,
    const std::filesystem::path& local_directory,
    const std::optional<std::filesystem::path>& cwd_filter,
    const std::filesystem::path& sessions_root,
    const std::optional<std::filesystem::path>& custom_directory) {
    ResolvedSessionArg result;
    result.arg = arg;

    // Path-like arguments resolve to an exact path (pi resolveSessionPath:
    // contains '/' or '\' or ends with ".jsonl"; resolvePath semantics with
    // leading-'~' expansion against the workspace).
    if (arg.find('/') != std::string::npos ||
        arg.find('\\') != std::string::npos ||
        arg.ends_with(".jsonl")) {
        result.kind = SessionArgKind::Path;
        result.path = session_paths::resolve_session_path_value(
            arg, workspace, coding_agent::home_directory());
        return result;
    }

    // Local first: exact id, then prefix id (pi SessionManager.list + find).
    auto local = list_directory_sessions(local_directory, cwd_filter);
    if (auto match = find_id_match(local, arg); match) {
        result.kind = SessionArgKind::Local;
        result.path = std::move(match->path);
        return result;
    }

    // Global across projects: exact id, then prefix id.
    auto all = list_all_sessions(sessions_root, custom_directory);
    if (auto match = find_id_match(all, arg); match) {
        result.kind = SessionArgKind::Global;
        result.path = std::move(match->path);
        result.cwd = std::move(match->cwd);
        return result;
    }

    result.kind = SessionArgKind::NotFound;
    return result;
}

} // namespace cch::coding_agent::session_discovery
