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
    src/common/ThreadPool.cpp
    src/common/RateLimiter.cpp
    src/net/Socket.cpp
    src/net/Poller.cpp
    src/net/EventLoop.cpp
    src/net/Http.cpp
    src/net/Resolver.cpp
    src/net/TcpServer.cpp
    src/net/HttpClient.cpp
    src/ai/Sse.cpp
    src/ai/Tokenizer.cpp
    src/ai/Prompt.cpp
    src/ai/Embedding.cpp
    src/ai/Cache.cpp
    src/ai/Router.cpp
    src/ai/OpenAiClient.cpp
    src/app/Config.cpp
    src/app/Gateway.cpp
)

set(AEGIS_CORE_HDR
    src/common/Status.h
    src/common/Util.h
    src/common/MpscQueue.h
    src/common/Json.h
    src/common/Log.h
    src/common/Metrics.h
    src/common/ThreadPool.h
    src/common/RateLimiter.h
    src/net/Buffer.h
    src/net/Socket.h
    src/net/Poller.h
    src/net/EventLoop.h
    src/net/Http.h
    src/net/Resolver.h
    src/net/TcpServer.h
    src/net/HttpClient.h
    src/ai/Sse.h
    src/ai/Tokenizer.h
    src/ai/Prompt.h
    src/ai/Embedding.h
    src/ai/Cache.h
    src/ai/Router.h
    src/ai/OpenAiClient.h
    src/app/Config.h
    src/app/Gateway.h
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

# ------------------------------------------------------------------ test helpers
add_library(aegis_tools STATIC tools/MockUpstream.cpp tools/MockUpstream.h)
target_link_libraries(aegis_tools PUBLIC aegis_core)
target_include_directories(aegis_tools PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/tools>)
target_compile_options(aegis_tools PRIVATE ${AEGIS_WARN_FLAGS})

# ------------------------------------------------------------------ binary
add_executable(aegis src/app/main.cpp)
target_link_libraries(aegis PRIVATE aegis_core)
target_compile_options(aegis PRIVATE ${AEGIS_WARN_FLAGS})

add_executable(mock_upstream tools/mock_upstream_main.cpp)
target_link_libraries(mock_upstream PRIVATE aegis_tools)
target_compile_options(mock_upstream PRIVATE ${AEGIS_WARN_FLAGS})

# ------------------------------------------------------------------ tests
if(AEGIS_BUILD_TESTS)
    enable_testing()

    add_executable(aegis_tests
        tests/test_main.cpp
        tests/test_core.cpp
        tests/test_json.cpp
        tests/test_http.cpp
        tests/test_net.cpp
        tests/test_ai.cpp
        tests/test_e2e.cpp
    )
    target_link_libraries(aegis_tests PRIVATE aegis_core aegis_tools)
    target_compile_options(aegis_tests PRIVATE ${AEGIS_WARN_FLAGS})
    add_test(NAME aegis_tests COMMAND aegis_tests)
endif()

# ------------------------------------------------------------------ bench
if(AEGIS_BUILD_BENCH)
    add_executable(aegis_bench bench/bench.cpp)
    target_link_libraries(aegis_bench PRIVATE aegis_core aegis_tools)
    target_compile_options(aegis_bench PRIVATE ${AEGIS_WARN_FLAGS})
endif()

# ------------------------------------------------------------------ layout
# Keep every configuration in a single bin/ directory so the runtime DLLs that
# get copied next to the binaries always sit beside them, also for multi config
# generators (Visual Studio).
foreach(target aegis aegis_tests aegis_bench mock_upstream)
    if(TARGET ${target})
        set_target_properties(${target} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
            RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin"
            RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
            RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/bin"
            RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_BINARY_DIR}/bin")
    endif()
endforeach()

# ------------------------------------------------------------------ Windows runtime
# MinGW builds link against the GCC runtime DLLs by default. They are not on PATH
# for a normal user, so copy them next to the binaries - double clicking
# build/bin/aegis.exe then just works. Set -DAEGIS_STATIC_RUNTIME=ON to link the
# runtime statically instead (binaries get bigger, no DLLs required).
option(AEGIS_STATIC_RUNTIME "Link the GCC runtime statically on MinGW" OFF)

if(WIN32 AND MINGW)
    if(AEGIS_STATIC_RUNTIME)
        target_link_options(aegis_core INTERFACE -static-libgcc -static-libstdc++ -static)
        message(STATUS "MinGW runtime: static")
    else()
        get_filename_component(AEGIS_MINGW_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
        set(AEGIS_RUNTIME_DLLS
            "${AEGIS_MINGW_BIN}/libgcc_s_seh-1.dll"
            "${AEGIS_MINGW_BIN}/libstdc++-6.dll"
            "${AEGIS_MINGW_BIN}/libwinpthread-1.dll")

        add_custom_target(aegis_runtime ALL
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/bin"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${AEGIS_RUNTIME_DLLS}
                    "${CMAKE_BINARY_DIR}/bin"
            COMMENT "Copying MinGW runtime DLLs into bin/")

        foreach(target aegis aegis_tests aegis_bench mock_upstream)
            if(TARGET ${target})
                add_dependencies(${target} aegis_runtime)
            endif()
        endforeach()
        message(STATUS "MinGW runtime: copied into ${CMAKE_BINARY_DIR}/bin")
    endif()
endif()

if(MSVC)
    # Copy the Visual C++ runtime next to the binaries as well, so build\bin is
    # runnable on a machine without the VC++ redistributable.
    include(InstallRequiredSystemLibraries)
    if(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
        add_custom_target(msvc_runtime ALL
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/bin"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS}
                    "${CMAKE_BINARY_DIR}/bin"
            COMMENT "Copying MSVC runtime DLLs into bin/")
        foreach(target aegis aegis_tests aegis_bench mock_upstream)
            if(TARGET ${target})
                add_dependencies(${target} msvc_runtime)
            endif()
        endforeach()
        message(STATUS "MSVC runtime: copied into ${CMAKE_BINARY_DIR}/bin")
    endif()
endif()

message(STATUS "Aegis | compiler ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}"
               " | build ${CMAKE_BUILD_TYPE} | c++${CMAKE_CXX_STANDARD}")
