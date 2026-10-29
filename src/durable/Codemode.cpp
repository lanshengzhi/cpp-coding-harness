#include <cch/durable/Codemode.hpp>

#include "durable/AwaitResult.hpp"
#include "durable/CodemodeExecution.hpp"
#include "support/Json.hpp"

#include <cch/codemode/Declarations.hpp>
#include <cch/codemode/Source.hpp>
#include <cch/support/OutputLimiter.hpp>

#include <boost/uuid/random_generator.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <format>
#include <map>
#include <mutex>
#include <set>
#include <variant>

namespace cch::durable {
namespace {
using Json = support::JsonValue;
using Object = Json::object_t;
using Array = Json::array_t;
using RunResult = codemode::CodemodeRunResult;
using HostCall = codemode::CodemodeHostCall;

support::Error failure(std::string message) {
    return support::make_error(support::ErrorCode::Tool, std::move(message));
}

const Json* field(const Json& value, std::string_view key) {
    const auto* object = value.get_if<Object>();
    if (!object) return nullptr;
    const auto found = object->find(std::string{key});
    return found == object->end() ? nullptr : &found->second;
}

bool string_field(const Json& value, std::string_view key) {
    const auto* item = field(value, key);
    return item && item->holds<std::string>();
}

bool integer_field(const Json& value, std::string_view key, double maximum = 9007199254740991.0) {
    const auto* item = field(value, key);
    if (!item || !item->holds<double>()) return false;
    const double number = item->get_number();
    return std::isfinite(number) && number >= 0 && number <= maximum && std::trunc(number) == number;
}

bool valid_result(const Json& value) {
    const auto* output = field(value, "output");
    const auto* error = field(value, "isError");
    const auto* detail = field(value, "details");
    if (!output || !output->holds<Array>() || !error || !error->holds<bool>() || !detail || !detail->holds<Object>())
        return false;
    for (const auto& block : output->get_array()) {
        if (!string_field(block, "type")) return false;
        if (block.at("type").get_string() == "text") {
            if (!string_field(block, "text")) return false;
        } else if (block.at("type").get_string() != "image" || !string_field(block, "data") ||
                   !string_field(block, "mimeType"))
            return false;
    }
    return true;
}

bool valid_state(const Json& state) {
    if (!state.holds<Object>() || !string_field(state, "source") || !integer_field(state, "version") ||
            !integer_field(state, "startedAt") || !integer_field(state, "seed", 4294967295.0))
        return false;
    for (const auto* key : {"store", "acks"}) {
        const auto* value = field(state, key);
        if (!value || !value->holds<Object>()) return false;
    }
    for (const auto* key : {"tools", "calls", "entries"}) {
        const auto* value = field(state, key);
        if (!value || !value->holds<Array>()) return false;
    }
    if (field(state, "fingerprint") && !string_field(state, "fingerprint")) return false;
    if (field(state, "deadline") && !integer_field(state, "deadline")) return false;
    for (const auto& tool : state.at("tools").get_array()) {
        if (!string_field(tool, "name") || !string_field(tool, "jsName") || !string_field(tool, "description"))
            return false;
        const auto* safe = field(tool, "replaySafe");
        if (!safe || !safe->holds<bool>()) return false;
    }
    const auto& calls = state.at("calls").get_array();
    for (std::size_t index = 0; index < calls.size(); ++index) {
        const auto& call = calls[index];
        if (!integer_field(call, "id") || call.at("id").get_number() != static_cast<double>(index + 1) ||
                !string_field(call, "name") || !string_field(call, "args") || !string_field(call, "status"))
            return false;
        if (field(call, "error") && !string_field(call, "error")) return false;
        if (const auto* started = field(call, "started"); started && !started->holds<bool>()) return false;
    }
    for (const auto& entry : state.at("entries").get_array()) {
        const auto* outcome = field(entry, "outcome");
        if (!integer_field(entry, "id", static_cast<double>(calls.size())) || entry.at("id").get_number() == 0 ||
                !integer_field(entry, "at") || !outcome || !string_field(*outcome, "value"))
            return false;
        const auto* ok = field(*outcome, "ok");
        if (!ok || !ok->holds<bool>()) return false;
    }
    for (const auto& [key, value] : state.at("acks").get_object()) {
        (void)key;
        if (!value.holds<double>() || !std::isfinite(value.get_number())) return false;
    }
    const auto* result = field(state, "result");
    return !result || valid_result(*result);
}

std::int64_t clock_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
}

std::uint32_t random_seed() {
    const auto uuid = boost::uuids::random_generator{}();
    std::uint32_t seed = 0;
    for (std::size_t index = 0; index < 4; ++index)
        seed = (seed << 8) | uuid.data[index];
    return seed;
}

DocumentDefinition run_document() {
    auto definition =
            define_document_family(DocumentDefinitionBuilder{"pi.codemode.run:v1"}.task(), [](const chord::Json&) {
                return chord::Json{chord::Json::Object{{"state", chord::Json{chord::Json::Object{}}}}};
            });
    if (!definition) std::terminate(); // fixed valid built-in definition
    return std::move(*definition);
}

DocumentAddress run_address(TaskRuntime* runtime, const std::string& call_id) {
    return DocumentAddress{.task_id = runtime->task_id(), .key = call_id};
}

support::Expected<Json> load(ToolExecutionContext& context) {
    Json state;
    auto loaded = await_result(context.runtime->commit(
            [&state, &context](Transaction& tx, const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
                auto document = tx.doc(run_document(), run_address(context.runtime, context.call_id), chord::Json{});
                if (!document) return std::unexpected(document.error());
                auto value = document->get({chord::path_key("state")});
                if (!value) return std::unexpected(value.error());
                if (!value->has_value()) return std::unexpected(failure("Invalid persisted codemode document"));
                state = (**value).to_support();
                return std::nullopt;
            }));
    if (!loaded) return std::unexpected(loaded.error());
    return state;
}

support::ExpectedVoid save(ToolExecutionContext& context,
        const Json& state,
        const std::optional<codemode::CodemodeStoreWrites>& writes = std::nullopt) {
    // debt: replacing a complete run state costs O(n) per answer, hence O(n²)
    // over a script; split the journal when long fan-out workflows are admitted.
    return await_result(context.session->commit([&context, &state, &writes](Transaction& tx) -> support::ExpectedVoid {
        // An observed result must survive the abort mark. This commit changes
        // documents and memos only, never the task's lifecycle state.
        auto current = tx.task(context.runtime->task_id());
        if (!current) return std::unexpected(current.error());
        if (!current->has_value() || (*current)->state.status != "running")
            return std::unexpected(failure("Codemode task is no longer running"));
        auto document = tx.doc(run_document(), run_address(context.runtime, context.call_id), chord::Json{});
        if (!document) return std::unexpected(document.error());
        auto json = chord::Json::from_support(state);
        if (!json) return std::unexpected(json.error());
        if (auto changed = document->set({chord::path_key("state")}, *json); !changed)
            return std::unexpected(changed.error());
        if (writes) {
            auto store =
                    tx.doc(codemode_store_document(), DocumentAddress{.conversation_id = (*current)->conversation_id});
            if (!store) return std::unexpected(store.error());
            for (const auto& key : writes->deleted) {
                if (auto erased = store->erase({chord::path_key("values"), chord::path_key(key)}); !erased)
                    return std::unexpected(erased.error());
            }
            for (const auto& [key, value] : writes->set) {
                auto stored = chord::Json::from_support(value);
                if (!stored) return std::unexpected(stored.error());
                if (auto changed = store->set({chord::path_key("values"), chord::path_key(key)}, *stored); !changed)
                    return std::unexpected(changed.error());
            }
        }
        TaskRecord task = **current;
        task.memos.insert_or_assign("codemode.active", chord::Json{context.call_id});
        return tx.set_task(task);
    }));
}

support::Expected<Json> initial_state(ToolExecutionContext& context, const codemode::CodemodeSource& source) {
    Json store;
    auto loaded = await_result(context.runtime->commit([&context, &store](Transaction& tx, const TaskRecord& current)
                                                               -> support::Expected<std::optional<NextTaskState>> {
        auto document = tx.doc(codemode_store_document(), DocumentAddress{.conversation_id = current.conversation_id});
        if (!document) return std::unexpected(document.error());
        auto value = document->get({chord::path_key("values")});
        if (!value) return std::unexpected(value.error());
        if (!value->has_value()) return std::unexpected(failure("Invalid persisted codemode store"));
        store = (**value).to_support();
        return std::nullopt;
    }));
    if (!loaded) return std::unexpected(loaded.error());
    Array tools;
    for (const auto& tool : context.tools) {
        tools.push_back(Json{Object{{"name", tool.name},
                {"jsName", codemode::to_codemode_identifier(tool.name)},
                {"description", tool.description},
                {"replaySafe",
                        std::ranges::find(context.replay_safe_tools, tool.name) != context.replay_safe_tools.end()}}});
    }
    const auto started = clock_ms();
    Object state{{"version", 2},
            {"source", source.code},
            {"startedAt", Json{static_cast<double>(started)}},
            {"seed", Json{static_cast<double>(random_seed())}},
            {"tools", Json{std::move(tools)}},
            {"store", std::move(store)},
            {"calls", Json{Array{}}},
            {"entries", Json{Array{}}},
            {"acks", Json{Object{}}}};
    if (source.options.timeout_ms)
        state.emplace("deadline", Json{static_cast<double>(started + *source.options.timeout_ms)});
    return Json{std::move(state)};
}

Json call_row(const HostCall& call) {
    Object row{{"id", Json{static_cast<double>(call.id)}},
            {"name", call.name},
            {"args", call.arguments_json.value_or("")},
            {"status", "running"}};
    return Json{std::move(row)};
}

bool same_call(const Json& row, const HostCall& call) {
    if (row.at("name").get_string() != call.name) return false;
    const auto& old_args = row.at("args").get_string();
    const auto new_args = call.arguments_json.value_or("");
    if (old_args.empty() || new_args.empty()) return old_args == new_args;
    const auto old_json = support::read_json(old_args);
    const auto new_json = support::read_json(new_args);
    if (!old_json || !new_json) return false;
    const auto old_canonical = support::canonical_json(*old_json);
    const auto new_canonical = support::canonical_json(*new_json);
    return old_canonical && new_canonical && *old_canonical == *new_canonical;
}

Json outcome_json(const ai::ToolResultMessage& result) {
    std::string text;
    for (const auto& block : result.content) {
        if (const auto* item = std::get_if<ai::TextContent>(&block)) {
            if (!text.empty()) text += '\n';
            text += item->text;
        }
    }
    return Json{Object{{"ok", !result.is_error}, {"value", std::move(text)}}};
}

support::Expected<codemode::CodemodeOutcome> outcome_from_json(const Json& value) {
    const bool ok = value.at("ok").get_boolean();
    const auto& text = value.at("value").get_string();
    codemode::CodemodeOutcome result{
            .ok = ok, .value_json = {}, .message = ok ? std::string{} : text, .properties_json = std::nullopt};
    if (ok) {
        auto encoded = support::write_json(Json{text});
        if (!encoded) return std::unexpected(encoded.error());
        result.value_json = std::move(*encoded);
    }
    return result;
}

Json details(const Json& state) {
    Array calls;
    for (const auto& stored : state.at("calls").get_array()) {
        Object row{{"id", stored.at("id")},
                {"name", stored.at("name")},
                {"args", support::bounded_text(stored.at("args").get_string(), 200, "...")},
                {"status", stored.at("status").get_string() == "running" ? Json{"cancelled"} : stored.at("status")}};
        if (const auto* error = field(stored, "error"))
            row.emplace("error", support::bounded_text(error->get_string(), 500, "..."));
        if (const auto* duration = field(stored, "durationMs")) row.emplace("durationMs", *duration);
        calls.push_back(Json{std::move(row)});
    }
    return Json{Object{{"calls", Json{std::move(calls)}}}};
}

std::string error_text(const codemode::CodemodeError& error, const Json& call_details) {
    std::string text;
    switch (error.kind) {
    case codemode::CodemodeError::Kind::Script:
        text = error.stack.value_or(error.name.value_or("Error") + ": " + error.message);
        break;
    case codemode::CodemodeError::Kind::Timeout:
        text = "Script timed out: " + error.message;
        break;
    case codemode::CodemodeError::Kind::Aborted:
        text = "Script aborted: " + error.message;
        break;
    case codemode::CodemodeError::Kind::Sandbox:
        text = "Script sandbox failed: " + error.message;
        break;
    }
    text += "\n\n";
    const auto& calls = call_details.at("calls").get_array();
    if (calls.empty()) return text + "No tool calls were made.";
    text += "Tool calls made before the failure (they are not undone): ";
    for (std::size_t index = 0; index < calls.size(); ++index) {
        if (index != 0) text += ", ";
        text += std::format("{} ({})", calls[index].at("name").get_string(), calls[index].at("status").get_string());
    }
    return text;
}

void append_text(std::vector<ai::Content>& output, std::string text) {
    if (!output.empty()) {
        if (auto* previous = std::get_if<ai::TextContent>(&output.back())) {
            if (!previous->text.empty() && !previous->text.ends_with('\n')) previous->text += '\n';
            previous->text += text;
            return;
        }
    }
    output.push_back(ai::text_content(std::move(text)));
}

std::size_t image_byte_count(std::string_view data) {
    // The guest has already validated base64. Count its bytes without decoding.
    const std::size_t padding = data.ends_with("==") ? 2 : data.ends_with('=') ? 1 : 0;
    return data.size() * 3 / 4 >= padding ? data.size() * 3 / 4 - padding : 0;
}

ai::ToolResultMessage format_result(const RunResult& result, const Json& state) {
    auto output = result.output;
    if (!result.error && result.value_json) {
        auto value = support::read_json(*result.value_json);
        output.push_back(codemode::CodemodeOutputItem{
                .data = value && value->holds<std::string>() ? value->get_string() : *result.value_json,
                .mime_type = {}});
    }
    ai::ToolResultMessage tool{.details = details(state), .is_error = result.error.has_value()};
    append_text(tool.content,
            std::format("Script {}\nWall time {:.1f} seconds\nOutput:\n",
                    result.error ? "failed" : "completed",
                    (clock_ms() - state.at("startedAt").get_number()) / 1000.0));
    std::vector<ai::Content> body;
    const auto count = std::ranges::count_if(output,
            [](const auto& item) { return item.kind == codemode::CodemodeOutputItem::Kind::Text && !item.console; });
    std::size_t index = 0;
    std::string console;
    for (const auto& item : output) {
        if (item.kind == codemode::CodemodeOutputItem::Kind::Image) {
            append_text(body,
                    std::format("[Image ({}, {} bytes) could not be saved: no execution environment]",
                            item.mime_type,
                            image_byte_count(item.data)));
            body.push_back(ai::image_content(item.data, item.mime_type));
        } else if (item.console) {
            if (!console.empty()) console += '\n';
            console += item.data;
        } else {
            ++index;
            append_text(body, count > 1 ? std::format("==> text {}/{} <==\n{}", index, count, item.data) : item.data);
        }
    }
    if (!console.empty()) append_text(body, "<console_output>\n" + console + "\n</console_output>");
    if (result.error) append_text(body, "Script error:\n" + error_text(*result.error, *tool.details));
    std::size_t remaining_bytes = 4 * 1024 * 1024;
    std::size_t remaining_lines = 100000;
    for (auto& block : body) {
        if (auto* text = std::get_if<ai::TextContent>(&block)) {
            auto bounded = support::truncate_output_head(
                    text->text, support::OutputLimit{.max_bytes = remaining_bytes, .max_lines = remaining_lines});
            remaining_bytes -= bounded.output_bytes;
            remaining_lines -= bounded.output_lines;
            text->text = std::move(bounded.text);
        }
        tool.content.push_back(std::move(block));
    }
    return tool;
}

Json encoded_result(const ai::ToolResultMessage& result) {
    Array output;
    for (const auto& block : result.content) {
        if (const auto* text = std::get_if<ai::TextContent>(&block))
            output.push_back(Json{Object{{"type", "text"}, {"text", text->text}}});
        else if (const auto* image = std::get_if<ai::ImageContent>(&block))
            output.push_back(Json{Object{{"type", "image"}, {"data", image->data}, {"mimeType", image->mime_type}}});
    }
    return Json{
            Object{{"output", Json{std::move(output)}}, {"isError", result.is_error}, {"details", *result.details}}};
}

ai::ToolResultMessage decoded_result(const Json& result) {
    ai::ToolResultMessage tool{.details = result.at("details"), .is_error = result.at("isError").get_boolean()};
    for (const auto& block : result.at("output").get_array()) {
        if (block.at("type").get_string() == "text")
            tool.content.push_back(ai::text_content(block.at("text").get_string()));
        else
            tool.content.push_back(ai::image_content(block.at("data").get_string(), block.at("mimeType").get_string()));
    }
    return tool;
}

struct Started {
    std::string fingerprint{};
};
struct Idle {
    std::int64_t seq{0};
    std::int64_t issued{0};
};
using EventVariant = std::variant<Started, HostCall, Idle, support::Expected<RunResult>>;

struct Events {
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<EventVariant> pending;
    void push(EventVariant event) {
        std::lock_guard lock(mutex);
        pending.push_back(std::move(event));
        ready.notify_one();
    }
    EventVariant pop() {
        std::unique_lock lock(mutex);
        ready.wait(lock, [this] { return !pending.empty(); });
        auto event = std::move(pending.front());
        pending.pop_front();
        return event;
    }
};

class Execution final {
public:
    Execution(codemode::CodemodeSandbox& sandbox, ToolExecutionContext context, Json state)
        : sandbox_(sandbox), context_(std::move(context)), state_(std::move(state)) {}

    support::Expected<ai::ToolResultMessage> execute(const codemode::CodemodeSource& source) {
        if (const auto* result = field(state_, "result")) return decoded_result(*result);
        if (state_.at("source").get_string() != source.code || state_.at("version").get_number() != 2)
            return finish_error("The script cannot resume: its source or code mode version changed.");
        codemode::CodemodeRunOptions options{};
        options.started_at = static_cast<std::int64_t>(state_.at("startedAt").get_number());
        options.seed = static_cast<std::uint64_t>(state_.at("seed").get_number());
        options.stop_token = context_.runtime->stop_token();
        if (const auto* deadline = field(state_, "deadline")) {
            const auto remaining = deadline->get_number() - static_cast<double>(clock_ms());
            if (remaining <= 0) return finish_error("Script timed out: its deadline passed while it was interrupted.");
            options.timeout = std::chrono::milliseconds{static_cast<std::int64_t>(remaining)};
        }
        for (const auto& [key, value] : state_.at("store").get_object())
            options.store.emplace(key, value);
        for (const auto& tool : state_.at("tools").get_array())
            options.tools.push_back(codemode::CodemodeToolDescriptor{.name = tool.at("name").get_string(),
                    .js_name = tool.at("jsName").get_string(),
                    .description = tool.at("description").get_string()});
        if (const auto* fingerprint = field(state_, "fingerprint"))
            options.expected_fingerprint = fingerprint->get_string();
        auto events = std::make_shared<Events>();
        options.callbacks.on_started = [events](std::string_view fingerprint) {
            events->push(Started{std::string{fingerprint}});
        };
        options.callbacks.on_call = [events](const HostCall& call) { events->push(call); };
        options.callbacks.on_idle = [events](
                                            std::int64_t seq, std::int64_t issued) { events->push(Idle{seq, issued}); };
        auto started = sandbox_.start(source.code, std::move(options));
        if (!started) return finish_error(started.error().message);
        auto run = std::move(*started);
        run.result().start([events](support::Expected<RunResult> result) noexcept { events->push(std::move(result)); });
        while (true) {
            auto event = events->pop();
            if (const auto* fingerprint = std::get_if<Started>(&event)) {
                if (const auto* recorded = field(state_, "fingerprint");
                        recorded && recorded->get_string() != fingerprint->fingerprint)
                    return finish_error("The script cannot resume: the sandbox fingerprint changed.");
                state_.get_object().insert_or_assign("fingerprint", fingerprint->fingerprint);
                if (auto saved = save(context_, state_); !saved) return std::unexpected(saved.error());
            } else if (const auto* call = std::get_if<HostCall>(&event)) {
                observed_.emplace(call->id, *call);
                auto& rows = state_.at("calls").get_array();
                const auto index = static_cast<std::size_t>(call->id - 1);
                if (index < rows.size()) {
                    if (!same_call(rows[index], *call))
                        return finish_error("The script took a different path when resumed after a restart: a nested "
                                            "call changed.");
                } else
                    rows.push_back(call_row(*call));
            } else if (const auto* idle = std::get_if<Idle>(&event)) {
                if (auto advanced = advance(run, *idle); !advanced) {
                    if (context_.runtime->stop_token().stop_requested()) return std::unexpected(advanced.error());
                    return finish_error(advanced.error().message);
                }
            } else {
                auto result = std::move(std::get<support::Expected<RunResult>>(event));
                if (!result) return std::unexpected(result.error());
                if (!result->error && result->during &&
                        state_.at("acks").get_object().contains(std::to_string(result->during->seq)))
                    return finish_error("The script took a different path when resumed after a restart: it ended where "
                                        "it previously continued.");
                if (next_entry_ < state_.at("entries").get_array().size())
                    return finish_error("The script took a different path when resumed after a restart: it ended "
                                        "before its recorded results.");
                return finish(*result);
            }
        }
    }

private:
    support::ExpectedVoid advance(codemode::CodemodeSandbox::Run& run, Idle idle) {
        if (context_.runtime->stop_token().stop_requested())
            return std::unexpected(
                    support::make_error(support::ErrorCode::Cancelled, "Codemode invocation was interrupted"));
        auto& acks = state_.at("acks").get_object();
        const auto key = std::to_string(idle.seq);
        if (const auto found = acks.find(key); found != acks.end() && found->second.get_number() != idle.issued)
            return std::unexpected(failure(
                    "The script took a different path when resumed after a restart: its issued call count changed."));
        acks.insert_or_assign(key, Json{static_cast<double>(idle.issued)});
        // Record the issued frontier and identities before admitting any side effect.
        if (auto saved = save(context_, state_); !saved) return saved;
        auto& entries = state_.at("entries").get_array();
        if (next_entry_ < entries.size()) {
            const auto& entry = entries[next_entry_];
            const auto id = static_cast<std::int64_t>(entry.at("id").get_number());
            if (!observed_.contains(id))
                return std::unexpected(failure(
                        "The script took a different path when resumed after a restart: a recorded call is missing."));
            auto answer = outcome_from_json(entry.at("outcome"));
            if (!answer) return std::unexpected(answer.error());
            auto delivered = run.deliver(static_cast<std::int64_t>(++next_entry_),
                    static_cast<std::int64_t>(entry.at("at").get_number()),
                    id,
                    std::move(*answer));
            if (delivered) return std::unexpected(failure(delivered->message));
            delivered_.insert(id);
            return {};
        }
        for (const auto& [id, call] : observed_) {
            if (delivered_.contains(id)) continue;
            auto args = call.arguments_json ? support::read_json(*call.arguments_json)
                                            : support::Expected<Json>{Json{Object{}}};
            if (!args) return std::unexpected(args.error());
            auto& row = state_.at("calls").get_array().at(static_cast<std::size_t>(id - 1)).get_object();
            const bool admitted = row.contains("started") && row.at("started").get_boolean();
            const auto& tools = state_.at("tools").get_array();
            const auto tool = std::ranges::find_if(
                    tools, [&call](const Json& value) { return value.at("name").get_string() == call.name; });
            const bool replay_safe =
                    tool != tools.end() && tool->at("replaySafe").get_boolean() &&
                    std::ranges::find(context_.replay_safe_tools, call.name) != context_.replay_safe_tools.end();
            support::Expected<ai::ToolResultMessage> output;
            if (admitted && !replay_safe) {
                output = ai::ToolResultMessage{.content = {ai::text_content(std::format(
                                                       "Tool {} was interrupted and cannot be replayed", call.name))},
                        .is_error = true};
            } else {
                // Admission commits before the physical operation. An unsafe
                // interrupted operation is never silently repeated.
                row.insert_or_assign("started", true);
                if (auto saved = save(context_, state_); !saved) return saved;
                if (context_.runtime->stop_token().stop_requested())
                    return std::unexpected(
                            support::make_error(support::ErrorCode::Cancelled, "Codemode invocation was interrupted"));
                // debt: nested operations are serialized on the generation thread;
                // independent parallel tool tasks replace this when that scheduler slice lands.
                output = context_.execute_tool(ai::ToolCallContent{.id = context_.call_id + "/c" + std::to_string(id),
                        .name = call.name,
                        .arguments = std::move(*args)});
            }
            if (!output) return std::unexpected(output.error());
            const auto outcome = outcome_json(*output);
            row.insert_or_assign("status", output->is_error ? Json{"error"} : Json{"ok"});
            if (output->is_error) row.insert_or_assign("error", outcome.at("value"));
            if (output->duration_ms)
                row.insert_or_assign("durationMs", Json{static_cast<double>(*output->duration_ms)});
            const auto at = clock_ms();
            entries.push_back(Json{Object{{"id", Json{static_cast<double>(id)}},
                    {"at", Json{static_cast<double>(at)}},
                    {"outcome", outcome}}});
            // A restart can replay this answer only after its commit succeeds.
            if (auto saved = save(context_, state_); !saved) return saved;
            if (context_.runtime->stop_token().stop_requested())
                return std::unexpected(
                        support::make_error(support::ErrorCode::Cancelled, "Codemode invocation was interrupted"));
            auto answer = outcome_from_json(outcome);
            if (!answer) return std::unexpected(answer.error());
            auto delivered = run.deliver(static_cast<std::int64_t>(++next_entry_), at, id, std::move(*answer));
            if (delivered) return std::unexpected(failure(delivered->message));
            delivered_.insert(id);
            return {};
        }
        return {};
    }

    support::Expected<ai::ToolResultMessage> finish_error(std::string message) {
        if (context_.runtime->stop_token().stop_requested())
            return std::unexpected(
                    support::make_error(support::ErrorCode::Cancelled, "Codemode invocation was interrupted"));
        RunResult result{};
        result.error =
                codemode::CodemodeError{.kind = codemode::CodemodeError::Kind::Sandbox, .message = std::move(message)};
        return finish(result);
    }

    support::Expected<ai::ToolResultMessage> finish(const RunResult& result) {
        if (context_.runtime->stop_token().stop_requested() && result.error &&
                result.error->kind == codemode::CodemodeError::Kind::Aborted)
            return std::unexpected(
                    support::make_error(support::ErrorCode::Cancelled, "Codemode invocation was interrupted"));
        auto tool = format_result(result, state_);
        state_.get_object().insert_or_assign("result", encoded_result(tool));
        if (auto saved = save(context_, state_, result.error ? std::nullopt : result.store_writes); !saved)
            return std::unexpected(saved.error());
        return tool;
    }

    codemode::CodemodeSandbox& sandbox_; // extension snapshot outlives execute
    ToolExecutionContext context_;
    Json state_;
    std::map<std::int64_t, HostCall> observed_;
    std::set<std::int64_t> delivered_;
    std::size_t next_entry_{0};
};

ai::ToolResultMessage rejected(std::string message) {
    return ai::ToolResultMessage{.content = {ai::text_content(std::move(message))}, .is_error = true};
}

support::Expected<ai::ToolResultMessage> execute(
        codemode::CodemodeSandbox& sandbox, Json arguments, ToolExecutionContext context) {
    if (!context.runtime || !context.session || !context.execute_tool)
        return rejected("No durable tool execution context");
    const auto* code = field(arguments, "code");
    if (!code || !code->holds<std::string>()) return rejected("Invalid codemode source");
    auto parsed = codemode::parse_codemode_source(code->get_string());
    if (!parsed) return rejected(parsed.error().message);
    auto state = load(context);
    if (!state) return std::unexpected(state.error());
    if (state->holds<Object>() && state->get_object().empty()) {
        state = initial_state(context, *parsed);
        if (!state) return std::unexpected(state.error());
        if (auto saved = save(context, *state); !saved) return std::unexpected(saved.error());
    }
    if (!valid_state(*state)) return rejected("Invalid persisted codemode state");
    return Execution{sandbox, std::move(context), std::move(*state)}.execute(*parsed);
}

} // namespace

Extension codemode_extension(std::unique_ptr<codemode::CodemodeSandbox> sandbox) {
    Extension extension{.name = "codemode"};
    extension.tools.push_back(ToolRegistration{
            .declaration = ai::Tool{.name = "codemode",
                    .description = "Run JavaScript with tools, text(), image(), store() and load().",
                    .parameters = Json{Object{{"type", "object"},
                            {"properties", Json{Object{{"code", Json{Object{{"type", "string"}}}}}}},
                            {"required", Json{Array{"code"}}}}}},
            .execute_durable = [sandbox = std::move(sandbox)](Json arguments,
                                       ToolExecutionContext context) -> support::Expected<ai::ToolResultMessage> {
                if (!sandbox) return std::unexpected(failure("No codemode sandbox is configured"));
                return execute(*sandbox, std::move(arguments), std::move(context));
            },
            .replay_safe = true});
    return extension;
}

DocumentDefinition codemode_store_document() {
    return detail::builtin_document("pi.codemode.store:v1", DocumentHistory::Rewindable, DocumentFork::AsOf, [] {
        return chord::Json{chord::Json::Object{{"values", chord::Json{chord::Json::Object{}}}}};
    });
}

support::Expected<std::optional<ai::ToolResultMessage>> aborted_codemode_result(
        Transaction& tx, const TaskRecord& task) {
    const auto active = task.memos.find("codemode.active");
    if (active == task.memos.end() || !active->second.as_string()) return std::nullopt;
    const auto& call_id = *active->second.as_string();
    auto document = tx.doc(run_document(), DocumentAddress{.task_id = task.id, .key = call_id}, chord::Json{});
    if (!document) return std::unexpected(document.error());
    auto value = document->get({chord::path_key("state")});
    if (!value) return std::unexpected(value.error());
    if (!value->has_value()) return std::unexpected(failure("Invalid persisted codemode document"));
    const auto state = (**value).to_support();
    if (!valid_state(state)) return std::unexpected(failure("Invalid persisted codemode state"));
    auto view = tx.context_view(task.conversation_id);
    if (!view) return std::unexpected(view.error());
    for (const auto& contribution : view->contributions) {
        for (const auto& message : contribution) {
            const auto json = message.to_support();
            if (const auto* id = field(json, "toolCallId");
                    id && id->holds<std::string>() && id->get_string() == call_id)
                return std::nullopt;
        }
    }
    if (const auto* completed = field(state, "result")) {
        auto result = decoded_result(*completed);
        result.tool_name = "codemode";
        result.tool_call_id = call_id;
        return result;
    }
    RunResult aborted{};
    aborted.error =
            codemode::CodemodeError{.kind = codemode::CodemodeError::Kind::Aborted, .message = "Execution aborted"};
    auto result = format_result(aborted, state);
    result.tool_name = "codemode";
    result.tool_call_id = call_id;
    return result;
}

} // namespace cch::durable
