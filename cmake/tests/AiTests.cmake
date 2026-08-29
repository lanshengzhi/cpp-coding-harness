include_guard(GLOBAL)

# Orchestration include: top-level CMakeLists.txt only (relies on CMAKE_CURRENT_SOURCE_DIR = repo root).

    # AI
    add_executable(cch_tests_ai
        tests/Catch2Main.cpp
        tests/ai/BuiltinProvidersTest.cpp
        tests/ai/GlazeRoundTripTest.cpp
        tests/ai/MessageContractTest.cpp
        tests/ai/MessageConversionTest.cpp
        tests/ai/ModelTest.cpp
        tests/ai/ModelsTest.cpp
        tests/ai/ProviderPolicyTest.cpp
        tests/ai/RetryClassifierTest.cpp
        tests/ai/SimpleOptionsTest.cpp
        tests/ai/ToolContractTest.cpp
        tests/ai/UsageTest.cpp
        tests/ai/api/AnthropicMessagesAdapterTest.cpp
        tests/ai/api/OpenAICodexResponsesAdapterTest.cpp
        tests/ai/api/OpenAIResponsesAdapterTest.cpp
        tests/ai/api/PartialJsonTest.cpp
        tests/ai/auth/KimiCodingOAuthTest.cpp
        tests/ai/auth/OpenAICodexOAuthTest.cpp
        tests/ai/providers/BoostBeastStreamTransportTest.cpp
        tests/ai/providers/BoostBeastWebSocketTransportTest.cpp
        tests/ai/providers/FakeProviderTest.cpp
        tests/ai/providers/SseParserTest.cpp
        tests/ai/providers/StreamExecutionEngineTest.cpp
)
    target_include_directories(cch_tests_ai PRIVATE ${CCH_FORMAL_TEST_INCLUDE_DIRS})
    target_link_libraries(cch_tests_ai
        PRIVATE
            cch_agent_core
            cch_ai
            Boost::headers
            Catch2::Catch2
)
    target_compile_definitions(cch_tests_ai PRIVATE
        CCH_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)
    target_compile_options(cch_tests_ai PRIVATE ${CCH_WARNING_OPTIONS})
    catch_discover_tests(cch_tests_ai ADD_TAGS_AS_LABELS)
    add_dependencies(cch_tests_ai ${CCH_PARITY_BUILD_GATE_TARGET})

    # The private Boost.Asio ModelStream bridge carries exception-shaped
    # detail in the strict policy (ADR 0042): this no-exception test target
    # validates the bridge contract directly. The AsyncResult bridge itself
    # lives in cch_support and is covered by cch_tests_support_async_bridge.
    add_executable(cch_tests_ai_async_bridge
        tests/Catch2Main.cpp
        tests/ai/ModelStreamBridgeTest.cpp
    )
    target_include_directories(cch_tests_ai_async_bridge PRIVATE ${CCH_FORMAL_TEST_INCLUDE_DIRS})
    target_link_libraries(cch_tests_ai_async_bridge
        PRIVATE
            cch_ai
            Boost::headers
            Catch2::Catch2
    )
    target_compile_definitions(cch_tests_ai_async_bridge PRIVATE
        BOOST_ASIO_NO_EXCEPTIONS
        CCH_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_compile_options(cch_tests_ai_async_bridge PRIVATE
        ${CCH_WARNING_OPTIONS}
        -fno-exceptions
    )
    catch_discover_tests(cch_tests_ai_async_bridge ADD_TAGS_AS_LABELS)
    add_dependencies(cch_tests_ai_async_bridge ${CCH_PARITY_BUILD_GATE_TARGET})
