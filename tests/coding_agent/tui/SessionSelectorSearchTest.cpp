// P13: the session-selector search seams (pi `session-selector-search.test.ts`
// mirror): regex / quoted-phrase / fuzzy matching, sort modes, and the named
// filter over `SessionInfo` values.

#include <catch2/catch_test_macros.hpp>

#include "coding_agent/tui/SessionSelectorSearch.hpp"

#include <chrono>
#include <filesystem>
#include <string>

using namespace cch;
namespace tui = cch::coding_agent::tui;
namespace session_discovery = cch::coding_agent::session_discovery;

namespace {

cch::coding_agent::session_discovery::SessionInfo make_session(
    std::string id,
    std::filesystem::file_time_type modified,
    std::string all_messages_text,
    std::optional<std::string> name = std::nullopt,
    std::string cwd = "") {
    cch::coding_agent::session_discovery::SessionInfo session;
    session.path = std::filesystem::path{"/tmp/"} / (id + ".jsonl");
    session.id = std::move(id);
    session.cwd = std::move(cwd);
    session.name = std::move(name);
    session.modified = modified;
    session.message_count = 1;
    session.first_message = "(no messages)";
    session.all_messages_text = std::move(all_messages_text);
    return session;
}

std::filesystem::file_time_type at_hours(int hours) {
    return std::filesystem::file_time_type::clock::now() +
        std::chrono::hours(hours);
}

} // namespace

TEST_CASE("session search filters by quoted phrase with whitespace normalization", "[coding_agent][tui][session-selector-search][issue409]") {
    std::vector<cch::coding_agent::session_discovery::SessionInfo> sessions{
        make_session("a", at_hours(-1), "node\n\n   cve was discussed"),
        make_session("b", at_hours(-2), "node something else"),
    };
    const auto result = tui::filter_and_sort_sessions(
        std::move(sessions), "\"node cve\"", tui::SessionSortMode::Recent,
        tui::SessionNameFilter::All);
    REQUIRE(result.size() == 1);
    CHECK(result[0].id == "a");
}

TEST_CASE("session search filters by regex and is case-insensitive", "[coding_agent][tui][session-selector-search][issue409]") {
    std::vector<cch::coding_agent::session_discovery::SessionInfo> sessions{
        make_session("a", at_hours(-1), "Brave is great"),
        make_session("b", at_hours(-2), "bravery is not the same"),
    };
    const auto result = tui::filter_and_sort_sessions(
        std::move(sessions), "re:\\bbrave\\b", tui::SessionSortMode::Recent,
        tui::SessionNameFilter::All);
    REQUIRE(result.size() == 1);
    CHECK(result[0].id == "a");
}

TEST_CASE("session search recent sort preserves input order", "[coding_agent][tui][session-selector-search][issue409]") {
    std::vector<cch::coding_agent::session_discovery::SessionInfo> sessions{
        make_session("newer", at_hours(-1), "brave"),
        make_session("older", at_hours(-2), "brave"),
        make_session("nomatch", at_hours(-3), "something else"),
    };
    const auto result = tui::filter_and_sort_sessions(
        std::move(sessions), "brave", tui::SessionSortMode::Recent,
        tui::SessionNameFilter::All);
    REQUIRE(result.size() == 2);
    CHECK(result[0].id == "newer");
    CHECK(result[1].id == "older");
}

TEST_CASE("session search relevance sort orders by score and tie-breaks by modified desc", "[coding_agent][tui][session-selector-search][issue409]") {
    std::vector<cch::coding_agent::session_discovery::SessionInfo> sessions{
        make_session("late", at_hours(-1), "xxxx brave"),
        make_session("early", at_hours(-2), "brave xxxx"),
    };
    const auto result1 = tui::filter_and_sort_sessions(
        std::move(sessions), "brave", tui::SessionSortMode::Relevance,
        tui::SessionNameFilter::All);
    REQUIRE(result1.size() == 2);
    CHECK(result1[0].id == "early");
    CHECK(result1[1].id == "late");

    std::vector<cch::coding_agent::session_discovery::SessionInfo> tie_sessions{
        make_session("newer", at_hours(-1), "brave"),
        make_session("older", at_hours(-2), "brave"),
    };
    const auto result2 = tui::filter_and_sort_sessions(
        std::move(tie_sessions), "brave", tui::SessionSortMode::Relevance,
        tui::SessionNameFilter::All);
    REQUIRE(result2.size() == 2);
    CHECK(result2[0].id == "newer");
    CHECK(result2[1].id == "older");
}

TEST_CASE("session search returns empty for invalid regex and empty for unclosed quotes", "[coding_agent][tui][session-selector-search][issue409]") {
    std::vector<cch::coding_agent::session_discovery::SessionInfo> sessions{
        make_session("a", at_hours(-1), "brave"),
    };
    const auto invalid = tui::filter_and_sort_sessions(
        std::move(sessions), "re:[", tui::SessionSortMode::Recent,
        tui::SessionNameFilter::All);
    CHECK(invalid.empty());
}

TEST_CASE("session search fuzzy tokens match and unclosed quotes fall back to tokens", "[coding_agent][tui][session-selector-search][issue409]") {
    std::vector<cch::coding_agent::session_discovery::SessionInfo> sessions{
        make_session("a", at_hours(-1), "node cve discussion"),
        make_session("b", at_hours(-2), "unrelated"),
    };
    // Fuzzy token "ndcve" matches "node cve" subsequence-wise.
    const auto fuzzy = tui::filter_and_sort_sessions(
        std::move(sessions), "ndcve", tui::SessionSortMode::Recent,
        tui::SessionNameFilter::All);
    REQUIRE(fuzzy.size() == 1);
    CHECK(fuzzy[0].id == "a");

    // An unclosed quote falls back to plain whitespace tokenization (pi keeps
    // the quote character in the token, and every token must match, so the
    // quoted part itself never matches).
    std::vector<cch::coding_agent::session_discovery::SessionInfo> sessions2{
        make_session("a", at_hours(-1), "node cve discussion"),
        make_session("b", at_hours(-2), "node elsewhere"),
    };
    const auto unclosed = tui::filter_and_sort_sessions(
        std::move(sessions2), "node \"cve", tui::SessionSortMode::Recent,
        tui::SessionNameFilter::All);
    CHECK(unclosed.empty());

    // A leading unclosed quote produces a token that cannot match (pi's
    // whitespace split keeps the quote).
    std::vector<cch::coding_agent::session_discovery::SessionInfo> sessions3{
        make_session("a", at_hours(-1), "node cve discussion"),
    };
    const auto quote_only = tui::filter_and_sort_sessions(
        std::move(sessions3), "\"node", tui::SessionSortMode::Recent,
        tui::SessionNameFilter::All);
    CHECK(quote_only.empty());
}

TEST_CASE("session search named filter keeps only named sessions", "[coding_agent][tui][session-selector-search][issue409]") {
    std::vector<cch::coding_agent::session_discovery::SessionInfo> sessions{
        make_session("named", at_hours(-1), "hello", std::string{"My Session"}),
        make_session("blank-name", at_hours(-2), "hello", std::string{"   "}),
        make_session("unnamed", at_hours(-3), "hello"),
    };
    CHECK(tui::has_session_name(sessions[0]));
    CHECK_FALSE(tui::has_session_name(sessions[1]));
    CHECK_FALSE(tui::has_session_name(sessions[2]));

    const auto named = tui::filter_and_sort_sessions(
        std::move(sessions), "", tui::SessionSortMode::Recent,
        tui::SessionNameFilter::Named);
    REQUIRE(named.size() == 1);
    CHECK(named[0].id == "named");
}

TEST_CASE("session search matches across id, name, messages, and cwd", "[coding_agent][tui][session-selector-search][issue409]") {
    std::vector<cch::coding_agent::session_discovery::SessionInfo> sessions{
        make_session("abc-123", at_hours(-1), "message text", std::nullopt, "/work/project-x"),
    };
    const auto by_id = tui::filter_and_sort_sessions(
        std::move(sessions), "abc-123", tui::SessionSortMode::Recent,
        tui::SessionNameFilter::All);
    REQUIRE(by_id.size() == 1);

    std::vector<cch::coding_agent::session_discovery::SessionInfo> by_cwd{
        make_session("s1", at_hours(-1), "text", std::nullopt, "/work/project-x"),
    };
    const auto cwd_match = tui::filter_and_sort_sessions(
        std::move(by_cwd), "project-x", tui::SessionSortMode::Recent,
        tui::SessionNameFilter::All);
    REQUIRE(cwd_match.size() == 1);
}
