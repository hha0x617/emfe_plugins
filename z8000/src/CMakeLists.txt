add_library(emfe_plugin_z8000 SHARED
    plugin_z8000.cpp
    Z8000Cpu.cpp
    Z8000Disasm.cpp
    Z8000Memory.cpp
    Z8000Uart.cpp
    Z8000Timer.cpp
)

target_include_directories(emfe_plugin_z8000 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${EMFE_HEADER_DIR}
)

target_compile_definitions(emfe_plugin_z8000 PRIVATE EMFE_PLUGIN_EXPORTS)

if(MSVC)
    target_compile_options(emfe_plugin_z8000 PRIVATE /W4 /O2 /GL)
    target_link_options(emfe_plugin_z8000 PRIVATE /LTCG)
    set_target_properties(emfe_plugin_z8000 PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    set(MODULE_DEF "${CMAKE_CURRENT_SOURCE_DIR}/plugin_z8000.def")
    if(EXISTS "${MODULE_DEF}")
        target_sources(emfe_plugin_z8000 PRIVATE "${MODULE_DEF}")
    endif()
endif()
