include_guard(GLOBAL)

# Orchestration include: top-level CMakeLists.txt only (relies on CMAKE_CURRENT_SOURCE_DIR = repo root).

    # support
    add_executable(cch_tests_support
        tests/Catch2Main.cpp
        tests/support/AllocationCounter.cpp
        tests/support/AsyncResultTest.cpp
        tests/support/BoundedTextTest.cpp
        tests/support/ExpectedErrorTest.cpp
        tests/support/ExpectedMacrosTest.cpp
        tests/support/JsonValueIoTest.cpp
        tests/support/PumpUntilTest.cpp
        tests/support/RedactorTest.cpp
        tests/support/TestRunnerIsolationTest.cpp
)
    target_include_directories(cch_tests_support PRIVATE ${CCH_FORMAL_TEST_INCLUDE_DIRS})
    target_link_libraries(cch_tests_support
        PRIVATE
            cch_support
            glaze::glaze
            Catch2::Catch2
)
    target_compile_definitions(cch_tests_support PRIVATE
        CCH_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)
    target_compile_options(cch_tests_support PRIVATE ${CCH_WARNING_OPTIONS})
    # Fatal-contract probes (tagged [fatal]) fork a child that must die from
    # SIGABRT. A hang means the probe broke, so they get a tightened hard
    # timeout instead of waiting out the suite-wide default (ADR 0039; #447).
    catch_discover_tests(cch_tests_support TEST_SPEC "~[fatal]" ADD_TAGS_AS_LABELS)
    catch_discover_tests(cch_tests_support TEST_SPEC "[fatal]" PROPERTIES TIMEOUT 60 ADD_TAGS_AS_LABELS)

    # The private Boost.Asio AsyncResult bridge carries the only
    # exception-shaped detail in the strict policy (ADR 0042): this
    # no-exception test target validates the bridge contract directly.
    add_executable(cch_tests_support_async_bridge
        tests/Catch2Main.cpp
        tests/support/AsyncBridgeTest.cpp
    )
    target_include_directories(cch_tests_support_async_bridge PRIVATE ${CCH_FORMAL_TEST_INCLUDE_DIRS})
    target_link_libraries(cch_tests_support_async_bridge
        PRIVATE
            cch_support
            Boost::headers
            Catch2::Catch2
    )
    target_compile_definitions(cch_tests_support_async_bridge PRIVATE
        BOOST_ASIO_NO_EXCEPTIONS
        CCH_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_compile_options(cch_tests_support_async_bridge PRIVATE
        ${CCH_WARNING_OPTIONS}
        -fno-exceptions
    )
    catch_discover_tests(cch_tests_support_async_bridge ADD_TAGS_AS_LABELS)
    add_dependencies(cch_tests_support_async_bridge ${CCH_PARITY_BUILD_GATE_TARGET})
    # Every test entry point requires fresh successful build-phase Gate evidence
    # (ADR 0039; #470): the shard builds the Gate after all production targets.
    add_dependencies(cch_tests_support ${CCH_PARITY_BUILD_GATE_TARGET})
