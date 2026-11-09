#include <cch/durable/Environment.hpp>

#include <cch/ai/Message.hpp>
#include <cch/support/JsonValue.hpp>

#include "durable/FrozenExpectation.hpp"
#include "support/PeakMemory.hpp"

#include "support/TempWorkspace.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using cch::support::JsonValue;

namespace {

namespace fs = std::filesystem;

using cch::tests::read_peak_memory;

// Supplies one valid line, then reports a read error on the next extraction.
class FailingStatusBuffer final : public std::stringbuf {
public:
    FailingStatusBuffer() : std::stringbuf("VmHWM: 12 kB\n") {}

    void attach(std::istream& stream) { stream_ = &stream; }

protected:
    int_type underflow() override {
        stream_->setstate(std::ios::badbit);
        return traits_type::eof();
    }

private:
    std::istream* stream_ = nullptr; // The stream outlives all reads from this buffer.
};

/// Renders the frozen diagnostic block the stored message ends with (frozen
/// `appendToolResult` -> `renderDiagnostics`).
std::string rendered_diagnostics(const JsonValue& diagnostics) {
    std::string rendered = "<harness>\n";
    const auto& items = diagnostics.get_array();
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i > 0) rendered.push_back('\n');
        const auto* item = items[i].get_if<JsonValue::object_t>();
        const auto* severity = item->at("severity").get_if<std::string>();
        const auto* message = item->at("message").get_if<std::string>();
        rendered += "[" + *severity + "] " + *message;
    }
    rendered += "\n</harness>";
    return rendered;
}

/// Deep equality over the artifact's own JSON values.
bool json_equal(const JsonValue& left, const JsonValue& right) {
    if (left.holds<double>() && right.holds<double>()) return left.get_number() == right.get_number();
    if (left.holds<std::string>() && right.holds<std::string>()) return left.get_string() == right.get_string();
    if (left.holds<bool>() && right.holds<bool>()) return left.get_boolean() == right.get_boolean();
    const auto* left_object = left.get_if<JsonValue::object_t>();
    const auto* right_object = right.get_if<JsonValue::object_t>();
    if (left_object != nullptr || right_object != nullptr) {
        if (left_object == nullptr || right_object == nullptr || left_object->size() != right_object->size())
            return false;
        for (const auto& [key, value] : *left_object) {
            const auto found = right_object->find(key);
            if (found == right_object->end() || !json_equal(value, found->second)) return false;
        }
        return true;
    }
    const auto* left_array = left.get_if<JsonValue::array_t>();
    const auto* right_array = right.get_if<JsonValue::array_t>();
    if (left_array != nullptr || right_array != nullptr) {
        if (left_array == nullptr || right_array == nullptr || left_array->size() != right_array->size()) return false;
        for (std::size_t i = 0; i < left_array->size(); ++i)
            if (!json_equal(left_array->at(i), right_array->at(i))) return false;
        return true;
    }
    return left.holds<nullptr_t>() == right.holds<nullptr_t>();
}

/// Runs the durable read tool exactly as the registry exposes it.
cch::support::Expected<cch::ai::ToolResultMessage> run_read(
        const std::shared_ptr<cch::durable::Environment>& env, const JsonValue& args) {
    auto extension = cch::durable::coding_tools(env);
    for (auto& tool : extension.tools) {
        if (tool.declaration.name != "read") continue;
        REQUIRE(tool.execute_durable);
        return tool.execute_durable(args, cch::durable::ToolExecutionContext{});
    }
    FAIL("read tool is not registered");
    return std::unexpected(cch::support::make_error(cch::support::ErrorCode::Tool, "missing"));
}

} // namespace

TEST_CASE("Durable read tool matches the frozen one-based line read contract", "[durable][environment][issue1197]") {
    const auto artifact = cch::tests::durable::frozen_artifact("fixtures/pi-v1.3.0/audit-1197/read-tool.json");
    const auto* scenarios = artifact.at("scenarios").get_if<JsonValue::array_t>();
    REQUIRE(scenarios != nullptr);

    for (const auto& scenario : *scenarios) {
        const auto& inputs = scenario.at("inputs");
        const auto& expected = scenario.at("expected");
        const auto* args = inputs.at("args").get_if<JsonValue::object_t>();
        REQUIRE(args != nullptr);
        const auto* path_value = args->at("path").get_if<std::string>();
        REQUIRE(path_value != nullptr);
        cch::tests::TempWorkspace workspace;
        if (inputs.get_object().find("files") != inputs.get_object().end()) {
            const auto* files = inputs.at("files").get_if<JsonValue::object_t>();
            REQUIRE(files != nullptr);
            for (const auto& [relative, content] : *files) {
                if (relative.ends_with('/')) {
                    fs::create_directories(workspace.path() / relative);
                    continue;
                }
                fs::create_directories((workspace.path() / relative).parent_path());
                const auto* text = content.get_if<std::string>();
                std::ofstream output(workspace.path() / relative, std::ios::binary | std::ios::trunc);
                output << (text == nullptr ? std::string{} : *text);
            }
        } else {
            // Single-file scenarios carry their exact content in the artifact.
            const auto& relative = inputs.at("file").get_string();
            const auto& content = inputs.at("content").get_string();
            fs::create_directories((workspace.path() / relative).parent_path());
            std::ofstream output(workspace.path() / relative, std::ios::binary | std::ios::trunc);
            output << content;
        }
        auto env = cch::durable::local_environment(workspace.path().string());
        auto result = run_read(env, inputs.at("args"));
        INFO(std::format("scenario: {}", scenario.at("name").get_string()));

        const auto* error = expected.get_object().find("error") == expected.get_object().end()
                                    ? nullptr
                                    : expected.at("error").get_if<std::string>();
        if (error != nullptr && error->starts_with("Offset")) {
            REQUIRE(result.has_value());
            CHECK(result->is_error);
            REQUIRE(result->content.size() == 1);
            const auto* text = std::get_if<cch::ai::TextContent>(&result->content.front());
            REQUIRE(text != nullptr);
            CHECK(text->text == std::format("<harness>\n[error] {}\n</harness>", *error));
            CHECK_FALSE(result->details.has_value());
            continue;
        }
        REQUIRE(result.has_value());
        CHECK_FALSE(result->is_error);

        // The stored content is the frozen output text followed by the
        // rendered diagnostics block (frozen `appendToolResult`).
        std::vector<std::string> expected_content;
        const auto* expected_text = expected.at("text").get_if<std::string>();
        if (expected_text != nullptr && !expected_text->empty()) expected_content.push_back(*expected_text);
        const auto* diagnostics = expected.at("diagnostics").get_if<JsonValue::array_t>();
        REQUIRE(diagnostics != nullptr);
        if (!diagnostics->empty()) expected_content.push_back(rendered_diagnostics(expected.at("diagnostics")));

        std::vector<std::string> actual_content;
        for (const auto& block : result->content) {
            const auto* text = std::get_if<cch::ai::TextContent>(&block);
            REQUIRE(text != nullptr);
            actual_content.push_back(text->text);
        }
        CHECK(actual_content == expected_content);

        const bool has_details = expected.get_object().find("details") != expected.get_object().end();
        CHECK(result->details.has_value() == has_details);
        if (has_details) {
            REQUIRE(result->details.has_value());
            CHECK(json_equal(*result->details, expected.at("details")));
        }
    }
}

TEST_CASE("Durable read tool formats oversized line sizes like frozen pi", "[durable][environment][issue1232]") {
    cch::tests::TempWorkspace workspace;
    auto env = cch::durable::local_environment(workspace.path().string());
    for (const auto& [size, expected] : std::vector<std::pair<std::size_t, std::string_view>>{{51455, "50.2KB"},
                 {51456, "50.3KB"},
                 {51457, "50.3KB"},
                 {1048575, "1024.0KB"},
                 {1048576, "1.0MB"},
                 {1310719, "1.2MB"},
                 {1310720, "1.3MB"},
                 {1310721, "1.3MB"}}) {
        INFO("line size: " << size);
        {
            std::ofstream output(workspace.path() / "line.txt", std::ios::binary | std::ios::trunc);
            output << std::string(size, 'a');
            REQUIRE(output.good());
        }
        auto result = run_read(env, JsonValue(JsonValue::object_t{{"path", JsonValue("line.txt")}}));
        REQUIRE(result.has_value());
        CHECK(result->content.size() == 2);
        REQUIRE_FALSE(result->content.empty());
        const auto* diagnostic = std::get_if<cch::ai::TextContent>(&result->content.back());
        REQUIRE(diagnostic != nullptr);
        CHECK(diagnostic->text.contains(std::format("Line 1 is {}, exceeds the 50.0KB limit", expected)));
    }
}

TEST_CASE("Peak memory reading rejects a missing status field", "[durable][environment][issue1232]") {
    std::istringstream status("Name:\tpike\n");
    CHECK_FALSE(read_peak_memory(status).has_value());
}

TEST_CASE("Peak memory reading validates status values and read failures", "[durable][environment][issue1232]") {
    std::istringstream valid("Name:\tpike\nVmHWM:\t 1234 kB\nVmRSS:\t100 kB\n");
    CHECK(read_peak_memory(valid) == 1263616ull);

    for (const std::string_view input : {"VmHWM: 0 kB\n",
                 "VmHWM: abc kB\n",
                 "VmHWM: 12x kB\n",
                 "VmHWM: 12 MB\n",
                 "VmHWM: 12kB\n",
                 "VmHWM: 18446744073709551616 kB\n",
                 "VmHWM: 18014398509481984 kB\n",
                 "VmHWM: 12 kB extra\n",
                 "VmHWM: 12 kB\nVmHWM: 13 kB\n"}) {
        INFO(input);
        std::istringstream status{std::string(input)};
        CHECK_FALSE(read_peak_memory(status).has_value());
    }
    std::istringstream unreadable("VmHWM: 12 kB\n");
    unreadable.setstate(std::ios::badbit);
    CHECK_FALSE(read_peak_memory(unreadable).has_value());

    FailingStatusBuffer buffer;
    std::istream failed(&buffer);
    buffer.attach(failed);
    CHECK_FALSE(read_peak_memory(failed).has_value());
    CHECK(failed.bad());
}

TEST_CASE("Durable read tool never loads an unbounded file into memory", "[durable][environment][issue1197]") {
    cch::tests::TempWorkspace workspace;
    auto env = cch::durable::local_environment(workspace.path().string());
    // A 256 MiB sparse file: a whole-file read raises the high-water mark by
    // the file size, while the bounded line scan touches only its chunks.
    const std::string sparse = workspace.path() / "sparse.txt";
    {
        std::ofstream output(sparse, std::ios::binary | std::ios::trunc);
        output.seekp(256 * 1024 * 1024 - 1);
        output.put('\n');
    }
    std::ifstream before_status("/proc/self/status");
    const auto before = read_peak_memory(before_status);
    REQUIRE(before.has_value());
    auto result = run_read(env, JsonValue(JsonValue::object_t{{"path", JsonValue(std::string{"sparse.txt"})}}));
    std::ifstream after_status("/proc/self/status");
    const auto after = read_peak_memory(after_status);
    REQUIRE(after.has_value());
    REQUIRE(result.has_value());
    CHECK_FALSE(result->is_error);
    // The scan is streaming; the sparse file's pages are never touched, so the
    // high-water mark rises by far less than the file's 256 MiB.
    CHECK(*after >= *before);
    CHECK((*after >= *before && *after - *before < 64 * 1024 * 1024));
}

TEST_CASE("Durable read tool reports an offset past the last line as an error", "[durable][environment][issue1197]") {
    cch::tests::TempWorkspace workspace;
    auto env = cch::durable::local_environment(workspace.path().string());
    const auto args =
            JsonValue(JsonValue::object_t{{"path", JsonValue(std::string{"three.txt"})}, {"offset", JsonValue(4.0)}});
    // A file with no trailing newline: three lines, so offset 4 is past the end.
    {
        std::ofstream output(workspace.path() / "three.txt", std::ios::binary | std::ios::trunc);
        output << "a\nb\nc";
    }
    auto result = run_read(env, args);
    REQUIRE(result.has_value());
    CHECK(result->is_error);
    REQUIRE(result->content.size() == 1);
    const auto* text = std::get_if<cch::ai::TextContent>(&result->content.front());
    REQUIRE(text != nullptr);
    CHECK(text->text == "<harness>\n[error] Offset 4 is beyond end of file (3 lines total)\n</harness>");
}
