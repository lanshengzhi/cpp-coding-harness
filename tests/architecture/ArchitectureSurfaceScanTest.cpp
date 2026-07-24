#include "../../third_party/catch2/catch_test_macros.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifndef CCH_SOURCE_DIR
#define CCH_SOURCE_DIR "."
#endif

namespace {

std::string read_text(const std::filesystem::path& path) {
    std::ifstream in(path);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

std::size_t count_occurrences(const std::string& text, const std::string& needle) {
    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = text.find(needle, pos)) != std::string::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

std::vector<std::filesystem::path> files_under(std::initializer_list<std::string> roots) {
    std::vector<std::filesystem::path> files;
    const auto source_root = std::filesystem::path(CCH_SOURCE_DIR);
    for (const auto& root_name : roots) {
        const auto root = source_root / root_name;
        if (!std::filesystem::exists(root)) {
            continue;
        }
        for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
            if (entry.is_regular_file()) {
                files.push_back(entry.path());
            }
        }
    }
    return files;
}

std::vector<std::filesystem::path> public_headers() {
    std::vector<std::filesystem::path> headers;
    for (const auto& path : files_under({"include/cch"})) {
        if (path.extension() == ".hpp") {
            headers.push_back(path);
        }
    }
    return headers;
}

} // namespace

TEST_CASE("library publishes include as contract surface and keeps src private", "[architecture][u1]") {
    const auto cmake = read_text(std::filesystem::path(CCH_SOURCE_DIR) / "CMakeLists.txt");

    CHECK(cmake.find("${CMAKE_CURRENT_SOURCE_DIR}/include") != std::string::npos);
    CHECK(cmake.find("PRIVATE\n        ${CMAKE_CURRENT_SOURCE_DIR}/src") != std::string::npos);
    CHECK(cmake.find("PUBLIC\n        ${CMAKE_CURRENT_SOURCE_DIR}/include\n        ${CMAKE_CURRENT_SOURCE_DIR}/src") == std::string::npos);
}

TEST_CASE("public headers do not include private src paths", "[architecture][u1]") {
    const auto headers = public_headers();
    REQUIRE_FALSE(headers.empty());

    for (const auto& header : headers) {
        const auto text = read_text(header);
        CHECK(text.find("#include \"../../src") == std::string::npos);
        CHECK(text.find("#include <src/") == std::string::npos);
    }
}

TEST_CASE("core public contracts do not expose Glaze generic machinery", "[architecture][u7]") {
    const auto source_root = std::filesystem::path(CCH_SOURCE_DIR);

    const auto error_header = read_text(source_root / "include" / "cch" / "util" / "Error.hpp");
    CHECK(error_header.find("glaze/glaze.hpp") == std::string::npos);
    CHECK(error_header.find("glz::") == std::string::npos);
    CHECK(error_header.find("read_json") == std::string::npos);
    CHECK(error_header.find("write_json") == std::string::npos);

    const auto domain_headers = {
        source_root / "include" / "cch" / "ai" / "Content.hpp",
        source_root / "include" / "cch" / "ai" / "Message.hpp",
        source_root / "include" / "cch" / "agent" / "AgentTool.hpp",
    };
    for (const auto& header : domain_headers) {
        const auto text = read_text(header);
        CHECK(text.find("glaze/glaze.hpp") == std::string::npos);
        CHECK(text.find("glz::generic") == std::string::npos);
        CHECK(text.find("JsonValue") != std::string::npos);
    }
}

TEST_CASE("tool argument contracts stay passive and dependency-free", "[architecture][ai][issue24]") {
    const auto source_root = std::filesystem::path(CCH_SOURCE_DIR);
    const auto tool_header = read_text(source_root / "include" / "cch" / "ai" / "Tool.hpp");
    const auto tool_factories = read_text(source_root / "src" / "tools" / "AsyncToolFactories.cpp");
    const auto removed_recursive_type = std::string{"Json"} + "Schema";

    CHECK(tool_header.find("util::JsonValue parameters") != std::string::npos);
    CHECK(tool_header.find(removed_recursive_type) == std::string::npos);
    CHECK(tool_header.find("glaze/glaze.hpp") == std::string::npos);
    CHECK(tool_header.find("glz::") == std::string::npos);
    CHECK(tool_factories.find("ai/glaze") == std::string::npos);
}

TEST_CASE("tool scheduling vocabulary stays in the agent package", "[architecture][agent]") {
    const auto source_root = std::filesystem::path(CCH_SOURCE_DIR);
    const auto ai_tool = read_text(source_root / "include" / "cch" / "ai" / "Tool.hpp");
    const auto agent_tool = read_text(source_root / "include" / "cch" / "agent" / "AgentTool.hpp");
    const auto agent_context = read_text(source_root / "include" / "cch" / "agent" / "AgentContext.hpp");

    CHECK(ai_tool.find("ToolConcurrency") == std::string::npos);
    CHECK(ai_tool.find("ToolExecutionPolicy") == std::string::npos);
    CHECK(ai_tool.find("ToolExecutionMode") == std::string::npos);
    CHECK(agent_tool.find("ToolConcurrency") != std::string::npos);
    CHECK(agent_context.find("ToolExecutionPolicy") != std::string::npos);
    CHECK(agent_context.find("BoundedParallelToolExecution") != std::string::npos);
}

TEST_CASE("stateful Agent stays independent of coding-agent product concerns", "[architecture][agent][issue35]") {
    const auto source_root = std::filesystem::path(CCH_SOURCE_DIR);
    const auto agent_header = read_text(source_root / "include" / "cch" / "agent" / "Agent.hpp");
    const auto agent_source = read_text(source_root / "src" / "agent" / "Agent.cpp");
    const auto combined = agent_header + agent_source;

    CHECK(combined.find("cch/coding_agent") == std::string::npos);
    CHECK(combined.find("cch/harness") == std::string::npos);
    CHECK(combined.find("AgentSession") == std::string::npos);
    CHECK(combined.find("SessionStore") == std::string::npos);
    CHECK(combined.find("ProjectResource") == std::string::npos);
    CHECK(combined.find("CliConfig") == std::string::npos);
}

TEST_CASE(
    "the coroutine loop is private behind the stateful Agent",
    "[architecture][agent][session][issue37]") {
    const auto source_root = std::filesystem::path(CCH_SOURCE_DIR);
    const auto former_loop_header = std::string{"Agent"} + "Loop.hpp";
    const auto former_loop_type = std::string{"AsyncAgent"} + "Loop";
    const auto former_run_result = std::string{"AsyncAgentRun"} + "Result";

    CHECK_FALSE(std::filesystem::exists(
        source_root / "include" / "cch" / "agent" / former_loop_header));
    CHECK(std::filesystem::exists(
        source_root / "src" / "agent" / former_loop_header));

    for (const auto& header : public_headers()) {
        const auto text = read_text(header);
        CHECK(text.find(former_loop_type) == std::string::npos);
        CHECK(text.find(former_run_result) == std::string::npos);
    }

    const auto private_agent_dir = source_root / "src" / "agent";
    const auto agent_source_path = private_agent_dir / "Agent.cpp";
    const auto loop_source_path = private_agent_dir / "AgentLoop.cpp";
    const auto loop_header_path = private_agent_dir / former_loop_header;
    const auto agent_source = read_text(agent_source_path);
    CHECK(agent_source.find("#include \"AgentLoop.hpp\"") != std::string::npos);
    CHECK(agent_source.find("impl->loop.continue_with(") != std::string::npos);

    for (const auto& file : files_under({"src"})) {
        const auto is_loop_implementation =
            file == agent_source_path ||
            file == loop_source_path ||
            file == loop_header_path;
        if (is_loop_implementation) {
            continue;
        }
        const auto text = read_text(file);
        CHECK(text.find(former_loop_header) == std::string::npos);
        CHECK(text.find(former_loop_type) == std::string::npos);
        CHECK(text.find(former_run_result) == std::string::npos);
    }
}

TEST_CASE(
    "AgentSessionRuntime composes the stateful Agent without a second loop or live history",
    "[architecture][agent][session][issue36][issue37]") {
    const auto source_root = std::filesystem::path(CCH_SOURCE_DIR);
    const auto runtime_header = read_text(
        source_root / "src" / "coding_agent" / "runtime" / "AgentSessionRuntime.hpp");
    const auto runtime_source = read_text(
        source_root / "src" / "coding_agent" / "runtime" / "AgentSessionRuntime.cpp");

    const auto direct_loop_call = std::string{"continue_"} + "with(";
    const auto duplicate_registry = std::string{"subscribers"} + "_";

    CHECK(runtime_header.find("agent::Agent") != std::string::npos);
    CHECK(runtime_source.find("agent_->prompt(") != std::string::npos);
    CHECK(runtime_header.find("AsyncAgentLoop") == std::string::npos);
    CHECK(runtime_source.find(direct_loop_call) == std::string::npos);
    CHECK(runtime_header.find(duplicate_registry) == std::string::npos);
    CHECK(runtime_header.find("agent::AgentState") == std::string::npos);
    CHECK(runtime_header.find("std::vector<ai::MessageVariant>") == std::string::npos);
    // OpenSession history is resume input only and is moved exactly once into
    // AgentInitialState; prompt execution never advances or copies it back.
    CHECK(count_occurrences(runtime_source, "session_.history") == 1);
}

TEST_CASE("provider DTOs stay out of the public contract surface", "[architecture][u4]") {
    const auto source_root = std::filesystem::path(CCH_SOURCE_DIR);
    CHECK_FALSE(std::filesystem::exists(source_root / "include" / "cch" / "ai" / "glaze" / "ProviderDtos.hpp"));
    CHECK_FALSE(std::filesystem::exists(source_root / "include" / "cch" / "util" / "Json.hpp"));

    const auto glaze_dir = source_root / "include" / "cch" / "ai" / "glaze";
    if (std::filesystem::exists(glaze_dir)) {
        for (const auto& entry : std::filesystem::directory_iterator(glaze_dir)) {
            CHECK(entry.path().extension() != ".hpp");
        }
    }

    const auto openai_header = read_text(source_root / "include" / "cch" / "ai" / "providers" / "OpenAIChatClient.hpp");
    CHECK(openai_header.find("BoostBeastStreamTransport.hpp") == std::string::npos);
    CHECK(openai_header.find("StreamTransport.hpp") != std::string::npos);

    const auto providers_dir = source_root / "include" / "cch" / "ai" / "providers";
    const std::vector<std::string> allowed_provider_headers = {
        "StreamTransport.hpp",
        "OpenAIChatClient.hpp",
        "OpenAICompletionsCompat.hpp",
    };
    for (const auto& entry : std::filesystem::directory_iterator(providers_dir)) {
        if (entry.path().extension() != ".hpp") {
            continue;
        }
        const auto filename = entry.path().filename().string();
        CHECK(std::find(allowed_provider_headers.begin(), allowed_provider_headers.end(), filename) != allowed_provider_headers.end());
    }
}

TEST_CASE("AI message surface does not accept new runtime-only message variants", "[architecture][ai]") {
    const auto source_root = std::filesystem::path(CCH_SOURCE_DIR);
    const auto message_header = read_text(source_root / "include" / "cch" / "ai" / "Message.hpp");

    const auto section_start = message_header.find("// ── pi extended runtime message types ──");
    const auto variant_start = message_header.find("using MessageVariant", section_start);
    REQUIRE(section_start != std::string::npos);
    REQUIRE(variant_start != std::string::npos);

    const auto legacy_runtime_section = message_header.substr(section_start, variant_start - section_start);
    CHECK(count_occurrences(legacy_runtime_section, "struct ") == 4);
    CHECK(legacy_runtime_section.find("BashExecutionMessage") != std::string::npos);
    CHECK(legacy_runtime_section.find("CustomMessage") != std::string::npos);
    CHECK(legacy_runtime_section.find("BranchSummaryMessage") != std::string::npos);
    CHECK(legacy_runtime_section.find("CompactionSummaryMessage") != std::string::npos);
}

TEST_CASE("coding_agent loaders stay out of the public contract surface", "[architecture][u4]") {
    const auto source_root = std::filesystem::path(CCH_SOURCE_DIR);
    CHECK_FALSE(std::filesystem::exists(source_root / "include" / "cch" / "coding_agent" / "SkillLoader.hpp"));
    CHECK_FALSE(std::filesystem::exists(source_root / "include" / "cch" / "coding_agent" / "PromptTemplateLoader.hpp"));
    CHECK_FALSE(std::filesystem::exists(source_root / "include" / "cch" / "coding_agent" / "SkillFormatting.hpp"));
}

TEST_CASE("concrete prompt processors stay out of the public contract surface", "[architecture][prompt]") {
    const auto source_root = std::filesystem::path(CCH_SOURCE_DIR);
    const auto public_prompt_dir = source_root / "include" / "cch" / "coding_agent";

    CHECK_FALSE(std::filesystem::exists(public_prompt_dir / "PromptProcessing.hpp"));
    CHECK_FALSE(std::filesystem::exists(public_prompt_dir / "PromptProcessingPipeline.hpp"));
    CHECK_FALSE(std::filesystem::exists(public_prompt_dir / "PromptProcessor.hpp"));
    CHECK_FALSE(std::filesystem::exists(public_prompt_dir / "PromptTemplateExpander.hpp"));
    CHECK_FALSE(std::filesystem::exists(public_prompt_dir / "SkillExpander.hpp"));
}

TEST_CASE("RuntimeServices remains internal to the coding_agent runtime package", "[architecture][session]") {
    const auto source_root = std::filesystem::path(CCH_SOURCE_DIR);
    const auto files = files_under({"src", "tests"});
    const auto runtime_dir = source_root / "src" / "coding_agent" / "runtime";
    REQUIRE_FALSE(files.empty());

    // Build the needle dynamically so this test file does not match itself.
    const auto header_needle = std::string{"RuntimeServices"} + ".hpp";

    for (const auto& file : files) {
        if (file.parent_path() == runtime_dir) {
            continue;
        }
        const auto text = read_text(file);
        CHECK(text.find(header_needle) == std::string::npos);
    }
}


TEST_CASE("AgentSession has one prompt completion and event subscription path", "[architecture][session][sdk]") {
    const auto source_root = std::filesystem::path(CCH_SOURCE_DIR);
    const auto sdk_header = read_text(source_root / "include" / "cch" / "coding_agent" / "Sdk.hpp");
    const auto runtime_header = read_text(
        source_root / "src" / "coding_agent" / "runtime" / "AgentSessionRuntime.hpp");
    const auto runtime_source = read_text(
        source_root / "src" / "coding_agent" / "runtime" / "AgentSessionRuntime.cpp");
    const auto rpc_source = read_text(
        source_root / "src" / "coding_agent" / "runtime" / "RpcMode.cpp");

    const auto public_result = std::string{"Prompt"} + "Result";
    const auto private_result = std::string{"Prompt"} + "RunResult";
    const auto prompt_sink_field = std::string{"event"} + "_sink";
    const auto prompt_scope_name = std::string{"per"} + "_prompt";

    CHECK(sdk_header.find(public_result) == std::string::npos);
    CHECK(runtime_header.find(private_result) == std::string::npos);
    CHECK(sdk_header.find(prompt_sink_field) == std::string::npos);
    CHECK(runtime_header.find(prompt_scope_name) == std::string::npos);
    CHECK(count_occurrences(
              sdk_header,
              "boost::asio::awaitable<util::ExpectedVoid> prompt(") == 1);
    CHECK(count_occurrences(sdk_header, "util::ExpectedVoid prompt_blocking(") == 1);
    CHECK(runtime_source.find("result = co_await agent_->prompt(") != std::string::npos);
    CHECK(count_occurrences(runtime_source, "boost::asio::co_spawn(") == 1);
    CHECK(count_occurrences(sdk_header, "AgentEventSink") == 1);
    CHECK(sdk_header.find("subscribe(") != std::string::npos);
    CHECK(count_occurrences(rpc_source, "config.session.subscribe(") == 1);
    CHECK(rpc_source.find(prompt_sink_field) == std::string::npos);
    CHECK(sdk_header.find("preflight_result") == std::string::npos);
    CHECK(rpc_source.find("AgentSessionPromptAccess::prompt_blocking") != std::string::npos);
}

TEST_CASE("removed event and command contracts stay out of session ownership", "[architecture][agent][session][sdk]") {
    const auto source_root = std::filesystem::path(CCH_SOURCE_DIR);
    const auto event_header = read_text(source_root / "include" / "cch" / "agent" / "AgentEvent.hpp");
    const auto sdk_header = read_text(source_root / "include" / "cch" / "coding_agent" / "Sdk.hpp");
    const auto runtime_header = read_text(
        source_root / "src" / "coding_agent" / "runtime" / "AgentSessionRuntime.hpp");
    const auto factory_header = read_text(
        source_root / "src" / "coding_agent" / "runtime" / "SessionFactory.hpp");
    const auto factory_source = read_text(
        source_root / "src" / "coding_agent" / "runtime" / "SessionFactory.cpp");
    const auto cli_source = read_text(
        source_root / "src" / "cli" / "InteractiveCliFrontend.cpp");

    const std::vector<std::string> removed_event_types{
        std::string{"QueuedMessage"} + "StartEvent",
        std::string{"QueuedMessage"} + "EndEvent",
        std::string{"Thinking"} + "UpdateEvent",
        std::string{"ToolCallStream"} + "StartEvent",
        std::string{"ToolCallStream"} + "UpdateEvent",
        std::string{"ToolCallStream"} + "EndEvent",
    };
    for (const auto& removed : removed_event_types) {
        CHECK(event_header.find(removed) == std::string::npos);
    }

    const auto registry_name = std::string{"Command"} + "Registry";
    CHECK(runtime_header.find(registry_name) == std::string::npos);
    CHECK(factory_header.find(registry_name) == std::string::npos);
    CHECK(factory_source.find(registry_name) == std::string::npos);
    CHECK(cli_source.find(registry_name) != std::string::npos);

    const auto sdk_command = std::string{"Sdk"} + "Command";
    const auto command_handler = std::string{"Command"} + "Handler";
    CHECK(sdk_header.find(sdk_command) == std::string::npos);
    CHECK(sdk_header.find(command_handler) == std::string::npos);
}

TEST_CASE("Runtime session persistence stays behind the narrow SessionStore capability", "[architecture][session]") {
    const auto source_root = std::filesystem::path(CCH_SOURCE_DIR);
    const auto runtime_header = read_text(
        source_root / "src" / "coding_agent" / "runtime" / "AgentSessionRuntime.hpp");
    const auto lifecycle_header = read_text(
        source_root / "src" / "coding_agent" / "runtime" / "SessionLifecycle.hpp");
    const auto lifecycle_source = read_text(
        source_root / "src" / "coding_agent" / "runtime" / "SessionLifecycle.cpp");
    const auto store_header = read_text(
        source_root / "include" / "cch" / "harness" / "session" / "SessionStore.hpp");
    const auto in_memory_store_header = read_text(
        source_root / "src" / "harness" / "session" / "InMemorySessionStore.hpp");

    const auto concrete_store = std::string{"Jsonl"} + "SessionStore";
    const auto in_memory_store = std::string{"InMemory"} + "SessionStore";
    CHECK(runtime_header.find(concrete_store) == std::string::npos);
    CHECK(runtime_header.find(in_memory_store) == std::string::npos);
    CHECK(lifecycle_header.find(concrete_store) == std::string::npos);
    CHECK(lifecycle_header.find(in_memory_store) == std::string::npos);
    CHECK(lifecycle_header.find("SessionStore") != std::string::npos);
    CHECK(lifecycle_source.find(concrete_store) != std::string::npos);
    CHECK(lifecycle_source.find(in_memory_store) != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(
        source_root / "include" / "cch" / "harness" / "session" /
        "InMemorySessionStore.hpp"));
    CHECK(in_memory_store_header.find("std::vector") == std::string::npos);
    CHECK(in_memory_store_header.find("history") == std::string::npos);

    CHECK(store_header.find("append(const ai::MessageVariant&") != std::string::npos);
    CHECK(store_header.find("optional<std::filesystem::path> path() const") != std::string::npos);
    CHECK(store_header.find("SessionMetadata") == std::string::npos);
    CHECK(store_header.find("SessionTree") == std::string::npos);
    CHECK(store_header.find("SessionJournal") == std::string::npos);
    CHECK(store_header.find("open_as_tree") == std::string::npos);
    CHECK(store_header.find("append_model_change") == std::string::npos);
}

TEST_CASE("AgentSessionRuntime is constructed only by the session factory", "[architecture][session]") {
    const auto source_root = std::filesystem::path(CCH_SOURCE_DIR);
    const auto files = files_under({"src", "tests"});
    const auto factory = source_root / "src" / "coding_agent" / "runtime" / "SessionFactory.cpp";
    REQUIRE_FALSE(files.empty());

    // Build the needles dynamically so this test file does not match itself.
    const auto make_unique_needle = std::string{"std::make_unique"} + "<AgentSessionRuntime>";
    const auto new_needle = std::string{"new "} + "AgentSessionRuntime";

    std::size_t factory_constructions = 0;
    for (const auto& file : files) {
        const auto text = read_text(file);
        const auto make_unique_count = count_occurrences(text, make_unique_needle);
        const auto new_count = count_occurrences(text, new_needle);
        if (file == factory) {
            factory_constructions += make_unique_count + new_count;
        } else {
            CHECK(make_unique_count == 0);
            CHECK(new_count == 0);
        }
    }
    CHECK(factory_constructions > 0);
}


TEST_CASE("the deleted CLI preflight module and its build registrations stay gone", "[architecture][cli]") {
    const auto source_root = std::filesystem::path(CCH_SOURCE_DIR);
    // Build the needle dynamically so this test file does not match itself.
    const auto module_needle = std::string{"Cli"} + "Preflight";

    CHECK_FALSE(std::filesystem::exists(source_root / "src" / "cli" / (module_needle + ".hpp")));
    CHECK_FALSE(std::filesystem::exists(source_root / "src" / "cli" / (module_needle + ".cpp")));

    const auto cmake = read_text(source_root / "CMakeLists.txt");
    CHECK(cmake.find(module_needle) == std::string::npos);

    const auto files = files_under({"src", "tests"});
    REQUIRE_FALSE(files.empty());
    for (const auto& file : files) {
        CHECK(read_text(file).find(module_needle) == std::string::npos);
    }
}

TEST_CASE("active source tree does not retain legacy sync contracts", "[architecture][u2]") {
    const auto files = files_under({"include", "src", "tests"});
    REQUIRE_FALSE(files.empty());

    for (const auto& file : files) {
        if (file.filename() == "ArchitectureSurfaceScanTest.cpp") {
            continue;
        }
        const auto text = read_text(file);
        CHECK(text.find("util::Result") == std::string::npos);
        CHECK(text.find("boost::json") == std::string::npos);
        CHECK(text.find("src/util/Result.hpp") == std::string::npos);
        CHECK(text.find("src/tools/Tools.hpp") == std::string::npos);
        CHECK(text.find("make_read_file_tool") == std::string::npos);
        CHECK(text.find("make_bash_tool") == std::string::npos);
    }
}


