cmake_minimum_required(VERSION 3.24)
project(reflex_framework CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose the type of build" FORCE)
endif()

# =============================================================================
# OPTIONS
# =============================================================================
option(REFLEX_NATIVE_ARCH "Build first-party targets with -march=native" ON)
option(REFLEX_ENABLE_SANITIZERS "Build first-party targets with ASan+UBSan" OFF)

# Optionally build the Disruptor benchmark (requires Boost + disruptor-cpp)
option(REFLEX_BUILD_DISRUPTOR_BENCHMARK "Build the Disruptor vs DisruptorPlus benchmark" OFF)

# =============================================================================
# DEPENDENCIES
# =============================================================================
# Some fetched dependencies declare cmake_minimum_required() older than CMake 4
# accepts; treat them as CMake 3.5 projects instead of hard-failing.
if(NOT DEFINED CMAKE_POLICY_VERSION_MINIMUM)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
endif()

include(FetchContent)

if(REFLEX_BUILD_DISRUPTOR_BENCHMARK)
    find_package(Boost 1.83 REQUIRED COMPONENTS system thread atomic chrono)

    FetchContent_Declare(
            disruptor-cpp
            GIT_REPOSITORY https://github.com/Abc-Arbitrage/Disruptor-cpp.git
            GIT_TAG        7d5b3c232a2209a3b7cb1d879b50f8aac398ac19
    )
    FetchContent_MakeAvailable(disruptor-cpp)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
        target_compile_definitions(DisruptorStatic PUBLIC DISRUPTOR_STD_YIELD)
        target_compile_definitions(DisruptorShared PUBLIC DISRUPTOR_STD_YIELD)
    endif()

    add_executable(disruptor_benchmark perf/tests/disruptor_benchmark.cpp)
    target_link_libraries(disruptor_benchmark PRIVATE DisruptorStatic ${Boost_LIBRARIES})
endif()

# disruptorplus: header-only ring buffer / sequence barrier primitives
FetchContent_Declare(
        disruptorplus
        GIT_REPOSITORY https://github.com/lewissbaker/disruptorplus.git
        GIT_TAG 748e354476e0bf86dfdbb55f1d925222d7ee631b
)
FetchContent_MakeAvailable(disruptorplus)

if(REFLEX_BUILD_DISRUPTOR_BENCHMARK)
    target_include_directories(disruptor_benchmark SYSTEM PRIVATE
            ${Boost_INCLUDE_DIRS}
            ${CMAKE_BINARY_DIR}/_deps/disruptor-cpp-src
            ${disruptorplus_SOURCE_DIR}/include
    )
endif()

# Patch disruptorplus for macOS std::min overload resolution
if(APPLE)
    set(DISRUPTORPLUS_HEADER
            "${disruptorplus_SOURCE_DIR}/include/disruptorplus/multi_threaded_claim_strategy.hpp")
    if(EXISTS "${DISRUPTORPLUS_HEADER}")
        file(READ "${DISRUPTORPLUS_HEADER}" DISRUPTORPLUS_HEADER_CONTENT)
        if(NOT DISRUPTORPLUS_HEADER_CONTENT MATCHES "std::min<sequence_t>")
            string(REPLACE
                    "reducedCount = std::min(count, static_cast<sequence_t>(diff + 1));"
                    "reducedCount = std::min<sequence_t>(static_cast<sequence_t>(count), static_cast<sequence_t>(diff + 1));"
                    DISRUPTORPLUS_HEADER_CONTENT_MODIFIED
                    "${DISRUPTORPLUS_HEADER_CONTENT}")
            if(NOT DISRUPTORPLUS_HEADER_CONTENT STREQUAL DISRUPTORPLUS_HEADER_CONTENT_MODIFIED)
                file(WRITE "${DISRUPTORPLUS_HEADER}" "${DISRUPTORPLUS_HEADER_CONTENT_MODIFIED}")
                message(STATUS "Patched disruptorplus multi_threaded_claim_strategy for macOS std::min overloads")
            endif()
        endif()
    endif()
endif()

# spdlog for logging
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.14.1
)
FetchContent_MakeAvailable(spdlog)

# yyjson for fast JSON parsing
FetchContent_Declare(
    yyjson
    GIT_REPOSITORY https://github.com/ibireme/yyjson.git
    GIT_TAG 0.8.0
)
FetchContent_MakeAvailable(yyjson)

# libwebsockets for WebSocket connections
FetchContent_Declare(
    libwebsockets
    GIT_REPOSITORY https://github.com/warmcat/libwebsockets.git
    GIT_TAG v4.3.3
)
set(LWS_WITH_SSL ON)
set(LWS_WITH_OPENSSL ON)
set(LWS_OPENSSL_SUPPORT ON)
set(LWS_WITH_STATIC ON CACHE BOOL "Build static library")
set(LWS_WITHOUT_TESTAPPS ON CACHE BOOL "Don't build test apps")
set(LWS_WITHOUT_SERVER ON CACHE BOOL "Don't build server support")
set(LWS_WITH_CLIENT ON CACHE BOOL "Build client support")
FetchContent_MakeAvailable(libwebsockets)

# GoogleTest for unit testing
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0
)
FetchContent_MakeAvailable(googletest)

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

# =============================================================================
# FIRST-PARTY FLAGS
# =============================================================================
# Placed AFTER all FetchContent_MakeAvailable / find_package calls so that only
# our own targets (defined below) pick these up — third-party dependencies keep
# their own flags.
add_compile_options(-Wall -Wextra)

if(REFLEX_NATIVE_ARCH)
    add_compile_options(-march=native)
endif()

if(REFLEX_ENABLE_SANITIZERS)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

include(CheckIPOSupported)
check_ipo_supported(RESULT REFLEX_IPO_SUPPORTED OUTPUT REFLEX_IPO_ERROR LANGUAGES CXX)
if(REFLEX_IPO_SUPPORTED)
    message(STATUS "Reflex: LTO/IPO enabled for first-party targets")
else()
    message(STATUS "Reflex: LTO/IPO not supported, continuing without it: ${REFLEX_IPO_ERROR}")
endif()

function(reflex_enable_lto target)
    if(REFLEX_IPO_SUPPORTED AND NOT REFLEX_ENABLE_SANITIZERS)
        set_target_properties(${target} PROPERTIES INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
endfunction()

# =============================================================================
# CORE LIBRARY
# =============================================================================
file(GLOB_RECURSE CORE_SOURCES CONFIGURE_DEPENDS
        "core/*.cpp"
        "components/*.cpp"
)
list(FILTER CORE_SOURCES EXCLUDE REGEX ".*test.*\\.cpp$")
list(FILTER CORE_SOURCES EXCLUDE REGEX ".*main.*\\.cpp$")

set(BACKTEST_IMPL_SOURCES
    "backtest/binary_reader.cpp"
    "backtest/backtest_engine.cpp"
    "backtest/exchange_simulator.cpp"
    "backtest/backtest_order_writer.cpp"
    "backtest/binary_splitter.cpp"
)
list(APPEND CORE_SOURCES ${BACKTEST_IMPL_SOURCES})

add_library(reflex_core ${CORE_SOURCES})

target_include_directories(reflex_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/core
)
target_include_directories(reflex_core SYSTEM PUBLIC
        ${disruptorplus_SOURCE_DIR}/include
)

target_link_libraries(reflex_core
    PUBLIC
        spdlog::spdlog
        yyjson
        websockets
        OpenSSL::SSL
        OpenSSL::Crypto
        Threads::Threads
)

reflex_enable_lto(reflex_core)

# =============================================================================
# EXECUTABLES
# =============================================================================
function(add_reflex_executable target_name source_file)
    add_executable(${target_name} ${source_file})
    target_link_libraries(${target_name} reflex_core)
    reflex_enable_lto(${target_name})
endfunction()

# Live-trading launchers (OKX websocket ingest)
add_reflex_executable(blob_only_launcher "services/launcher/blob_only_launcher.cpp")
add_reflex_executable(blob_and_offload_launcher "services/launcher/blob_and_offload_launcher.cpp")
add_reflex_executable(blob_and_writer_launcher "services/launcher/blob_and_writer_launcher.cpp")

# Example: minimal strategy wired into the backtest engine + exchange simulator
add_reflex_executable(example_backtest "examples/example_backtest_main.cpp")

# Data tooling: split multi-instrument capture files into per-instrument chunks
add_reflex_executable(splitter_main "backtest/splitter_main.cpp")

# Micro-benchmarks
add_reflex_executable(backtest_engine_bench "perf/backtest_engine_bench.cpp")
add_reflex_executable(exchange_simulator_bench "perf/exchange_simulator_bench.cpp")

# =============================================================================
# TESTS
# =============================================================================
if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
    include(CTest)
    if(BUILD_TESTING)
        file(GLOB_RECURSE TEST_SOURCES CONFIGURE_DEPENDS "tests/*.cpp")
        if(TEST_SOURCES)
            add_executable(reflex_tests ${TEST_SOURCES})
            target_link_libraries(reflex_tests
                reflex_core
                gtest_main
                gmock_main
            )
            include(GoogleTest)
            gtest_discover_tests(reflex_tests)
        endif()
    endif()
endif()
