add_executable(test_z80 test_z80.cpp)

target_include_directories(test_z80 PRIVATE
    ${EMFE_HEADER_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

target_link_libraries(test_z80 PRIVATE emfe_plugin_z80)

if(MSVC)
    set_target_properties(test_z80 PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_test(NAME z80_tests COMMAND test_z80)
