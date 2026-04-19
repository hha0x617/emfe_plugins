cmake_minimum_required(VERSION 3.20)
if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()

project(emfe_plugin_em8
    VERSION 1.0.0
    DESCRIPTION "EM8 custom 8-bit CPU emulator plugin for emfe"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(EMFE_HEADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../api"
    CACHE PATH "Path to the emfe shared header directory")

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

add_subdirectory(src)

option(EMFE_PLUGIN_BUILD_TESTS "Build test harness" ON)
if(EMFE_PLUGIN_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
