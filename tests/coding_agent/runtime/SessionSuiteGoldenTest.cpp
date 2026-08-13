// P25 (#421): session- and value-suite differential goldens. The committed
// snapshots under `fixtures/pi-coding-agent/sessions/` are captured from the
// frozen pi baseline (`83114817`) by the capture sidecar, which drives the
// frozen pi `AgentSession`/`SessionManager` sources through the same scripted
// scenarios this file drives through the C++ session runtime. Both sides
// apply the same canonical projection (message/content-block level: identity
// fields kept, wall-clock timestamps / usage / entry ids / paths dropped), so
// the byte comparison below is the alignment evidence for the session
// surfaces (R2 §6: session lifecycle, resume, compaction, model switching,
// session-family flows).

#include <cch/ai/Message.hpp>
#include <cch/harness/session/JsonlSessionStore.hpp>
#include <cch/util/JsonValue.hpp>

#include "coding_agent/SessionDiscovery.hpp"

#include "ai/glaze/AiJson.hpp"
#include "coding_agent/AgentSession.hpp"
#include "coding_agent/runtime/SessionFactory.hpp"
#include "support/EnvVarGuard.hpp"
#include "support/JsonCompare.hpp"
#include "support/ModelsFixture.hpp"
#include "support/TempWorkspace.hpp"
#include "util/Json.hpp"

#include <catch2/catch_test_macros.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>

#include <algorithm>
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

using namespace cch;

namespace {

// ── Scripted turn provider ──────────────────────────────────────────────────
// Responds to every prompt with the next scripted assistant message, carrying
// the frozen faux identity (api/provider "fake", model id from the request
// model) so the captured message values match the pi side byte-for-byte.

class ScriptedTurnProvider final : public tests::ScriptedProvider {
public:
  explicit ScriptedTurnProvider(std::vector<ai::AssistantMessage> turns)
      : ScriptedProvider("fake"), turns_(turns.begin(), turns.end()) {}

  [[nodiscard]] std::vector<ai::Model> models() const override {
    return models_;
  }

  void set_model(ai::Model model) { models_.push_back(std::move(model)); }

  void add_model(ai::Model model) { models_.push_back(std::move(model)); }

  [[nodiscard]] boost::asio::awaitable<util::Expected<ai::AssistantMessage>>
  stream(const ai::Model &model, const ai::AiContext &,
         ai::ProviderStreamOptions, ai::AssistantEventSink sink) override {
    REQUIRE(!turns_.empty());
    auto response = std::move(turns_.front());
    turns_.pop_front();
    response.provider = "fake";
    response.api = "fake";
    response.model = model.id;
    if (sink) {
      if (auto emitted = sink(ai::AssistantStartEvent{response}); !emitted) {
        co_return std::unexpected(emitted.error());
      }
      for (std::size_t index = 0; index < response.content.size(); ++index) {
        const auto &block = response.content[index];
        if (const auto *text = std::get_if<ai::TextContent>(&block)) {
          if (auto emitted =
                  sink(ai::TextDeltaEvent{index, text->text, response});
              !emitted) {
            co_return std::unexpected(emitted.error());
          }
        } else if (const auto *thinking =
                       std::get_if<ai::ThinkingContent>(&block)) {
          if (auto emitted = sink(
                  ai::ThinkingDeltaEvent{index, thinking->thinking, response});
              !emitted) {
            co_return std::unexpected(emitted.error());
          }
        } else if (const auto *call =
                       std::get_if<ai::ToolCallContent>(&block)) {
          if (auto emitted = sink(ai::ToolCallStartEvent{index, response});
              !emitted) {
            co_return std::unexpected(emitted.error());
          }
          if (auto emitted = sink(ai::ToolCallEndEvent{index, *call, response});
              !emitted) {
            co_return std::unexpected(emitted.error());
          }
        }
      }
    }
    co_return response;
  }

private:
  std::deque<ai::AssistantMessage> turns_;
  std::vector<ai::Model> models_;
};

/// Scripted model switch target (faux-2, reasoning) used by the model-switch
/// scenario on both sides.
[[nodiscard]] ai::Model reasoning_model(std::string id, std::string name) {
  auto model = tests::scripted_request_model("fake", id);
  model.api = "fake";
  model.name = name;
  // pi's faux reasoning models carry no thinkingLevelMap: every extended
  // level is supported except xhigh/max (pi `getSupportedThinkingLevels`).
  model.reasoning = true;
  return model;
}

/// Deterministic agent config directory with a settings.json override.
struct SettingsFixture {
  tests::TempWorkspace agent_dir;
  tests::EnvVarGuard dir_guard{"PI_CODING_AGENT_DIR"};

  explicit SettingsFixture(std::string_view json) {
    dir_guard.set(agent_dir.path().string());
    std::ofstream out(agent_dir.path() / "settings.json", std::ios::binary);
    out << json;
  }
};

[[nodiscard]] ai::AssistantMessage text_turn(std::string text) {
  // Deterministic wall-clock value (dropped by the canonical projection).
  auto message = ai::assistant_text_message(std::move(text), 1720000000000);
  message.stop_reason = ai::AssistantStopReason::Stop;
  return message;
}

// ── Canonical projection (mirrors the capture sidecar) ──────────────────────
// Reduces both the pi-captured JSON and the C++ MessageDto serialization to
// the same message/content-block-level object. Timestamps, usage, diagnostics,
// response ids, and engine-specific fields are dropped; entry ids project to
// positional ordinals; session ids/paths are identity, not value.

[[nodiscard]] util::JsonValue
canonical_content_block(const util::JsonValue &block) {
  const auto *object = block.get_if<util::JsonValue::object_t>();
  REQUIRE(object != nullptr);
  const auto type = [&]() -> std::string {
    const auto it = object->find("type");
    REQUIRE(it != object->end());
    return it->second.get<std::string>();
  }();
  util::JsonValue::object_t out{{"type", type}};
  if (type == "text") {
    const auto it = object->find("text");
    REQUIRE(it != object->end());
    out.emplace("text", it->second);
  } else if (type == "thinking") {
    const auto it = object->find("thinking");
    REQUIRE(it != object->end());
    out.emplace("thinking", it->second);
  } else if (type == "toolCall") {
    for (const char *key : {"id", "name", "arguments"}) {
      const auto it = object->find(key);
      REQUIRE(it != object->end());
      out.emplace(key, it->second);
    }
  } else if (type == "image") {
    for (const char *key : {"data", "mimeType"}) {
      const auto it = object->find(key);
      if (it != object->end()) {
        out.emplace(key, it->second);
      }
    }
  }
  return util::JsonValue{std::move(out)};
}

[[nodiscard]] util::JsonValue
canonical_message(const util::JsonValue &message) {
  const auto *object = message.get_if<util::JsonValue::object_t>();
  REQUIRE(object != nullptr);
  util::JsonValue::object_t out;
  const auto copy_field = [&](const char *key) {
    if (const auto it = object->find(key); it != object->end()) {
      out.emplace(key, it->second);
    }
  };
  copy_field("role");
  if (const auto it = object->find("content"); it != object->end()) {
    if (const auto *text = it->second.get_if<std::string>()) {
      out.emplace("content", util::JsonValue{util::JsonValue::array_t{
                                 util::JsonValue{util::JsonValue::object_t{
                                     {"type", "text"},
                                     {"text", *text},
                                 }}}});
    } else if (const auto *array =
                   it->second.get_if<util::JsonValue::array_t>()) {
      util::JsonValue::array_t blocks;
      for (const auto &block : *array) {
        blocks.push_back(canonical_content_block(block));
      }
      out.emplace("content", util::JsonValue{std::move(blocks)});
    }
  }
  const auto role = [&]() -> std::string {
    const auto it = object->find("role");
    REQUIRE(it != object->end());
    return it->second.get<std::string>();
  }();
  if (role == "assistant") {
    copy_field("api");
    copy_field("provider");
    copy_field("model");
    copy_field("stopReason");
    copy_field("errorMessage");
  } else if (role == "toolResult") {
    copy_field("toolCallId");
    copy_field("toolName");
    copy_field("isError");
  }
  copy_field("summary");
  copy_field("customType");
  return util::JsonValue{std::move(out)};
}

[[nodiscard]] util::JsonValue
canonical_message(const ai::MessageVariant &message) {
  auto serialized = util::write_json(ai::glaze::to_message_dto(message));
  REQUIRE(serialized);
  auto parsed = util::read_json(*serialized);
  REQUIRE(parsed);
  return canonical_message(*parsed);
}

[[nodiscard]] util::JsonValue
canonical_messages(const std::vector<ai::MessageVariant> &messages) {
  util::JsonValue::array_t out;
  out.reserve(messages.size());
  for (const auto &message : messages) {
    out.push_back(canonical_message(message));
  }
  return util::JsonValue{std::move(out)};
}

/// pi `convertToLlm`-equivalent applied to the live history (the C++
/// `to_llm_messages` helper from the session golden tests).
[[nodiscard]] std::vector<ai::MessageVariant>
to_llm_messages(const std::vector<ai::MessageVariant> &messages) {
  std::vector<ai::MessageVariant> converted;
  converted.reserve(messages.size());
  for (const auto &message : messages) {
    if (const auto *bash = std::get_if<ai::BashExecutionMessage>(&message)) {
      if (!bash->exclude_from_context) {
        converted.push_back(ai::bash_execution_to_user_message(*bash));
      }
    } else if (const auto *custom = std::get_if<ai::CustomMessage>(&message)) {
      converted.push_back(ai::custom_message_to_user_message(*custom));
    } else if (const auto *branch =
                   std::get_if<ai::BranchSummaryMessage>(&message)) {
      converted.push_back(ai::branch_summary_to_user_message(*branch));
    } else if (const auto *compaction =
                   std::get_if<ai::CompactionSummaryMessage>(&message)) {
      converted.push_back(ai::compaction_summary_to_user_message(*compaction));
    } else {
      converted.push_back(message);
    }
  }
  return converted;
}

/// Project the persisted session entries (from the JSONL raw lines) to the
/// canonical entry shape: type + positional id/parentId + type-specific
/// value fields. Header entries are excluded (pi `getEntries()` excludes the
/// header too).
[[nodiscard]] util::JsonValue
project_entries(const harness::session::LoadedSession &loaded) {
  util::JsonValue::array_t out;
  std::size_t ordinal = 0;
  std::map<std::string, std::size_t> id_to_ordinal;
  // First pass: assign ordinals by position among non-header entries.
  std::size_t position = 0;
  for (const auto &entry : loaded.entries) {
    if (entry.kind == harness::session::SessionEntryKind::Header) {
      continue;
    }
    id_to_ordinal.emplace(entry.entry_id, position++);
  }
  for (const auto &entry : loaded.entries) {
    if (entry.kind == harness::session::SessionEntryKind::Header) {
      continue;
    }
    auto parsed = util::read_json(entry.raw_line);
    REQUIRE(parsed);
    const auto *object = parsed->get_if<util::JsonValue::object_t>();
    REQUIRE(object != nullptr);
    util::JsonValue::object_t projected{{"type", util::JsonValue{"message"}}};
    if (const auto it = object->find("type"); it != object->end()) {
      projected["type"] = it->second;
    }
    // Entry ids are identity and parentId chains are tree-structure; both
    // project out (linear order is the structure, like the TS side).
    projected.emplace("id",
                      util::JsonValue{"entry-" + std::to_string(ordinal)});
    const auto type = projected["type"].get<std::string>();
    if (type == "message") {
      const auto it = object->find("message");
      REQUIRE(it != object->end());
      projected.emplace("message", canonical_message(it->second));
    } else if (type == "model_change") {
      for (const char *key : {"provider", "modelId"}) {
        const auto it = object->find(key);
        REQUIRE(it != object->end());
        projected.emplace(key, it->second);
      }
    } else if (type == "thinking_level_change") {
      const auto it = object->find("thinkingLevel");
      REQUIRE(it != object->end());
      projected.emplace("thinkingLevel", it->second);
    } else if (type == "compaction") {
      const auto it = object->find("summary");
      REQUIRE(it != object->end());
      projected.emplace("summary", it->second);
      if (const auto kept = object->find("firstKeptEntryId");
          kept != object->end()) {
        const auto found = id_to_ordinal.find(kept->second.get<std::string>());
        if (found != id_to_ordinal.end()) {
          projected.emplace(
              "firstKeptEntryId",
              util::JsonValue{"entry-" + std::to_string(found->second)});
        }
      }
    } else if (type == "session_info") {
      const auto it = object->find("name");
      if (it != object->end()) {
        projected.emplace("name", it->second);
      }
    }
    out.push_back(util::JsonValue{std::move(projected)});
    ++ordinal;
  }
  return util::JsonValue{std::move(out)};
}

// ── Fixture I/O ─────────────────────────────────────────────────────────────

[[nodiscard]] std::string fixture_dir() {
  return std::string{CCH_SOURCE_DIR} + "/fixtures/pi-coding-agent/sessions/";
}

[[nodiscard]] util::Expected<util::JsonValue>
read_snapshot(std::string_view name) {
  const auto path = std::filesystem::path{fixture_dir()} / name;
  std::ifstream input{path, std::ios::binary};
  if (!input) {
    return std::unexpected(util::make_error(
        util::ErrorCode::Unknown, "Failed to open snapshot: " + path.string()));
  }
  const std::string json{std::istreambuf_iterator<char>{input},
                         std::istreambuf_iterator<char>{}};
  return util::read_json(json);
}

[[nodiscard]] util::Expected<coding_agent::CompactionResult>
run_compact(coding_agent::AgentSession &session) {
  boost::asio::io_context io;
  std::optional<util::Expected<coding_agent::CompactionResult>> result;
  boost::asio::co_spawn(
      io,
      [&]() -> boost::asio::awaitable<void> {
        result = co_await session.compact();
        co_return;
      },
      boost::asio::detached);
  io.run();
  REQUIRE(result.has_value());
  return std::move(*result);
}

void check_snapshot(std::string_view name, const util::JsonValue &actual) {
  const auto expected = read_snapshot(name);
  REQUIRE(expected);
  if (auto mismatch = tests::json_mismatch(*expected, actual); mismatch) {
    std::cerr << "SESSION GOLDEN MISMATCH (" << name << "):\n"
              << *mismatch << "\n--- actual ---\n"
              << util::write_json(actual).value_or("") << "\n";
    CHECK(false);
  }
}

} // namespace

TEST_CASE("session lifecycle golden: scripted turns persist pi-shaped messages",
          "[coding-agent][runtime][golden][issue421]") {
  tests::TempWorkspace workspace;
  const auto path = workspace.path() / "lifecycle.jsonl";

  auto provider =
      std::make_shared<ScriptedTurnProvider>(std::vector<ai::AssistantMessage>{
          text_turn("Hello there!"),
          text_turn("Second reply."),
      });
  auto model = tests::scripted_request_model("fake", "faux-1");
  model.api = "fake";
  model.name = "One";
  provider->set_model(model);

  tests::ModelsSessionOptions options;
  options.session_target =
      coding_agent::ExplicitOpenOrCreateSessionTarget{path};
  options.workspace = workspace.path();
  options.request_model = model;
  options.models = tests::models_from_provider(provider);

  auto created = tests::create_agent_session(std::move(options));
  REQUIRE(created);
  auto &session = created->session;
  REQUIRE(session->prompt_blocking("hi").has_value());
  REQUIRE(session->prompt_blocking("again").has_value());

  const auto snapshot = session->snapshot();
  const auto messages = snapshot.agent_state.messages;
  auto loaded = harness::session::JsonlSessionStore::load(path);
  REQUIRE(loaded);

  util::JsonValue::object_t golden{
      {"meta", util::JsonValue{util::JsonValue::object_t{
                   {"baseline", "83114817c68f5413e4d7ba6d7003ddc511cd31d2"},
                   {"artifact", "@earendil-works/pi-coding-agent@0.83.0"},
                   {"family", "session-lifecycle"},
               }}},
      {"messages", canonical_messages(messages)},
      {"context", canonical_messages(to_llm_messages(messages))},
      {"entries", project_entries(*loaded)},
      {"values", util::JsonValue{util::JsonValue::object_t{
                     {"model", "faux-1"},
                     {"provider", "fake"},
                     {"thinkingLevel", "off"},
                 }}},
  };

  check_snapshot("session-lifecycle.json", util::JsonValue{std::move(golden)});
  session->close();
}

TEST_CASE("session resume golden: persisted history restores at message level",
          "[coding-agent][runtime][golden][issue421]") {
  tests::TempWorkspace workspace;
  const auto path = workspace.path() / "resume.jsonl";

  auto provider =
      std::make_shared<ScriptedTurnProvider>(std::vector<ai::AssistantMessage>{
          text_turn("Hello there!"),
          text_turn("Second reply."),
      });
  auto model = tests::scripted_request_model("fake", "faux-1");
  model.api = "fake";
  model.name = "One";
  provider->set_model(model);

  {
    tests::ModelsSessionOptions options;
    options.session_target =
        coding_agent::ExplicitOpenOrCreateSessionTarget{path};
    options.workspace = workspace.path();
    options.request_model = model;
    options.models = tests::models_from_provider(provider);
    auto created = tests::create_agent_session(std::move(options));
    REQUIRE(created);
    REQUIRE(created->session->prompt_blocking("hi").has_value());
    REQUIRE(created->session->prompt_blocking("again").has_value());
    created->session->close();
  }

  // Resume: no request model, no prompt — the persisted model_change chain
  // restores the live model/thinking and the full history.
  tests::ModelsSessionOptions options;
  options.session_target = coding_agent::ExplicitResumeSessionTarget{path};
  options.workspace = workspace.path();
  options.models = tests::models_from_provider(provider);
  auto created = tests::create_agent_session(std::move(options));
  REQUIRE(created);
  auto &session = created->session;

  const auto snapshot = session->snapshot();
  const auto messages = snapshot.agent_state.messages;
  auto loaded = harness::session::JsonlSessionStore::load(path);
  REQUIRE(loaded);

  util::JsonValue::object_t golden{
      {"meta", util::JsonValue{util::JsonValue::object_t{
                   {"baseline", "83114817c68f5413e4d7ba6d7003ddc511cd31d2"},
                   {"artifact", "@earendil-works/pi-coding-agent@0.83.0"},
                   {"family", "session-resume"},
               }}},
      {"messages", canonical_messages(messages)},
      {"context", canonical_messages(to_llm_messages(messages))},
      {"entries", project_entries(*loaded)},
      {"values", util::JsonValue{util::JsonValue::object_t{
                     {"model", "faux-1"},
                     {"provider", "fake"},
                     {"thinkingLevel", "off"},
                 }}},
  };

  check_snapshot("session-resume.json", util::JsonValue{std::move(golden)});
  session->close();
}

TEST_CASE("session compaction golden: manual compaction pins summary and "
          "rebuilt context",
          "[coding-agent][runtime][golden][issue421]") {
  tests::TempWorkspace workspace;
  // keepRecentTokens: 0 forces a full cut so the split-turn compaction
  // exercise is deterministic on both engines (same settings on the TS side).
  SettingsFixture settings(R"({"compaction":{"keepRecentTokens":0}})");
  const auto path = workspace.path() / "compaction.jsonl";

  auto provider =
      std::make_shared<ScriptedTurnProvider>(std::vector<ai::AssistantMessage>{
          text_turn("first reply."),
          text_turn("second reply."),
          text_turn("third reply."),
          text_turn("summary of the work so far."),
          text_turn("turn context summary."),
      });
  auto model = tests::scripted_request_model("fake", "faux-1");
  model.api = "fake";
  model.name = "One";
  provider->set_model(model);

  tests::ModelsSessionOptions options;
  options.session_target =
      coding_agent::ExplicitOpenOrCreateSessionTarget{path};
  options.workspace = workspace.path();
  options.request_model = model;
  options.models = tests::models_from_provider(provider);
  auto created = tests::create_agent_session(std::move(options));
  REQUIRE(created);
  auto &session = created->session;

  REQUIRE(session->prompt_blocking("first").has_value());
  REQUIRE(session->prompt_blocking("second").has_value());
  REQUIRE(session->prompt_blocking("third").has_value());
  auto compacted = run_compact(*session);
  REQUIRE(compacted.has_value());

  const auto snapshot = session->snapshot();
  const auto messages = snapshot.agent_state.messages;
  auto loaded = harness::session::JsonlSessionStore::load(path);
  REQUIRE(loaded);

  util::JsonValue::object_t golden{
      {"meta", util::JsonValue{util::JsonValue::object_t{
                   {"baseline", "83114817c68f5413e4d7ba6d7003ddc511cd31d2"},
                   {"artifact", "@earendil-works/pi-coding-agent@0.83.0"},
                   {"family", "session-compaction"},
               }}},
      {"messages", canonical_messages(messages)},
      {"context", canonical_messages(to_llm_messages(messages))},
      {"entries", project_entries(*loaded)},
      {"values", util::JsonValue{util::JsonValue::object_t{
                     {"summary", compacted->summary},
                 }}},
  };

  check_snapshot("session-compaction.json", util::JsonValue{std::move(golden)});
  session->close();
}

TEST_CASE("session model-switch golden: setModel pins entries, thinking "
          "re-clamp, settings default",
          "[coding-agent][runtime][golden][issue421]") {
  tests::TempWorkspace workspace;
  SettingsFixture settings(R"({})");
  const auto path = workspace.path() / "model-switch.jsonl";

  auto provider =
      std::make_shared<ScriptedTurnProvider>(std::vector<ai::AssistantMessage>{
          text_turn("Hello there!"),
          text_turn("After the switch."),
      });
  auto model = tests::scripted_request_model("fake", "faux-1");
  model.api = "fake";
  model.name = "One";
  provider->set_model(model);
  provider->add_model(reasoning_model("faux-2", "Two"));

  tests::ModelsSessionOptions options;
  options.session_target =
      coding_agent::ExplicitOpenOrCreateSessionTarget{path};
  options.workspace = workspace.path();
  options.request_model = model;
  options.models = tests::models_from_provider(provider);
  auto created = tests::create_agent_session(std::move(options));
  REQUIRE(created);
  auto &session = created->session;

  REQUIRE(session->prompt_blocking("hi").has_value());
  auto switched = session->set_model_blocking(reasoning_model("faux-2", "Two"));
  REQUIRE(switched.has_value());
  REQUIRE(session->prompt_blocking("after switch").has_value());

  const auto snapshot = session->snapshot();
  const auto messages = snapshot.agent_state.messages;
  auto loaded = harness::session::JsonlSessionStore::load(path);
  REQUIRE(loaded);

  util::JsonValue::object_t golden{
      {"meta", util::JsonValue{util::JsonValue::object_t{
                   {"baseline", "83114817c68f5413e4d7ba6d7003ddc511cd31d2"},
                   {"artifact", "@earendil-works/pi-coding-agent@0.83.0"},
                   {"family", "session-model-switch"},
               }}},
      {"messages", canonical_messages(messages)},
      {"context", canonical_messages(to_llm_messages(messages))},
      {"entries", project_entries(*loaded)},
      {"values", util::JsonValue{util::JsonValue::object_t{
                     {"model", "faux-2"},
                     {"provider", "fake"},
                     {"thinkingLevel", "medium"},
                 }}},
  };

  check_snapshot("session-model-switch.json",
                 util::JsonValue{std::move(golden)});
  session->close();
}

TEST_CASE("session-family golden: most-recent selection and header values",
          "[coding-agent][runtime][golden][issue421]") {
  tests::TempWorkspace workspace;
  const auto dir = workspace.path() / "sessions";
  std::filesystem::create_directories(dir);

  // Three pi-shaped session files with deterministic mtimes; the most-recent
  // selection (pi `findMostRecentSession`, the `--continue` flow) must pick
  // the newest mtime.
  auto make_session = [&](std::string name, int mtime_seconds) {
    const auto file = dir / (name + ".jsonl");
    harness::session::SessionMetadata metadata{
        .session_id = name,
        .created_at = "2026-07-05T00:00:00Z",
        .workspace = "/workspace",
        .provider = "fake",
        .model = "faux-1",
    };
    auto store =
        harness::session::JsonlSessionStore::create_new(file, metadata);
    REQUIRE(store.has_value());
    REQUIRE(store->append(ai::MessageVariant{ai::user_text_message("hello")})
                .has_value());
    auto assistant = ai::assistant_text_message("hi there", 1720000000000);
    assistant.provider = "fake";
    assistant.api = "fake";
    assistant.model = "faux-1";
    REQUIRE(
        store->append(ai::MessageVariant{std::move(assistant)}).has_value());
    const auto now = std::filesystem::file_time_type::clock::now();
    std::filesystem::last_write_time(file,
                                     now - std::chrono::seconds(mtime_seconds));
  };
  make_session("older", 300);
  make_session("mid", 200);
  make_session("newer", 100);

  const auto most_recent =
      coding_agent::session_discovery::find_most_recent_session(dir,
                                                                std::nullopt);
  REQUIRE(most_recent.has_value());

  auto loaded = harness::session::JsonlSessionStore::load(most_recent->path);
  REQUIRE(loaded);

  util::JsonValue::object_t golden{
      {"meta", util::JsonValue{util::JsonValue::object_t{
                   {"baseline", "83114817c68f5413e4d7ba6d7003ddc511cd31d2"},
                   {"artifact", "@earendil-works/pi-coding-agent@0.83.0"},
                   {"family", "session-family"},
               }}},
      {"mostRecent", util::JsonValue{most_recent->path.stem().string()}},
      {"entries", project_entries(*loaded)},
  };

  check_snapshot("session-family.json", util::JsonValue{std::move(golden)});
}
