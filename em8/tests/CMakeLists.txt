add_executable(test_em8 test_em8.cpp)

target_include_directories(test_em8 PRIVATE
    ${EMFE_HEADER_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

target_link_libraries(test_em8 PRIVATE emfe_plugin_em8)

if(MSVC)
    set_target_properties(test_em8 PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_test(NAME em8_tests COMMAND test_em8)
