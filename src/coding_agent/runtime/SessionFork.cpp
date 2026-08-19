#include "SessionFork.hpp"

#include "coding_agent/SessionPathPolicy.hpp"
#include "agent/harness/session/EntrySerializer.hpp"
#include "agent/harness/session/SessionJournal.hpp"
#include "support/Json.hpp"

#include <cch/ai/Message.hpp>
#include <cch/agent/harness/session/SessionStore.hpp>
#include <cch/agent/harness/session/SessionTree.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace cch::coding_agent::runtime {
namespace {

/// pi `createBranchedSession` failure mapping: the runtime wraps every
/// session-file failure in pi's verbatim "Failed to create forked session"
/// with the underlying reason as detail.
[[nodiscard]] support::Error fork_session_error(const support::Error& error) {
    return support::make_error(
        support::ErrorCode::Session,
        "Failed to create forked session",
        error.message);
}

/// pi `SessionManager.forkFrom` load failures, verbatim (G3 pins the strings
/// for the fork surface; the in-session branch of a missing/invalid source
/// reports them the same way).
[[nodiscard]] support::Error invalid_fork_source_error(
    const std::filesystem::path& source_path) {
    return support::make_error(
        support::ErrorCode::Session,
        "Cannot fork: source session file is empty or invalid: " +
            source_path.string());
}

[[nodiscard]] support::Error missing_fork_header_error(
    const std::filesystem::path& source_path) {
    return support::make_error(
        support::ErrorCode::Session,
        "Cannot fork: source session has no header: " +
            source_path.string());
}

/// pi forkFrom source classification on a load failure: a file with no
/// parseable content is "empty or invalid"; a file whose first entry parses
/// but is not a session header is "has no header" (the C++ loader rejects
/// both with one internal error, so the classification re-reads the first
/// non-empty line).
[[nodiscard]] support::Error fork_source_load_error(
    const std::filesystem::path& source_path) {
    std::ifstream input(source_path, std::ios::binary);
    if (!input) {
        return invalid_fork_source_error(source_path);
    }
    std::string line;
    while (std::getline(input, line)) {
        const auto is_space = [](unsigned char character) {
            return std::isspace(character) != 0;
        };
        const auto first = std::find_if_not(line.begin(), line.end(), is_space);
        const auto last = std::find_if_not(line.rbegin(), line.rend(), is_space).base();
        if (first >= last) {
            continue;
        }
        auto parsed = support::read_json(
            line.substr(
                static_cast<std::size_t>(first - line.begin()),
                static_cast<std::size_t>(last - first)));
        if (!parsed) {
            return invalid_fork_source_error(source_path);
        }
        const auto* object = parsed->get_if<support::JsonValue::object_t>();
        const auto type = object != nullptr ? object->find("type") : object->end();
        if (type == object->end() || !type->second.holds<std::string>() ||
            type->second.get_string() != "session") {
            return missing_fork_header_error(source_path);
        }
        // A valid session header parses; the loader failure was something
        // else (permissions, journal shape) — wrap it like pi.
        return invalid_fork_source_error(source_path);
    }
    return invalid_fork_source_error(source_path);
}

[[nodiscard]] support::Error invalid_entry_error() {
    return support::make_error(
        support::ErrorCode::Session,
        "Invalid entry ID for forking");
}

[[nodiscard]] support::Error unsaved_session_error() {
    return support::make_error(
        support::ErrorCode::Session,
        "This session has not been saved yet. Wait for the first assistant "
        "response before cloning or forking it.");
}

/// Fresh 8-char hex entry id (pi `generateId`, the C++ store's id scheme).
[[nodiscard]] std::string generate_entry_id() {
    thread_local std::random_device rd;
    thread_local std::mt19937_64 gen(rd());
    thread_local std::uniform_int_distribution<unsigned> dist(0, 15);
    const char hex_chars[] = "0123456789abcdef";
    std::string id(8, '0');
    for (auto& character : id) {
        character = hex_chars[dist(gen)];
    }
    return id;
}

/// pi `extractUserMessageText`: the string content, or the text blocks
/// concatenated with no separator.
[[nodiscard]] std::string fork_message_text(const ai::UserMessage& message) {
    if (const auto* text = std::get_if<std::string>(&message.content)) {
        return *text;
    }
    return ai::text_from_user_message(message);
}

/// The user messages with text from one entry sequence, in store order (pi
/// `getUserMessagesForForking` over the SessionManager entries): the
/// persisted file's parsed entries or the in-memory store's live tree
/// snapshot.
[[nodiscard]] std::vector<UserForkMessage> user_messages_from_entries(
    const std::vector<harness::session::SessionEntry>& entries) {
    std::vector<UserForkMessage> messages;
    for (const auto& entry : entries) {
        if (entry.kind != harness::session::SessionEntryKind::Message ||
            !entry.message) {
            continue;
        }
        const auto* user = std::get_if<ai::UserMessage>(&*entry.message);
        if (user == nullptr) {
            continue;
        }
        auto text = fork_message_text(*user);
        if (text.empty()) {
            continue;
        }
        messages.push_back(UserForkMessage{
            .entry_id = entry.entry_id,
            .text = std::move(text),
        });
    }
    return messages;
}

/// The retained branch path with labels removed and parents re-chained (pi
/// `createBranchedSession` `pathWithoutLabels`), root first.
[[nodiscard]] std::vector<const harness::session::SessionEntry*>
branch_path_without_labels(
    const std::vector<const harness::session::SessionEntry*>& root_to_leaf) {
    std::vector<const harness::session::SessionEntry*> retained;
    retained.reserve(root_to_leaf.size());
    for (const auto* entry : root_to_leaf) {
        if (entry->kind == harness::session::SessionEntryKind::Label) {
            continue;
        }
        retained.push_back(entry);
    }
    return retained;
}

/// The labels targeting retained entries, in first-label-entry order, with
/// the latest label entry's timestamp (pi `labelsById` /
/// `labelTimestampsById`; an empty label clears the target).
struct LabelFacts {
    std::string label;
    ai::TimestampMs timestamp{0};
};

[[nodiscard]] std::vector<std::pair<std::string, LabelFacts>> labels_for_path(
    const harness::session::SessionTree& tree,
    const std::vector<const harness::session::SessionEntry*>& retained) {
    std::map<std::string, std::size_t> target_order;
    std::vector<std::pair<std::string, LabelFacts>> labels;
    for (const auto& entry : tree.entries()) {
        if (entry.kind != harness::session::SessionEntryKind::Label) {
            continue;
        }
        const auto& value = std::get<harness::session::LabelEntryValue>(entry.value);
        auto found = target_order.find(value.target_id);
        if (found == target_order.end()) {
            found = target_order.emplace(value.target_id, labels.size()).first;
            labels.emplace_back(value.target_id, LabelFacts{});
        }
        if (value.label && !value.label->empty()) {
            labels[found->second].second.label = *value.label;
            labels[found->second].second.timestamp = entry.timestamp;
        } else {
            labels[found->second].second.label.clear();
            labels[found->second].second.timestamp = ai::TimestampMs{0};
        }
    }
    std::vector<std::pair<std::string, LabelFacts>> kept;
    for (const auto& [target_id, facts] : labels) {
        if (facts.label.empty()) {
            continue;
        }
        const bool in_path = std::any_of(
            retained.begin(), retained.end(),
            [&](const harness::session::SessionEntry* entry) {
                return entry->entry_id == target_id;
            });
        if (in_path) {
            kept.emplace_back(target_id, facts);
        }
    }
    return kept;
}

/// Write one new session file at `session_path` from a header line and the
/// retained entry lines; failures map to pi's verbatim fork error.
[[nodiscard]] support::ExpectedVoid write_session_file(
    const std::filesystem::path& session_path,
    const std::string& header_line,
    const std::vector<std::string>& entry_lines) {
    auto journal = harness::session::SessionJournal::create_new(
        session_path, header_line);
    if (!journal) {
        return std::unexpected(fork_session_error(journal.error()));
    }
    for (const auto& line : entry_lines) {
        if (auto appended = journal->append_line(line + '\n'); !appended) {
            return std::unexpected(fork_session_error(appended.error()));
        }
    }
    return {};
}

/// pi `createBranchedSession` persisted path: write a new session file whose
/// entries are the root-to-leaf path (labels re-created with their latest
/// timestamps) and whose header carries the fresh identity and the source
/// path as `parentSession`. Returns the new file path.
///
/// Divergence note: pi defers the branch file write until the first
/// assistant response; the C++ publishes session files at creation (the
/// #331 new-session initial-entries contract — the session store writes the
/// header and the resume path appends the restored thinking entry), so the
/// branched file exists immediately after the fork.
[[nodiscard]] support::Expected<std::filesystem::path> create_branched_session_file(
    const std::filesystem::path& source_path,
    const std::filesystem::path& session_dir,
    const std::filesystem::path& workspace,
    const std::string& target_leaf_id) {
    auto loaded = harness::session::SessionStore::load(source_path);
    if (!loaded) {
        // G3 verbatim: the source file could not be read as a session.
        return std::unexpected(invalid_fork_source_error(source_path));
    }
    harness::session::SessionTree tree(std::move(*loaded));
    if (tree.entries().empty()) {
        return std::unexpected(invalid_fork_source_error(source_path));
    }
    // The branch header carries the source's model identity; a source whose
    // first entry is not a session header fails like pi forkFrom.
    if (tree.metadata().session_id.empty()) {
        return std::unexpected(missing_fork_header_error(source_path));
    }

    // pi `getBranch(leafId)`: leaf-to-root in the C++ tree; reverse to
    // root-to-leaf and reject an empty path.
    auto leaf_to_root = tree.getBranch(target_leaf_id);
    if (leaf_to_root.empty()) {
        return std::unexpected(invalid_entry_error());
    }
    std::reverse(leaf_to_root.begin(), leaf_to_root.end());

    // Filter labels and re-chain parents (pi `pathWithoutLabels`).
    const auto retained = branch_path_without_labels(leaf_to_root);
    if (retained.empty()) {
        return std::unexpected(fork_session_error(support::make_error(
            support::ErrorCode::Session, "entry not found in session")));
    }

    // Re-create the label entries for retained targets, chained after the
    // last retained entry (pi labelsToWrite).
    const auto labels = labels_for_path(tree, retained);
    harness::session::EntrySerializer serializer;
    std::vector<std::string> entry_lines;
    entry_lines.reserve(retained.size() + labels.size());
    std::optional<std::string> chain_parent;
    for (const auto* entry : retained) {
        auto rewritten = *entry;
        rewritten.parent_id = chain_parent;
        auto line = serializer.serialize_entry(rewritten);
        if (!line) {
            return std::unexpected(fork_session_error(line.error()));
        }
        entry_lines.push_back(std::move(*line));
        chain_parent = entry->entry_id;
    }
    for (const auto& [target_id, facts] : labels) {
        harness::session::SessionEntry label_entry;
        label_entry.kind = harness::session::SessionEntryKind::Label;
        label_entry.entry_id = generate_entry_id();
        label_entry.parent_id = chain_parent;
        label_entry.timestamp = facts.timestamp;
        label_entry.value = harness::session::LabelEntryValue{
            .target_id = target_id,
            .label = facts.label,
        };
        auto line = serializer.serialize_entry(label_entry);
        if (!line) {
            return std::unexpected(fork_session_error(line.error()));
        }
        entry_lines.push_back(std::move(*line));
        chain_parent = label_entry.entry_id;
    }

    // Fresh identity + timestamped filename in the source session's directory
    // (pi `createSessionId` + `${fileTimestamp}_${sessionId}.jsonl`).
    const auto identity = session_paths::generate_automatic_session_identity();
    const auto session_path =
        session_dir / session_paths::automatic_session_filename(identity);
    harness::session::SessionMetadata metadata{
        .session_id = identity.session_id,
        .created_at = identity.created_at,
        .workspace = workspace,
        .provider = tree.metadata().provider,
        .model = tree.metadata().model,
        .parent_session = source_path,
    };
    auto header_line = serializer.serialize_header(metadata);
    if (!header_line) {
        return std::unexpected(fork_session_error(header_line.error()));
    }
    if (auto written = write_session_file(session_path, *header_line, entry_lines);
        !written) {
        return std::unexpected(written.error());
    }
    return session_path;
}

/// pi `createBranchedSession` no-target-leaf path (a root user message):
/// a fresh empty session file with the `parentSession` pointer.
[[nodiscard]] support::Expected<std::filesystem::path> create_empty_branch_session(
    const std::filesystem::path& session_dir,
    const std::filesystem::path& workspace,
    const std::filesystem::path& source_path) {
    const auto identity = session_paths::generate_automatic_session_identity();
    const auto session_path =
        session_dir / session_paths::automatic_session_filename(identity);
    harness::session::SessionMetadata metadata{
        .session_id = identity.session_id,
        .created_at = identity.created_at,
        .workspace = workspace,
        .provider = {},
        .model = {},
        .parent_session = source_path,
    };
    harness::session::EntrySerializer serializer;
    auto header_line = serializer.serialize_header(metadata);
    if (!header_line) {
        return std::unexpected(fork_session_error(header_line.error()));
    }
    if (auto written = write_session_file(session_path, *header_line, {}); !written) {
        return std::unexpected(written.error());
    }
    return session_path;
}

/// pi `createBranchedSession` in-memory path: the branch context projected
/// from the source store's live tree (root-to-leaf path, labels and all,
/// through `buildSessionContext`) plus the parent pointer and, for position
/// "before", the chosen message's text; the empty context covers the
/// no-target-leaf in-memory case.
struct InMemoryBranchResult {
    InMemoryBranchSeed seed;
    std::optional<std::string> selected_text;
};

[[nodiscard]] support::Expected<InMemoryBranchResult> in_memory_branch_seed(
    const InMemoryForkSource& source,
    std::string_view entry_id,
    ForkPosition position) {
    auto& store = *source.store;
    const auto selected = store.get_entry(entry_id);
    if (!selected.has_value()) {
        return std::unexpected(invalid_entry_error());
    }

    std::optional<std::string> target_leaf;
    std::optional<std::string> selected_text;
    if (position == ForkPosition::At) {
        // Branch at the entry itself (same contract as the persisted path).
        target_leaf = selected->entry_id;
    } else {
        // pi: position "before" requires a user message with non-blank text;
        // `targetLeafId = selectedEntry.parentId` over the effective parent
        // chain (nullopt at the root), and the text pre-fills the editor.
        const auto* user = selected->message
            ? std::get_if<ai::UserMessage>(&*selected->message)
            : nullptr;
        if (selected->kind != harness::session::SessionEntryKind::Message ||
            user == nullptr) {
            return std::unexpected(invalid_entry_error());
        }
        auto text = fork_message_text(*user);
        if (text.empty()) {
            return std::unexpected(invalid_entry_error());
        }
        selected_text = std::move(text);
        target_leaf = store.effective_parent_id(entry_id);
    }

    // An in-memory source has no session file, so the seed's parent pointer
    // stays absent (pi header `parentSession` is unset for in-memory
    // sources).
    InMemoryBranchSeed seed;
    if (target_leaf.has_value()) {
        // The retained path (leaf-to-root from the store, reversed),
        // projected exactly like pi's buildSessionContext over the branch.
        auto path_entries = store.get_branch(*target_leaf);
        std::reverse(path_entries.begin(), path_entries.end());
        std::vector<const harness::session::SessionEntry*> path;
        path.reserve(path_entries.size());
        for (const auto& entry : path_entries) {
            path.push_back(&entry);
        }
        seed.context = harness::session::buildSessionContext(path);
    }
    return InMemoryBranchResult{
        .seed = std::move(seed),
        .selected_text = std::move(selected_text),
    };
}

} // namespace

std::vector<UserForkMessage> user_messages_for_forking(
    const ForkSource& source) {
    if (const auto* persisted =
            std::get_if<PersistedForkSource>(&source.source);
        persisted != nullptr && !persisted->session_path.empty()) {
        auto loaded =
            harness::session::SessionStore::load(persisted->session_path);
        if (!loaded) {
            return {};
        }
        harness::session::SessionTree tree(std::move(*loaded));
        return user_messages_from_entries(tree.entries());
    }
    if (const auto* memory = std::get_if<InMemoryForkSource>(&source.source);
        memory != nullptr && memory->store) {
        return user_messages_from_entries(memory->store->entries());
    }
    return {};
}

support::Expected<ForkPreparation> prepare_fork(
    const ForkSource& source,
    std::string_view entry_id,
    ForkPosition position) {
    ForkPreparation result;
    if (const auto* persisted =
            std::get_if<PersistedForkSource>(&source.source);
        persisted != nullptr && !persisted->session_path.empty()) {
        const auto& session_path = persisted->session_path;
        // pi fork: a persisted session whose file has not been written yet
        // (or was removed) cannot be branched. The C++ publishes session
        // files at creation (the #331 new-session initial-entries contract),
        // so the unsaved state only occurs for a removed file; pi's
        // entry-before-file precedence is unreachable here for the same
        // reason (pi reads live manager entries, the C++ re-reads the file).
        std::error_code exists_ec;
        if (!std::filesystem::exists(session_path, exists_ec)) {
            return std::unexpected(unsaved_session_error());
        }
        auto loaded = harness::session::SessionStore::load(session_path);
        if (!loaded) {
            // G3 verbatim source classification (pi forkFrom strings).
            return std::unexpected(fork_source_load_error(session_path));
        }
        harness::session::SessionTree tree(std::move(*loaded));
        const auto* selected = tree.getEntry(entry_id);
        if (selected == nullptr) {
            return std::unexpected(invalid_entry_error());
        }
        std::optional<std::string> target_leaf;
        if (position == ForkPosition::At) {
            target_leaf = selected->entry_id;
        } else {
            const auto* user = selected->message
                ? std::get_if<ai::UserMessage>(&*selected->message)
                : nullptr;
            if (selected->kind != harness::session::SessionEntryKind::Message ||
                user == nullptr) {
                return std::unexpected(invalid_entry_error());
            }
            auto text = fork_message_text(*user);
            if (text.empty()) {
                return std::unexpected(invalid_entry_error());
            }
            result.selected_text = std::move(text);
            // pi `targetLeafId = selectedEntry.parentId`: the effective
            // parent chain (the C++ tree infers parents from linear ordering
            // when entries carry no explicit parent).
            const auto path = tree.getBranch(selected->entry_id);
            if (path.size() >= 2) {
                target_leaf = path[1]->entry_id;
            }
        }
        if (!target_leaf) {
            // pi: a fork before a root user message has no target leaf — a
            // fresh session with the parent pointer.
            auto created = create_empty_branch_session(
                session_path.parent_path(),
                source.workspace,
                session_path);
            if (!created) {
                return std::unexpected(created.error());
            }
            result.branched_path = std::move(*created);
            return result;
        }
        auto branched = create_branched_session_file(
            session_path,
            session_path.parent_path(),
            source.workspace,
            *target_leaf);
        if (!branched) {
            return std::unexpected(branched.error());
        }
        result.branched_path = std::move(*branched);
        return result;
    }

    const auto* memory = std::get_if<InMemoryForkSource>(&source.source);
    if (memory == nullptr || !memory->store) {
        return std::unexpected(invalid_entry_error());
    }
    auto seed = in_memory_branch_seed(*memory, entry_id, position);
    if (!seed) {
        return std::unexpected(seed.error());
    }
    result.selected_text = std::move(seed->selected_text);
    result.in_memory_seed = std::move(seed->seed);
    return result;
}

} // namespace cch::coding_agent::runtime
