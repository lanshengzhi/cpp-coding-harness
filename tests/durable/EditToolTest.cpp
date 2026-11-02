#include <cch/ai/Content.hpp>
#include <cch/ai/Models.hpp>
#include <cch/durable/Environment.hpp>
#include <cch/durable/Harness.hpp>
#include <cch/durable/Storage.hpp>
#include <cch/durable/Workflow.hpp>
#include "durable/FrozenExpectation.hpp"
#include "durable/Outcome.hpp"
#include "support/TempWorkspace.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <semaphore>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace cch::tests::durable {
using namespace cch::durable;

namespace {

constexpr std::string_view kFrozenArtifact = "fixtures/pi-v1.3.0/audit-1159/edit-tool.json";

SubmissionDraft text_input(std::string text) {
    return SubmissionDraft{.body = InputSubmissionDraft{.content = chord::Json{std::move(text)}}};
}

const chord::Json& entry_model(const EntryRecord& entry) {
    const auto* envelope = entry.data.as_object();
    REQUIRE(envelope != nullptr);
    const auto found = envelope->find("model");
    REQUIRE(found != envelope->end());
    const auto* messages = found->second.as_array();
    REQUIRE(messages != nullptr);
    REQUIRE(messages->size() == 1);
    return messages->front();
}

/// The model-visible text of one recorded tool result message.
std::string result_text(const chord::Json& message) {
    const auto* fields = message.as_object();
    REQUIRE(fields != nullptr);
    const auto content = fields->find("content");
    REQUIRE(content != fields->end());
    REQUIRE(content->second.as_array() != nullptr);
    std::string text;
    for (const auto& block : *content->second.as_array()) {
        const auto* block_fields = block.as_object();
        REQUIRE(block_fields != nullptr);
        const auto kind = block_fields->find("type");
        REQUIRE(kind != block_fields->end());
        if (kind->second.as_string() == nullptr || *kind->second.as_string() != "text") continue;
        const auto value = block_fields->find("text");
        REQUIRE(value != block_fields->end());
        REQUIRE(value->second.as_string() != nullptr);
        text += *value->second.as_string();
    }
    return text;
}

/// Every durable `Environment` member, forwarded to the wrapped one.
class ForwardingEnvironment : public Environment {
public:
    explicit ForwardingEnvironment(std::shared_ptr<Environment> inner) : inner_(std::move(inner)) {}

    support::Expected<std::string> read(std::string_view path, std::stop_token stop) override {
        return inner_->read(path, stop);
    }
    support::Expected<std::string> read(
            std::string_view path, std::uintmax_t offset, std::size_t length, std::stop_token stop) override {
        return inner_->read(path, offset, length, stop);
    }
    support::ExpectedVoid write(std::string_view path, std::string_view content, std::stop_token stop) override {
        return inner_->write(path, content, stop);
    }
    support::ExpectedVoid append(std::string_view path, std::string_view content, std::stop_token stop) override {
        return inner_->append(path, content, stop);
    }
    support::Expected<FileInfo> stat(std::string_view path, std::stop_token stop) override {
        return inner_->stat(path, stop);
    }
    support::Expected<std::unique_ptr<DirectoryReader>> open_directory(
            std::string_view path, std::stop_token stop) override {
        return inner_->open_directory(path, stop);
    }
    support::Expected<std::vector<FileInfo>> list(std::string_view path, std::stop_token stop) override {
        return inner_->list(path, stop);
    }
    support::Expected<std::string> create_temp_dir(std::string_view prefix, std::stop_token stop) override {
        return inner_->create_temp_dir(prefix, stop);
    }
    support::Expected<ShellResult> exec(const ShellCommand& command,
            std::chrono::milliseconds timeout,
            OutputSink output,
            std::stop_token stop,
            ShellOutputWindow window) override {
        return inner_->exec(command, timeout, std::move(output), stop, window);
    }
    support::Expected<std::unique_ptr<FileWatch>> watch(
            std::string_view path, WatchChangeSink on_change, WatchOptions options) override {
        return inner_->watch(path, std::move(on_change), std::move(options));
    }

protected:
    [[nodiscard]] Environment& inner() { return *inner_; }

private:
    std::shared_ptr<Environment> inner_;
};

/// Blocks the first read and counts overlapping reads, so a mutation that is not
/// serialized shows up as two reads in flight and a follower that never observes
/// the leader's write.
class GatedReadEnvironment final : public ForwardingEnvironment {
public:
    explicit GatedReadEnvironment(std::shared_ptr<Environment> inner) : ForwardingEnvironment(std::move(inner)) {}

    support::Expected<std::string> read(std::string_view path, std::stop_token stop) override {
        const auto active = active_.fetch_add(1) + 1;
        auto peak = peak_.load();
        while (active > peak && !peak_.compare_exchange_weak(peak, active)) {
        }
        if (first_read_.exchange(false)) {
            entered_.release();
            released_.acquire();
        }
        auto result = inner().read(path, stop);
        active_.fetch_sub(1);
        return result;
    }

    void wait_until_gated_read() { entered_.acquire(); }
    void release_gated_read() { released_.release(); }
    [[nodiscard]] int peak_reads() const { return peak_.load(); }

private:
    std::atomic_int active_{0};
    std::atomic_int peak_{0};
    std::atomic_bool first_read_{true};
    std::binary_semaphore entered_{0};
    std::binary_semaphore released_{0};
};

/// The `edit` registration of the public coding-tools extension.
ToolRegistration& edit_registration(Extension& extension) {
    const auto found = std::ranges::find_if(
            extension.tools, [](const ToolRegistration& tool) { return tool.declaration.name == "edit"; });
    REQUIRE(found != extension.tools.end());
    return *found;
}

support::Expected<ai::ToolResultMessage> call_edit(ToolRegistration& tool, const chord::Json& arguments) {
    return tool.execute_durable(arguments.to_support(), ToolExecutionContext{});
}

} // namespace

TEST_CASE(
        "Durable edit tool reproduces the frozen BOM CRLF diff and rejection results", "[durable][tools][issue1159]") {
    const std::vector<std::string_view> scenarios{
            "a BOM and CRLF file edits when the old text is LF and leaves the normalized content",
            "a single replacement returns the frozen diff patch and first changed line",
            "a replacement on a two-digit line keeps the frozen diff gutter and context window",
            "two concurrent edits of one file both land without a lost update",
            "an old text that appears twice fails with the frozen uniqueness message",
            "an old text that is absent from a multi-edit call fails with the frozen indexed message",
            "an edit that reproduces the original text fails with the frozen no-change message",
            "an empty old text fails with the frozen empty-target message",
            "two edits that target overlapping text fail with the frozen overlap message",
            "an old text that appears twice in a multi-edit call fails with the frozen indexed uniqueness message",
    };

    for (const auto scenario : scenarios) {
        DYNAMIC_SECTION(scenario) {
            const auto frozen = FrozenScenario::load(kFrozenArtifact, scenario);
            REQUIRE(frozen);
            const auto* inputs = frozen->inputs().get_if<support::JsonValue::object_t>();
            REQUIRE(inputs != nullptr);
            const auto seed = inputs->find("seed");
            const auto calls = inputs->find("calls");
            REQUIRE(seed != inputs->end());
            REQUIRE(calls != inputs->end());
            REQUIRE(seed->second.get_if<support::JsonValue::object_t>() != nullptr);
            REQUIRE(calls->second.get_if<support::JsonValue::array_t>() != nullptr);

            tests::TempWorkspace workspace;
            for (const auto& [name, content] : seed->second.get_object()) {
                REQUIRE(content.get_if<std::string>() != nullptr);
                workspace.write(name, content.get_string());
            }

            // The scripted round is built once, outside the stream callbacks: a
            // streamed producer is noexcept, so no assertion may run inside it.
            auto tool_call_message = ai::assistant_text_message("edited");
            tool_call_message.content.clear();
            for (const auto& call : calls->second.get_array()) {
                const auto* fields = call.get_if<support::JsonValue::object_t>();
                REQUIRE(fields != nullptr);
                const auto id = fields->find("id");
                const auto arguments = fields->find("args");
                REQUIRE(id != fields->end());
                REQUIRE(arguments != fields->end());
                REQUIRE(id->second.get_if<std::string>() != nullptr);
                tool_call_message.content.push_back(ai::ToolCallContent{
                        .id = id->second.get_string(), .name = "edit", .arguments = arguments->second});
            }
            tool_call_message.stop_reason = ai::AssistantStopReason::ToolUse;

            Registry registry;
            REQUIRE(registry.install(coding_tools(local_environment(workspace.path().string()))).has_value());
            int requests = 0;
            HarnessOptions options{.registry = &registry};
            // The stream factory is stored by the Harness and runs while this
            // test frame lives, which is the frame `requests` belongs to.
            options.stream = [&requests, tool_call_message = std::move(tool_call_message)](
                                     ai::Model, ai::AiContext, ai::SimpleStreamOptions) {
                const auto turn = requests++;
                return ai::ModelStream{
                        ai::ModelStreamProducer{[turn, tool_call_message](ai::AssistantEventSink sink,
                                                        ai::ModelStreamCompletion completion) mutable noexcept {
                            auto message =
                                    turn == 0 ? std::move(tool_call_message) : ai::assistant_text_message("edited");
                            auto delivered =
                                    sink(ai::AssistantDoneEvent{.reason = message.stop_reason, .message = message});
                            if (!delivered)
                                completion(std::unexpected(delivered.error()));
                            else
                                completion(std::move(message));
                        }}};
            };

            MemoryStorage storage;
            auto opened = consume(Harness::open(storage, std::move(options)));
            REQUIRE(opened.has_value());
            auto harness = std::move(opened.value());
            auto root = consume(harness->root());
            REQUIRE(root.has_value());
            auto submission = consume(harness->submit(root.value(), text_input("edit")));
            REQUIRE(submission.has_value());
            auto settled = consume(submission.value().wait());
            REQUIRE(settled.has_value());
            CHECK(settled.value().status == "done");

            const auto* expected = frozen->array_member("results");
            REQUIRE(expected != nullptr);
            auto entries = storage.scan_entries(root.value(), 100);
            REQUIRE(entries);
            std::ranges::reverse(*entries);
            std::vector<const chord::Json*> observed;
            for (const auto& entry : *entries)
                if (entry.kind == "pi.tool-result") observed.push_back(&entry_model(entry));
            REQUIRE(observed.size() == expected->size());
            for (std::size_t index = 0; index < expected->size(); ++index) {
                const auto* want = expected->at(index).get_if<support::JsonValue::object_t>();
                REQUIRE(want != nullptr);
                const auto& message = *observed[index];
                const auto* fields = message.as_object();
                REQUIRE(fields != nullptr);
                const auto is_error = fields->find("isError");
                REQUIRE(is_error != fields->end());
                REQUIRE(is_error->second.as_boolean() != nullptr);
                CHECK(*is_error->second.as_boolean() == want->at("isError").get_boolean());
                CHECK(result_text(message) == want->at("text").get_string());
                // `details` is the frozen diff payload, and absent on a rejection.
                const auto details = fields->find("details");
                const auto& want_details = want->at("details");
                if (want_details.get_if<support::JsonValue::object_t>() == nullptr) {
                    CHECK(details == fields->end());
                    continue;
                }
                REQUIRE(details != fields->end());
                const auto* detail_fields = details->second.as_object();
                REQUIRE(detail_fields != nullptr);
                for (const auto& [key, value] : want_details.get_object()) {
                    const auto found = detail_fields->find(key);
                    REQUIRE(found != detail_fields->end());
                    if (value.get_if<std::string>() != nullptr) {
                        REQUIRE(found->second.as_string() != nullptr);
                        CHECK(*found->second.as_string() == value.get_string());
                        continue;
                    }
                    REQUIRE(found->second.as_number() != nullptr);
                    CHECK(*found->second.as_number() == value.get_number());
                }
            }

            const auto* files = frozen->object_member("files");
            REQUIRE(files != nullptr);
            for (const auto& [name, view] : *files) {
                const auto* fields = view.get_if<support::JsonValue::object_t>();
                REQUIRE(fields != nullptr);
                const auto actual = workspace.read(name);
                CHECK(actual == fields->at("text").get_string());
                CHECK(static_cast<double>(actual.size()) == fields->at("bytes").get_number());
            }
        }
    }
}

TEST_CASE("Durable edit and write calls of one file apply one after another", "[durable][tools][issue1159]") {
    tests::TempWorkspace workspace;
    workspace.write("queued.txt", "alpha\n");
    // The follower names the same file with a different spelling. The slot key is
    // the environment's canonical path, so a second spelling must not buy a
    // second, concurrent slot. A symbolic-link spelling is not reachable here:
    // the durable environment refuses to write through one (the #1160 boundary).

    auto gated = std::make_shared<GatedReadEnvironment>(local_environment(workspace.path().string()));
    auto extension = coding_tools(gated);
    auto& edit = edit_registration(extension);

    const chord::Json first_call = chord::Json::Object{{"path", "queued.txt"},
            {"edits", chord::Json::Array{chord::Json::Object{{"oldText", "alpha"}, {"newText", "beta"}}}}};
    const chord::Json second_call = chord::Json::Object{{"path", "./queued.txt"},
            {"edits", chord::Json::Array{chord::Json::Object{{"oldText", "beta"}, {"newText", "gamma"}}}}};

    std::optional<support::Expected<ai::ToolResultMessage>> first;
    std::optional<support::Expected<ai::ToolResultMessage>> second;
    std::jthread leading([&] { first = call_edit(edit, first_call); });
    gated->wait_until_gated_read();
    std::jthread following([&] { second = call_edit(edit, second_call); });
    // The follower waits for the slot, so it is still before its own read.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    gated->release_gated_read();
    leading.join();
    following.join();

    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    // Both calls landed, in the order the queue admitted them: the second call
    // replaced text the first call wrote, which one stale read cannot see.
    REQUIRE(first->has_value());
    REQUIRE(second->has_value());
    CHECK_FALSE(first->value().is_error);
    CHECK_FALSE(second->value().is_error);
    CHECK(ai::text_from_content(first->value().content) == "Successfully replaced 1 block(s) in queued.txt.");
    CHECK(ai::text_from_content(second->value().content) == "Successfully replaced 1 block(s) in ./queued.txt.");
    CHECK(workspace.read("queued.txt") == "gamma\n");
    // One read at a time: the follower never read the original text.
    CHECK(gated->peak_reads() == 1);
    REQUIRE(second->value().details.has_value());
    const auto* details = second->value().details->get_if<support::JsonValue::object_t>();
    REQUIRE(details != nullptr);
    CHECK(details->at("diff").get_string() == "-1 beta\n+1 gamma");
}

TEST_CASE("Durable edit rejects text that appears twice or is absent", "[durable][tools][issue1159]") {
    tests::TempWorkspace workspace;
    workspace.write("twice.txt", "same\nsame\n");
    workspace.write("absent.txt", "alpha\n");
    auto extension = coding_tools(local_environment(workspace.path().string()));
    auto& edit = edit_registration(extension);

    SECTION("the duplicate text fails with the frozen uniqueness message") {
        const chord::Json call = chord::Json::Object{{"path", "twice.txt"},
                {"edits", chord::Json::Array{chord::Json::Object{{"oldText", "same\n"}, {"newText", "once\n"}}}}};
        const auto result = call_edit(edit, call);
        REQUIRE(result.has_value());
        CHECK(result->is_error);
        CHECK(ai::text_from_content(result->content) ==
                "<harness>\n[error] Found 2 occurrences of the text in twice.txt. The text must be unique. Please "
                "provide more context to make it unique.\n</harness>");
        CHECK_FALSE(result->details.has_value());
        CHECK(workspace.read("twice.txt") == "same\nsame\n");
    }

    SECTION("the absent text fails with the frozen single-edit message") {
        const chord::Json call = chord::Json::Object{{"path", "absent.txt"},
                {"edits", chord::Json::Array{chord::Json::Object{{"oldText", "delta"}, {"newText", "epsilon"}}}}};
        const auto result = call_edit(edit, call);
        REQUIRE(result.has_value());
        CHECK(result->is_error);
        CHECK(ai::text_from_content(result->content) ==
                "<harness>\n[error] Could not find the exact text in absent.txt. The old text must match exactly "
                "including all whitespace and newlines.\n</harness>");
        CHECK(workspace.read("absent.txt") == "alpha\n");
    }
}

} // namespace cch::tests::durable
