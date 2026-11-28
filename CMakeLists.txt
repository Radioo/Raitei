cmake_minimum_required(VERSION 3.25)

project(raitei LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(NOT "$ENV{RAITEI_VERSION}" STREQUAL "")
    set(RAITEI_VERSION "$ENV{RAITEI_VERSION}")
else()
    find_package(Git QUIET)
    if(GIT_FOUND)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" describe --tags --always --dirty
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            OUTPUT_VARIABLE RAITEI_VERSION
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
    endif()
    if("${RAITEI_VERSION}" STREQUAL "")
        set(RAITEI_VERSION "unknown")
    endif()
endif()

set(RAITEI_VERSION_MAJOR 0)
set(RAITEI_VERSION_MINOR 0)
set(RAITEI_VERSION_PATCH 0)
if(RAITEI_VERSION MATCHES "([0-9]+)\\.([0-9]+)\\.([0-9]+)")
    set(RAITEI_VERSION_MAJOR ${CMAKE_MATCH_1})
    set(RAITEI_VERSION_MINOR ${CMAKE_MATCH_2})
    set(RAITEI_VERSION_PATCH ${CMAKE_MATCH_3})
endif()
message(STATUS "Raitei version ${RAITEI_VERSION}")
configure_file(src/version.rc.in "${CMAKE_BINARY_DIR}/version.rc" @ONLY)

find_package(LibDataChannel CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_path(DETOURS_INCLUDE_DIR detours/detours.h REQUIRED)
find_library(DETOURS_LIBRARY detours REQUIRED)
find_package(safetyhook CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(Catch2 3 CONFIG REQUIRED)

add_library(raitei_core STATIC
    src/core/address_queries.cpp
    src/core/import_table.cpp
    src/core/ipv4.cpp
    src/core/module_scan.cpp
    src/core/services_hook.cpp
    src/core/signaling.cpp
    src/core/tunnel.cpp
    src/games/ccj/ccj_hooks.cpp
    src/games/ccj/unet_bridge.cpp
    src/games/games.cpp
    src/games/iidx/iidx_hooks.cpp
    src/games/iidx/virtual_network.cpp
    src/games/sdvx/ess_hooks.cpp
    src/games/sdvx/frames.cpp
)
target_include_directories(raitei_core PUBLIC src ${DETOURS_INCLUDE_DIR})
target_compile_definitions(raitei_core PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(raitei_core PUBLIC $<$<COMPILE_LANGUAGE:CXX>:/W4 /utf-8 /permissive->)
target_link_libraries(raitei_core PUBLIC
    LibDataChannel::LibDataChannel
    nlohmann_json::nlohmann_json
    ${DETOURS_LIBRARY}
    safetyhook::safetyhook
    spdlog::spdlog
    ws2_32
)

add_library(raitei SHARED
    src/dllmain.cpp
    "${CMAKE_BINARY_DIR}/version.rc"
)
target_compile_definitions(raitei PRIVATE RAITEI_VERSION="${RAITEI_VERSION}")
target_link_libraries(raitei PRIVATE raitei_core)

enable_testing()
add_executable(raitei_tests
    tests/address_queries_test.cpp
    tests/frames_test.cpp
    tests/ipv4_test.cpp
    tests/signaling_test.cpp
    tests/signaling_tls_test.cpp
    tests/tunnel_test.cpp
    tests/unet_bridge_test.cpp
    tests/virtual_network_test.cpp
    tests/loopback_signaling.cpp
)
target_link_libraries(raitei_tests PRIVATE raitei_core Catch2::Catch2WithMain OpenSSL::Crypto)
include(Catch)
catch_discover_tests(raitei_tests DISCOVERY_MODE PRE_TEST)
