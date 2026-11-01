#include <cch/durable/Codemode.hpp>

#include "durable/AwaitResult.hpp"
#include "durable/CodemodeExecution.hpp"
#include "durable/ToolContent.hpp"
#include "durable/ToolTask.hpp"
#include "support/Json.hpp"

#include <cch/codemode/Declarations.hpp>
#include <cch/durable/Environment.hpp>
#include <cch/durable/Records.hpp>
#include <cch/codemode/Source.hpp>
#include <cch/support/OutputLimiter.hpp>

#include <boost/uuid/random_generator.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <thread>
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

/// Frozen pi's `pi.codemode.run` / `pi.codemode.journal` documents: one
/// task-scoped singleton per code mode call, written in the shape and at the
/// kind frozen pi itself reads, so either runtime can continue the other's run.
constexpr std::string_view kRunKind{"pi.codemode.run"};
constexpr std::string_view kJournalKind{"pi.codemode.journal"};
/// Frozen pi `CODEMODE_MODULE_VERSION`: the start shape a replay accepts.
constexpr double kModuleVersion = 2;
constexpr std::int64_t kDefaultMaxOutputTokens = 10000;

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
        if (!block.holds<Object>() || !string_field(block, "type")) return false;
        if (block.at("type").get_string() == "text") {
            if (!string_field(block, "text")) return false;
        } else if (block.at("type").get_string() != "image" || !string_field(block, "data") ||
                   !string_field(block, "mimeType"))
            return false;
    }
    return true;
}

bool valid_outcome(const Json& value) {
    if (!value.holds<Object>()) return false;
    const auto* ok = field(value, "ok");
    if (!ok || !ok->holds<bool>()) return false;
    if (ok->get_boolean()) return true;
    if (!string_field(value, "message")) return false;
    if (const auto* properties = field(value, "properties")) return properties->holds<Object>();
    return true;
}

bool valid_global_entry(const Json& entry) {
    const auto* name = field(entry, "name");
    const auto* outcome = field(entry, "outcome");
    if (name == nullptr || !name->holds<std::string>() || outcome == nullptr || !valid_outcome(*outcome)) return false;
    return true;
}

bool valid_start(const Json& start) {
    if (!start.holds<Object>() || !integer_field(start, "version") || !integer_field(start, "startedAt") ||
            !integer_field(start, "seed", 4294967295.0) || !integer_field(start, "maxOutputTokens"))
        return false;
    if (const auto* fingerprint = field(start, "fingerprint"); fingerprint && !fingerprint->holds<std::string>())
        return false;
    if (const auto* deadline = field(start, "deadline"); deadline && !integer_field(start, "deadline")) return false;
    const auto* store = field(start, "store");
    if (store == nullptr || !store->holds<Object>()) return false;
    const auto* tools = field(start, "tools");
    const auto* namespaces = field(start, "namespaces");
    const auto* globals = field(start, "globals");
    if (tools == nullptr || !tools->holds<Array>() || namespaces == nullptr || !namespaces->holds<Array>() ||
            globals == nullptr || !globals->holds<Array>())
        return false;
    for (const auto& tool : tools->get_array()) {
        if (!tool.holds<Object>() || !string_field(tool, "name") || !string_field(tool, "jsName") ||
                !string_field(tool, "description"))
            return false;
        for (const auto* key : {"structured", "hidden"}) {
            const auto* value = field(tool, key);
            if (value && !value->holds<bool>()) return false;
        }
    }
    for (const auto& global : globals->get_array()) {
        if (!global.holds<Object>() || !string_field(global, "name")) return false;
        if (const auto* spread = field(global, "spread"); spread && !spread->holds<bool>()) return false;
    }
    return true;
}

bool valid_run(const Json& run) {
    if (!run.holds<Object>()) return false;
    if (const auto* start = field(run, "start"); start != nullptr && !valid_start(*start)) return false;
    if (const auto* output = field(run, "outputDir"); output != nullptr && !output->holds<std::string>()) return false;
    const auto* result = field(run, "result");
    return result == nullptr || valid_result(*result);
}

bool valid_journal(const Json& journal) {
    if (!journal.holds<Object>()) return false;
    if (const auto* first = field(journal, "firstIssued"); first != nullptr && !integer_field(journal, "firstIssued"))
        return false;
    const auto* entries = field(journal, "entries");
    if (entries == nullptr || !entries->holds<Array>()) return false;
    std::set<std::int64_t> ids;
    for (const auto& entry : entries->get_array()) {
        if (!entry.holds<Object>() || !integer_field(entry, "id") || entry.at("id").get_number() == 0 ||
                !integer_field(entry, "at"))
            return false;
        if (!ids.insert(static_cast<std::int64_t>(entry.at("id").get_number())).second) return false;
        if (const auto* issued = field(entry, "issuedAfter"); issued != nullptr && !integer_field(entry, "issuedAfter"))
            return false;
        if (const auto* global = field(entry, "global"); global != nullptr && !valid_global_entry(*global))
            return false;
    }
    return true;
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
    auto definition = define_document(
            DocumentDefinitionBuilder{std::string{kRunKind}}.task(), [] { return chord::Json{chord::Json::Object{}}; });
    if (!definition) std::terminate(); // fixed valid built-in definition
    return std::move(*definition);
}

/// The serial #1094 family: recognized only so an interrupted serial run is
/// refused instead of being silently replaced by the frozen singleton shape.
DocumentDefinition serial_run_document() {
    auto definition = define_document_family(DocumentDefinitionBuilder{"pi.codemode.run:v1"}.task(),
            [](const chord::Json&) { return chord::Json{chord::Json::Object{}}; });
    if (!definition) std::terminate(); // fixed valid built-in definition
    return std::move(*definition);
}

DocumentDefinition journal_document() {
    auto definition = define_document(DocumentDefinitionBuilder{std::string{kJournalKind}}.task(),
            [] { return chord::Json{chord::Json::Object{{"entries", chord::Json{chord::Json::Array{}}}}}; });
    if (!definition) std::terminate(); // fixed valid built-in definition
    return std::move(*definition);
}

DocumentAddress task_address(TaskRuntime* runtime) { return DocumentAddress{.task_id = runtime->task_id()}; }

/// A draft cannot replace its root value; write the frozen members it owns.
support::ExpectedVoid write_run(chord::Draft& document, const Json& run) {
    if (const auto* start = field(run, "start")) {
        auto stored = chord::Json::from_support(*start);
        if (!stored) return std::unexpected(stored.error());
        if (auto changed = document.set({chord::path_key("start")}, *stored); !changed)
            return std::unexpected(changed.error());
    }
    if (const auto* result = field(run, "result")) {
        auto stored = chord::Json::from_support(*result);
        if (!stored) return std::unexpected(stored.error());
        if (auto changed = document.set({chord::path_key("result")}, *stored); !changed)
            return std::unexpected(changed.error());
    }
    if (const auto* output = field(run, "outputDir")) {
        auto stored = chord::Json::from_support(*output);
        if (!stored) return std::unexpected(stored.error());
        if (auto changed = document.set({chord::path_key("outputDir")}, *stored); !changed)
            return std::unexpected(changed.error());
    }
    return {};
}

support::ExpectedVoid write_journal(chord::Draft& document, const Json& journal) {
    if (const auto* entries = field(journal, "entries")) {
        auto stored = chord::Json::from_support(*entries);
        if (!stored) return std::unexpected(stored.error());
        if (auto changed = document.set({chord::path_key("entries")}, *stored); !changed)
            return std::unexpected(changed.error());
    }
    if (const auto* first = field(journal, "firstIssued")) {
        auto stored = chord::Json::from_support(*first);
        if (!stored) return std::unexpected(stored.error());
        if (auto changed = document.set({chord::path_key("firstIssued")}, *stored); !changed)
            return std::unexpected(changed.error());
    }
    return {};
}

/// The two frozen pi documents of one code mode call, and whether an earlier
/// attempt already started it.
struct StoredState {
    Json run{Object{}};
    Json journal{Object{{"entries", Array{}}}};
    bool rerun{false};
};

Json start_value(ToolExecutionContext& context, const codemode::CodemodeSource& source, Json store) {
    Array tools;
    for (const auto& tool : context.tools) {
        tools.push_back(Json{Object{{"name", tool.name},
                {"jsName", codemode::to_codemode_identifier(tool.name)},
                {"description", tool.description}}});
    }
    const auto started = clock_ms();
    Object start{{"version", Json{kModuleVersion}},
            {"startedAt", Json{static_cast<double>(started)}},
            {"seed", Json{static_cast<double>(random_seed())}},
            {"tools", Json{std::move(tools)}},
            {"namespaces", Json{Array{}}},
            {"globals", Json{Array{}}},
            {"maxOutputTokens",
                    Json{static_cast<double>(source.options.max_output_tokens.value_or(
                            static_cast<std::int64_t>(kDefaultMaxOutputTokens)))}},
            {"store", std::move(store)}};
    if (source.options.timeout_ms)
        start.emplace("deadline", Json{static_cast<double>(started + *source.options.timeout_ms)});
    return Json{Object{{"start", Json{std::move(start)}}}};
}

/// Load the run and journal, creating both with the frozen start data on the
/// first attempt. The store snapshot is read only for the first attempt, in the
/// same commit that records it in `start.store`.
support::Expected<StoredState> load_or_create(ToolExecutionContext& context, const codemode::CodemodeSource& source) {
    // #1094's family document has different replay semantics. Refuse it before
    // creating or reading the frozen singleton so an interrupted serial run is
    // never silently treated as fresh.
    auto legacy = await_result(context.session->snapshot(
            serial_run_document(), DocumentAddress{.task_id = context.runtime->task_id(), .key = context.call_id}));
    if (!legacy) return std::unexpected(legacy.error());
    if (legacy->has_value()) return std::unexpected(failure("A serial-era codemode run document cannot be continued"));
    StoredState state;
    auto loaded = await_result(context.runtime->commit(
            [&state, &context, &source](
                    Transaction& tx, const TaskRecord& current) -> support::Expected<std::optional<NextTaskState>> {
                auto run_doc = tx.doc(run_document(), task_address(context.runtime));
                if (!run_doc) return std::unexpected(run_doc.error());
                auto run = run_doc->get({});
                if (!run) return std::unexpected(run.error());
                if (run->has_value()) state.run = (**run).to_support();
                auto journal_doc = tx.doc(journal_document(), task_address(context.runtime));
                if (!journal_doc) return std::unexpected(journal_doc.error());
                auto journal = journal_doc->get({});
                if (!journal) return std::unexpected(journal.error());
                if (journal->has_value()) state.journal = (**journal).to_support();
                state.rerun = field(state.run, "start") != nullptr;
                if (state.rerun) return std::nullopt;
                auto store =
                        tx.doc(codemode_store_document(), DocumentAddress{.conversation_id = current.conversation_id});
                if (!store) return std::unexpected(store.error());
                auto values = store->get({chord::path_key("values")});
                if (!values) return std::unexpected(values.error());
                if (!values->has_value()) return std::unexpected(failure("Invalid persisted codemode store"));
                state.run = start_value(context, source, (**values).to_support());
                state.journal = Json{Object{{"entries", Array{}}}};
                if (auto written = write_run(*run_doc, state.run); !written) return std::unexpected(written.error());
                if (auto written = write_journal(*journal_doc, state.journal); !written)
                    return std::unexpected(written.error());
                return std::nullopt;
            }));
    if (!loaded) return std::unexpected(loaded.error());
    return state;
}

/// Commit both frozen documents together, plus the store writes a successful
/// script decided. An observed result must survive the abort mark, so this
/// commit changes documents and memos only, never the task's lifecycle state.
support::ExpectedVoid save(ToolExecutionContext& context,
        const Json& run,
        const Json& journal,
        const std::optional<codemode::CodemodeStoreWrites>& writes = std::nullopt) {
    // debt: replacing a complete journal costs O(n) per answer, hence O(n²)
    // over a script; split the journal when long fan-out workflows are admitted.
    return await_result(context.session->commit([&context, &run, &journal, &writes](
                                                        Transaction& tx) -> support::ExpectedVoid {
        auto current = tx.task(context.runtime->task_id());
        if (!current) return std::unexpected(current.error());
        if (!current->has_value() || (*current)->state.status != "running")
            return std::unexpected(failure("Codemode task is no longer running"));
        auto run_doc = tx.doc(run_document(), task_address(context.runtime));
        if (!run_doc) return std::unexpected(run_doc.error());
        if (auto written = write_run(*run_doc, run); !written) return std::unexpected(written.error());
        auto journal_doc = tx.doc(journal_document(), task_address(context.runtime));
        if (!journal_doc) return std::unexpected(journal_doc.error());
        if (auto written = write_journal(*journal_doc, journal); !written) return std::unexpected(written.error());
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

Json call_row(const HostCall& call) {
    Object row{{"id", Json{static_cast<double>(call.id)}},
            {"name", call.name},
            {"args", call.arguments_json.value_or("")},
            {"status", "running"}};
    return Json{std::move(row)};
}

/// Decode a frozen pi `CodemodeOutcome` from a journaled global answer.
support::Expected<codemode::CodemodeOutcome> outcome_from_json(const Json& value) {
    const auto* ok = field(value, "ok");
    if (ok == nullptr || !ok->holds<bool>()) return std::unexpected(failure("Invalid journaled codemode outcome"));
    codemode::CodemodeOutcome result{
            .ok = ok->get_boolean(), .value_json = {}, .message = {}, .properties_json = std::nullopt};
    if (result.ok) {
        if (const auto* payload = field(value, "value")) {
            auto encoded = support::write_json(*payload);
            if (!encoded) return std::unexpected(encoded.error());
            result.value_json = std::move(*encoded);
        }
        return result;
    }
    const auto* message = field(value, "message");
    if (message == nullptr || !message->holds<std::string>())
        return std::unexpected(failure("Invalid journaled codemode outcome"));
    result.message = message->get_string();
    if (const auto* properties = field(value, "properties")) {
        auto encoded = support::write_json(*properties);
        if (!encoded) return std::unexpected(encoded.error());
        result.properties_json = std::move(*encoded);
    }
    return result;
}

/// The text of a schema-less tool's stored value (frozen pi `textOf`).
std::string text_of_json(const Json& value) {
    if (const auto* text = value.get_if<std::string>()) return *text;
    std::string text;
    if (const auto* blocks = value.get_if<Array>()) {
        for (const auto& block : *blocks) {
            if (!string_field(block, "type") || block.at("type").get_string() != "text") continue;
            const auto* item = field(block, "text");
            if (item == nullptr || !item->holds<std::string>()) continue;
            if (!text.empty()) text += '\n';
            text += item->get_string();
        }
    }
    return text;
}

/// What a script sees of one nested call's stored result (frozen pi `outcomeOf`).
codemode::CodemodeOutcome nested_outcome(const NestedCallResult& result, std::string_view name, bool structured) {
    std::string diagnostics;
    for (const auto& diagnostic : result.diagnostics) {
        if (!diagnostics.empty()) diagnostics += '\n';
        diagnostics += diagnostic.message;
    }
    const std::string fallback = diagnostics.empty() ? std::format("Tool {} failed", name) : diagnostics;
    codemode::CodemodeOutcome outcome{.ok = true, .value_json = {}, .message = {}, .properties_json = std::nullopt};
    if (!structured) {
        const std::string text = result.structured_output ? text_of_json(*result.structured_output) : std::string{};
        if (!result.is_error) {
            auto encoded = support::write_json(Json{text});
            if (!encoded)
                return codemode::CodemodeOutcome{.ok = false,
                        .value_json = {},
                        .message = encoded.error().message,
                        .properties_json = std::nullopt};
            outcome.value_json = std::move(*encoded);
            return outcome;
        }
        outcome.ok = false;
        outcome.message = text.empty() ? fallback : text;
        if (!text.empty()) {
            auto properties = support::write_json(Json{Object{{"structuredOutput", text}}});
            if (properties) outcome.properties_json = std::move(*properties);
        }
        return outcome;
    }
    if (!result.is_error || result.structured_output.has_value()) {
        if (result.structured_output) {
            auto encoded = support::write_json(*result.structured_output);
            if (!encoded)
                return codemode::CodemodeOutcome{.ok = false,
                        .value_json = {},
                        .message = encoded.error().message,
                        .properties_json = std::nullopt};
            outcome.value_json = std::move(*encoded);
        }
        return outcome;
    }
    outcome.ok = false;
    outcome.message = fallback;
    return outcome;
}

/// The model-visible `details.calls` rows, in issue order; a call still running
/// when the script ends is reported cancelled (frozen pi `CodemodeToolDetails`).
Json details(
        const std::map<std::int64_t, Json>& calls, const std::optional<std::string>& full_output_path = std::nullopt) {
    Array rows;
    for (const auto& [id, stored] : calls) {
        (void)id;
        Object row{{"id", stored.at("id")},
                {"name", stored.at("name")},
                {"args", support::bounded_text(stored.at("args").get_string(), 200, "...")},
                {"status", stored.at("status").get_string() == "running" ? Json{"cancelled"} : stored.at("status")}};
        if (const auto* error = field(stored, "error"))
            row.emplace("error", support::bounded_text(error->get_string(), 500, "..."));
        if (const auto* duration = field(stored, "durationMs")) row.emplace("durationMs", *duration);
        rows.push_back(Json{std::move(row)});
    }
    Object value{{"calls", Json{std::move(rows)}}};
    if (full_output_path) value.emplace("fullOutputPath", *full_output_path);
    return Json{std::move(value)};
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

/// Frozen pi `CHARS_PER_TOKEN`.
constexpr std::size_t kCharsPerToken = 4;
/// Frozen pi codemode `OUTPUT_LIMITS`.
constexpr std::size_t kOutputMaxBytes = 4 * 1024 * 1024;
constexpr std::size_t kOutputMaxLines = 100000;
/// Header, markers and notes around the script's own text (frozen `LINE_MARGIN`).
constexpr std::size_t kOutputLineMargin = 100;

/// JS strings count and slice UTF-16 code units. Invalid UTF-8 decodes as
/// U+FFFD, matching a JS string boundary.
std::u16string to_utf16(std::string_view text) {
    std::u16string units;
    units.reserve(text.size());
    for (std::size_t offset = 0; offset < text.size();) {
        const auto lead = static_cast<unsigned char>(text[offset]);
        std::size_t length = 0;
        std::uint32_t codepoint = 0;
        if (lead < 0x80) {
            length = 1;
            codepoint = lead;
        } else if (lead >= 0xc2 && lead <= 0xdf) {
            length = 2;
            codepoint = lead & 0x1f;
        } else if (lead >= 0xe0 && lead <= 0xef) {
            length = 3;
            codepoint = lead & 0x0f;
        } else if (lead >= 0xf0 && lead <= 0xf4) {
            length = 4;
            codepoint = lead & 0x07;
        }
        bool valid = length != 0 && offset + length <= text.size();
        for (std::size_t index = 1; valid && index < length; ++index) {
            const auto byte = static_cast<unsigned char>(text[offset + index]);
            if ((byte & 0xc0) != 0x80) {
                valid = false;
                break;
            }
            codepoint = (codepoint << 6) | (byte & 0x3f);
        }
        if (valid) {
            if ((length == 2 && codepoint < 0x80) || (length == 3 && codepoint < 0x800) ||
                    (length == 4 && codepoint < 0x10000) || (codepoint >= 0xd800 && codepoint <= 0xdfff) ||
                    codepoint > 0x10ffff)
                valid = false;
        }
        if (!valid) {
            units.push_back(static_cast<char16_t>(0xfffd));
            ++offset;
            continue;
        }
        offset += length;
        if (codepoint >= 0x10000) {
            const auto adjusted = codepoint - 0x10000;
            units.push_back(static_cast<char16_t>(0xd800 + (adjusted >> 10)));
            units.push_back(static_cast<char16_t>(0xdc00 + (adjusted & 0x3ff)));
        } else {
            units.push_back(static_cast<char16_t>(codepoint));
        }
    }
    return units;
}

std::string to_utf8(std::u16string_view units) {
    std::string text;
    text.reserve(units.size());
    for (std::size_t index = 0; index < units.size(); ++index) {
        std::uint32_t codepoint = units[index];
        if (codepoint >= 0xd800 && codepoint <= 0xdbff && index + 1 < units.size() && units[index + 1] >= 0xdc00 &&
                units[index + 1] <= 0xdfff) {
            codepoint = 0x10000 + ((codepoint - 0xd800) << 10) + (units[index + 1] - 0xdc00);
            ++index;
        } else if (codepoint >= 0xd800 && codepoint <= 0xdfff) {
            codepoint = 0xfffd;
        }
        if (codepoint < 0x80) {
            text.push_back(static_cast<char>(codepoint));
        } else if (codepoint < 0x800) {
            text.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
            text.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
        } else if (codepoint < 0x10000) {
            text.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
            text.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
            text.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
        } else {
            text.push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
            text.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
            text.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
            text.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
        }
    }
    return text;
}

std::vector<std::u16string> split_utf16_lines(std::u16string_view text) {
    std::vector<std::u16string> lines;
    std::size_t start = 0;
    while (true) {
        const auto newline = text.find(u'\n', start);
        if (newline == std::u16string_view::npos) {
            lines.emplace_back(text.substr(start));
            return lines;
        }
        lines.emplace_back(text.substr(start, newline - start));
        start = newline + 1;
    }
}

std::u16string join_utf16_lines(const std::vector<std::u16string>& lines, std::size_t begin, std::size_t end) {
    std::u16string text;
    for (std::size_t index = begin; index < end; ++index) {
        if (index > begin) text.push_back(u'\n');
        text.append(lines[index]);
    }
    return text;
}

/// pi `valueText`: strings as-is, other values as compact JSON.
std::string value_text(std::string_view value_json) {
    auto parsed = support::read_json(value_json);
    if (parsed && parsed->holds<std::string>()) return parsed->get_string();
    return std::string{value_json};
}

bool is_text(const codemode::CodemodeOutputItem& item) { return item.kind == codemode::CodemodeOutputItem::Kind::Text; }

/// pi `formatItems`: number non-console text when there are several, then put
/// every console line after the other output in one block.
std::vector<ai::Content> format_items(const std::vector<codemode::CodemodeOutputItem>& items) {
    const auto total = std::ranges::count_if(items, [](const auto& item) { return is_text(item) && !item.console; });
    std::vector<ai::Content> formatted;
    std::vector<std::string> console_lines;
    std::size_t index = 0;
    for (const auto& item : items) {
        if (!is_text(item)) {
            formatted.push_back(ai::image_content(item.data, item.mime_type));
        } else if (item.console) {
            console_lines.push_back(item.data);
        } else {
            ++index;
            auto text = item.data;
            if (total > 1) text = std::format("==> text {}/{} <==\n{}", index, total, text);
            formatted.push_back(ai::text_content(std::move(text)));
        }
    }
    if (!console_lines.empty()) {
        std::string block = "<console_output>\n";
        for (std::size_t line = 0; line < console_lines.size(); ++line) {
            if (line > 0) block += '\n';
            block += console_lines[line];
        }
        block += "\n</console_output>";
        formatted.push_back(ai::text_content(std::move(block)));
    }
    return formatted;
}

/// pi `joinAdjacentText`: one text item per run, each part starting on its own
/// line unless the previous non-empty part already ends in a newline; images
/// flush the current run.
std::vector<ai::Content> join_adjacent_text(const std::vector<ai::Content>& items) {
    std::vector<ai::Content> joined;
    std::string parts;
    std::string last_part;
    const auto flush = [&] {
        if (!parts.empty()) joined.push_back(ai::text_content(std::move(parts)));
        parts.clear();
        last_part.clear();
    };
    for (const auto& item : items) {
        if (const auto* text = std::get_if<ai::TextContent>(&item)) {
            if (!last_part.empty() && last_part.back() != '\n') {
                parts += '\n';
                last_part = "\n";
            }
            parts += text->text;
            if (!text->text.empty()) last_part = text->text;
        } else {
            flush();
            joined.push_back(item);
        }
    }
    flush();
    return joined;
}

std::optional<std::string_view> image_extension(std::string_view mime_type) {
    if (mime_type == "image/png") return ".png";
    if (mime_type == "image/jpeg") return ".jpg";
    if (mime_type == "image/gif") return ".gif";
    if (mime_type == "image/webp") return ".webp";
    return std::nullopt;
}

support::Expected<std::string> decode_base64(std::string_view text) {
    std::string bytes;
    bytes.reserve(text.size() / 4 * 3);
    std::uint32_t accumulator = 0;
    int bits = 0;
    for (const char character : text) {
        if (character == '=') break;
        int value = -1;
        if (character >= 'A' && character <= 'Z')
            value = character - 'A';
        else if (character >= 'a' && character <= 'z')
            value = character - 'a' + 26;
        else if (character >= '0' && character <= '9')
            value = character - '0' + 52;
        else if (character == '+')
            value = 62;
        else if (character == '/')
            value = 63;
        else if (character == '\n' || character == '\r' || character == ' ' || character == '\t')
            continue;
        if (value < 0) return std::unexpected(failure("invalid image data"));
        accumulator = (accumulator << 6) | static_cast<std::uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            bytes.push_back(static_cast<char>((accumulator >> bits) & 0xff));
        }
    }
    return bytes;
}

/// One output file under the run's directory: its path, or why it was not
/// written (frozen pi `OutputFile`).
using OutputWriter = std::move_only_function<support::Expected<std::string>(std::string_view, std::string_view)>;

struct TruncatedOutput {
    std::vector<ai::Content> items;
    std::optional<std::string> full_output_path;
};

/// pi `truncate`: over the token or line budget, keep the start and end of all
/// text, move images after it, and spill the complete text through `write`.
TruncatedOutput truncate_output(std::vector<ai::Content> items, std::int64_t max_tokens, OutputWriter& write) {
    std::vector<std::string> texts;
    for (const auto& item : items) {
        if (const auto* text = std::get_if<ai::TextContent>(&item)) texts.push_back(text->text);
    }
    std::string combined;
    for (std::size_t index = 0; index < texts.size(); ++index) {
        if (index > 0) combined += '\n';
        combined += texts[index];
    }
    const auto units = to_utf16(combined);
    const auto budget =
            std::min<std::size_t>(static_cast<std::size_t>(std::max<std::int64_t>(max_tokens, 0)) * kCharsPerToken,
                    kOutputMaxBytes / kCharsPerToken);
    const auto line_budget = (kOutputMaxLines - kOutputLineMargin) / 2;
    const auto lines = split_utf16_lines(units).size();
    if (texts.empty() || (units.size() <= budget && lines <= 2 * line_budget))
        return TruncatedOutput{.items = std::move(items), .full_output_path = std::nullopt};

    const auto head_chars = budget / 2;
    const auto tail_chars = budget - head_chars;
    const auto head_part = std::u16string_view{units}.substr(0, head_chars);
    const auto head_lines = split_utf16_lines(head_part);
    const auto head = join_utf16_lines(head_lines, 0, std::min(head_lines.size(), line_budget));
    std::u16string tail;
    if (tail_chars > 0) {
        // The line budget alone can trigger truncation with a budget larger than
        // the text, where a JS negative slice starts at 0 instead of underflowing.
        const auto tail_start = units.size() > tail_chars ? units.size() - tail_chars : 0;
        const auto tail_part = std::u16string_view{units}.substr(tail_start);
        const auto tail_lines = split_utf16_lines(tail_part);
        const auto begin = tail_lines.size() > line_budget ? tail_lines.size() - line_budget : 0;
        tail = join_utf16_lines(tail_lines, begin, tail_lines.size());
    }
    const auto removed = units.size() - head.size() - tail.size();
    const auto total_tokens = (units.size() + kCharsPerToken - 1) / kCharsPerToken;
    const auto removed_tokens = (removed + kCharsPerToken - 1) / kCharsPerToken;
    std::string text =
            std::format("Warning: truncated output (original token count: {})\nTotal output lines: {}\n\n{}…{} tokens "
                        "truncated…{}",
                    total_tokens,
                    lines,
                    to_utf8(head),
                    removed_tokens,
                    to_utf8(tail));
    TruncatedOutput truncated;
    truncated.items.push_back(ai::text_content(std::move(text)));
    for (auto& item : items) {
        if (std::holds_alternative<ai::ImageContent>(item)) truncated.items.push_back(std::move(item));
    }
    auto spilled = write("output.txt", combined);
    if (spilled) {
        truncated.full_output_path = *spilled;
        auto& warning = std::get<ai::TextContent>(truncated.items.front());
        warning.text += std::format("\n\n[Full output: {} (read with offset/limit)]", *spilled);
    } else {
        auto& warning = std::get<ai::TextContent>(truncated.items.front());
        warning.text += std::format("\n\n[Could not save the full output: {}]", spilled.error().message);
    }
    return truncated;
}

/// pi `saveImages`: save each image's bytes once, put its path label before it,
/// and keep the image even when the write fails.
std::vector<ai::Content> save_images(std::vector<ai::Content> items, OutputWriter& write) {
    std::map<std::string, std::string> labels;
    std::vector<ai::Content> saved;
    for (const auto& item : items) {
        const auto* image = std::get_if<ai::ImageContent>(&item);
        if (image == nullptr) {
            saved.push_back(item);
            continue;
        }
        auto found = labels.find(image->data);
        if (found == labels.end()) {
            auto bytes = decode_base64(image->data);
            std::string label;
            if (!bytes) {
                label = std::format(
                        "[Image ({}, 0 bytes) could not be saved: {}]", image->mime_type, bytes.error().message);
            } else {
                const std::string kind = image->mime_type + ", " + std::to_string(bytes->size()) + " bytes";
                const auto extension = image_extension(image->mime_type);
                const std::string name =
                        std::format("image-{}{}", labels.size() + 1, extension.value_or(std::string_view{".bin"}));
                auto written = write(name, *bytes);
                label = written ? std::format("[Image saved to {} ({})]", *written, kind)
                                : std::format("[Image ({}) could not be saved: {}]", kind, written.error().message);
            }
            found = labels.emplace(image->data, std::move(label)).first;
        }
        saved.push_back(ai::text_content(found->second));
        saved.push_back(item);
    }
    return saved;
}

struct FormattedResult {
    ai::ToolResultMessage message;
    std::optional<std::string> full_output_path;
};

FormattedResult format_result(const RunResult& result,
        const Json& start,
        const std::map<std::int64_t, Json>& calls,
        std::int64_t max_tokens,
        OutputWriter write) {
    auto output = result.output;
    if (!result.error && result.value_json) {
        output.push_back(codemode::CodemodeOutputItem{
                .data = value_text(*result.value_json),
                .mime_type = {},
        });
    }
    auto formatted = format_items(output);
    const auto call_details = details(calls);
    if (result.error)
        formatted.push_back(ai::text_content("Script error:\n" + error_text(*result.error, call_details)));
    auto truncated = truncate_output(join_adjacent_text(formatted), max_tokens, write);
    auto content = join_adjacent_text(save_images(std::move(truncated.items), write));
    ai::ToolResultMessage message{
            .details = details(calls, truncated.full_output_path), .is_error = result.error.has_value()};
    message.content.push_back(ai::text_content(std::format("Script {}\nWall time {:.1f} seconds\nOutput:\n",
            result.error ? "failed" : "completed",
            (clock_ms() - start.at("startedAt").get_number()) / 1000.0)));
    for (auto& item : content)
        message.content.push_back(std::move(item));
    return FormattedResult{.message = std::move(message), .full_output_path = truncated.full_output_path};
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
/// One nested call that ended: its VM call ID, the clock reading at its end, and
/// the result its caller reads (frozen pi `pump` delivering a finished call).
struct Completed {
    std::int64_t id{0};
    std::int64_t at{0};
    NestedCallResult result{};
};
using EventVariant = std::variant<Started, HostCall, Idle, Completed, support::Expected<RunResult>>;

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

/// The result of a nested call that ended without storing one: the scheduler
/// faulted or orphaned it (frozen pi `fallbackResult`).
NestedCallResult fallback_result(const TaskRecord& record, std::string_view name) {
    NestedCallResult result{.is_error = true};
    std::string message;
    if (record.state.outcome.has_value() && record.state.outcome->status == "faulted" &&
            record.state.outcome->error.has_value()) {
        message = std::format("Tool {} failed: {}", name, record.state.outcome->error->message);
    } else if (record.state.outcome.has_value() && record.state.outcome->status == "orphaned") {
        message = std::format(
                "Tool {} could not resume: {}", name, record.state.outcome->reason.value_or("its owner ended"));
    } else {
        message = std::format("Tool {} ended without a result", name);
    }
    result.diagnostics.push_back(NestedCallDiagnostic{.code = "faulted", .message = std::move(message)});
    return result;
}

/// One launched nested call: its task and the thread waiting for it beside the
/// event loop. The thread ends before the invocation returns, so it may borrow
/// the invocation's runtime and Session.
struct Waiting {
    TaskId task{};
    std::jthread thread{};
};

class Execution final {
public:
    Execution(codemode::CodemodeSandbox& sandbox,
            ToolExecutionContext context,
            StoredState state,
            std::shared_ptr<Environment> environment)
        : sandbox_(sandbox), context_(std::move(context)), run_(std::move(state.run)),
          journal_(std::move(state.journal)), rerun_(state.rerun), environment_(std::move(environment)) {}

    support::Expected<ai::ToolResultMessage> execute(const codemode::CodemodeSource& source) {
        if (const auto* result = field(run_, "result")) return decoded_result(*result);
        const Json* start = field(run_, "start");
        if (start == nullptr) return finish_error("Invalid persisted codemode run");
        if (start->at("version").get_number() != kModuleVersion)
            return finish_error(
                    std::format("The script cannot resume: it started under code mode version {}, this is {}.",
                            start->at("version").get_number(),
                            kModuleVersion));
        const auto& loaded_entries = journal_.at("entries").get_array();
        journal_length_ = loaded_entries.size();
        for (std::size_t index = 0; index < loaded_entries.size(); ++index)
            replay_index_.insert_or_assign(
                    static_cast<std::int64_t>(loaded_entries[index].at("id").get_number()), index);
        replaying_ = journal_length_ > 0;
        if (const auto* first = field(journal_, "firstIssued"))
            first_issued_ = static_cast<std::int64_t>(first->get_number());
        durable_issued_ = first_issued_.value_or(0);
        for (const auto& entry : loaded_entries) {
            if (const auto* issued = field(entry, "issuedAfter"))
                durable_issued_ = std::max(durable_issued_, static_cast<std::int64_t>(issued->get_number()));
        }

        codemode::CodemodeRunOptions options{};
        options.started_at = static_cast<std::int64_t>(start->at("startedAt").get_number());
        options.seed = static_cast<std::uint64_t>(start->at("seed").get_number());
        options.stop_token = context_.runtime->stop_token();
        if (const auto* deadline = field(*start, "deadline")) {
            const auto remaining = deadline->get_number() - static_cast<double>(clock_ms());
            if (remaining <= 0) return finish_error("Script timed out: its deadline passed while it was interrupted.");
            options.timeout = std::chrono::milliseconds{static_cast<std::int64_t>(remaining)};
        }
        for (const auto& [key, value] : start->at("store").get_object())
            options.store.emplace(key, value);
        for (const auto& tool : start->at("tools").get_array()) {
            if (const auto* hidden = field(tool, "hidden");
                    hidden != nullptr && hidden->holds<bool>() && hidden->get_boolean())
                continue;
            options.tools.push_back(codemode::CodemodeToolDescriptor{.name = tool.at("name").get_string(),
                    .js_name = tool.at("jsName").get_string(),
                    .description = tool.at("description").get_string()});
        }
        for (const auto& global : start->at("globals").get_array()) {
            const auto* spread = field(global, "spread");
            options.globals.push_back(codemode::CodemodeGlobal{.name = global.at("name").get_string(),
                    .spread = spread != nullptr && spread->holds<bool>() && spread->get_boolean()});
        }
        if (const auto* fingerprint = field(*start, "fingerprint"))
            options.expected_fingerprint = fingerprint->get_string();
        events_ = std::make_shared<Events>();
        auto events = events_;
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
        WaitersGuard guard{*this};
        while (true) {
            auto event = events->pop();
            if (const auto* fingerprint = std::get_if<Started>(&event)) {
                auto& stored = run_.at("start");
                if (const auto* recorded = field(stored, "fingerprint");
                        recorded && recorded->get_string() != fingerprint->fingerprint)
                    return finish_error(
                            std::format("The script cannot resume: the sandbox changed since it started ({} -> {}).",
                                    recorded->get_string(),
                                    fingerprint->fingerprint));
                stored.get_object().insert_or_assign("fingerprint", Json{fingerprint->fingerprint});
                if (auto saved = save(context_, run_, journal_); !saved) return std::unexpected(saved.error());
            } else if (const auto* call = std::get_if<HostCall>(&event)) {
                if (auto observed = observe(run, *call); !observed) {
                    if (context_.runtime->stop_token().stop_requested()) return std::unexpected(observed.error());
                    return finish_error(observed.error().message);
                }
            } else if (const auto* idle = std::get_if<Idle>(&event)) {
                if (auto advanced = advance(run, *idle); !advanced) {
                    if (context_.runtime->stop_token().stop_requested()) return std::unexpected(advanced.error());
                    return finish_error(advanced.error().message);
                }
            } else if (const auto* completed = std::get_if<Completed>(&event)) {
                if (auto recorded = record(*completed); !recorded) {
                    if (context_.runtime->stop_token().stop_requested()) return std::unexpected(recorded.error());
                    return finish_error(recorded.error().message);
                }
                if (auto delivered = deliver_ready(run); !delivered) {
                    if (context_.runtime->stop_token().stop_requested()) return std::unexpected(delivered.error());
                    return finish_error(delivered.error().message);
                }
            } else {
                auto result = std::move(std::get<support::Expected<RunResult>>(event));
                if (!result) return std::unexpected(result.error());
                if (rerun_ && result->during && result->during->seq <= static_cast<std::int64_t>(journal_length_)) {
                    const auto seq = result->during->seq;
                    const auto& current = journal_.at("entries").get_array();
                    bool continued =
                            seq == 0 ? journal_length_ > 0 || first_issued_.has_value()
                                     : field(current[static_cast<std::size_t>(seq - 1)], "issuedAfter") != nullptr;
                    if (!continued) {
                        for (std::size_t index = static_cast<std::size_t>(seq); index < current.size(); ++index) {
                            if (static_cast<std::int64_t>(current[index].at("id").get_number()) >
                                    result->during->issued) {
                                continued = true;
                                break;
                            }
                        }
                    }
                    if (continued)
                        return finish_error(std::format(
                                "The script took a different path when resumed after a restart: it ended after result "
                                "{}, where it went on before.",
                                seq));
                }
                if (next_entry_ < journal_.at("entries").get_array().size())
                    return finish_error("The script took a different path when resumed after a restart: it ended "
                                        "before its recorded results.");
                return finish(*result);
            }
        }
    }

private:
    /// Stop the waits before this invocation ends. A script that decided its
    /// outcome leaves no call running behind it, so those calls are aborted
    /// (frozen pi `ToolTask.settle` aborting its owned calls). An interrupted
    /// invocation (close, or the parent's abort) aborts nothing: its nested tasks
    /// stay owned by the parent, resume after a restart, or are marked by the
    /// parent's own abort cascade.
    void stop_waiters() {
        finish_.request_stop();
        for (auto& waiting : waiting_) {
            if (waiting.thread.joinable()) waiting.thread.join();
        }
        if (decided_) {
            std::set<std::int64_t> settled;
            {
                std::lock_guard lock(settled_mutex_);
                settled = settled_tasks_;
            }
            for (const auto& waiting : waiting_) {
                if (settled.contains(waiting.task.value)) continue;
                (void)await_result(context_.runtime->abort_task(waiting.task));
            }
        }
        waiting_.clear();
    }

    /// The guard runs before the result is committed: a script that ends while a
    /// nested call still runs abandons no durable work.
    struct WaitersGuard {
        Execution& execution;
        ~WaitersGuard() { execution.stop_waiters(); }
    };

    [[nodiscard]] bool structured_tool(std::string_view name) const {
        const Json* start = field(run_, "start");
        if (start == nullptr) return false;
        for (const auto& tool : start->at("tools").get_array()) {
            if (tool.at("name").get_string() != name) continue;
            const auto* structured = field(tool, "structured");
            return structured != nullptr && structured->holds<bool>() && structured->get_boolean();
        }
        return false;
    }

    static bool same_arguments(const Json* recorded, const HostCall& call) {
        if (recorded == nullptr) return !call.arguments_json.has_value();
        if (!call.arguments_json) return false;
        auto incoming = support::read_json(*call.arguments_json);
        if (!incoming) return false;
        const auto left = support::canonical_json(*recorded);
        const auto right = support::canonical_json(*incoming);
        return left && right && *left == *right;
    }

    support::ExpectedVoid observe(codemode::CodemodeSandbox::Run& run, const HostCall& call) {
        observed_.insert_or_assign(call.id, call);
        if (call.target == HostCall::Target::Tool) {
            calls_.try_emplace(call.id, call_row(call));
        }
        const auto replayed = replay_index_.find(call.id);
        if (replayed == replay_index_.end()) return {};
        const auto& entry = journal_.at("entries").get_array()[replayed->second];
        if (const auto* global = field(entry, "global")) {
            if (call.target != HostCall::Target::Global || global->at("name").get_string() != call.name)
                return std::unexpected(failure(std::format(
                        "The script took a different path when resumed after a restart: call {} is {}, but was a call "
                        "to {}.",
                        call.id,
                        call.name,
                        global->at("name").get_string())));
            if (!same_arguments(field(*global, "args"), call))
                return std::unexpected(failure(std::format(
                        "The script took a different path when resumed after a restart: call {} changed its "
                        "arguments.",
                        call.id)));
            const auto* outcome = field(*global, "outcome");
            if (outcome == nullptr) return std::unexpected(failure("Invalid journaled codemode outcome"));
            auto decoded = outcome_from_json(*outcome);
            if (!decoded) return std::unexpected(decoded.error());
            answered_.insert_or_assign(call.id, std::move(*decoded));
            return deliver_ready(run);
        }
        if (call.target != HostCall::Target::Tool)
            return std::unexpected(failure(std::format(
                    "The script took a different path when resumed after a restart: call {} is not a tool call.",
                    call.id)));
        if (launched_.contains(call.id)) return {};
        if (auto started = start_call(call.id, call); !started) return started;
        return {};
    }

    support::ExpectedVoid advance(codemode::CodemodeSandbox::Run& run, Idle idle) {
        if (context_.runtime->stop_token().stop_requested())
            return std::unexpected(
                    support::make_error(support::ErrorCode::Cancelled, "Codemode invocation was interrupted"));
        if (idle.seq == 0) {
            if (const auto* recorded = field(journal_, "firstIssued")) {
                if (recorded->get_number() != static_cast<double>(idle.issued))
                    return std::unexpected(
                            failure("The script took a different path when resumed after a restart: its first run made "
                                    "another number of calls."));
            } else {
                journal_.get_object().insert_or_assign("firstIssued", Json{static_cast<double>(idle.issued)});
            }
        } else {
            auto& entries = journal_.at("entries").get_array();
            if (idle.seq < 0 || static_cast<std::size_t>(idle.seq) > entries.size())
                return std::unexpected(
                        failure("The script took a different path when resumed after a restart: its issued call count "
                                "changed."));
            auto& entry = entries[static_cast<std::size_t>(idle.seq - 1)];
            if (const auto* recorded = field(entry, "issuedAfter")) {
                if (recorded->get_number() != static_cast<double>(idle.issued))
                    return std::unexpected(failure(
                            "The script took a different path when resumed after a restart: its issued call count "
                            "changed."));
            } else {
                entry.get_object().insert_or_assign("issuedAfter", Json{static_cast<double>(idle.issued)});
            }
        }
        // Record the issued frontier and identities before admitting any side effect.
        if (auto saved = save(context_, run_, journal_); !saved) return saved;
        durable_issued_ = std::max(durable_issued_, idle.issued);
        if (replaying_ && idle.seq == static_cast<std::int64_t>(next_entry_)) {
            const auto& entries = journal_.at("entries").get_array();
            if (next_entry_ < entries.size()) {
                const auto next_id = static_cast<std::int64_t>(entries[next_entry_].at("id").get_number());
                if (next_id > idle.issued)
                    return std::unexpected(failure(std::format(
                            "The script took a different path when resumed after a restart: it waits without making "
                            "call {}, which it made before.",
                            next_id)));
            } else {
                replaying_ = false;
            }
        }
        if (auto delivered = deliver_ready(run); !delivered) return delivered;
        return launch_ready(run);
    }

    /// Deliver the journal's results in order, each once the VM has issued the
    /// call it answers (frozen pi `deliverReplayed`). A tool's answer comes from
    /// its nested call's stored result; a global's is in the journal entry.
    support::ExpectedVoid deliver_ready(codemode::CodemodeSandbox::Run& run) {
        auto& entries = journal_.at("entries").get_array();
        while (next_entry_ < entries.size()) {
            const auto& entry = entries[next_entry_];
            const auto id = static_cast<std::int64_t>(entry.at("id").get_number());
            if (!observed_.contains(id)) break;
            const auto answer = answered_.find(id);
            if (answer == answered_.end()) break;
            auto delivered = run.deliver(static_cast<std::int64_t>(next_entry_ + 1),
                    static_cast<std::int64_t>(entry.at("at").get_number()),
                    id,
                    answer->second);
            if (delivered) return std::unexpected(failure(delivered->message));
            answered_.erase(answer);
            delivered_.insert(id);
            ++next_entry_;
        }
        if (replaying_ && next_entry_ == entries.size()) replaying_ = false;
        if (replaying_) return {};
        return launch_ready(run);
    }

    /// Launch every observed call the journal does not answer yet, once the
    /// replayed part is delivered and its issued count is committed: each
    /// becomes an independent nested task (frozen pi `release`/`launchCovered`).
    support::ExpectedVoid launch_ready(codemode::CodemodeSandbox::Run& run) {
        (void)run;
        if (replaying_) return {};
        for (const auto& [id, call] : observed_) {
            if (delivered_.contains(id) || launched_.contains(id) || replay_index_.contains(id)) continue;
            if (id > durable_issued_) continue;
            if (call.target != HostCall::Target::Tool)
                return std::unexpected(failure(std::format("Unknown global {}", call.name)));
            if (auto started = start_call(id, call); !started) return started;
        }
        return {};
    }

    support::ExpectedVoid start_call(std::int64_t id, const HostCall& call) {
        ai::ToolCallContent nested{.id = std::format("{}/c{}", context_.call_id, id),
                .name = call.name,
                .arguments = support::JsonValue{support::JsonValue::object_t{}}};
        if (call.arguments_json) {
            auto parsed = support::read_json(*call.arguments_json);
            if (!parsed) return std::unexpected(parsed.error());
            nested.arguments = *parsed;
        }
        const std::string key = "c" + std::to_string(id);
        auto created = start_nested_call(*context_.runtime, context_.runtime->task_id(), context_.call_id, key, nested);
        if (!created)
            return std::unexpected(
                    failure(std::format("The script took a different path when resumed after a restart: call {}: {}",
                            id,
                            created.error().message)));
        launched_.insert(id);
        Waiting waiting;
        waiting.task = *created;
        auto events = events_;
        auto* runtime = context_.runtime;
        auto* session = context_.session;
        const auto invocation = runtime->stop_token();
        const auto finish = finish_.get_token();
        const TaskId task = *created;
        const std::string name = call.name;
        // The wait runs beside the event loop: the VM keeps processing what it
        // already has while this call runs (frozen pi `launch`). The thread
        // borrows `this` and the invocation's runtime and Session; `stop_waiters`
        // joins every thread before this Execution or the runtime is destroyed
        // (CODING_STANDARDS.md §7.5).
        waiting.thread = std::jthread([this, events, runtime, session, task, id, key, name, invocation, finish] {
            auto combined = std::stop_source{};
            std::stop_callback interrupted(invocation, [&combined] { combined.request_stop(); });
            std::stop_callback ended(finish, [&combined] { combined.request_stop(); });
            auto settled = await_result(runtime->wait_for_task(task, combined.get_token()));
            if (!settled) {
                if (invocation.stop_requested() || finish.stop_requested()) return;
                events->push(
                        Completed{.id = id, .at = clock_ms(), .result = fallback_result(TaskRecord{.id = task}, name)});
                return;
            }
            {
                std::lock_guard lock(settled_mutex_);
                settled_tasks_.insert(task.value);
            }
            auto document = await_result(session->snapshot(
                    nested_call_document(), DocumentAddress{.task_id = runtime->task_id(), .key = key}));
            support::Expected<NestedCallResult> decoded =
                    support::Expected<NestedCallResult>{fallback_result(*settled, name)};
            if (document && document->has_value()) {
                const auto state = (*document)->to_support();
                if (const Json* stored = field(state, "result")) {
                    if (auto converted = chord::Json::from_support(*stored)) {
                        // An unreadable member keeps the fallback error, so the
                        // script always receives an answer for its call.
                        if (auto read = decode_nested_result(*converted)) decoded = std::move(*read);
                    }
                }
            }
            events->push(Completed{.id = id, .at = clock_ms(), .result = std::move(*decoded)});
        });
        waiting_.push_back(std::move(waiting));
        return {};
    }

    /// Record one finished nested call: a new call's journal entry (and the
    /// replayed call's answer) is known before the VM receives it (frozen pi
    /// `pump`). A tool's outcome is not copied into the journal, which is what
    /// makes the document readable by frozen pi.
    support::ExpectedVoid record(const Completed& completed) {
        const auto call = observed_.find(completed.id);
        if (call == observed_.end()) return std::unexpected(failure("A nested call ended without being observed"));
        const auto& name = call->second.name;
        answered_.insert_or_assign(completed.id, nested_outcome(completed.result, name, structured_tool(name)));
        const auto row = calls_.find(completed.id);
        if (row != calls_.end()) {
            auto& value = row->second.get_object();
            value.insert_or_assign("status", Json{completed.result.is_error ? "error" : "ok"});
            if (completed.result.is_error) {
                std::string error;
                for (const auto& diagnostic : completed.result.diagnostics) {
                    if (!error.empty()) error += '\n';
                    error += diagnostic.message;
                }
                if (error.empty() && completed.result.structured_output)
                    error = text_of_json(*completed.result.structured_output);
                if (error.empty()) error = std::format("Tool {} failed", name);
                value.insert_or_assign("error", Json{std::move(error)});
            }
            if (completed.result.duration_ms)
                value.insert_or_assign("durationMs", Json{static_cast<double>(*completed.result.duration_ms)});
        }
        if (replay_index_.contains(completed.id)) return {};
        auto& entries = journal_.at("entries").get_array();
        Object entry{{"id", Json{static_cast<double>(completed.id)}}, {"at", Json{static_cast<double>(completed.at)}}};
        entries.push_back(Json{std::move(entry)});
        return save(context_, run_, journal_);
    }

    /// The run's output directory, created once and recorded before a file is
    /// written so a rerun writes the same fixed names (frozen pi `outputDir`).
    support::Expected<std::string> ensure_output_dir() {
        if (const auto* recorded = field(run_, "outputDir"); recorded != nullptr && recorded->holds<std::string>())
            return recorded->get_string();
        if (!environment_) return std::unexpected(failure("no execution environment"));
        auto created = environment_->create_temp_dir("pi-codemode-", context_.runtime->stop_token());
        if (!created) return std::unexpected(created.error());
        run_.get_object().insert_or_assign("outputDir", *created);
        if (auto saved = save(context_, run_, journal_); !saved) return std::unexpected(saved.error());
        return *created;
    }

    /// One output file under the run's directory; a failed write returns its
    /// reason so the caller can keep a truthful label (frozen pi `outputFiles`).
    support::Expected<std::string> write_output(std::string_view name, std::string_view data) {
        // A recorded `outputDir` is not enough to write: the run may continue in
        // a Harness whose extension has no environment at all (frozen pi checks
        // `env === undefined` before it reads the directory).
        if (!environment_) return std::unexpected(failure("no execution environment"));
        auto directory = ensure_output_dir();
        if (!directory) return std::unexpected(directory.error());
        const auto path = (std::filesystem::path{*directory} / std::string{name}).string();
        if (auto written = environment_->write(path, data, context_.runtime->stop_token()); !written)
            return std::unexpected(written.error());
        return path;
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
        decided_ = true;
        const Json start = run_.at("start");
        const auto* max_tokens = field(start, "maxOutputTokens");
        auto formatted = format_result(result,
                start,
                calls_,
                max_tokens != nullptr && max_tokens->holds<double>()
                        ? static_cast<std::int64_t>(max_tokens->get_number())
                        : kDefaultMaxOutputTokens,
                [this](std::string_view name, std::string_view data) { return write_output(name, data); });
        auto tool = std::move(formatted.message);
        run_.get_object().insert_or_assign("result", encoded_result(tool));
        if (auto saved = save(context_, run_, journal_, result.error ? std::nullopt : result.store_writes); !saved)
            return std::unexpected(saved.error());
        return tool;
    }

    codemode::CodemodeSandbox& sandbox_; // extension snapshot outlives execute
    ToolExecutionContext context_;
    Json run_;
    Json journal_;
    bool rerun_{false};
    bool replaying_{false};
    std::shared_ptr<Environment> environment_;
    std::size_t journal_length_{0};
    std::size_t next_entry_{0};
    std::optional<std::int64_t> first_issued_;
    std::int64_t durable_issued_{0};
    std::map<std::int64_t, std::size_t> replay_index_;
    std::map<std::int64_t, HostCall> observed_;
    std::map<std::int64_t, codemode::CodemodeOutcome> answered_;
    std::map<std::int64_t, Json> calls_;
    std::set<std::int64_t> delivered_;
    std::set<std::int64_t> launched_;
    std::shared_ptr<Events> events_;
    /// Requests when this invocation ends, so no wait outlives it.
    std::stop_source finish_;
    std::vector<Waiting> waiting_;
    std::mutex settled_mutex_;
    std::set<std::int64_t> settled_tasks_;
    /// Whether this invocation decided the call's outcome; only then do its
    /// still-running nested calls have to be aborted.
    bool decided_{false};
};

ai::ToolResultMessage rejected(std::string message) {
    return ai::ToolResultMessage{.content = {ai::text_content(std::move(message))}, .is_error = true};
}

support::Expected<ai::ToolResultMessage> execute(codemode::CodemodeSandbox& sandbox,
        Json arguments,
        ToolExecutionContext context,
        std::shared_ptr<Environment> environment) {
    if (!context.runtime || !context.session) return rejected("No durable tool execution context");
    const auto* code = field(arguments, "code");
    if (!code || !code->holds<std::string>()) return rejected("Invalid codemode source");
    auto parsed = codemode::parse_codemode_source(code->get_string());
    if (!parsed) return rejected(parsed.error().message);
    auto state = load_or_create(context, *parsed);
    if (!state) {
        // An unusable stored document is a model-visible tool error, not a task
        // fault; the caller still receives the explicit refusal.
        if (state.error().message == "A serial-era codemode run document cannot be continued")
            return rejected(state.error().message);
        return std::unexpected(state.error());
    }
    if (!valid_run(state->run) || !valid_journal(state->journal)) return rejected("Invalid persisted codemode state");
    return Execution{sandbox, std::move(context), std::move(*state), std::move(environment)}.execute(*parsed);
}

} // namespace

Extension codemode_extension(
        std::unique_ptr<codemode::CodemodeSandbox> sandbox, std::shared_ptr<Environment> environment) {
    Extension extension{.name = "codemode"};
    extension.tools.push_back(ToolRegistration{
            .declaration = ai::Tool{.name = "codemode",
                    .description = "Run JavaScript with tools, text(), image(), store() and load().",
                    .parameters = Json{Object{{"type", "object"},
                            {"properties", Json{Object{{"code", Json{Object{{"type", "string"}}}}}}},
                            {"required", Json{Array{"code"}}}}}},
            .execute_durable = [sandbox = std::move(sandbox), environment = std::move(environment)](Json arguments,
                                       ToolExecutionContext context) -> support::Expected<ai::ToolResultMessage> {
                if (!sandbox) return std::unexpected(failure("No codemode sandbox is configured"));
                return execute(*sandbox, std::move(arguments), std::move(context), environment);
            },
            .replay_safe = true});
    return extension;
}

DocumentDefinition codemode_store_document() {
    return detail::builtin_document("pi.codemode.store", DocumentHistory::Rewindable, DocumentFork::AsOf, [] {
        return chord::Json{chord::Json::Object{{"values", chord::Json{chord::Json::Object{}}}}};
    });
}

support::Expected<std::optional<ai::ToolResultMessage>> aborted_codemode_result(
        Transaction& tx, const TaskRecord& task) {
    const auto active = task.memos.find("codemode.active");
    if (active == task.memos.end() || !active->second.as_string()) return std::nullopt;
    const auto& call_id = *active->second.as_string();
    auto document = tx.doc(run_document(), DocumentAddress{.task_id = task.id});
    if (!document) return std::unexpected(document.error());
    auto value = document->get({});
    if (!value) return std::unexpected(value.error());
    if (!value->has_value()) return std::unexpected(failure("Invalid persisted codemode document"));
    const auto state = (**value).to_support();
    if (!valid_run(state)) return std::unexpected(failure("Invalid persisted codemode run"));
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
    // The abort handler reports what a model-visible call reports when it is
    // aborted: one harness error, with no details (frozen pi `ToolTask.abort`).
    ai::ToolResultMessage aborted{.tool_call_id = call_id,
            .tool_name = "codemode",
            .content = harness_error_content("Tool codemode was aborted"),
            .is_error = true};
    return aborted;
}

} // namespace cch::durable
