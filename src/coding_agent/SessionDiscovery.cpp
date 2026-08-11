#include "SessionDiscovery.hpp"

#include "SessionPathPolicy.hpp"
#include "util/Json.hpp"
#include <cch/coding_agent/AgentConfigDir.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace cch::coding_agent::session_discovery {
namespace {

/// Upper bound for the bounded first-line header read (pi
/// `SESSION_HEADER_READ_BUFFER_SIZE` is 4 KiB for discovery; the C++ headers
/// are small, but a long cwd or custom metadata field must not be truncated
/// into a false parse failure).
constexpr std::size_t kMaxHeaderLineBytes = 64 * 1024;

/// Upper bound for one discovery entry line. pi's discovery scan reads the
/// full file; the C++ bounded read skips over-limit lines exactly like the
/// bounded header read (a pathological line must not stall discovery).
constexpr std::size_t kMaxEntryLineBytes = 4 * 1024 * 1024;

/// Parse `YYYY-MM-DDTHH:MM:SS(.sss)?Z` (the C++/pi header timestamp shape)
/// into epoch milliseconds; nullopt for any other shape (pi `new Date` also
/// yields NaN for unparseable strings, which the activity-time selection
/// ignores).
[[nodiscard]] std::optional<std::int64_t> parse_iso_timestamp_ms(
    std::string_view timestamp) {
    // YYYY-MM-DDTHH:MM:SS(.sss)?Z — digits and separators at fixed offsets
    // (§9.3: std::from_chars for number parsing).
    const auto digits = [&](std::size_t offset, std::size_t count) {
        if (offset + count > timestamp.size()) return std::optional<int>{};
        int value = 0;
        const auto [pointer, error] = std::from_chars(
            timestamp.data() + offset,
            timestamp.data() + offset + count,
            value);
        if (error != std::errc{} || pointer != timestamp.data() + offset + count) {
            return std::optional<int>{};
        }
        return std::optional<int>{value};
    };
    const auto year = digits(0, 4);
    const auto month = digits(5, 2);
    const auto day = digits(8, 2);
    const auto hour = digits(11, 2);
    const auto minute = digits(14, 2);
    const auto second = digits(17, 2);
    if (!year || !month || !day || !hour || !minute || !second ||
        timestamp.size() < 20 || timestamp[4] != '-' || timestamp[7] != '-' ||
        timestamp[10] != 'T' || timestamp[13] != ':' || timestamp[16] != ':' ||
        timestamp[19] != 'Z') {
        return std::nullopt;
    }
    std::int64_t millis = 0;
    if (timestamp.size() > 20) {
        if (timestamp[20] != '.') return std::nullopt;
        std::int64_t fraction = 0;
        std::size_t scale = 100;
        std::size_t count = 0;
        for (std::size_t index = 21; index < timestamp.size(); ++index) {
            const char character = timestamp[index];
            if (character < '0' || character > '9') return std::nullopt;
            if (count < 3) {
                fraction += (character - '0') * scale;
                scale /= 10;
            }
            ++count;
        }
        millis = fraction;
    }
    // Days-from-civil (Howard Hinnant's algorithm) for the proleptic
    // Gregorian calendar, then UTC time-of-day.
    const auto days_from_civil = [](int y, int m, int d) {
        y -= m <= 2;
        const int era = (y >= 0 ? y : y - 399) / 400;
        const unsigned yoe = static_cast<unsigned>(y - era * 400);
        const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
        const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        return era * 146097 + static_cast<int>(doe) - 719468;
    };
    const auto days = days_from_civil(*year, *month, *day);
    return (static_cast<std::int64_t>(days) * 86400 +
            *hour * 3600 + *minute * 60 + *second) *
               1000 +
        millis;
}

/// Convert an epoch-millisecond activity timestamp to the filesystem clock
/// so rich session facts sort and display like pi's `modified` Date.
[[nodiscard]] std::filesystem::file_time_type file_time_from_epoch_ms(
    std::int64_t epoch_ms) {
    const auto system = std::chrono::system_clock::time_point{
        std::chrono::milliseconds{epoch_ms}};
    return std::chrono::file_clock::from_sys(system);
}

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
    auto parsed = util::read_json(*line);
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

/// pi `buildSessionInfo`: parse one session file into selector-facing facts.
/// Returns nullopt for unreadable files and for files whose first entry is
/// not a session header. Malformed later lines are skipped (pi
/// `parseSessionEntryLine` returns null and the loop continues).
[[nodiscard]] std::optional<SessionInfo> read_session_info(
    const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::nullopt;
    }

    std::optional<SessionInfo> info;
    std::optional<std::int64_t> last_activity_ms;
    std::string pending_line;
    bool first_entry = true;

    const auto handle_line = [&](std::string line) {
        if (first_entry) {
            first_entry = false;
            auto parsed = util::read_json(line);
            if (!parsed) {
                return;
            }
            const auto* object = parsed->get_if<util::JsonValue::object_t>();
            if (object == nullptr) {
                return;
            }
            const auto type = object->find("type");
            if (type == object->end() || !type->second.holds<std::string>() ||
                type->second.get_string() != "session") {
                return;
            }
            const auto id = object->find("id");
            if (id == object->end() || !id->second.holds<std::string>()) {
                return;
            }
            SessionInfo built;
            built.path = path;
            built.id = id->second.get_string();
            if (const auto cwd_field = object->find("cwd");
                cwd_field != object->end() &&
                cwd_field->second.holds<std::string>()) {
                built.cwd = cwd_field->second.get_string();
            }
            if (const auto created_field = object->find("timestamp");
                created_field != object->end() &&
                created_field->second.holds<std::string>()) {
                built.created = created_field->second.get_string();
                if (auto created_ms = parse_iso_timestamp_ms(built.created)) {
                    last_activity_ms = *created_ms;
                }
            }
            if (const auto parent_field = object->find("parentSession");
                parent_field != object->end() &&
                parent_field->second.holds<std::string>()) {
                built.parent_session_path =
                    std::filesystem::path{parent_field->second.get_string()};
            }
            info = std::move(built);
            return;
        }

        if (!info) {
            return;
        }
        auto parsed = util::read_json(line);
        if (!parsed) {
            return;
        }
        const auto* object = parsed->get_if<util::JsonValue::object_t>();
        if (object == nullptr) {
            return;
        }
        const auto type = object->find("type");
        if (type == object->end() || !type->second.holds<std::string>()) {
            return;
        }
        const auto type_text = type->second.get_string();

        // Session name: the latest non-blank `session_info` entry wins (pi
        // uses the latest entry, including explicit clears).
        if (type_text == "session_info") {
            if (const auto name_field = object->find("name");
                name_field != object->end() &&
                name_field->second.holds<std::string>()) {
                auto name = name_field->second.get_string();
                const auto not_space = [](unsigned char character) {
                    return character != ' ' && character != '\t';
                };
                name.erase(
                    name.begin(),
                    std::find_if(name.begin(), name.end(), not_space));
                name.erase(
                    std::find_if(name.rbegin(), name.rend(), not_space).base(),
                    name.end());
                if (!name.empty()) {
                    info->name = std::move(name);
                }
            }
            return;
        }

        if (type_text != "message") {
            return;
        }
        ++info->message_count;

        // Activity time: the message's own numeric timestamp, else the entry
        // timestamp (pi `getMessageActivityTime`).
        std::optional<std::int64_t> entry_ms;
        if (const auto timestamp_field = object->find("timestamp");
            timestamp_field != object->end() &&
            timestamp_field->second.holds<double>()) {
            entry_ms = static_cast<std::int64_t>(
                timestamp_field->second.get<double>());
        } else if (const auto timestamp_field = object->find("timestamp");
                   timestamp_field != object->end() &&
                   timestamp_field->second.holds<std::string>()) {
            entry_ms = parse_iso_timestamp_ms(
                timestamp_field->second.get_string());
        }
        const auto message_field = object->find("message");
        if (message_field == object->end() ||
            !message_field->second.holds<util::JsonValue::object_t>()) {
            if (entry_ms) {
                last_activity_ms = std::max(
                    last_activity_ms.value_or(0), *entry_ms);
            }
            return;
        }
        const auto& message = message_field->second.get_object();
        const auto role = message.find("role");
        if (role == message.end() || !role->second.holds<std::string>()) {
            if (entry_ms) {
                last_activity_ms = std::max(
                    last_activity_ms.value_or(0), *entry_ms);
            }
            return;
        }
        const auto role_text = role->second.get_string();
        if (role_text != "user" && role_text != "assistant") {
            if (entry_ms) {
                last_activity_ms = std::max(
                    last_activity_ms.value_or(0), *entry_ms);
            }
            return;
        }
        if (const auto message_timestamp = message.find("timestamp");
            message_timestamp != message.end() &&
            message_timestamp->second.holds<double>()) {
            entry_ms = std::max(
                entry_ms.value_or(0),
                static_cast<std::int64_t>(
                    message_timestamp->second.get<double>()));
        }
        if (entry_ms) {
            last_activity_ms = std::max(
                last_activity_ms.value_or(0), *entry_ms);
        }

        // Message text: the string content or the joined text blocks (pi
        // `extractTextContent` joins blocks with a space; a blank result
        // contributes nothing).
        const auto content = message.find("content");
        std::string text;
        if (content != message.end()) {
            if (content->second.holds<std::string>()) {
                text = content->second.get_string();
            } else if (
                content->second.holds<util::JsonValue::array_t>()) {
                for (const auto& block : content->second.get_array()) {
                    if (!block.holds<util::JsonValue::object_t>()) {
                        continue;
                    }
                    const auto& block_object = block.get_object();
                    const auto block_type = block_object.find("type");
                    if (block_type == block_object.end() ||
                        !block_type->second.holds<std::string>() ||
                        block_type->second.get_string() != "text") {
                        continue;
                    }
                    const auto text_field = block_object.find("text");
                    if (text_field == block_object.end() ||
                        !text_field->second.holds<std::string>()) {
                        continue;
                    }
                    if (!text.empty()) {
                        text.push_back(' ');
                    }
                    text += text_field->second.get_string();
                }
            }
        }
        if (text.empty()) {
            return;
        }
        if (!info->all_messages_text.empty()) {
            info->all_messages_text.push_back(' ');
        }
        info->all_messages_text += text;
        if (info->first_message.empty() && role_text == "user") {
            info->first_message = text;
        }
    };

    std::string line;
    line.reserve(4096);
    while (true) {
        const int character = input.get();
        if (character == std::char_traits<char>::eof()) {
            if (!line.empty()) {
                handle_line(std::move(line));
            }
            break;
        }
        if (character == '\n') {
            if (!line.empty()) {
                handle_line(std::move(line));
            }
            line.clear();
            continue;
        }
        if (line.size() >= kMaxEntryLineBytes) {
            line.clear();
            // Skip the rest of the over-limit line.
            while (true) {
                const int skip = input.get();
                if (skip == std::char_traits<char>::eof() || skip == '\n') {
                    break;
                }
            }
            continue;
        }
        line.push_back(static_cast<char>(character));
    }

    if (!info) {
        return std::nullopt;
    }
    if (info->first_message.empty()) {
        info->first_message = "(no messages)";
    }
    std::error_code ec;
    const auto mtime = std::filesystem::last_write_time(path, ec);
    if (last_activity_ms) {
        info->modified = file_time_from_epoch_ms(*last_activity_ms);
    } else if (!ec) {
        info->modified = mtime;
    }
    return info;
}

/// pi `listSessionsFromDir`-style directory scan over rich session facts:
/// every `*.jsonl` file whose first entry parses as a session header,
/// newest modification time first, optionally cwd-filtered.
[[nodiscard]] std::vector<SessionInfo> list_directory_sessions_info(
    const std::filesystem::path& directory,
    const std::optional<std::filesystem::path>& cwd_filter) {
    std::vector<SessionInfo> sessions;
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
        auto info = read_session_info(entry.path());
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
        [](const SessionInfo& first, const SessionInfo& second) {
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

std::optional<SessionInfo> build_session_info(
    const std::filesystem::path& path) {
    return read_session_info(path);
}

std::vector<SessionInfo> list_sessions_info(
    const std::filesystem::path& directory,
    const std::optional<std::filesystem::path>& cwd_filter) {
    return list_directory_sessions_info(directory, cwd_filter);
}

std::vector<SessionInfo> list_all_sessions_info(
    const std::filesystem::path& sessions_root,
    const std::optional<std::filesystem::path>& custom_directory) {
    if (custom_directory) {
        return list_directory_sessions_info(*custom_directory, std::nullopt);
    }
    if (sessions_root.empty()) {
        return {};
    }
    std::vector<SessionInfo> sessions;
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
        auto nested = list_directory_sessions_info(entry.path(), std::nullopt);
        sessions.insert(
            sessions.end(),
            std::make_move_iterator(nested.begin()),
            std::make_move_iterator(nested.end()));
    }
    std::sort(
        sessions.begin(), sessions.end(),
        [](const SessionInfo& first, const SessionInfo& second) {
            return first.modified > second.modified;
        });
    return sessions;
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
