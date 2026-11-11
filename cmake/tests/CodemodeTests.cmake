include_guard(GLOBAL)

# Orchestration include: top-level CMakeLists.txt only (relies on CMAKE_CURRENT_SOURCE_DIR = repo root).

    # The codemode owner's public-caller shard: it links the reusable library,
    # never cch_coding_agent, so a regression that reintroduces an application
    # dependency fails here as well as in the Parity Architecture Gate.
    add_executable(cch_tests_codemode
        tests/Catch2Main.cpp
        tests/codemode/CodemodeOwnerTest.cpp
        tests/codemode/CodemodeRemoteTest.cpp
        tests/codemode/RemoteWorkerLifetimeTest.cpp
)
    target_include_directories(cch_tests_codemode PRIVATE ${CCH_FORMAL_TEST_INCLUDE_DIRS})
    target_compile_definitions(cch_tests_codemode PRIVATE
        CCH_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)
    target_compile_options(cch_tests_codemode PRIVATE ${CCH_WARNING_OPTIONS})

    # #1232 compiles the codemode owner a second time for the lifetime case with
    # its test-only worker gate enabled. This is a real static library, so the
    # test executable links exactly one codemode implementation instead of the
    # production archive plus an included translation unit. The variant compiles
    # configure-time copies of the same source list: compiling a production
    # source path twice would violate the Parity Gate's
    # RULE_DUPLICATE_SOURCE_COMPILATION, and an `#include` wrapper would instead
    # reintroduce the GCC -Wsubobject-linkage diagnostic that an included
    # Remote.cpp triggers. A copied source is its own translation unit, so the
    # production path still compiles exactly once and the variant stays free of
    # warning suppressions. The copies live under the build tree and are never
    # part of the production archive.
    set(cch_codemode_variant_dir "${CMAKE_CURRENT_BINARY_DIR}/codemode_test_variant")
    set(cch_codemode_variant_sources "")
    foreach(cch_codemode_source IN LISTS CCH_CODEMODE_SOURCES)
        set(cch_codemode_variant_source "${cch_codemode_variant_dir}/${cch_codemode_source}")
        get_filename_component(cch_codemode_variant_parent "${cch_codemode_variant_source}" DIRECTORY)
        file(MAKE_DIRECTORY "${cch_codemode_variant_parent}")
        configure_file(
            "${CMAKE_CURRENT_SOURCE_DIR}/${cch_codemode_source}"
            "${cch_codemode_variant_source}"
            COPYONLY)
        list(APPEND cch_codemode_variant_sources "${cch_codemode_variant_source}")
    endforeach()

    add_library(cch_tests_codemode_worker_variant STATIC ${cch_codemode_variant_sources})
    target_include_directories(cch_tests_codemode_worker_variant
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src/codemode/include
            ${CMAKE_CURRENT_SOURCE_DIR}/src
)
    target_link_libraries(cch_tests_codemode_worker_variant
        PUBLIC
            cch_ai
            cch_support
        PRIVATE
            Boost::headers
            Threads::Threads
            wasmedge
)
    target_compile_definitions(cch_tests_codemode_worker_variant PRIVATE
        CCH_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
        # The deterministic worker's lifetime regression (#1232) drives the
        # implementation's private parking seam instead of racing a real
        # retirement; the production archive never defines this.
        CCH_REMOTE_WORKER_TEST_HOOKS
)
    target_compile_options(cch_tests_codemode_worker_variant PRIVATE ${CCH_WARNING_OPTIONS})
    # Force-include the test-owned gate into the Remote and Deterministic
    # translation units only, so production source contains no include of a
    # tests/support header (CODING_STANDARDS.md section 11.5). Other variant
    # sources stay byte-for-byte the production compilation. Deterministic.cpp
    # needs it for the delivery-loop parks and the stop-notification phases the
    # lost-wake case observes.
    set_property(SOURCE
        "${cch_codemode_variant_dir}/src/codemode/Remote.cpp"
        "${cch_codemode_variant_dir}/src/codemode/Deterministic.cpp"
        APPEND PROPERTY COMPILE_OPTIONS
        -include "${CMAKE_CURRENT_SOURCE_DIR}/tests/support/DeterministicWorkerTestGate.hpp")

    target_link_libraries(cch_tests_codemode
        PRIVATE
            cch_tests_codemode_worker_variant
            Catch2::Catch2
)
    # The archive symbol check below executes `nm` on the production archive, so
    # building this shard must also build the production archive.
    add_dependencies(cch_tests_codemode cch_codemode)

    catch_discover_tests(cch_tests_codemode TEST_SPEC "~[issue1232]" ADD_TAGS_AS_LABELS)
    # The lifetime cases park a worker and can abort the process on the recorded
    # defect: a hard timeout is their only backstop, and the gate is process-wide,
    # so they never overlap another case. Each internal wait is 5 s and the
    # abandonment case's measured bound on the destroyed frame is 10 s, all well
    # under the 60 s timeout.
    catch_discover_tests(cch_tests_codemode TEST_SPEC "[issue1232]~[issue1232wake]"
        PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE ADD_TAGS_AS_LABELS)
    # The lost-wake discriminator's red is an unbounded join on the unpaired
    # callback: its 30 s timeout is that recorded defect's backstop, and its own
    # internal waits are 5 s each.
    catch_discover_tests(cch_tests_codemode TEST_SPEC "[issue1232wake]"
        PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE ADD_TAGS_AS_LABELS)
    add_dependencies(cch_tests_codemode ${CCH_PARITY_BUILD_GATE_TARGET})

    # Verify that the test executable links one Remote implementation and that
    # the production archive carries no test-gate symbols (#1232).
    add_test(NAME codemode_remote_symbol_ownership
        COMMAND
            ${CMAKE_COMMAND}
            -DCMAKE_NM=${CMAKE_NM}
            -DARCHIVE=$<TARGET_FILE:cch_codemode>
            -DBINARY=$<TARGET_FILE:cch_tests_codemode>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/tests/RemoteProductionSymbols.cmake
    )
    set_tests_properties(codemode_remote_symbol_ownership PROPERTIES
        LABELS "codemode;issue1232")

    # The portable no-Node entry's check shard (#1194): it links only
    # cch_codemode_portable, so a portable subset that secretly pulls a
    # native-only dependency fails to link or fails the configure-time graph
    # check in cmake/targets/Codemode.cmake.
    add_executable(cch_tests_codemode_portable
        tests/Catch2Main.cpp
        tests/codemode/CodemodePortableTest.cpp
)
    target_include_directories(cch_tests_codemode_portable PRIVATE ${CCH_FORMAL_TEST_INCLUDE_DIRS})
    target_link_libraries(cch_tests_codemode_portable
        PRIVATE
            cch_codemode_portable
            Catch2::Catch2
)
    target_compile_definitions(cch_tests_codemode_portable PRIVATE
        CCH_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)
    target_compile_options(cch_tests_codemode_portable PRIVATE ${CCH_WARNING_OPTIONS})
    catch_discover_tests(cch_tests_codemode_portable ADD_TAGS_AS_LABELS)
    add_dependencies(cch_tests_codemode_portable ${CCH_PARITY_BUILD_GATE_TARGET})
