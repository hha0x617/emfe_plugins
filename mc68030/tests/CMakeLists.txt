add_executable(test_plugin test_plugin.cpp)

target_include_directories(test_plugin PRIVATE
    ${EMFE_HEADER_DIR}
)

target_compile_options(test_plugin PRIVATE
    /utf-8
    /EHsc
)

set_target_properties(test_plugin PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Ensure the plugin DLL is available next to test_plugin.exe
add_dependencies(test_plugin emfe_plugin_mc68030)

add_test(
    NAME plugin_smoke_test
    COMMAND test_plugin
    WORKING_DIRECTORY $<TARGET_FILE_DIR:test_plugin>
)
