#pragma once

#include "coding_agent/SessionDiscovery.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cch::coding_agent::tui {

/// pi `session-selector-search.ts` sort modes (the selector's "Sort:"
/// label renders `Relevance` as "Fuzzy", exactly like pi).
enum class SessionSortMode { Threaded, Recent, Relevance };

/// pi `NameFilter`: "all" shows every session; "named" only sessions with a
/// non-blank `session_info` name.
enum class SessionNameFilter { All, Named };

/// pi `ParsedSearchQuery`: either a `re:<pattern>` regex, or whitespace
/// tokens where `"quoted"` phrases match the whitespace-normalized text.
struct ParsedSessionQuery {
    bool regex_mode{false};
    /// The validated `re:` pattern (regex mode only; `match_session` compiles
    /// it — parse already rejected malformed patterns).
    std::optional<std::string> regex_pattern;
    /// Regex parse failure: the query matches nothing (pi returns []).
    std::optional<std::string> regex_error;
    struct Token {
        bool phrase{false};
        std::string value;
    };
    std::vector<Token> tokens;
    /// Unclosed quotes fall back to plain whitespace tokenization (pi).
    bool unclosed_quote{false};
};

/// pi `parseSearchQuery`: `re:<pattern>` enters regex mode; otherwise the
/// query splits into fuzzy tokens with `"..."` phrase support.
[[nodiscard]] ParsedSessionQuery parse_session_search_query(
    std::string_view query);

/// pi `hasSessionName`: a session carries a non-blank name.
[[nodiscard]] bool has_session_name(
    const session_discovery::SessionInfo& session);

/// pi `matchSession`: the search text is
/// `<id> <name?> <allMessagesText> <cwd>`; regex mode scores the first match
/// position, token mode sums per-token fuzzy/phrase scores.
struct SessionMatchResult {
    bool matches{false};
    /// Lower is better; only meaningful when matches is true.
    double score{0.0};
};

[[nodiscard]] SessionMatchResult match_session(
    const session_discovery::SessionInfo& session,
    const ParsedSessionQuery& query);

/// pi `filterAndSortSessions`: applies the name filter, then filters by the
/// query and sorts: `recent` keeps the incoming (newest-first) order,
/// `relevance` sorts by score ascending with the modification time as the
/// tie-break. An unparseable query yields an empty list.
[[nodiscard]] std::vector<session_discovery::SessionInfo>
filter_and_sort_sessions(
    std::vector<session_discovery::SessionInfo> sessions,
    std::string_view query,
    SessionSortMode sort_mode,
    SessionNameFilter name_filter);

} // namespace cch::coding_agent::tui
