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
find_package(safetyhook CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)

add_library(raitei SHARED
    src/dllmain.cpp
    src/ess_hooks.cpp
    src/ipv4.cpp
    src/module_scan.cpp
    src/services_hook.cpp
    src/signaling.cpp
    src/tunnel.cpp
    "${CMAKE_BINARY_DIR}/version.rc"
)

target_compile_definitions(raitei PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX RAITEI_VERSION="${RAITEI_VERSION}")
target_compile_options(raitei PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /utf-8 /permissive->)

target_link_libraries(raitei PRIVATE
    LibDataChannel::LibDataChannel
    nlohmann_json::nlohmann_json
    safetyhook::safetyhook
    spdlog::spdlog
    ws2_32
)
