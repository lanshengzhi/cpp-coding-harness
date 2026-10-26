#include "StorageEncoding.hpp"
#include "WorkflowRuntime.hpp"

#include <cch/chord/Json.hpp>
#include <cch/durable/Session.hpp>
#include <cch/durable/Storage.hpp>

#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <format>
#include <limits>
#include <mutex>
#include <type_traits>
#include <utility>

namespace cch::durable {
namespace {

// Only the existing Harness execution thread waits here. Public admission and
// root operations remain callback based; no additional execution domain is created.
template <typename T> support::Expected<T> await_result(support::AsyncResult<T> operation) {
    std::mutex mutex;
    std::condition_variable ready;
    std::optional<support::Expected<T>> outcome;
    // The wait below keeps these locals alive until completion returns.
    std::move(operation).start([&mutex, &ready, &outcome](support::Expected<T> result) noexcept {
        std::lock_guard lock(mutex);
        outcome.emplace(std::move(result));
        ready.notify_one();
    });
    std::unique_lock lock(mutex);
    ready.wait(lock, [&outcome] { return outcome.has_value(); });
    return std::move(*outcome);
}

using Json = chord::Json;
using Object = Json::Object;
using Array = Json::Array;

Json numeric(std::int64_t value) { return Json{static_cast<double>(value)}; }

Json tool_json(const ai::Tool& tool) {
    Object value{{"name", tool.name}, {"description", tool.description},
            {"parameters", *Json::from_support(tool.parameters)}};
    if (tool.output_schema) value.emplace("outputSchema", *Json::from_support(*tool.output_schema));
    return Json{std::move(value)};
}

template <typename Block> Json block_json(const Block& block) {
    return std::visit([](const auto& item) -> Json {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, ai::TextContent>) {
            Object value{{"type", "text"}, {"text", item.text}};
            if (item.text_signature) value.emplace("textSignature", *item.text_signature);
            return Json{std::move(value)};
        } else if constexpr (std::is_same_v<T, ai::ThinkingContent>) {
            Object value{{"type", "thinking"}, {"thinking", item.thinking}};
            if (item.thinking_signature) value.emplace("thinkingSignature", *item.thinking_signature);
            if (item.redacted) value.emplace("redacted", true);
            return Json{std::move(value)};
        } else if constexpr (std::is_same_v<T, ai::ImageContent>) {
            return Json{Object{{"type", "image"}, {"data", item.data}, {"mimeType", item.mime_type}}};
        } else {
            Object value{{"type", "toolCall"}, {"id", item.id}, {"name", item.name},
                    {"arguments", item.arguments ? *Json::from_support(*item.arguments) : Json{Object{}}}};
            if (item.thought_signature) value.emplace("thoughtSignature", *item.thought_signature);
            return Json{std::move(value)};
        }
    }, block);
}

template <typename Blocks> Json blocks_json(const Blocks& blocks) {
    Array result;
    for (const auto& block : blocks) result.push_back(block_json(block));
    return Json{std::move(result)};
}

Json user_json(const ai::UserMessage& message) {
    Json content;
    if (const auto* text = std::get_if<std::string>(&message.content)) content = Json{*text};
    else content = blocks_json(std::get<std::vector<ai::Content>>(message.content));
    return Json{Object{{"role", "user"}, {"content", std::move(content)}, {"timestamp", numeric(message.timestamp)}}};
}

Json system_json(const ai::SystemMessage& message) {
    Object result{{"role", "system"}, {"content", message.content}, {"timestamp", numeric(message.timestamp)}};
    Object sections;
    for (const auto& section : message.sections) {
        sections.emplace(section.name, section.text ? Json{*section.text} : Json{});
    }
    if (!sections.empty()) result.emplace("sections", Json{std::move(sections)});
    Array tools;
    for (const auto& tool : message.tools_added) tools.push_back(tool_json(tool));
    if (!tools.empty()) result.emplace("toolsAdded", Json{std::move(tools)});
    return Json{std::move(result)};
}

Json assistant_json(const ai::AssistantMessage& message) {
    Object result{{"role", "assistant"}, {"content", blocks_json(message.content)},
            {"api", message.api}, {"provider", message.provider}, {"model", message.model},
            {"stopReason", ai::stop_reason_to_string(message.stop_reason)}, {"timestamp", numeric(message.timestamp)}};
    if (message.error_message) result.emplace("errorMessage", *message.error_message);
    return Json{std::move(result)};
}

Json tool_result_json(const ai::ToolResultMessage& message) {
    Object result{{"role", "toolResult"}, {"toolCallId", message.tool_call_id}, {"toolName", message.tool_name},
            {"content", blocks_json(message.content)}, {"isError", message.is_error},
            {"timestamp", numeric(message.timestamp)}};
    if (message.duration_ms) result.emplace("durationMs", numeric(*message.duration_ms));
    return Json{std::move(result)};
}

Json entry_data(Json message, std::optional<TaskId> task = std::nullopt) {
    Object value{{"model", Json{Array{std::move(message)}}}};
    if (task) value.emplace("byTaskId", numeric(task->value));
    return Json{std::move(value)};
}

chord::Json as_chord(Json json) {
    return chord::Json::from_support(json.to_support()).value();
}

std::string thinking_name(ai::ThinkingLevel level) {
    for (const auto& [model_level, name] : ai::kModelThinkingLevels) {
        (void)model_level;
        if (ai::parse_stream_thinking_level(name) == level) return std::string{name};
    }
    std::terminate();
}

std::vector<std::pair<std::string, Json>> initial_documents(const RootOptions& options, std::string provider_session) {
    Object agent;
    if (!options.model.id.empty()) {
        agent.emplace("model", Json{Object{{"provider", options.model.provider}, {"modelId", options.model.id}}});
    }
    if (options.thinking_level) agent.emplace("thinkingLevel", thinking_name(*options.thinking_level));
    return {{"pi.agent:v1", Json{std::move(agent)}},
            {"pi.inbox:v1", Json{Object{{"items", Json{Array{}}}}}},
            {"pi.live:v1", Json{Object{}}},
            {"pi.provider:v1", Json{Object{{"sessionId", std::move(provider_session)}}}},
            {"pi.usage:v1", Json{Object{{"models", Json{Object{}}}, {"tools", Json{Object{}}}}}}};
}

support::Error validation(std::string message) {
    return support::make_error(support::ErrorCode::Validation, std::move(message));
}

support::Expected<std::int64_t> integer_field(const Json& json, std::string_view field) {
    const auto* object = json.as_object();
    if (object == nullptr) return std::unexpected(validation("Expected an object"));
    const auto found = object->find(std::string{field});
    const auto* number = found == object->end() ? nullptr : found->second.as_number();
    // The strict bounds avoid double rounding INT64_MAX up to 2^63.
    if (number == nullptr || !(*number >= -0x1p63 && *number < 0x1p63) || std::trunc(*number) != *number) {
        return std::unexpected(validation("Expected an integer field"));
    }
    return static_cast<std::int64_t>(*number);
}

support::Expected<ai::UserMessage> input_user(const EntryRecord& entry) {
    const auto* envelope = entry.data.as_object();
    if (envelope == nullptr) return std::unexpected(validation("Invalid input entry"));
    const auto model = envelope->find("model");
    const auto* messages = model == envelope->end() ? nullptr : model->second.as_array();
    if (messages == nullptr || messages->size() != 1 || messages->front().as_object() == nullptr) {
        return std::unexpected(validation("Invalid input message"));
    }
    const auto& message = *messages->front().as_object();
    const auto content = message.find("content");
    const auto* blocks = content == message.end() ? nullptr : content->second.as_array();
    if (blocks == nullptr || blocks->size() != 1 || blocks->front().as_object() == nullptr) {
        return std::unexpected(validation("Invalid input content"));
    }
    const auto& block = *blocks->front().as_object();
    const auto text = block.find("text");
    if (text == block.end() || text->second.as_string() == nullptr) return std::unexpected(validation("Invalid input text"));
    auto timestamp = integer_field(messages->front(), "timestamp");
    if (!timestamp) return std::unexpected(timestamp.error());
    return ai::user_text_message(*text->second.as_string(), *timestamp);
}

} // namespace

TaskHookRegistration task_hook(std::string task_kind, std::string hook_name, TaskHook handler) {
    TaskHookRegistration reg{.task = std::move(task_kind)};
    reg.handlers.emplace(std::move(hook_name), std::move(handler));
    return reg;
}

TaskHookRegistration task_hook(const TaskDefinition& task, std::string hook_name, TaskHook handler) {
    return task_hook(task.kind(), std::move(hook_name), std::move(handler));
}

TaskHookRegistration task_hook(
        const TaskDefinition& task, std::string hook_name, std::move_only_function<support::ExpectedVoid()> handler) {
    return task_hook(task.kind(),
            std::move(hook_name),
            [fn = std::move(handler)](const chord::Json&, TaskRuntime&) mutable -> support::AsyncResult<chord::Json> {
                if (!fn) {
                    return support::AsyncResult<chord::Json>{chord::Json{nullptr}};
                }
                auto res = fn();
                if (!res) {
                    return support::AsyncResult<chord::Json>{std::unexpected(res.error())};
                }
                return support::AsyncResult<chord::Json>{chord::Json{nullptr}};
            });
}

struct InstalledExtension {
    std::string name{};
    std::vector<std::shared_ptr<ToolRegistration>> tools{};
    std::vector<PromptSection> sections{};
    std::vector<std::shared_ptr<TaskDefinition>> tasks{};
    std::vector<std::shared_ptr<TaskHookRegistration>> hooks{};
};

struct Registry::Impl {
    mutable std::mutex mutex;
    mutable std::mutex notify_mutex;
    std::vector<InstalledExtension> extensions;
    Snapshot current;
    std::uint64_t next_subscriber_id{1};
    std::map<std::uint64_t, std::shared_ptr<std::move_only_function<void()>>> subscribers;

    void rebuild_snapshot() {
        Snapshot next;
        for (const auto& ext : extensions) {
            for (const auto& tool : ext.tools) {
                auto existing = std::ranges::find_if(next.tools,
                        [&tool](const auto& item) { return item->declaration.name == tool->declaration.name; });
                if (existing != next.tools.end()) {
                    *existing = tool;
                } else {
                    next.tools.push_back(tool);
                }
            }
            for (const auto& section : ext.sections) {
                auto existing = std::ranges::find_if(
                        next.sections, [&section](const auto& item) { return item.key == section.key; });
                if (existing != next.sections.end()) {
                    *existing = section;
                } else {
                    next.sections.push_back(section);
                }
            }
            for (const auto& task : ext.tasks) {
                next.tasks.push_back(task);
            }
            for (const auto& hook : ext.hooks) {
                next.hooks.push_back(hook);
            }
        }
        current = std::move(next);
    }

    [[nodiscard]] std::vector<std::shared_ptr<std::move_only_function<void()>>> copy_subscribers() const {
        std::vector<std::shared_ptr<std::move_only_function<void()>>> out;
        out.reserve(subscribers.size());
        for (const auto& [id, fn] : subscribers) {
            (void)id;
            out.push_back(fn);
        }
        return out;
    }

    void notify_all(const std::vector<std::shared_ptr<std::move_only_function<void()>>>& listeners) {
        std::lock_guard notify_lock(notify_mutex);
        for (const auto& fn : listeners) {
            if (fn && *fn) {
                (*fn)();
            }
        }
    }
};

Registry::Registry() : impl_(std::make_unique<Impl>()) {}
Registry::Registry(Registry&&) noexcept = default;
Registry& Registry::operator=(Registry&&) noexcept = default;
Registry::~Registry() = default;

support::ExpectedVoid Registry::install(Extension extension) {
    if (extension.name.empty()) {
        return std::unexpected(validation("Extension name must be non-empty"));
    }
    {
        std::vector<std::string> tool_names;
        for (const auto& tool : extension.tools) {
            if (tool.declaration.name.empty() || !tool.execute ||
                    std::ranges::find(tool_names, tool.declaration.name) != tool_names.end()) {
                return std::unexpected(validation("Tool name must be non-empty, unique and executable"));
            }
            if (auto parameters = Json::from_support(tool.declaration.parameters); !parameters) {
                return std::unexpected(parameters.error());
            }
            if (tool.declaration.output_schema) {
                if (auto schema = Json::from_support(*tool.declaration.output_schema); !schema) {
                    return std::unexpected(schema.error());
                }
            }
            tool_names.push_back(tool.declaration.name);
        }
    }
    {
        std::vector<std::string> section_keys;
        for (const auto& section : extension.sections) {
            if (section.key.empty() || std::ranges::find(section_keys, section.key) != section_keys.end()) {
                return std::unexpected(validation("Section key must be non-empty and unique"));
            }
            section_keys.push_back(section.key);
        }
    }
    {
        std::vector<std::string> task_kinds;
        for (const auto& task : extension.tasks) {
            if (task.kind().empty() || task.kind() == "pi.generation" ||
                    std::ranges::find(task_kinds, task.kind()) != task_kinds.end()) {
                return std::unexpected(validation(
                        "Task " + task.kind() + " of extension " + extension.name + " is already installed"));
            }
            task_kinds.push_back(task.kind());
        }
    }

    std::vector<std::shared_ptr<std::move_only_function<void()>>> listeners;
    {
        std::lock_guard lock(impl_->mutex);
        for (const auto& task : extension.tasks) {
            for (const auto& existing_ext : impl_->extensions) {
                if (existing_ext.name == extension.name) continue;
                for (const auto& existing_task : existing_ext.tasks) {
                    if (existing_task->kind() == task.kind()) {
                        return std::unexpected(validation(
                                "Task " + task.kind() + " of extension " + extension.name + " is already installed"));
                    }
                }
            }
        }

        InstalledExtension installed_ext{.name = std::move(extension.name), .sections = std::move(extension.sections)};
        for (auto& tool : extension.tools) {
            installed_ext.tools.push_back(std::make_shared<ToolRegistration>(std::move(tool)));
        }
        for (auto& task : extension.tasks) {
            installed_ext.tasks.push_back(std::make_shared<TaskDefinition>(std::move(task)));
        }
        for (auto& hook : extension.hooks) {
            installed_ext.hooks.push_back(std::make_shared<TaskHookRegistration>(std::move(hook)));
        }

        auto existing = std::ranges::find_if(
                impl_->extensions, [&installed_ext](const auto& item) { return item.name == installed_ext.name; });
        if (existing != impl_->extensions.end()) {
            *existing = std::move(installed_ext);
        } else {
            impl_->extensions.push_back(std::move(installed_ext));
        }
        impl_->rebuild_snapshot();
        listeners = impl_->copy_subscribers();
    }
    impl_->notify_all(listeners);
    return {};
}

void Registry::uninstall(std::string_view extension_name) {
    std::vector<std::shared_ptr<std::move_only_function<void()>>> listeners;
    {
        std::lock_guard lock(impl_->mutex);
        auto existing = std::ranges::find_if(
                impl_->extensions, [extension_name](const auto& item) { return item.name == extension_name; });
        if (existing == impl_->extensions.end()) {
            return;
        }
        impl_->extensions.erase(existing);
        impl_->rebuild_snapshot();
        listeners = impl_->copy_subscribers();
    }
    impl_->notify_all(listeners);
}

Registry::Snapshot Registry::snapshot() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->current;
}

std::uint64_t Registry::subscribe(std::move_only_function<void()> listener) {
    std::lock_guard lock(impl_->mutex);
    const std::uint64_t id = impl_->next_subscriber_id++;
    impl_->subscribers.emplace(id, std::make_shared<std::move_only_function<void()>>(std::move(listener)));
    return id;
}

void Registry::unsubscribe(std::uint64_t id) {
    if (id == 0) return;
    std::lock_guard notify_lock(impl_->notify_mutex);
    std::lock_guard lock(impl_->mutex);
    impl_->subscribers.erase(id);
}

WorkflowRuntime::WorkflowRuntime(Session& session, Storage& storage, HarnessOptions& options,
        std::shared_ptr<std::move_only_function<std::int64_t()>> clock)
    : session_(session), storage_(storage), registry_(options.registry ? *options.registry : empty_registry_),
      stream_(std::move(options.stream)), settings_(std::move(options.settings)), clock_(std::move(clock)) {}

std::int64_t WorkflowRuntime::now() const { return (*clock_)(); }

TaskDefinition WorkflowRuntime::definition() {
    TaskDefinitionBuilder builder{"pi.generation", 1};
    builder.initial([](const Json&) { return Json{Object{{"phase", "generate"}}}; });
    // This runtime is owned by Harness::Impl, which joins every execution
    // before releasing the definition or this callback's borrowed pointer.
    builder.phase("generate", [this](const TaskRecord& task, TaskRuntime& runtime) { return generate(task, runtime); });
    auto result = builder.build();
    if (!result) std::terminate(); // fixed, valid built-in definition
    return std::move(*result);
}

support::AsyncResult<ConversationId> WorkflowRuntime::root(RootOptions options) {
    auto selected = std::make_shared<RootOptions>(std::move(options));
    auto created = std::make_shared<bool>(false);
    return support::AsyncResult<ConversationId>{support::AsyncProducer<ConversationId, support::Error>{
            [this, selected, created](support::AsyncCompletion<ConversationId, support::Error> done) mutable noexcept {
                session_.commit([selected, created](Transaction& tx) -> support::ExpectedVoid {
                    auto existing = tx.conversation(ConversationId{.value = 1});
                    if (!existing) return std::unexpected(existing.error());
                    if (existing->has_value()) return {};
                    auto record = tx.create_root_conversation();
                    if (!record) return std::unexpected(record.error());
                    const auto documents = initial_documents(*selected, boost::uuids::to_string(boost::uuids::random_generator{}()));
                    for (const auto& [kind, initial] : documents) {
                        auto definition = define_document(kind, [initial] { return initial; });
                        auto doc = tx.doc(definition, DocumentAddress{.conversation_id = record->id});
                        if (!doc) return std::unexpected(doc.error());
                    }
                    *created = true;
                    return {};
                }).start([this, selected, created, done = std::move(done)](support::ExpectedVoid result) mutable noexcept {
                    if (!result) {
                        done(std::unexpected(result.error()));
                        return;
                    }
                    if (*created) {
                        model_ = std::move(selected->model);
                        thinking_level_ = selected->thinking_level;
                    }
                    done(ConversationId{.value = 1});
                });
            }}};
}

support::AsyncResult<Submission> WorkflowRuntime::submit(
        ConversationId conversation_id, std::string text, const TaskDefinition& definition) {
    auto admission = std::make_shared<Submission>();
    return support::AsyncResult<Submission>{support::AsyncProducer<Submission, support::Error>{
            [this, conversation_id, text = std::move(text), definition = &definition, admission](
                    support::AsyncCompletion<Submission, support::Error> done) mutable noexcept {
                if (!stream_) {
                    done(std::unexpected(validation("Harness has no model stream factory")));
                    return;
                }
                session_.commit([this, conversation_id, text = std::move(text), definition, admission](
                        Transaction& tx) -> support::ExpectedVoid {
                    auto conversation = tx.conversation(conversation_id);
                    if (!conversation) return std::unexpected(conversation.error());
                    if (!conversation->has_value() || conversation_id.value != 1) {
                        return std::unexpected(validation("The workflow requires the root conversation"));
                    }
                    auto tasks = tx.scan_tasks(std::numeric_limits<std::size_t>::max());
                    if (!tasks) return std::unexpected(tasks.error());
                    for (const auto& task : *tasks) {
                        if (task.conversation_id == conversation_id && task.state.status != "terminal") {
                            return std::unexpected(validation("Conversation is busy"));
                        }
                    }
                    const auto user = ai::user_text_message(text, now());
                    auto entry = tx.append_entry(conversation_id, "pi.user", entry_data(user_json(user)), std::nullopt,
                            std::vector<chord::Json>{as_chord(user_json(user))});
                    if (!entry) return std::unexpected(entry.error());
                    auto task = tx.create_task(*definition, Json{Object{{"entry", numeric(entry->id.value)}}},
                            TaskOptions{.conversation_id = conversation_id});
                    if (!task) return std::unexpected(task.error());
                    *admission = Submission{.id = *task, .conversation_id = conversation_id, .entry = entry->id};
                    return {};
                }).start([done = std::move(done), admission](support::ExpectedVoid result) mutable noexcept {
                    if (!result) done(std::unexpected(result.error()));
                    else done(*admission);
                });
            }}};
}

support::AsyncResult<void> WorkflowRuntime::generate(const TaskRecord& task, TaskRuntime& runtime) {
    return support::AsyncResult<void>{run(task, runtime)};
}

WorkflowRuntime::PromptPlan WorkflowRuntime::plan_prompt(const Registry::Snapshot& snapshot,
        std::span<const ai::Tool> shown_tools, std::span<const ai::SystemMessageSection> shown_sections,
        std::int64_t timestamp) {
    PromptPlan plan{.system = ai::SystemMessage{.timestamp = timestamp}};
    for (const auto& tool : snapshot.tools) {
        plan.context.tools.push_back(tool->declaration);
        if (std::ranges::none_of(shown_tools, [&tool](const auto& shown) { return shown.name == tool->declaration.name; })) {
            plan.system.tools_added.push_back(tool->declaration);
        }
    }
    for (const auto& section : snapshot.sections) {
        const std::string text = section.tag ? std::format("<{}>\n{}\n</{}>", section.key, section.text, section.key) : section.text;
        if (plan.context.system_prompt) *plan.context.system_prompt += "\n\n";
        else plan.context.system_prompt.emplace();
        *plan.context.system_prompt += text;
        if (std::ranges::none_of(shown_sections, [&section](const auto& shown) { return shown.name == section.key; })) {
            plan.system.sections.push_back(ai::SystemMessageSection{.name = section.key, .text = text});
        }
    }
    return plan;
}

ai::ToolResultMessage WorkflowRuntime::execute_tool(const ai::ToolCallContent& call,
        const Registry::Snapshot& snapshot, std::stop_token stop_token) {
    ai::ToolResultMessage result{.tool_call_id = call.id, .tool_name = call.name, .timestamp = now()};
    auto found = std::ranges::find_if(snapshot.tools, [&call](const auto& tool) { return tool->declaration.name == call.name; });
    if (found == snapshot.tools.end() || !call.arguments_valid || !call.arguments) {
        result.is_error = true;
        result.content.push_back(ai::text_content(found == snapshot.tools.end() ? std::format("Tool {} not found", call.name) : "Invalid tool arguments"));
        return result;
    }
    const auto started = std::chrono::steady_clock::now();
    auto output = await_result((*found)->execute(*call.arguments, stop_token));
    result.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    result.is_error = !output.has_value();
    result.content = output ? std::move(*output) : std::vector<ai::Content>{ai::text_content(output.error().message)};
    return result;
}

support::ExpectedVoid WorkflowRuntime::run(const TaskRecord& task, TaskRuntime& runtime) {
    auto input = integer_field(task.input, "entry");
    if (!input) return std::unexpected(input.error());
    auto entry = storage_.entry(EntryId{.value = *input});
    if (!entry) return std::unexpected(entry.error());
    if (!entry->has_value()) return std::unexpected(validation("Input entry not found"));
    auto user = input_user(entry->value().entry);
    if (!user) return std::unexpected(user.error());
    messages_.push_back(std::move(*user));

    while (!runtime.stop_token().stop_requested()) {
        const auto snapshot = registry_.snapshot();
        auto plan = plan_prompt(snapshot, shown_tools_, shown_sections_, now());
        auto& context = plan.context;
        const auto& system = plan.system;
        if (!system.sections.empty() || !system.tools_added.empty()) {
            auto committed = await_result(runtime.commit([id = task.conversation_id, system](Transaction& tx, const TaskRecord&)
                    -> support::Expected<std::optional<NextTaskState>> {
                auto entry = tx.append_entry(id, "pi.system", entry_data(system_json(system)), std::nullopt,
                            std::vector<chord::Json>{as_chord(system_json(system))});
                if (!entry) return std::unexpected(entry.error());
                return std::nullopt;
            }));
            if (!committed) return committed;
            messages_.push_back(system);
            for (const auto& section : system.sections) shown_sections_.push_back(section);
            shown_tools_ = context.tools;
        }
        context.messages = messages_;
        auto options = settings_.stream;
        if (thinking_level_) options.reasoning = thinking_level_;
        options.stop_token = runtime.stop_token();
        auto stream = stream_(model_, std::move(context), options);
        auto response = await_result(std::move(stream).run([](const ai::AssistantStreamEvent&) -> support::ExpectedVoid { return {}; }));
        if (!response) return std::unexpected(response.error());
        const bool tools = response->stop_reason == ai::AssistantStopReason::ToolUse;
        const bool failed = response->stop_reason == ai::AssistantStopReason::Error;
        const bool aborted = response->stop_reason == ai::AssistantStopReason::Aborted;
        auto committed = await_result(runtime.commit([id = task.conversation_id, response = *response, tools, failed, aborted](
                Transaction& tx, const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
            auto entry = tx.append_entry(id, "pi.assistant", entry_data(assistant_json(response)), std::nullopt,
                            std::vector<chord::Json>{as_chord(assistant_json(response))});
            if (!entry) return std::unexpected(entry.error());
            if (tools) return std::nullopt;
            return NextTaskState::terminal(TaskOutcome{.status = aborted ? "aborted" : failed ? "failed" : "completed",
                    .result = Json{Object{{"answer", numeric(entry->id.value)}}},
                    .error = failed ? std::optional<TaskOutcomeError>{TaskOutcomeError{.message = response.error_message.value_or("Generation failed")}} : std::nullopt});
        }));
        if (!committed) return committed;
        messages_.push_back(*response);
        if (!tools) return {};
        for (const auto& content : response->content) {
            const auto* call = std::get_if<ai::ToolCallContent>(&content);
            if (call == nullptr) continue;
            auto result = execute_tool(*call, snapshot, runtime.stop_token());
            auto saved = await_result(runtime.commit([id = task.conversation_id, tid = task.id, result](Transaction& tx, const TaskRecord&)
                    -> support::Expected<std::optional<NextTaskState>> {
                auto entry = tx.append_entry(id, "pi.tool-result", entry_data(tool_result_json(result), tid), std::nullopt,
                            std::vector<chord::Json>{as_chord(tool_result_json(result))}, tid);
                if (!entry) return std::unexpected(entry.error());
                return std::nullopt;
            }));
            if (!saved) {
                return std::unexpected(validation("Tool result commit failed: " + saved.error().message));
            }
            messages_.push_back(std::move(result));
        }
    }
    return std::unexpected(validation("Request was aborted"));
}

support::Expected<SettledSubmission> WorkflowRuntime::receipt(const TaskRecord& task) {
    if (task.kind != "pi.generation" || task.state.status != "terminal" || !task.state.outcome) {
        return std::unexpected(validation("Not a settled generation task"));
    }
    auto input = integer_field(task.input, "entry");
    if (!input) return std::unexpected(input.error());
    SettledSubmission result{.id = task.id, .conversation_id = task.conversation_id,
            .entry = EntryId{.value = *input},
            .status = task.state.outcome->status == "completed" ? "done" : task.state.outcome->status};
    if (task.state.outcome->result) {
        auto answer = integer_field(*task.state.outcome->result, "answer");
        if (!answer) return std::unexpected(answer.error());
        result.answer = EntryId{.value = *answer};
    }
    if (task.state.outcome->error) result.error = task.state.outcome->error->message;
    return result;
}

} // namespace cch::durable
