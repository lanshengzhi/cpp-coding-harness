cmake_minimum_required(VERSION 4.4)

set(CCH_TOOLCHAIN_ROLE "build" CACHE STRING "Supported compiler role: build or conformance")
set_property(CACHE CCH_TOOLCHAIN_ROLE PROPERTY STRINGS build conformance)

include(cmake/SupportedBuild.cmake)
cch_validate_supported_build_before_project()

project(cpp_coding_harness VERSION 0.1.0 LANGUAGES CXX)
cch_validate_supported_build_after_project()

include(cmake/ProjectSetup.cmake)       # IPO, C++23, options, warnings, sanitizer
include(cmake/Dependencies.cmake)      # pinned packages and imported-target checks
include(cmake/ParityArchitecture.cmake) # central production-target constructors
include(cmake/TargetHelpers.cmake)     # owner-local declaration helpers

include(cmake/targets/Support.cmake)
include(cmake/targets/Tui.cmake)
include(cmake/targets/Ai.cmake)
include(cmake/targets/AgentCore.cmake)
include(cmake/targets/CodingAgent.cmake)
include(cmake/targets/Runtime.cmake)    # pike Runtime executable

include(cmake/ParityGate.cmake)         # configure/build Gate and post-build attachment
include(cmake/RuntimeInstall.cmake)     # existing Runtime-only install rules
cch_runtime_collect_licenses(CCH_RUNTIME_LICENSE_FILES)
cch_runtime_install_rules(
    TARGET pike
    MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/parity/manifest.json"
    PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}"
    LICENSE_FILES ${CCH_RUNTIME_LICENSE_FILES}
    EXTERNAL_INCLUDE_ROOTS ${CCH_PARITY_EXTERNAL_INCLUDE_ROOTS}
)

if(CCH_BUILD_TESTS)
    include(cmake/tests/TestsSetup.cmake)
    include(cmake/tests/ArchitectureTests.cmake)
    include(cmake/tests/InstallTests.cmake)
    include(cmake/tests/SupportTests.cmake)
    include(cmake/tests/TuiTests.cmake)
    include(cmake/tests/AiTests.cmake)
    include(cmake/tests/AgentTests.cmake)
    include(cmake/tests/CodingAgentTests.cmake)
    include(cmake/tests/CliTests.cmake)
    include(cmake/tests/TestAggregate.cmake)
endif()
