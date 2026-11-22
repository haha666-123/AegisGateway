cmake_minimum_required(VERSION 3.21)

project(Aegis
    VERSION 1.0.0
    DESCRIPTION "Aegis - a Linux-first, cross-platform LLM gateway in modern C++"
    LANGUAGES CXX)

# ------------------------------------------------------------------ language
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(AEGIS_BUILD_TESTS "Build unit tests" ON)
option(AEGIS_BUILD_BENCH "Build benchmarks" ON)
option(AEGIS_SANITIZE "Enable ASan/UBSan (gcc/clang only)" OFF)
option(AEGIS_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)

find_package(Threads REQUIRED)

# ------------------------------------------------------------------ warnings
set(AEGIS_WARN_FLAGS "")
if(MSVC)
    list(APPEND AEGIS_WARN_FLAGS /W4 /permissive- /utf-8 /Zc:__cplusplus)
else()
    list(APPEND AEGIS_WARN_FLAGS -Wall -Wextra -Wpedantic -Wshadow
                                 -Wnon-virtual-dtor -Wold-style-cast)
endif()
if(AEGIS_WARNINGS_AS_ERRORS)
    if(MSVC)
        list(APPEND AEGIS_WARN_FLAGS /WX)
    else()
        list(APPEND AEGIS_WARN_FLAGS -Werror)
    endif()
endif()

# ------------------------------------------------------------------ sources
set(AEGIS_CORE_SRC
    src/common/Util.cpp
    src/common/Json.cpp
    src/common/Log.cpp
    src/common/Metrics.cpp
    src/net/Socket.cpp
    src/net/Poller.cpp
    src/net/EventLoop.cpp
    src/net/Http.cpp
    src/common/ThreadPool.cpp
    src/net/Resolver.cpp
    src/net/TcpServer.cpp
)

set(AEGIS_CORE_HDR
    src/common/Status.h
    src/common/Util.h
    src/common/MpscQueue.h
    src/common/Json.h
    src/common/Log.h
    src/common/Metrics.h
    src/net/Buffer.h
    src/net/Socket.h
    src/net/Poller.h
    src/net/EventLoop.h
    src/net/Http.h
    src/common/ThreadPool.h
    src/net/Resolver.h
    src/net/TcpServer.h
)

add_library(aegis_core STATIC ${AEGIS_CORE_SRC} ${AEGIS_CORE_HDR})

target_include_directories(aegis_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>)

target_link_libraries(aegis_core PUBLIC Threads::Threads)

target_compile_options(aegis_core PRIVATE ${AEGIS_WARN_FLAGS})

if(WIN32)
    target_compile_definitions(aegis_core PUBLIC
        _WIN32_WINNT=0x0602
        WIN32_LEAN_AND_MEAN
        NOMINMAX)
    target_link_libraries(aegis_core PUBLIC ws2_32)
endif()

if(UNIX AND NOT APPLE)
    target_compile_definitions(aegis_core PUBLIC AEGIS_PLATFORM_LINUX=1)
elseif(APPLE)
    target_compile_definitions(aegis_core PUBLIC AEGIS_PLATFORM_MACOS=1)
elseif(WIN32)
    target_compile_definitions(aegis_core PUBLIC AEGIS_PLATFORM_WINDOWS=1)
endif()

if(AEGIS_SANITIZE AND NOT MSVC)
    target_compile_options(aegis_core PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(aegis_core PUBLIC -fsanitize=address,undefined)
endif()

# ------------------------------------------------------------------ binary
add_executable(aegis src/app/main.cpp)
target_link_libraries(aegis PRIVATE aegis_core)
target_compile_options(aegis PRIVATE ${AEGIS_WARN_FLAGS})

# ------------------------------------------------------------------ tests
if(AEGIS_BUILD_TESTS)
    enable_testing()

    add_executable(aegis_tests
        tests/test_main.cpp
        tests/test_core.cpp
        tests/test_json.cpp
        tests/test_http.cpp
        tests/test_net.cpp
    )
    target_link_libraries(aegis_tests PRIVATE aegis_core)
    target_compile_options(aegis_tests PRIVATE ${AEGIS_WARN_FLAGS})
    add_test(NAME aegis_tests COMMAND aegis_tests)
endif()

# ------------------------------------------------------------------ bench
if(AEGIS_BUILD_BENCH)
    add_executable(aegis_bench bench/bench.cpp)
    target_link_libraries(aegis_bench PRIVATE aegis_core)
    target_compile_options(aegis_bench PRIVATE ${AEGIS_WARN_FLAGS})
endif()

# ------------------------------------------------------------------ layout
set_target_properties(aegis aegis_tests aegis_bench PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

message(STATUS "Aegis | compiler ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}"
               " | build ${CMAKE_BUILD_TYPE} | c++${CMAKE_CXX_STANDARD}")
