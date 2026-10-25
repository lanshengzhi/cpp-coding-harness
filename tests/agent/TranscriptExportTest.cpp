// #1044: `/export` builds a plain `TranscriptExportHandler` and runs it directly
// (InteractiveEngineHost.cpp:362). It borrowed `TaskScheduler::Handler` before
// 97a5aaf73, so this file drives the handler through the public header alone:
// no scheduler, no TaskStore. That independence is the property under test, and
// it is also what proves the retired SQLite subsystem was never on this path.
//
// The written file is pi v1.3.0 `serializeSessionBranch`: the session header,
// then the current branch in root-to-leaf order with every parentId rewritten
// to the previous written entry's id, joined by "\n" with one trailing "\n".

#include <cch/agent/harness/session/SessionStore.hpp>

#include "agent/harness/TranscriptExport.hpp"
#include "agent/harness/session/EntrySerializer.hpp"
#include "support/Json.hpp"
#include "support/TempWorkspace.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
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

/// The transcript the handler must write: the header line, then one serialized
/// wire line per branch entry in root-to-leaf order, each chained to the
/// previous entry. Built independently from the same public serializer the
/// handler uses, so a handler that dropped, reordered, duplicated, or
/// blank-line-separated a line fails the comparison.
std::string expected_transcript(const harness::session::SessionStore& store) {
    const harness::session::EntrySerializer serializer;
    auto header = serializer.serialize_header(store.metadata());
    REQUIRE(header.has_value());
    // `serialize_entry` terminates its line but `serialize_header` does not, so
    // the header's terminator is added here; pi joins its whole entry array
    // with "\n" and appends one trailing "\n".
    std::vector<std::string> lines{*header};
    // `get_branch` is leaf-to-root; the export writes pi's root-to-leaf order.
    auto branch = store.get_branch();
    std::reverse(branch.begin(), branch.end());
    std::optional<std::string> parent_id;
    for (const auto& entry : branch) {
        auto chained = entry;
        // pi writes `parentId: this.leafId`, so the first line is an explicit
        // `null` root and every later line names the entry written before it.
        chained.parent_id = cch::harness::session::parent_id_from_optional(parent_id);
        auto line = serializer.serialize_entry(chained);
        REQUIRE(line.has_value());
        REQUIRE(line->ends_with('\n'));
        lines.push_back(line->substr(0, line->size() - 1));
        parent_id = entry.entry_id;
    }
    std::string transcript;
    for (const auto& line : lines) {
        transcript += line;
        transcript += '\n';
    }
    return transcript;
}

/// The JSON object of one wire line; REQUIREs the line is a JSON object.
support::JsonValue::object_t parse_object(std::string_view line) {
    auto parsed = support::read_json(line);
    REQUIRE(parsed.has_value());
    const auto* object = parsed->get_if<support::JsonValue::object_t>();
    REQUIRE(object != nullptr);
    return *object;
}

/// `nullopt` for a JSON null or an absent member, the string otherwise.
std::optional<std::string> optional_string_member(const support::JsonValue::object_t& object, const std::string& key) {
    const auto found = object.find(key);
    if (found == object.end()) return std::nullopt;
    if (const auto* text = found->second.get_if<std::string>()) return *text;
    return std::nullopt;
}

std::string string_member(const support::JsonValue::object_t& object, const std::string& key) {
    return optional_string_member(object, key).value_or(std::string{});
}

std::optional<double> number_member(const support::JsonValue::object_t& object, const std::string& key) {
    const auto found = object.find(key);
    if (found == object.end()) return std::nullopt;
    if (const auto* number = found->second.get_if<double>()) return *number;
    return std::nullopt;
}

/// How many times `"key":` appears in one wire line — the field is emitted
/// exactly once, so a rewrite that added a second copy is visible here.
std::size_t count_key_occurrences(std::string_view line, std::string_view key) {
    const std::string needle = "\"" + std::string{key} + "\":";
    std::size_t count = 0;
    for (auto found = line.find(needle); found != std::string_view::npos;
            found = line.find(needle, found + needle.size())) {
        ++count;
    }
    return count;
}

/// The file's lines without their terminators; a blank line survives as an
/// empty string, so a double-newline regression shows up as an extra element.
std::vector<std::string> split_lines(std::string_view text) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start < text.size()) {
        const auto end = text.find('\n', start);
        if (end == std::string_view::npos) {
            lines.emplace_back(text.substr(start));
            break;
        }
        lines.emplace_back(text.substr(start, end - start));
        start = end + 1;
    }
    return lines;
}

} // namespace

TEST_CASE("transcript export writes the session header and every branch entry as one wire line per entry",
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
    // The header plus two messages, exactly one newline each: a line that
    // carried a blank line after it would raise this count while leaving a
    // non-empty check green.
    REQUIRE(std::count(expected.begin(), expected.end(), '\n') == 3);

    auto exported = harness::transcript_export_handler(store.metadata(), store.get_branch(), destination.string())({});
    REQUIRE(exported.has_value());
    CHECK(*exported == destination.string());

    REQUIRE(std::filesystem::exists(destination));
    const auto written = read_file(destination);
    CHECK(written == expected);
    CHECK(std::count(written.begin(), written.end(), '\n') == 3);
    const auto lines = split_lines(written);
    REQUIRE(lines.size() == 3);
    CHECK(string_member(parse_object(lines.front()), "type") == "session");
    CHECK(string_member(parse_object(lines[1]), "type") == "message");
    CHECK(string_member(parse_object(lines[2]), "type") == "message");
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

    auto handler = harness::transcript_export_handler(store.metadata(), store.get_branch(), destination.string());
    REQUIRE(handler({}).has_value());
    const auto first_run = read_file(destination);
    REQUIRE(handler({}).has_value());

    // Replay is the crash-recovery contract the old scheduler case claimed: a
    // second run replaces the snapshot, so the file is byte-identical rather
    // than carrying the transcript twice.
    CHECK(read_file(destination) == first_run);
    CHECK(read_file(destination) == expected_transcript(store));
}

TEST_CASE("transcript export owns its session snapshot after the store is gone",
        "[agent][harness][export][issue1067][spec]") {
    tests::TempWorkspace workspace;
    const auto destination = workspace.path() / "exported.jsonl";
    std::string expected;
    auto handler = [&] {
        const auto session_path = workspace.path() / "session.jsonl";
        auto created = harness::session::SessionStore::create_new(session_path, metadata_for(workspace));
        REQUIRE(created.has_value());
        auto store = std::move(*created);
        REQUIRE(store.append(user_message("first")).has_value());
        REQUIRE(store.append(user_message("second")).has_value());
        expected = expected_transcript(store);

        // The factory receives passive values. Returning it destroys the
        // SessionStore before the export runs.
        return harness::transcript_export_handler(store.metadata(), store.get_branch(), destination.string());
    }();

    auto exported = handler({});

    REQUIRE(exported.has_value());
    CHECK(*exported == destination.string());
    CHECK(read_file(destination) == expected);
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
    auto exported = harness::transcript_export_handler(store.metadata(), store.get_branch(), destination.string())(
            source.get_token());

    REQUIRE_FALSE(exported.has_value());
    CHECK(exported.error().code == support::ErrorCode::Validation);
    // The guard precedes the write, so an aborted run leaves no destination.
    std::error_code exists_ec;
    CHECK_FALSE(std::filesystem::exists(destination, exists_ec));
}

TEST_CASE("transcript export begins with the session header line", "[agent][harness][export][issue1044][spec]") {
    tests::TempWorkspace workspace;
    const auto session_path = workspace.path() / "session.jsonl";
    const auto destination = workspace.path() / "exported.jsonl";
    const auto metadata = metadata_for(workspace);

    auto created = harness::session::SessionStore::create_new(session_path, metadata);
    REQUIRE(created.has_value());
    auto store = std::move(*created);
    REQUIRE(store.append(user_message("first")).has_value());
    REQUIRE(store.append(user_message("second")).has_value());

    REQUIRE(harness::transcript_export_handler(store.metadata(), store.get_branch(), destination.string())({})
                    .has_value());

    // pi `serializeSessionBranch` pushes the SessionHeader before any entry, so
    // the first line is the header and not the first transcript entry. A file
    // without it is unloadable (`SessionStore::load` reports "session header is
    // missing"), which is the property the next case asserts directly.
    const auto lines = split_lines(read_file(destination));
    REQUIRE(lines.size() == 3);
    const auto header = parse_object(lines.front());
    CHECK(string_member(header, "type") == "session");
    CHECK(number_member(header, "version") == std::optional<double>{3.0});
    CHECK(string_member(header, "id") == metadata.session_id);
    CHECK(string_member(header, "cwd") == metadata.workspace.string());
    CHECK_FALSE(string_member(header, "timestamp").empty());
    // A header carries no branch parent: the chain starts at the first entry.
    CHECK_FALSE(header.contains("parentId"));
    // The header is written exactly once; the next line is already an entry.
    CHECK(string_member(parse_object(lines[1]), "type") == "message");
}

TEST_CASE("transcript export round-trips through Session Store load", "[agent][harness][export][issue1044][spec]") {
    tests::TempWorkspace workspace;
    const auto session_path = workspace.path() / "session.jsonl";
    const auto destination = workspace.path() / "exported.jsonl";

    auto created = harness::session::SessionStore::create_new(session_path, metadata_for(workspace));
    REQUIRE(created.has_value());
    auto store = std::move(*created);
    REQUIRE(store.append(user_message("first")).has_value());
    REQUIRE(store.append(user_message("second")).has_value());
    const auto first_id = store.entries().front().entry_id;

    REQUIRE(harness::transcript_export_handler(store.metadata(), store.get_branch(), destination.string())({})
                    .has_value());

    // The exported file is a session file, not just a transcript dump: it must
    // reload through the same reader the resume flow uses.
    auto reloaded = harness::session::SessionStore::load(destination);
    REQUIRE(reloaded.has_value());
    CHECK(reloaded->metadata.session_id == store.metadata().session_id);
    CHECK(reloaded->metadata.workspace == store.metadata().workspace);

    const auto branch = store.get_branch();
    REQUIRE(branch.size() == 2);
    REQUIRE(reloaded->entries.size() == 3);
    CHECK(reloaded->entries.front().kind == harness::session::SessionEntryKind::Header);

    // Each reloaded entry is the exported line: same id, same rewritten parent.
    // `store` holds the flat-file shape (no wire parents), so the reloaded
    // parent chain can only come from the export's rewrite.
    std::optional<std::string> expected_parent;
    for (std::size_t index = 0; index < branch.size(); ++index) {
        const auto& reloaded_entry = reloaded->entries[index + 1];
        CHECK(reloaded_entry.entry_id == branch[branch.size() - 1 - index].entry_id);
        CHECK(cch::harness::session::parent_entry_id(reloaded_entry.parent_id) == expected_parent);
        expected_parent = reloaded_entry.entry_id;
    }
    CHECK(reloaded->entries[1].parent_id.kind == cch::harness::session::ParentIdKind::Root);
    CHECK(cch::harness::session::parent_entry_id(reloaded->entries[2].parent_id) ==
            std::optional<std::string>{first_id});

    // The branch order survives the round trip.
    REQUIRE(reloaded->messages.size() == 2);
    CHECK(ai::text_from_user_message(std::get<ai::UserMessage>(reloaded->messages[0])) == "first");
    CHECK(ai::text_from_user_message(std::get<ai::UserMessage>(reloaded->messages[1])) == "second");
}

TEST_CASE("transcript export writes only the current branch of a branched session",
        "[agent][harness][export][issue1044][spec]") {
    tests::TempWorkspace workspace;
    const auto session_path = workspace.path() / "session.jsonl";
    const auto destination = workspace.path() / "exported.jsonl";

    auto created = harness::session::SessionStore::create_new(session_path, metadata_for(workspace));
    REQUIRE(created.has_value());
    auto store = std::move(*created);

    // A tree with an abandoned branch: `kept` has two children, `abandoned`
    // (written first, then left behind) and the model change that became the
    // active leaf.
    REQUIRE(store.append(user_message("kept")).has_value());
    const auto kept_id = store.leaf_id();
    REQUIRE(store.append(user_message("abandoned")).has_value());
    const auto abandoned_id = store.leaf_id();
    REQUIRE(store.branch(kept_id).has_value());
    REQUIRE(store.append_model_change(kept_id, "fake", "branch-model").has_value());
    const auto active_id = store.leaf_id();
    REQUIRE(store.entries().size() == 3);

    REQUIRE(harness::transcript_export_handler(store.metadata(), store.get_branch(), destination.string())({})
                    .has_value());

    const auto written = read_file(destination);
    // pi exports `getBranch()`, not `getEntries()`: the abandoned sibling is
    // still in the session but is not part of the exported transcript.
    CHECK(written.find(abandoned_id) == std::string::npos);
    CHECK(written.find(kept_id) != std::string::npos);
    CHECK(written.find(active_id) != std::string::npos);

    const auto lines = split_lines(written);
    REQUIRE(lines.size() == 3);
    CHECK(string_member(parse_object(lines[1]), "id") == kept_id);
    CHECK(string_member(parse_object(lines[2]), "id") == active_id);
}

TEST_CASE("transcript export rewrites the branch parent chain to start at null",
        "[agent][harness][export][issue1044][spec]") {
    tests::TempWorkspace workspace;
    const auto session_path = workspace.path() / "session.jsonl";
    const auto destination = workspace.path() / "exported.jsonl";

    auto created = harness::session::SessionStore::create_new(session_path, metadata_for(workspace));
    REQUIRE(created.has_value());
    auto store = std::move(*created);
    REQUIRE(store.append(user_message("first")).has_value());
    REQUIRE(store.append(user_message("second")).has_value());
    REQUIRE(store.append(user_message("third")).has_value());

    // The store now writes pi's real chain (`parentId: this.leafId`), so the
    // first entry is an explicit null root and each later one names its
    // predecessor. The export still rewrites the chain itself, which the
    // assertions below pin.
    const auto stored = store.entries();
    REQUIRE(stored.size() == 3);
    CHECK(stored[0].parent_id.kind == harness::session::ParentIdKind::Root);
    for (std::size_t i = 1; i < stored.size(); ++i) {
        CHECK(stored[i].parent_id.kind == harness::session::ParentIdKind::Entry);
        CHECK(stored[i].parent_id.entry_id == stored[i - 1].entry_id);
    }

    REQUIRE(harness::transcript_export_handler(store.metadata(), store.get_branch(), destination.string())({})
                    .has_value());

    const auto lines = split_lines(read_file(destination));
    REQUIRE(lines.size() == 4);

    // pi: `entries.push({...entry, parentId}); parentId = entry.id;` — every
    // written line's parentId is the previous written entry's id, and the
    // first one starts at null. The stored entries carry null parents, so a
    // line that emitted its own stored parent instead of the chained one is
    // visible from the second line onward.
    std::optional<std::string> expected_parent;
    for (std::size_t index = 1; index < lines.size(); ++index) {
        const auto object = parse_object(lines[index]);
        const auto id = string_member(object, "id");
        REQUIRE_FALSE(id.empty());
        REQUIRE(object.contains("parentId"));
        CHECK(optional_string_member(object, "parentId") == expected_parent);
        // The rewrite replaces the field; it does not add a second one. Two
        // `parentId` keys in one line would leave the JSON ambiguous for the
        // reader even though a lenient parse still yields an object.
        CHECK(count_key_occurrences(lines[index], "parentId") == 1);
        expected_parent = id;
    }
    CHECK(expected_parent == std::optional<std::string>{store.leaf_id()});
}

TEST_CASE("transcript export over an opened branched session writes the active branch in root-to-leaf order",
        "[agent][harness][export][issue1044][spec]") {
    tests::TempWorkspace workspace;
    const auto destination = workspace.path() / "exported.jsonl";

    // The production `/export` path opens the on-disk session
    // (InteractiveEngineHost.cpp:362), so this drives `open_existing` over a
    // pi-captured branched file: entries carry real wire parents and three
    // entries are off the active path. The active leaf is the last file
    // entry (pi `_buildIndex`) — pi never writes a leaf record.
    const auto source = std::filesystem::path{std::string{CCH_SOURCE_DIR}} /
                        "fixtures/pi-agent-core/projection-branch-session.jsonl";
    const auto session_path = workspace.path() / "session.jsonl";
    std::filesystem::copy_file(source, session_path);
    std::error_code permissions_ec;
    std::filesystem::permissions(session_path,
            std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
            std::filesystem::perm_options::replace,
            permissions_ec);
    REQUIRE_FALSE(permissions_ec);

    auto opened = harness::session::SessionStore::open_existing(session_path);
    REQUIRE(opened.has_value());
    auto store = std::move(*opened);

    // `get_branch` is leaf-to-root: the export has to reverse it. Asserting the
    // observed order first keeps the expectation below anchored to the store
    // rather than to this comment.
    const auto branch = store.get_branch();
    REQUIRE(branch.size() == 3);
    REQUIRE(branch[0].entry_id == "9be9baa6");
    REQUIRE(branch[1].entry_id == "d78bb056");
    REQUIRE(branch[2].entry_id == "c946c057");
    REQUIRE(store.entries().size() == 5);

    REQUIRE(harness::transcript_export_handler(store.metadata(), store.get_branch(), destination.string())({})
                    .has_value());

    const auto written = read_file(destination);
    const auto lines = split_lines(written);
    REQUIRE(lines.size() == 4);
    CHECK(string_member(parse_object(lines[0]), "type") == "session");

    // Root-to-leaf, and the chained parent of each line is the previous one.
    const std::vector<std::string> expected_ids{"c946c057", "d78bb056", "9be9baa6"};
    std::optional<std::string> expected_parent;
    for (std::size_t index = 0; index < expected_ids.size(); ++index) {
        const auto object = parse_object(lines[index + 1]);
        CHECK(string_member(object, "id") == expected_ids[index]);
        CHECK(optional_string_member(object, "parentId") == expected_parent);
        CHECK(count_key_occurrences(lines[index + 1], "parentId") == 1);
        expected_parent = expected_ids[index];
    }

    // The three entries off the active path stay out of the export.
    for (const auto& off_branch : {"24b6819f", "d451f610"}) {
        CHECK(written.find(off_branch) == std::string::npos);
    }
}
