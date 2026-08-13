// P14: the navigateTree-equivalent runtime capability (pi `agent-session.ts`
// `navigateTree` subset, G2 decision 13) — active-path switching with the
// leaf/active-path semantics of the pi v3 Session Format (user-message
// targets move the leaf to the parent and return the editor text; other
// targets become the leaf), the persisted `leaf` marker, the live Agent
// context rebuild, the verbatim streaming guard, the `label`-entry creation
// (editLabel), the tree topology access, and the in-memory truncation path.
// Branch summarization generation stays absent with no placeholder.

#include "coding_agent/AgentSession.hpp"

#include "coding_agent/runtime/SessionFactory.hpp"
#include "support/EnvVarGuard.hpp"
#include "support/ModelsFixture.hpp"
#include "support/TempWorkspace.hpp"

#include <cch/harness/session/JsonlSessionStore.hpp>
#include <cch/harness/session/SessionTree.hpp>

#include <catch2/catch_test_macros.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <deque>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

using namespace cch;
namespace runtime = cch::coding_agent::runtime;

namespace {

struct Fixture {
    tests::TempWorkspace workspace;
    tests::TempWorkspace agent_dir;
    tests::EnvVarGuard dir_guard{"PI_CODING_AGENT_DIR"};
    tests::EnvVarGuard home_guard{"HOME"};
    std::filesystem::path session_file;

    Fixture() {
        dir_guard.set(agent_dir.path().string());
        home_guard.set(workspace.path().string());
        session_file = workspace.path() / "session.jsonl";
    }
};

harness::session::SessionMetadata test_metadata(const Fixture& fixture) {
    return {
        .session_id = "tree-session",
        .created_at = "2026-07-05T00:00:00Z",
        .workspace = fixture.workspace.path(),
        .provider = "fake",
        .model = "fake-model",
    };
}

ai::MessageVariant user_msg(std::string text) {
    return ai::MessageVariant{ai::user_text_message(std::move(text))};
}

ai::MessageVariant assistant_msg(std::string text) {
    auto message = ai::assistant_text_message(std::move(text));
    message.api = "fake-api";
    message.provider = "fake";
    message.model = "fake-model";
    message.timestamp = 1750000000000;
    return ai::MessageVariant{std::move(message)};
}

/// Build a linear session: user-0, assistant-0, user-1, assistant-1,
/// user-2, assistant-2 (the resumed live history matches this order).
void build_linear_session(
    const std::filesystem::path& path,
    const Fixture& fixture) {
    auto created = harness::session::JsonlSessionStore::create_new(
        path, test_metadata(fixture));
    REQUIRE(created.has_value());
    for (std::size_t index = 0; index < 3; ++index) {
        REQUIRE(created->append(user_msg("user-" + std::to_string(index))).has_value());
        REQUIRE(created->append(assistant_msg("assistant-" + std::to_string(index))).has_value());
    }
}

/// The entry ids of the message entries in store order.
[[nodiscard]] std::vector<std::string> message_entry_ids(
    const std::filesystem::path& path) {
    auto loaded = harness::session::JsonlSessionStore::load(path);
    REQUIRE(loaded.has_value());
    harness::session::SessionTree tree(std::move(*loaded));
    std::vector<std::string> ids;
    for (const auto& entry : tree.entries()) {
        if (entry.kind == harness::session::SessionEntryKind::Message) {
            ids.push_back(entry.entry_id);
        }
    }
    return ids;
}

/// The newest leaf marker's target (pi wire: `targetId: null` at the root
/// position).
struct PersistedLeafMarker {
    bool present{false};
    std::optional<std::string> target;
};

[[nodiscard]] PersistedLeafMarker persisted_leaf_target(
    const std::filesystem::path& path) {
    auto loaded = harness::session::JsonlSessionStore::load(path);
    REQUIRE(loaded.has_value());
    for (auto it = loaded->entries.rbegin(); it != loaded->entries.rend(); ++it) {
        if (it->kind == harness::session::SessionEntryKind::Leaf) {
            const auto& value = std::get<harness::session::LeafEntryValue>(it->value);
            return PersistedLeafMarker{.present = true, .target = value.target_id};
        }
    }
    return PersistedLeafMarker{};
}

/// A concrete scripted provider whose stream never runs in the pure
/// navigation tests (they never prompt); replies deterministically if it does.
class QuietProvider final : public tests::ScriptedProvider {
public:
    QuietProvider() : tests::ScriptedProvider("fake") {}

    boost::asio::awaitable<util::Expected<ai::AssistantMessage>> stream(
        const ai::Model& model,
        const ai::AiContext& context,
        ai::ProviderStreamOptions options,
        ai::AssistantEventSink sink) override {
        (void)model;
        (void)context;
        (void)options;
        ai::AssistantMessage message = ai::assistant_text_message("reply");
        message.api = "fake-api";
        message.provider = "fake";
        message.model = "fake-model";
        if (sink) {
            (void)sink(ai::AssistantStartEvent{message});
        }
        co_return message;
    }
};

/// Open the session over the existing file (resume path).
[[nodiscard]] std::unique_ptr<coding_agent::AgentSession> open_session(
    const Fixture& fixture,
    std::shared_ptr<ai::Models> models = nullptr) {
    coding_agent::runtime::AgentSessionCreationRequest request;
    request.no_skills = true;
    request.no_prompt_templates = true;
    request.workspace = fixture.workspace.path();
    request.session_target =
        coding_agent::ExplicitOpenOrCreateSessionTarget{fixture.session_file};
    request.request_model = tests::scripted_request_model("fake", "fake-model");
    auto created = models
        ? coding_agent::create_agent_session_for_testing(std::move(request), std::move(models))
        : coding_agent::create_agent_session_for_testing(
              std::move(request),
              tests::models_from_provider(
                  std::make_shared<QuietProvider>()));
    REQUIRE(created.has_value());
    return std::move(created->session);
}

/// The user texts in the live session context.
[[nodiscard]] std::vector<std::string> live_user_texts(coding_agent::AgentSession& session) {
    std::vector<std::string> texts;
    for (const auto& message : session.snapshot().agent_state.messages) {
        if (const auto* user = std::get_if<ai::UserMessage>(&message)) {
            texts.push_back(ai::text_from_user_message(*user));
        }
    }
    return texts;
}

/// A scripted provider answering from a FIFO of replies (the
/// navigate-then-prompt branch tests).
class ReplyProvider final : public tests::ScriptedProvider {
public:
    explicit ReplyProvider(std::deque<ai::AssistantMessage> replies)
        : tests::ScriptedProvider("fake"), replies_(std::move(replies)) {}

    boost::asio::awaitable<util::Expected<ai::AssistantMessage>> stream(
        const ai::Model& model,
        const ai::AiContext& context,
        ai::ProviderStreamOptions options,
        ai::AssistantEventSink sink) override {
        (void)model;
        (void)context;
        (void)options;
        REQUIRE(!replies_.empty());
        auto message = std::move(replies_.front());
        replies_.pop_front();
        if (sink) {
            (void)sink(ai::AssistantStartEvent{message});
        }
        co_return message;
    }

    std::deque<ai::AssistantMessage> replies_;
};

[[nodiscard]] ai::AssistantMessage scripted_reply(std::string text) {
    ai::AssistantMessage message = ai::assistant_text_message(std::move(text));
    message.api = "fake-api";
    message.provider = "fake";
    message.model = "fake-model";
    message.timestamp = 1750000000000;
    return message;
}

/// A gated scripted provider: the first stream blocks until released, so a
/// prompt can be held in flight deterministically (the streaming guard).
class GatedProvider final : public tests::ScriptedProvider {
public:
    GatedProvider() : tests::ScriptedProvider("fake") {}

    boost::asio::awaitable<util::Expected<ai::AssistantMessage>> stream(
        const ai::Model& model,
        const ai::AiContext& context,
        ai::ProviderStreamOptions options,
        ai::AssistantEventSink sink) override {
        (void)model;
        (void)context;
        (void)options;
        started = true;
        if (release_timer_) {
            boost::system::error_code wait_error;
            co_await release_timer_->async_wait(
                boost::asio::redirect_error(
                    boost::asio::use_awaitable, wait_error));
        }
        ai::AssistantMessage message = ai::assistant_text_message("held reply");
        message.api = model.api;
        message.provider = model.provider;
        message.model = model.id;
        if (sink) {
            (void)sink(ai::AssistantStartEvent{message});
        }
        co_return message;
    }

    std::atomic<bool> started{false};
    std::optional<boost::asio::steady_timer> release_timer_;
};

} // namespace

TEST_CASE(
    "navigate_tree moves the leaf to a user message's parent and returns its text",
    "[coding_agent][runtime][tree-navigation][issue410]") {
    Fixture fixture;
    build_linear_session(fixture.session_file, fixture);
    auto session = open_session(fixture);
    const auto ids = message_entry_ids(fixture.session_file);
    REQUIRE(ids.size() == 6);

    // The resumed live context carries the full history.
    REQUIRE(live_user_texts(*session).size() == 3);

    // Navigate to the root user message: the leaf resets to null, its text
    // returns to the editor, and the live context ends before the first
    // entry (pi: `result.editorText` + `resetLeaf`).
    auto result = session->navigate_tree(ids[0]);
    REQUIRE(result.has_value());
    CHECK_FALSE(result->cancelled);
    REQUIRE(result->editor_text.has_value());
    CHECK(*result->editor_text == "user-0");
    CHECK(live_user_texts(*session).empty());

    // The leaf marker persisted the root position (`targetId: null`).
    const auto leaf_marker = persisted_leaf_target(fixture.session_file);
    REQUIRE(leaf_marker.present);
    CHECK_FALSE(leaf_marker.target.has_value());
}

TEST_CASE(
    "navigate_tree to a non-user message makes it the leaf with no editor text",
    "[coding_agent][runtime][tree-navigation][issue410]") {
    Fixture fixture;
    build_linear_session(fixture.session_file, fixture);
    auto session = open_session(fixture);
    const auto ids = message_entry_ids(fixture.session_file);

    // Navigate to the second assistant message (ids[3]): the leaf becomes
    // that entry and the live context is the root-to-leaf path.
    auto result = session->navigate_tree(ids[3]);
    REQUIRE(result.has_value());
    CHECK_FALSE(result->cancelled);
    CHECK_FALSE(result->editor_text.has_value());
    const auto users = live_user_texts(*session);
    REQUIRE(users.size() == 2);
    CHECK(users[0] == "user-0");
    CHECK(users[1] == "user-1");

    const auto leaf_marker = persisted_leaf_target(fixture.session_file);
    REQUIRE(leaf_marker.present);
    REQUIRE(leaf_marker.target.has_value());
    CHECK(*leaf_marker.target == ids[3]);
}

TEST_CASE(
    "navigate_tree to the current leaf is a no-op",
    "[coding_agent][runtime][tree-navigation][issue410]") {
    Fixture fixture;
    build_linear_session(fixture.session_file, fixture);
    auto session = open_session(fixture);
    const auto before = persisted_leaf_target(fixture.session_file);
    // The resumed leaf is the last file entry (the restored thinking
    // entry), so the live leaf comes from the topology.
    auto topology = session->session_tree();
    REQUIRE(topology.has_value());
    REQUIRE_FALSE(topology->leaf_id.empty());

    auto result = session->navigate_tree(topology->leaf_id);
    REQUIRE(result.has_value());
    CHECK_FALSE(result->cancelled);
    CHECK_FALSE(result->editor_text.has_value());
    CHECK(live_user_texts(*session).size() == 3);
    // No new leaf marker was written.
    const auto after = persisted_leaf_target(fixture.session_file);
    CHECK(after.present == before.present);
    CHECK(after.target == before.target);
}

TEST_CASE(
    "navigate_tree rejects an unknown entry with pi's verbatim error",
    "[coding_agent][runtime][tree-navigation][issue410]") {
    Fixture fixture;
    build_linear_session(fixture.session_file, fixture);
    auto session = open_session(fixture);

    auto result = session->navigate_tree("deadbeef");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message == "Entry deadbeef not found");
    // Nothing moved.
    CHECK(live_user_texts(*session).size() == 3);
}

TEST_CASE(
    "navigate_tree rejects while a response is streaming with pi's verbatim error",
    "[coding_agent][runtime][tree-navigation][issue410]") {
    Fixture fixture;
    auto provider = std::make_shared<GatedProvider>();
    auto* provider_ptr = provider.get();
    auto models = tests::models_from_provider(std::move(provider));
    auto session = open_session(fixture, std::move(models));

    boost::asio::io_context io;
    boost::asio::steady_timer release(io);
    provider_ptr->release_timer_.emplace(io);
    provider_ptr->release_timer_->expires_at(
        std::chrono::steady_clock::time_point::max());

    std::optional<util::ExpectedVoid> prompt_result;
    boost::asio::co_spawn(
        io,
        session->prompt("first"),
        [&](std::exception_ptr, util::ExpectedVoid result) {
            prompt_result.emplace(std::move(result));
        });
    // Drive the io until the stream is in flight.
    while (!provider_ptr->started.load() && io.poll() != 0) {
    }
    REQUIRE(provider_ptr->started.load());

    // The streaming guard rejects navigation without moving the leaf.
    auto result = session->navigate_tree("some-entry");
    REQUIRE_FALSE(result.has_value());
    CHECK(
        result.error().message ==
        "Wait for the current response to finish before navigating the session tree.");

    // Release the held response and drain.
    provider_ptr->release_timer_->cancel();
    while (io.poll() != 0) {
    }
    REQUIRE(prompt_result.has_value());
    REQUIRE(prompt_result->has_value());
}

TEST_CASE(
    "set_entry_label appends label entries and session_tree resolves them",
    "[coding_agent][runtime][tree-navigation][issue410]") {
    Fixture fixture;
    build_linear_session(fixture.session_file, fixture);
    auto session = open_session(fixture);
    const auto ids = message_entry_ids(fixture.session_file);

    // Label the second user message.
    REQUIRE(session->set_entry_label(ids[2], std::string{"important"}).has_value());

    auto topology = session->session_tree();
    REQUIRE(topology.has_value());
    REQUIRE(topology->roots.size() == 1);
    // The chain: u0 → a0 → u1 → a1 → u2 → a2 → thinking → label (the label
    // hangs under the resumed leaf, the restored thinking entry).
    const auto& root = topology->roots.front();
    REQUIRE(root.children.size() == 1);
    const auto& first_assistant = root.children.front();
    REQUIRE(first_assistant.children.size() == 1);
    const auto& labeled = first_assistant.children.front();
    CHECK(labeled.entry.entry_id == ids[2]);
    REQUIRE(labeled.label.has_value());
    CHECK(*labeled.label == "important");
    REQUIRE(labeled.label_timestamp.has_value());
    CHECK(*labeled.label_timestamp > 0);

    // Clearing the label removes it from the resolved tree.
    REQUIRE(session->set_entry_label(ids[2], std::nullopt).has_value());
    topology = session->session_tree();
    REQUIRE(topology.has_value());
    const auto& relabeled =
        topology->roots.front().children.front().children.front();
    CHECK_FALSE(relabeled.label.has_value());

    // Unknown targets fail with pi's verbatim error.
    auto invalid = session->set_entry_label("deadbeef", std::string{"x"});
    REQUIRE_FALSE(invalid.has_value());
    CHECK(invalid.error().message == "Entry deadbeef not found");
}

TEST_CASE(
    "navigate_tree after branching appends to the new active path",
    "[coding_agent][runtime][tree-navigation][issue410]") {
    Fixture fixture;
    // Three scripted replies: the first two complete the initial turns; the
    // third answers the post-navigation prompt on the new branch.
    std::deque<ai::AssistantMessage> replies;
    replies.push_back(scripted_reply("first reply"));
    replies.push_back(scripted_reply("second reply"));
    replies.push_back(scripted_reply("branch reply"));
    auto scripted = std::make_shared<ReplyProvider>(std::move(replies));
    auto models = tests::models_from_provider(scripted);

    // In-memory session (no file) so the branch append is observable live.
    coding_agent::runtime::AgentSessionCreationRequest request;
    request.no_skills = true;
    request.no_prompt_templates = true;
    request.workspace = fixture.workspace.path();
    request.session_target = coding_agent::InMemorySessionTarget{};
    request.request_model = tests::scripted_request_model("fake", "fake-model");
    auto created = coding_agent::create_agent_session_for_testing(
        std::move(request), std::move(models));
    REQUIRE(created.has_value());
    auto& session = *created->session;
    REQUIRE(session.prompt_blocking("first").has_value());
    REQUIRE(session.prompt_blocking("second").has_value());
    REQUIRE(live_user_texts(session).size() == 2);

    // Navigate back to the first user message (in-memory truncation: the
    // live context ends before it and its text returns to the editor).
    auto topology = session.session_tree();
    REQUIRE(topology.has_value());
    REQUIRE(topology->roots.size() == 1);
    const auto& first_user = topology->roots.front().entry;
    CHECK(first_user.entry_id.rfind("mem-", 0) == 0);
    auto result = session.navigate_tree(first_user.entry_id);
    REQUIRE(result.has_value());
    REQUIRE(result->editor_text.has_value());
    CHECK(*result->editor_text == "first");
    CHECK(live_user_texts(session).empty());

    // The next prompt continues from the navigated point: its user message
    // is the new first message of the live context (a fresh branch).
    REQUIRE(session.prompt_blocking("branch prompt").has_value());
    const auto users = live_user_texts(session);
    REQUIRE(users.size() == 1);
    CHECK(users[0] == "branch prompt");
}

TEST_CASE(
    "persisted navigation then prompt appends to the navigated leaf",
    "[coding_agent][runtime][tree-navigation][issue410]") {
    Fixture fixture;
    build_linear_session(fixture.session_file, fixture);
    std::deque<ai::AssistantMessage> replies;
    replies.push_back(scripted_reply("branch reply"));
    auto scripted = std::make_shared<ReplyProvider>(std::move(replies));
    auto models = tests::models_from_provider(scripted);

    // Open the session with the scripted provider (the resumed run answers
    // the post-navigation prompt).
    coding_agent::runtime::AgentSessionCreationRequest request;
    request.no_skills = true;
    request.no_prompt_templates = true;
    request.workspace = fixture.workspace.path();
    request.session_target =
        coding_agent::ExplicitOpenOrCreateSessionTarget{fixture.session_file};
    request.request_model = tests::scripted_request_model("fake", "fake-model");
    auto created = coding_agent::create_agent_session_for_testing(
        std::move(request), std::move(models));
    REQUIRE(created.has_value());
    auto& session = *created->session;
    const auto ids = message_entry_ids(fixture.session_file);

    // Navigate to the first assistant message (ids[1]): the leaf moves there.
    auto result = session.navigate_tree(ids[1]);
    REQUIRE(result.has_value());
    CHECK_FALSE(result->editor_text.has_value());
    const auto users = live_user_texts(session);
    REQUIRE(users.size() == 1);
    CHECK(users[0] == "user-0");

    // The next prompt appends under the navigated leaf: the new message is a
    // child of ids[1] in the file (the leaf marker moved the append parent).
    REQUIRE(session.prompt_blocking("branch prompt").has_value());

    auto loaded = harness::session::JsonlSessionStore::load(fixture.session_file);
    REQUIRE(loaded.has_value());
    harness::session::SessionTree tree(std::move(*loaded));
    const harness::session::SessionEntry* branch_user = nullptr;
    for (const auto& entry : tree.entries()) {
        if (entry.kind == harness::session::SessionEntryKind::Message &&
            entry.message.has_value()) {
            const auto* user = std::get_if<ai::UserMessage>(&*entry.message);
            if (user != nullptr && ai::text_from_user_message(*user) == "branch prompt") {
                branch_user = &entry;
            }
        }
    }
    REQUIRE(branch_user != nullptr);
    REQUIRE(branch_user->parent_id.has_value());
    CHECK(*branch_user->parent_id == ids[1]);
    // The live context is the navigated path plus the branch turn.
    const auto live_users = live_user_texts(session);
    REQUIRE(live_users.size() == 2);
    CHECK(live_users[0] == "user-0");
    CHECK(live_users[1] == "branch prompt");
}

TEST_CASE(
    "session_tree exposes the topology for an empty persisted session",
    "[coding_agent][runtime][tree-navigation][issue410]") {
    Fixture fixture;
    auto created = harness::session::JsonlSessionStore::create_new(
        fixture.session_file, test_metadata(fixture));
    REQUIRE(created.has_value());
    auto session = open_session(fixture);

    // The resumed header-only session carries the restored thinking entry
    // (the #331 resume chain), so the tree has exactly that one root.
    auto topology = session->session_tree();
    REQUIRE(topology.has_value());
    REQUIRE(topology->roots.size() == 1);
    CHECK(topology->roots.front().entry.kind ==
          harness::session::SessionEntryKind::ThinkingLevelChange);
    CHECK(topology->leaf_id == topology->roots.front().entry.entry_id);
}
