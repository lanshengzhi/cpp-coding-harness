// #1044: `/export` builds a plain `TranscriptExportHandler` and runs it directly
// (InteractiveEngineHost.cpp:362). It borrowed `TaskScheduler::Handler` before
// 97a5aaf73, so this file drives the handler through the public header alone:
// no scheduler, no TaskStore. That independence is the property under test, and
// it is also what proves the retired SQLite subsystem was never on this path.

#include <cch/agent/harness/session/SessionStore.hpp>

#include "agent/harness/TranscriptExport.hpp"
#include "agent/harness/session/EntrySerializer.hpp"
#include "support/TempWorkspace.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stop_token>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

using namespace cch;

namespace {

harness::session::SessionMetadata metadata_for(const tests::TempWorkspace& workspace) {
    return harness::session::SessionMetadata{
            .session_id = "transcript-export-test",
            .created_at = "2026-07-05T00:00:00Z",
            .workspace = workspace.path(),
            .provider = "fake",
            .model = "fake-model",
    };
}

ai::MessageVariant user_message(std::string content) {
    return ai::MessageVariant{ai::user_text_message(std::move(content))};
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

/// The transcript the handler must write: one serialized wire line per entry.
/// Built independently from the same public serializer the handler uses, so a
/// handler that dropped, reordered, duplicated or blank-line-separated an entry
/// fails the comparison.
std::string expected_transcript(const harness::session::SessionStore& store) {
    const harness::session::EntrySerializer serializer;
    std::string transcript;
    for (const auto& entry : store.entries()) {
        auto line = serializer.serialize_entry(entry);
        REQUIRE(line.has_value());
        transcript += *line;
    }
    return transcript;
}

} // namespace

TEST_CASE("transcript export writes every session entry as one wire line per entry",
        "[agent][harness][export][issue1044][spec]") {
    tests::TempWorkspace workspace;
    const auto session_path = workspace.path() / "session.jsonl";
    const auto destination = workspace.path() / "exported.jsonl";

    auto created = harness::session::SessionStore::create_new(session_path, metadata_for(workspace));
    REQUIRE(created.has_value());
    auto store = std::move(*created);
    REQUIRE(store.append(user_message("first")).has_value());
    REQUIRE(store.append(user_message("second")).has_value());

    const auto expected = expected_transcript(store);
    // Two messages, exactly one newline each: an entry that carried a blank
    // line after it would double this count while leaving a non-empty check
    // green.
    REQUIRE(std::count(expected.begin(), expected.end(), '\n') == 2);

    auto exported = harness::transcript_export_handler(store, destination.string())({});
    REQUIRE(exported.has_value());
    CHECK(*exported == destination.string());

    REQUIRE(std::filesystem::exists(destination));
    const auto written = read_file(destination);
    CHECK(written == expected);
    CHECK(std::count(written.begin(), written.end(), '\n') == 2);
    // Known defect, tracked separately: `SessionTree` drops `SessionEntryKind::Header`
    // (SessionTree.cpp:17-22), so `entries()` never carries one and the export
    // therefore writes no session header line. The file is one wire line per
    // entry but not reloadable by `SessionStore::load`. Recorded here so the
    // gap stays visible rather than being implied fixed by this test.
    CHECK_FALSE(written.starts_with(R"({"type":"session")"));
}

TEST_CASE("transcript export overwrites the destination instead of appending on a second run",
        "[agent][harness][export][issue1044][spec]") {
    tests::TempWorkspace workspace;
    const auto session_path = workspace.path() / "session.jsonl";
    const auto destination = workspace.path() / "exported.jsonl";

    auto created = harness::session::SessionStore::create_new(session_path, metadata_for(workspace));
    REQUIRE(created.has_value());
    auto store = std::move(*created);
    REQUIRE(store.append(user_message("first")).has_value());
    REQUIRE(store.append(user_message("second")).has_value());

    auto handler = harness::transcript_export_handler(store, destination.string());
    REQUIRE(handler({}).has_value());
    const auto first_run = read_file(destination);
    REQUIRE(handler({}).has_value());

    // Replay is the crash-recovery contract the old scheduler case claimed: a
    // second run replaces the snapshot, so the file is byte-identical rather
    // than carrying the transcript twice.
    CHECK(read_file(destination) == first_run);
    CHECK(read_file(destination) == expected_transcript(store));
}

TEST_CASE("transcript export refuses a stop-requested run without writing the destination",
        "[agent][harness][export][issue1044][spec]") {
    tests::TempWorkspace workspace;
    const auto session_path = workspace.path() / "session.jsonl";
    const auto destination = workspace.path() / "exported.jsonl";

    auto created = harness::session::SessionStore::create_new(session_path, metadata_for(workspace));
    REQUIRE(created.has_value());
    auto store = std::move(*created);
    REQUIRE(store.append(user_message("first")).has_value());

    std::stop_source source;
    source.request_stop();
    auto exported = harness::transcript_export_handler(store, destination.string())(source.get_token());

    REQUIRE_FALSE(exported.has_value());
    CHECK(exported.error().code == support::ErrorCode::Validation);
    // The guard precedes the write, so an aborted run leaves no destination.
    std::error_code exists_ec;
    CHECK_FALSE(std::filesystem::exists(destination, exists_ec));
}
