add_library(emfe_plugin_z80 SHARED
    plugin_z80.cpp
    Z80Cpu.cpp
    Z80Memory.cpp
    Z80Uart.cpp
    Z80Timer.cpp
)

target_include_directories(emfe_plugin_z80 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${EMFE_HEADER_DIR}
)

target_compile_definitions(emfe_plugin_z80 PRIVATE EMFE_PLUGIN_EXPORTS)

if(MSVC)
    target_compile_options(emfe_plugin_z80 PRIVATE /W4 /O2 /GL)
    target_link_options(emfe_plugin_z80 PRIVATE /LTCG)
    set_target_properties(emfe_plugin_z80 PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    set(MODULE_DEF "${CMAKE_CURRENT_SOURCE_DIR}/plugin_z80.def")
    if(EXISTS "${MODULE_DEF}")
        target_sources(emfe_plugin_z80 PRIVATE "${MODULE_DEF}")
    endif()
endif()
