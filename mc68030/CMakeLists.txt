cmake_minimum_required(VERSION 3.20)

# Required for MSVC_RUNTIME_LIBRARY to work (CMP0091)
if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()

project(emfe_plugin_mc68030
    VERSION 1.0.0
    DESCRIPTION "MC68030 emulator plugin for emfe"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Em68030 source tree root (submodule at external/em68030_WinUI3Cpp)
# Override with -DEM68030_ROOT=... if using a different layout.
set(EM68030_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../external/em68030_WinUI3Cpp/Em68030"
    CACHE PATH "Path to the Em68030 source tree")

if(NOT EXISTS "${EM68030_ROOT}/Core/MC68030.cpp")
    message(FATAL_ERROR "Em68030 source tree not found at ${EM68030_ROOT}")
endif()

# emfe shared header location
set(EMFE_HEADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../api"
    CACHE PATH "Path to the emfe shared header directory")

# Common output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

add_subdirectory(src)

option(EMFE_PLUGIN_BUILD_TESTS "Build test harness" ON)
if(EMFE_PLUGIN_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
