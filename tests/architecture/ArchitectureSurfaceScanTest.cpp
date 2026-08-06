#include "../../third_party/catch2/catch_test_macros.hpp"

#include "support/TextHelpers.hpp"

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

TEST_CASE("reusable TUI stays independent of coding-agent implementation modules", "[architecture][tui][issue45]") {
    const auto source_root = std::filesystem::path(CCH_SOURCE_DIR);
    const auto files = files_under({"include/cch/tui", "src/tui"});
    REQUIRE_FALSE(files.empty());

    const std::vector<std::string> forbidden_dependencies{
        "cch/agent",
        "cch/ai",
        "cch/coding_agent",
        "cch/harness",
        "cch/tools",
        "CLI11",
        "boost/",
        "glaze/",
    };
    for (const auto& file : files) {
        const auto text = read_text(file);
        for (const auto& forbidden : forbidden_dependencies) {
            CHECK(text.find(forbidden) == std::string::npos);
        }
    }
}

TEST_CASE(
    "coding-agent TUI configuration stays outside reusable vocabulary and pi directories",
    "[architecture][tui][issue55][issue56][issue57]") {
    const auto source_root = std::filesystem::path(CCH_SOURCE_DIR);
    const auto tui_files = files_under({"include/cch/tui", "src/tui"});
    REQUIRE_FALSE(tui_files.empty());
    for (const auto& file : tui_files) {
        const auto text = read_text(file);
        CHECK(text.find("ThemeToken") == std::string::npos);
        CHECK(text.find("thinkingXhigh") == std::string::npos);
    }

    for (const auto& file : files_under({"src/coding_agent/tui"})) {
        const auto source = read_text(file);
        CHECK(source.find("/.pi") == std::string::npos);
        CHECK(source.find("../pi") == std::string::npos);
        CHECK(source.find("getCustomThemesDir") == std::string::npos);
        CHECK(source.find("reload_themes") == std::string::npos);
        CHECK(source.find("watch_theme") == std::string::npos);
        CHECK(source.find("reload_keybindings") == std::string::npos);
    }
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

TEST_CASE(
    "stateful Agent depends only on the ModelRuntime seam, not coding-agent product concerns",
    "[architecture][agent][issue35]") {
    const auto source_root = std::filesystem::path(CCH_SOURCE_DIR);
    const auto agent_header = read_text(source_root / "include" / "cch" / "agent" / "Agent.hpp");
    const auto agent_source = read_text(source_root / "src" / "agent" / "Agent.cpp");
    const auto combined = agent_header + agent_source;

    // The Agent is constructed on the sole injectable ModelRuntime seam
    // (#326/#331). That is the only coding-agent surface it may name.
    CHECK(combined.find("cch/coding_agent/ModelRuntime.hpp") != std::string::npos);
    CHECK(combined.find("cch/coding_agent/") == combined.find("cch/coding_agent/ModelRuntime.hpp"));
    CHECK(combined.rfind("cch/coding_agent/") == combined.find("cch/coding_agent/ModelRuntime.hpp"));
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
    CHECK(runtime_source.find("agent::detail::AgentPromptAccess::prompt(") != std::string::npos);
    CHECK(runtime_header.find("AsyncAgentLoop") == std::string::npos);
    CHECK(runtime_source.find(direct_loop_call) == std::string::npos);
    CHECK(runtime_header.find(duplicate_registry) == std::string::npos);
    CHECK(runtime_header.find("agent::AgentState") == std::string::npos);
    CHECK(runtime_header.find("std::vector<ai::MessageVariant>") == std::string::npos);
    // OpenSession history is resume input only and is moved exactly once into
    // AgentInitialState; prompt execution never advances or copies it back.
    CHECK(cch::tests::count_occurrences(runtime_source, "session_.history") == 1);
}

TEST_CASE(
    "streamSimple exposes only the supported caller option set",
    "[architecture][ai][issue339]") {
    const auto source_root = std::filesystem::path(CCH_SOURCE_DIR);
    const auto request_options = read_text(
        source_root / "include" / "cch" / "ai" / "RequestOptions.hpp");
    const auto struct_begin = request_options.find("struct SimpleStreamOptions {");
    REQUIRE(struct_begin != std::string::npos);
    const auto struct_end = request_options.find("\n};", struct_begin);
    REQUIRE(struct_end != std::string::npos);
    const auto members = request_options.substr(struct_begin, struct_end - struct_begin);

    const std::vector<std::string> required_members{
        "temperature{",
        "max_tokens{",
        "stop_token{",
        "api_key{",
        "headers{",
        "env{",
        "transform_headers{",
        "reasoning{",
        "session_id{",
        "cache_retention{",
        "timeout_ms{",
        "max_retries{",
        "max_retry_delay_ms{",
    };
    for (const auto& member : required_members) {
        CHECK(members.find(member) != std::string::npos);
    }
    CHECK(cch::tests::count_occurrences(members, ";") == required_members.size());

    const std::vector<std::string> forbidden_members{
        "service_tier",
        "reasoning_summary",
        "tool_choice",
        "metadata",
        "on_payload",
        "on_response",
        "thinking_budgets",
        "transport",
        "websocket_connect_timeout_ms",
    };
    for (const auto& member : forbidden_members) {
        CHECK(members.find(member) == std::string::npos);
    }

    // The legacy StreamChatRequest aggregate surface is gone with its header
    // (ADR 0034 / #362): the option surface above is the only request shape.
    CHECK_FALSE(std::filesystem::exists(
        source_root / "include" / "cch" / "ai" / "ChatClient.hpp"));

    const std::vector<std::string> forbidden_option_types{
        "OpenAIResponsesOptions",
        "CodexResponsesOptions",
        "AnthropicOptions",
    };
    for (const auto& file : files_under({"include/cch", "src"})) {
        const auto text = read_text(file);
        for (const auto& type : forbidden_option_types) {
            CHECK(text.find(type) == std::string::npos);
        }
    }
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

    // The Deferred openai-completions surface stays absent from the build and
    // the source tree (ADR 0033: no registry placeholder, no shim).
    CHECK_FALSE(std::filesystem::exists(
        source_root / "include" / "cch" / "ai" / "providers" / "OpenAICompletionsCompat.hpp"));
    CHECK_FALSE(std::filesystem::exists(
        source_root / "include" / "cch" / "ai" / "providers" / "OpenAIChatClient.hpp"));
    CHECK_FALSE(std::filesystem::exists(
        source_root / "src" / "ai" / "providers" / "OpenAIChatClient.hpp"));
    CHECK_FALSE(std::filesystem::exists(
        source_root / "src" / "ai" / "providers" / "OpenAIChatClient.cpp"));
    CHECK_FALSE(std::filesystem::exists(
        source_root / "src" / "ai" / "providers" / "OpenAIProvider.hpp"));
    CHECK_FALSE(std::filesystem::exists(
        source_root / "src" / "ai" / "providers" / "OpenAIProvider.cpp"));
    CHECK_FALSE(std::filesystem::exists(
        source_root / "src" / "ai" / "glaze" / "ProviderDtos.hpp"));

    const auto providers_dir = source_root / "include" / "cch" / "ai" / "providers";
    const std::vector<std::string> allowed_provider_headers = {
        "StreamTransport.hpp",
        "WebSocketTransport.hpp",
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
    const auto variant_end = message_header.find(">;", variant_start);
    REQUIRE(section_start != std::string::npos);
    REQUIRE(variant_start != std::string::npos);
    REQUIRE(variant_end != std::string::npos);

    const auto runtime_type_section = message_header.substr(section_start, variant_start - section_start);
    CHECK(cch::tests::count_occurrences(runtime_type_section, "struct ") == 4);
    CHECK(runtime_type_section.find("BashExecutionMessage") != std::string::npos);
    CHECK(runtime_type_section.find("CustomMessage") != std::string::npos);
    CHECK(runtime_type_section.find("BranchSummaryMessage") != std::string::npos);
    CHECK(runtime_type_section.find("CompactionSummaryMessage") != std::string::npos);

    const auto variant_section = message_header.substr(variant_start, variant_end - variant_start);
    CHECK(cch::tests::count_occurrences(variant_section, ",") == 7);
    CHECK(variant_section.find("SystemMessage") != std::string::npos);
    CHECK(variant_section.find("UserMessage") != std::string::npos);
    CHECK(variant_section.find("AssistantMessage") != std::string::npos);
    CHECK(variant_section.find("ToolResultMessage") != std::string::npos);
    CHECK(variant_section.find("BashExecutionMessage") != std::string::npos);
    CHECK(variant_section.find("CustomMessage") != std::string::npos);
    CHECK(variant_section.find("BranchSummaryMessage") != std::string::npos);
    CHECK(variant_section.find("CompactionSummaryMessage") != std::string::npos);
}

TEST_CASE(
    "Model is complete typed and required on every provider request",
    "[architecture][ai][issue336]") {
    const auto source_root = std::filesystem::path(CCH_SOURCE_DIR);

    const auto model_header = read_text(source_root / "include" / "cch" / "ai" / "Model.hpp");
    CHECK(model_header.find("struct Model") != std::string::npos);
    CHECK(model_header.find("struct AnthropicMessagesCompat") != std::string::npos);
    CHECK(model_header.find("force_adaptive_thinking") != std::string::npos);
    CHECK(model_header.find("allow_empty_signature") != std::string::npos);
    CHECK(model_header.find("OpenAIResponsesCompat") == std::string::npos);
    CHECK(model_header.find("JsonValue") == std::string::npos);

    // Every request has one complete authoritative Model on the frozen
    // Provider seam. The legacy request aggregate that once carried a Model is
    // gone (ADR 0034 / #362) and Provider construction retains no default.
    CHECK_FALSE(std::filesystem::exists(
        source_root / "include" / "cch" / "ai" / "ChatClient.hpp"));
    CHECK_FALSE(std::filesystem::exists(
        source_root / "include" / "cch" / "ai" / "ProviderRegistry.hpp"));
    const auto provider_header = read_text(source_root / "include" / "cch" / "ai" / "Provider.hpp");
    CHECK(provider_header.find("const Model& model") != std::string::npos);
    const auto agent_context_header = read_text(source_root / "include" / "cch" / "agent" / "AgentContext.hpp");
    CHECK(agent_context_header.find("struct AgentState") != std::string::npos);
    CHECK(agent_context_header.find("ai::Model model{};") != std::string::npos);

    // The third string slot and the three-way fallback chain stay gone.
    const auto context_header = read_text(source_root / "include" / "cch" / "ai" / "Context.hpp");
    CHECK(context_header.find("model;") == std::string::npos);
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
    CHECK(cch::tests::count_occurrences(
              sdk_header,
              "boost::asio::awaitable<util::ExpectedVoid> prompt(") == 1);
    CHECK(cch::tests::count_occurrences(sdk_header, "util::ExpectedVoid prompt_blocking(") == 1);
    CHECK(runtime_source.find(
              "result = co_await agent::detail::AgentPromptAccess::prompt(") !=
          std::string::npos);
    CHECK(cch::tests::count_occurrences(runtime_source, "boost::asio::co_spawn(") == 1);
    CHECK(cch::tests::count_occurrences(sdk_header, "AgentEventSink") == 1);
    CHECK(sdk_header.find("subscribe(") != std::string::npos);
    CHECK(cch::tests::count_occurrences(rpc_source, "config.session.subscribe(") == 1);
    CHECK(rpc_source.find(prompt_sink_field) == std::string::npos);
    CHECK(sdk_header.find("preflight_result") == std::string::npos);
    CHECK(sdk_header.find("shared_ptr<ai::Models>") == std::string::npos);
    CHECK(sdk_header.find("chat_client") == std::string::npos);
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
        source_root / "src" / "cli" / "OneShotCliFrontend.cpp");

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

TEST_CASE(
    "User Bash remains a private Native TUI capability",
    "[architecture][session][tui][issue85]") {
    const auto source_root = std::filesystem::path(CCH_SOURCE_DIR);
    const auto sdk_header = read_text(
        source_root / "include" / "cch" / "coding_agent" / "Sdk.hpp");
    const auto event_header = read_text(
        source_root / "include" / "cch" / "agent" / "AgentEvent.hpp");
    const auto rpc_source = read_text(
        source_root / "src" / "coding_agent" / "runtime" / "RpcMode.cpp");
    const auto json_source = read_text(
        source_root / "src" / "coding_agent" / "runtime" / "JsonEventPrinter.cpp");
    const auto services_header = read_text(
        source_root / "src" / "coding_agent" / "runtime" /
        (std::string{"RuntimeServices"} + ".hpp"));
    const auto runtime_source = read_text(
        source_root / "src" / "coding_agent" / "runtime" / "AgentSessionRuntime.cpp");

    CHECK_FALSE(std::filesystem::exists(
        source_root / "include" / "cch" / "coding_agent" / "UserShell.hpp"));
    CHECK_FALSE(std::filesystem::exists(
        source_root / "include" / "cch" / "coding_agent" / "AsyncUserShell.hpp"));
    CHECK(sdk_header.find("run_user_bash(") == std::string::npos);
    CHECK(sdk_header.find("cancel_user_bash(") == std::string::npos);
    CHECK(rpc_source.find("run_user_bash(") == std::string::npos);
    CHECK(rpc_source.find("abort_bash") == std::string::npos);
    CHECK(json_source.find("UserBash") == std::string::npos);
    CHECK(event_header.find("UserBash") == std::string::npos);

    CHECK(services_header.find("std::unique_ptr<AsyncUserShell> user_shell") !=
          std::string::npos);
    CHECK(runtime_source.find(
              "AgentMessageAccess::append_bash_execution") != std::string::npos);
    const auto live_commit = runtime_source.find(
        "AgentMessageAccess::append_bash_execution");
    const auto durable_commit = runtime_source.find(
        "session_.store->append(", live_commit);
    REQUIRE(live_commit != std::string::npos);
    CHECK(durable_commit > live_commit);
}

TEST_CASE(
    "Native TUI User Bash promotion keeps public and wire surfaces unchanged",
    "[architecture][session][tui][issue90]") {
    const auto source_root = std::filesystem::path(CCH_SOURCE_DIR);
    const auto sdk_header = read_text(
        source_root / "include" / "cch" / "coding_agent" / "Sdk.hpp");
    const auto event_header = read_text(
        source_root / "include" / "cch" / "agent" / "AgentEvent.hpp");
    const auto rpc_mode_source = read_text(
        source_root / "src" / "coding_agent" / "runtime" / "RpcMode.cpp");
    const auto rpc_jsonl_source = read_text(
        source_root / "src" / "coding_agent" / "runtime" / "RpcJsonl.cpp");
    const auto json_source = read_text(
        source_root / "src" / "coding_agent" / "runtime" / "JsonEventPrinter.cpp");
    const auto keybinding_catalog = read_text(
        source_root / "src" / "coding_agent" / "tui" / "KeybindingCatalog.cpp") +
        read_text(
            source_root / "src" / "coding_agent" / "tui" / "KeybindingCatalog.hpp");
    const auto command_registry_source = read_text(
        source_root / "src" / "coding_agent" / "CommandRegistry.cpp");
    const auto factory_source = read_text(
        source_root / "src" / "coding_agent" / "runtime" / "SessionFactory.cpp");
    const auto cli_runtime_source = read_text(
        source_root / "src" / "coding_agent" / "runtime" / "AsyncCliRuntime.cpp");

    // No public SDK User Bash method, option, or capability descriptor.
    CHECK(sdk_header.find("UserShell") == std::string::npos);
    CHECK(sdk_header.find("user_shell") == std::string::npos);
    CHECK(sdk_header.find("UserBash") == std::string::npos);
    CHECK(sdk_header.find("user_bash") == std::string::npos);
    CHECK(sdk_header.find("userBash") == std::string::npos);

    // No RPC command and no JSON protocol addition.
    CHECK(rpc_mode_source.find("abort_bash") == std::string::npos);
    CHECK(rpc_jsonl_source.find("abort_bash") == std::string::npos);
    CHECK(rpc_mode_source.find("\"bash\"") == std::string::npos);
    CHECK(rpc_jsonl_source.find("\"bash\"") == std::string::npos);
    CHECK(json_source.find("UserBash") == std::string::npos);
    CHECK(json_source.find("user_bash") == std::string::npos);

    // No Agent lifecycle event alternative.
    CHECK(event_header.find("UserBash") == std::string::npos);
    CHECK(event_header.find("Bash") == std::string::npos);

    // No hotkey action and no slash-command registration for the prefixes.
    CHECK(keybinding_catalog.find("bash") == std::string::npos);
    CHECK(keybinding_catalog.find("Bash") == std::string::npos);
    CHECK(command_registry_source.find("register_command(\"!") ==
          std::string::npos);

    // Production assembly: only the Native TUI CLI frontend gains the
    // Session-owned LocalUserShell; --enable-bash still controls only the
    // model tool registry.
    CHECK(factory_source.find("std::make_unique<LocalUserShell>") !=
          std::string::npos);
    CHECK(factory_source.find("plan.provide_user_shell") != std::string::npos);
    CHECK(cli_runtime_source.find(
              "request.provide_user_shell = frontend == Frontend::NativeTui") !=
          std::string::npos);
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
        const auto make_unique_count = cch::tests::count_occurrences(text, make_unique_needle);
        const auto new_count = cch::tests::count_occurrences(text, new_needle);
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

TEST_CASE(
    "process capability carries one output limit and one cancellation token",
    "[architecture][util][issue40][issue75]") {
    const auto source_root = std::filesystem::path(CCH_SOURCE_DIR);
    const auto process_header = read_text(source_root / "src" / "util" / "Process.hpp");

    CHECK(process_header.find("class AsyncProcessRunner") != std::string::npos);
    CHECK(process_header.find("class DefaultAsyncProcessRunner final") != std::string::npos);
    CHECK(process_header.find("class ProcessRunner") == std::string::npos);
    CHECK(process_header.find("DefaultProcessRunner") == std::string::npos);
    CHECK(process_header.find("OutputLimit output_limit") != std::string::npos);
    CHECK(process_header.find("std::stop_token stop_token") != std::string::npos);
    CHECK(process_header.find("stop_source") == std::string::npos);
    CHECK(process_header.find("max_output_bytes") == std::string::npos);
    CHECK(process_header.find("max_output_lines") == std::string::npos);
}

TEST_CASE("workspace filesystem responsibilities stay in four cohesive units", "[architecture][harness][issue76]") {
    const auto source_root = std::filesystem::path(CCH_SOURCE_DIR);
    const auto cmake = read_text(source_root / "CMakeLists.txt");
    const std::vector<std::string> units{
        "WorkspaceFileSystemLegacy.cpp",
        "WorkspaceFileSystemPi.cpp",
        "WorkspaceFileSystemFdWalk.cpp",
        "WorkspaceFileSystemTemp.cpp",
    };

    for (const auto& unit : units) {
        CHECK(std::filesystem::exists(source_root / "src" / "harness" / unit));
        CHECK(cmake.find("src/harness/" + unit) != std::string::npos);
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

TEST_CASE(
    "legacy chat-client surface stays removed with no shims",
    "[architecture][ai][agent][issue362]") {
    const auto source_root = std::filesystem::path(CCH_SOURCE_DIR);

    // The legacy headers themselves are gone (staged deletion #362): no
    // compatibility copy, alias, or fallback read may reintroduce them.
    CHECK_FALSE(std::filesystem::exists(
        source_root / "include" / "cch" / "ai" / "ChatClient.hpp"));
    CHECK_FALSE(std::filesystem::exists(
        source_root / "src" / "ai" / "ChatClient.hpp"));
    CHECK_FALSE(std::filesystem::exists(
        source_root / "tests" / "support" / "GatedChatClient.hpp"));

    // Build the needles dynamically so this test file does not match itself.
    const std::vector<std::string> needles{
        std::string{"Streaming"} + "ChatClient",
        std::string{"Stream"} + "ChatRequest",
        std::string{"OpenAI"} + "ChatClient",
        std::string{"make_scripted_fake"} + "_stream",
        std::string{"models_from"} + "_stream",
        std::string{"Gated"} + "ChatClient",
        "edit_file",
    };
    const auto files = files_under({"include", "src", "tests"});
    REQUIRE_FALSE(files.empty());
    for (const auto& file : files) {
        if (file.filename() == "ArchitectureSurfaceScanTest.cpp") {
            continue;
        }
        const auto text = read_text(file);
        for (const auto& needle : needles) {
            CHECK(text.find(needle) == std::string::npos);
        }
    }
}

TEST_CASE(
    "all Agent Turn cap inputs default to absent",
    "[architecture][agent][coding_agent][cli][sdk][issue68][issue80]") {
    const auto source_root = std::filesystem::path(CCH_SOURCE_DIR);
    const std::vector<std::filesystem::path> turn_cap_contracts{
        source_root / "include" / "cch" / "agent" / "AgentContext.hpp",
        source_root / "include" / "cch" / "coding_agent" / "Sdk.hpp",
        source_root / "src" / "cli" / "CliConfig.hpp",
        source_root / "src" / "coding_agent" / "runtime" / "AgentSessionRuntime.hpp",
        source_root / "src" / "coding_agent" / "runtime" / "SessionFactory.hpp",
        source_root / "src" / "coding_agent" / "runtime" / "SessionFactory.cpp",
    };

    // The explicit host-set max_turns extension remains available, but every
    // layer defaults it to std::nullopt. A non-null default here would silently
    // reintroduce the local product cap rejected by ADR 0015.
    for (const auto& contract : turn_cap_contracts) {
        const auto text = read_text(contract);
        CHECK(cch::tests::count_occurrences(
                  text,
                  "std::optional<int> max_turns{std::nullopt};") == 1);
    }
}

TEST_CASE(
    "execution environment contract has no tool-shaped or not-supported surface",
    "[architecture][harness][issue69]") {
    const auto source_root = std::filesystem::path(CCH_SOURCE_DIR);
    const auto env_header = read_text(source_root / "include" / "cch" / "harness" / "ExecutionEnv.hpp");
    const auto local_header = read_text(source_root / "include" / "cch" / "harness" / "LocalExecutionEnv.hpp");
    const auto public_contract = env_header + local_header;

    // Build the needles dynamically so this test file does not match itself.
    const auto bash_query = std::string{"bash_"} + "enabled()";
    const auto legacy_shell_method = std::string{"run_"} + "shell";
    const std::vector<std::string> removed_types{
        std::string{"AsyncFile"} + "ReadResult",
        std::string{"AsyncFile"} + "WriteResult",
        std::string{"AsyncFile"} + "EditResult",
        std::string{"AsyncShell"} + "Result",
    };

    // The public contract dropped the tool-shaped methods, the bash
    // availability query, and every default NotSupported body (ADR 0006).
    CHECK(public_contract.find(bash_query) == std::string::npos);
    CHECK(public_contract.find("read_file(") == std::string::npos);
    CHECK(public_contract.find("write_file(") == std::string::npos);
    CHECK(public_contract.find("edit_file(") == std::string::npos);
    CHECK(public_contract.find(" not supported") == std::string::npos);
    CHECK(public_contract.find("co_return std::unexpected") == std::string::npos);
    for (const auto& removed : removed_types) {
        CHECK(public_contract.find(removed) == std::string::npos);
    }

    // The removed surface stays gone across the active source tree.
    const auto files = files_under({"include", "src", "tests"});
    REQUIRE_FALSE(files.empty());
    for (const auto& file : files) {
        if (file.filename() == "ArchitectureSurfaceScanTest.cpp") {
            continue;
        }
        const auto text = read_text(file);
        CHECK(text.find(bash_query) == std::string::npos);
        CHECK(text.find(legacy_shell_method) == std::string::npos);
        for (const auto& removed : removed_types) {
            CHECK(text.find(removed) == std::string::npos);
        }
    }
}



TEST_CASE(
    "ModelRuntime replaces AuthLoader and ProviderConfigResolution with no aliases",
    "[architecture][sdk][issue345]") {
    const auto source_root = std::filesystem::path(CCH_SOURCE_DIR);
    const auto sdk_header = read_text(
        source_root / "include" / "cch" / "coding_agent" / "Sdk.hpp");
    const auto factory_header = read_text(
        source_root / "src" / "coding_agent" / "runtime" / "SessionFactory.hpp");

    // Removed legacy files and types stay gone with no compatibility aliases.
    CHECK_FALSE(std::filesystem::exists(
        source_root / "include" / "cch" / "coding_agent" / "AuthLoader.hpp"));
    CHECK_FALSE(std::filesystem::exists(
        source_root / "src" / "coding_agent" / "AuthLoader.cpp"));
    CHECK_FALSE(std::filesystem::exists(
        source_root / "src" / "coding_agent" / "ProviderConfigResolution.hpp"));
    CHECK_FALSE(std::filesystem::exists(
        source_root / "src" / "coding_agent" / "ProviderConfigResolution.cpp"));
    CHECK(sdk_header.find("SdkProviderConfig") == std::string::npos);
    CHECK(sdk_header.find("provider_config") == std::string::npos);

    // The ModelRuntime seam is the sole public model/auth injection surface.
    CHECK(std::filesystem::exists(
        source_root / "include" / "cch" / "coding_agent" / "ModelRuntime.hpp"));
    CHECK(sdk_header.find("std::shared_ptr<ModelRuntime> model_runtime") !=
          std::string::npos);
    CHECK(sdk_header.find("model_runtime()") != std::string::npos);
    CHECK(sdk_header.find("agent_dir") != std::string::npos);
    CHECK(sdk_header.find("shared_ptr<ai::Models>") == std::string::npos);

    // The private Models injection seam stays limited to the test-support
    // factory wrapper; it never appears in the public SDK surface.
    CHECK(factory_header.find("std::shared_ptr<ai::Models> models") !=
          std::string::npos);
}

TEST_CASE(
    "Input.hpp is the pi-canonical single-line component and the event surface stays in Keys.hpp",
    "[architecture][tui][issue380]") {
    const auto source_root = std::filesystem::path(CCH_SOURCE_DIR);

    // The single-line Input component claims the header name pi reserves for
    // it (components/input.ts); the component and its tests exist.
    CHECK(std::filesystem::exists(source_root / "include" / "cch" / "tui" / "Input.hpp"));
    CHECK(std::filesystem::exists(source_root / "src" / "tui" / "Input.cpp"));
    CHECK(std::filesystem::exists(source_root / "tests" / "tui" / "InputTest.cpp"));
    const auto input_header = read_text(
        source_root / "include" / "cch" / "tui" / "Input.hpp");
    CHECK(input_header.find("class Input") != std::string::npos);
    CHECK(input_header.find("InputSubmitSink") != std::string::npos);
    CHECK(input_header.find("InputEscapeSink") != std::string::npos);
    CHECK(input_header.find("handle_input") != std::string::npos);
    CHECK(input_header.find("cursor_location") != std::string::npos);

    // The key/paste event surface is not duplicated in the component header.
    CHECK(input_header.find("parse_key_id") == std::string::npos);
    CHECK(input_header.find("key_id(") == std::string::npos);
    CHECK(input_header.find("matches_key") == std::string::npos);
    CHECK(input_header.find("struct PasteEvent") == std::string::npos);
    CHECK(input_header.find("kMaxPasteBytes") == std::string::npos);

    // The event surface stays in the pi keys.ts header.
    const auto keys_header = read_text(
        source_root / "include" / "cch" / "tui" / "Keys.hpp");
    CHECK(keys_header.find("parse_key_id") != std::string::npos);
    CHECK(keys_header.find("key_id(") != std::string::npos);
    CHECK(keys_header.find("matches_key") != std::string::npos);
    CHECK(keys_header.find("PasteEvent") != std::string::npos);
    CHECK(keys_header.find("kMaxPasteBytes") != std::string::npos);
}

TEST_CASE(
    "width-utils subset is public through Utils.hpp and the detail header stays private",
    "[architecture][tui][issue377]") {
    const auto source_root = std::filesystem::path(CCH_SOURCE_DIR);
    const auto utils_header = read_text(
        source_root / "include" / "cch" / "tui" / "Utils.hpp");

    for (const auto& symbol : {
             "visible_width",
             "wrap_text",
             "truncate_text",
             "slice_by_column",
             "strip_terminal_sequences",
         }) {
        CHECK(utils_header.find(symbol) != std::string::npos);
    }

    // The private detail header is consumed only inside the TUI module and by
    // its dedicated detail test; general tests consume the public header.
    for (const auto& file : files_under({"include", "tests"})) {
        if (file == source_root / "tests" / "tui" / "UnicodeWidthTest.cpp") {
            continue;
        }
        if (file == source_root / "tests" / "architecture" / "ArchitectureSurfaceScanTest.cpp") {
            continue;
        }
        const auto text = read_text(file);
        CHECK(text.find("tui/UnicodeWidth.hpp") == std::string::npos);
    }

    // No stale detail-namespace references to the promoted subset remain.
    for (const auto& file : files_under({"include", "src", "tests"})) {
        if (file == source_root / "tests" / "architecture" / "ArchitectureSurfaceScanTest.cpp") {
            continue;
        }
        const auto text = read_text(file);
        CHECK(text.find("detail::visible_width") == std::string::npos);
        CHECK(text.find("detail::wrap_text") == std::string::npos);
        CHECK(text.find("detail::truncate_text") == std::string::npos);
    }
}

TEST_CASE(
    "Fuzzy is a public module with scoring, matched indices, and a ranking filter",
    "[architecture][tui][issue377]") {
    const auto source_root = std::filesystem::path(CCH_SOURCE_DIR);
    const auto fuzzy_header = read_text(
        source_root / "include" / "cch" / "tui" / "Fuzzy.hpp");

    CHECK(fuzzy_header.find("fuzzy_match(") != std::string::npos);
    CHECK(fuzzy_header.find("fuzzy_match_indices") != std::string::npos);
    CHECK(fuzzy_header.find("fuzzy_filter") != std::string::npos);

    // SettingsList consumes the public module; the private helpers are gone.
    const auto settings_list = read_text(
        source_root / "src" / "tui" / "SettingsList.cpp");
    CHECK(settings_list.find("cch/tui/Fuzzy.hpp") != std::string::npos);
    CHECK(settings_list.find("fuzzy_filter(") != std::string::npos);
    CHECK(settings_list.find("fuzzy_tokens") == std::string::npos);
    CHECK(settings_list.find("casefold_text") == std::string::npos);
    CHECK(settings_list.find("match_fuzzy_query") == std::string::npos);
}

TEST_CASE(
    "Autocomplete is the public async provider module and the sync editor surface is gone",
    "[architecture][tui][issue383]") {
    const auto source_root = std::filesystem::path(CCH_SOURCE_DIR);
    const auto header = read_text(
        source_root / "include" / "cch" / "tui" / "Autocomplete.hpp");

    // The pi autocomplete.ts surface: values, SlashCommand, the async
    // provider contract, and the combined provider.
    for (const auto& symbol : {
             "struct AutocompleteItem",
             "struct AutocompleteRequest",
             "struct AutocompleteSuggestions",
             "struct SlashCommand",
             "class AutocompleteProvider",
             "class CombinedAutocompleteProvider",
             "get_suggestions",
             "apply_completion",
             "should_trigger_file_completion",
             "AutocompleteResultSink",
             "AutocompleteDebounceTimer",
         }) {
        CHECK(header.find(symbol) != std::string::npos);
    }

    // The old synchronous surface is gone from the editor header: no provider
    // alias, no autocomplete value definitions, no refresh seam.
    const auto editor_header = read_text(
        source_root / "include" / "cch" / "tui" / "Editor.hpp");
    CHECK(editor_header.find("cch/tui/Autocomplete.hpp") != std::string::npos);
    CHECK(editor_header.find("using AutocompleteProvider =") == std::string::npos);
    CHECK(editor_header.find("struct AutocompleteItem") == std::string::npos);
    CHECK(editor_header.find("struct AutocompleteRequest") == std::string::npos);
    CHECK(editor_header.find("struct AutocompleteSuggestions") == std::string::npos);

    const auto editor_source = read_text(
        source_root / "src" / "tui" / "Editor.cpp");
    CHECK(editor_source.find("refresh_autocomplete") == std::string::npos);
    CHECK(editor_source.find("close_autocomplete") == std::string::npos);

    // The sync harness provider is gone; the app assembles the combined
    // provider at startup like pi's interactive mode.
    for (const auto& file : files_under({"include", "src", "tests"})) {
        if (file == source_root / "tests" / "architecture" / "ArchitectureSurfaceScanTest.cpp") {
            continue;
        }
        const auto text = read_text(file);
        CHECK(text.find("command_autocomplete_provider") == std::string::npos);
    }
    const auto interactive_mode = read_text(
        source_root / "src" / "coding_agent" / "tui" / "InteractiveMode.cpp");
    CHECK(interactive_mode.find("CombinedAutocompleteProvider") != std::string::npos);
    CHECK(interactive_mode.find("AsioAutocompleteDebounceTimer") != std::string::npos);
}

TEST_CASE(
    "terminal-image is the public pi-aligned image module with the private encoder gone",
    "[architecture][tui][issue385]") {
    const auto source_root = std::filesystem::path(CCH_SOURCE_DIR);
    const auto image_header = read_text(
        source_root / "include" / "cch" / "tui" / "TerminalImage.hpp");

    // The pi terminal-image.ts public surface: hyperlink, imageFallback, and
    // the env-rule capability detection with cache overrides.
    for (const auto& symbol : {
             "hyperlink(",
             "image_fallback(",
             "detect_image_capabilities",
             "get_image_capabilities",
             "set_image_capabilities",
             "reset_image_capabilities_cache",
             "TmuxHyperlinkProbe",
             "DetectedImageCapabilities",
             "ImagePixelSize",
         }) {
        CHECK(image_header.find(symbol) != std::string::npos);
    }
    // The encoder stays behind the sidecar seam in the detail namespace of the
    // same public module (tests and the terminal implementations consume it).
    CHECK(image_header.find("encode_terminal_image") != std::string::npos);
    CHECK(image_header.find("namespace detail") != std::string::npos);

    // The private src/tui/TerminalImage.hpp duplicate is gone: no file exists
    // under src/tui and nothing outside the module includes it by that path.
    CHECK_FALSE(std::filesystem::exists(
        source_root / "src" / "tui" / "TerminalImage.hpp"));
    for (const auto& file : files_under({"include", "src", "tests"})) {
        if (file == source_root / "tests" / "architecture" / "ArchitectureSurfaceScanTest.cpp") {
            continue;
        }
        const auto text = read_text(file);
        CHECK(text.find("\"tui/TerminalImage.hpp\"") == std::string::npos);
    }
}
