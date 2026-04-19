add_executable(test_z8000 test_z8000.cpp)

target_include_directories(test_z8000 PRIVATE
    ${EMFE_HEADER_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

target_link_libraries(test_z8000 PRIVATE emfe_plugin_z8000)

if(MSVC)
    set_target_properties(test_z8000 PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_test(NAME z8000_tests COMMAND test_z8000)
