add_library(emfe_plugin_em8 SHARED
    plugin_em8.cpp
    Em8Cpu.cpp
    Em8Memory.cpp
    Em8Uart.cpp
    Em8Timer.cpp
)

target_include_directories(emfe_plugin_em8 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${EMFE_HEADER_DIR}
)

target_compile_definitions(emfe_plugin_em8 PRIVATE EMFE_PLUGIN_EXPORTS)

if(MSVC)
    target_compile_options(emfe_plugin_em8 PRIVATE /W4 /O2 /GL)
    target_link_options(emfe_plugin_em8 PRIVATE /LTCG)
    set_target_properties(emfe_plugin_em8 PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    set(MODULE_DEF "${CMAKE_CURRENT_SOURCE_DIR}/plugin_em8.def")
    if(EXISTS "${MODULE_DEF}")
        target_sources(emfe_plugin_em8 PRIVATE "${MODULE_DEF}")
    endif()
endif()
