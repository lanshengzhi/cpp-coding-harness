cmake_minimum_required(VERSION 3.25)
project(remote_retirement_probe LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Catch2 3 CONFIG REQUIRED)
find_package(Boost CONFIG REQUIRED)
find_package(Threads REQUIRED)
include(CTest)
include(Catch)

add_executable(remote_retirement_test
    ../RemoteRetirementTest.cpp
    ../../../src/support/BoostExceptionHandler.cpp)
target_include_directories(remote_retirement_test PRIVATE ../../../src)
target_link_libraries(remote_retirement_test PRIVATE Catch2::Catch2WithMain Boost::headers Threads::Threads)
target_compile_options(remote_retirement_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
option(RETIREMENT_NO_EXCEPTIONS "Compile with exceptions disabled" ON)
if(RETIREMENT_NO_EXCEPTIONS)
    target_compile_options(remote_retirement_test PRIVATE -fno-exceptions)
endif()
catch_discover_tests(remote_retirement_test PROPERTIES TIMEOUT 10)
