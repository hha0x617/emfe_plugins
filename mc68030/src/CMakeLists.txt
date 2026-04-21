# ---------- Core engine sources ----------
set(CORE_SOURCES
    ${EM68030_ROOT}/Core/MC68030.cpp
    ${EM68030_ROOT}/Core/Memory.cpp
    ${EM68030_ROOT}/Core/Mmu.cpp
    ${EM68030_ROOT}/Core/Fpu.cpp
    ${EM68030_ROOT}/Core/Alu.cpp
    ${EM68030_ROOT}/Core/InstructionDecoder.cpp
    ${EM68030_ROOT}/Core/FpuInstructionDecoder.cpp
    ${EM68030_ROOT}/Core/AddressingModes.cpp
    ${EM68030_ROOT}/Core/Disassembler.cpp
    ${EM68030_ROOT}/Core/JitCompiler.cpp
)

# ---------- IO device sources ----------
set(IO_SOURCES
    ${EM68030_ROOT}/IO/FileLoader.cpp
    ${EM68030_ROOT}/IO/ConsoleDevice.cpp
    ${EM68030_ROOT}/IO/PccDevice.cpp
    ${EM68030_ROOT}/IO/Wd33c93Device.cpp
    ${EM68030_ROOT}/IO/Z8530Device.cpp
    ${EM68030_ROOT}/IO/Mk48t02Device.cpp
    ${EM68030_ROOT}/IO/Mvme147IoSpaceDevice.cpp
    ${EM68030_ROOT}/IO/LanceDevice.cpp
    ${EM68030_ROOT}/IO/ScsiDisk.cpp
    ${EM68030_ROOT}/IO/ScsiCdrom.cpp
    ${EM68030_ROOT}/IO/HddDevice.cpp
    ${EM68030_ROOT}/IO/FramebufferDevice.cpp
    ${EM68030_ROOT}/IO/InputDevice.cpp
    ${EM68030_ROOT}/IO/VirtualNetworkHandler.cpp
    ${EM68030_ROOT}/IO/TapNetworkHandler.cpp
    ${EM68030_ROOT}/IO/SlirpNetworkHandler.cpp
    ${EM68030_ROOT}/IO/Uart16550Device.cpp
)

# ---------- Config sources ----------
set(CONFIG_SOURCES
    ${EM68030_ROOT}/Config/EmulatorConfig.cpp
)

# ---------- Plugin wrapper sources ----------
set(PLUGIN_SOURCES
    plugin_mc68030.cpp
    pch.cpp
)

# ---------- DLL target ----------
add_library(emfe_plugin_mc68030 SHARED
    ${PLUGIN_SOURCES}
    ${CORE_SOURCES}
    ${IO_SOURCES}
    ${CONFIG_SOURCES}
    plugin_mc68030.def
)

target_include_directories(emfe_plugin_mc68030 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}            # For pch.h
    ${EM68030_ROOT}                        # For Core/, IO/, Config/
    ${EM68030_ROOT}/ThirdParty             # For nlohmann/json.hpp
    ${EMFE_HEADER_DIR}                     # For emfe_plugin.h
)

target_precompile_headers(emfe_plugin_mc68030 PRIVATE pch.h)

target_compile_definitions(emfe_plugin_mc68030 PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
)

target_compile_options(emfe_plugin_mc68030 PRIVATE
    /utf-8
    /fp:precise
    /W3
    /EHsc
    # Ship a PDB with the Release build so VS can decode C++ frames when
    # the plugin AVs under an emfe frontend. Optimization is preserved.
    $<$<CONFIG:Release>:/Zi>
)

target_link_options(emfe_plugin_mc68030 PRIVATE
    # /DEBUG emits a .pdb; /OPT:REF + /OPT:ICF preserve Release-level
    # dead-code and COMDAT-folding (the default /DEBUG flips them off).
    $<$<CONFIG:Release>:/DEBUG>
    $<$<CONFIG:Release>:/OPT:REF>
    $<$<CONFIG:Release>:/OPT:ICF>
)

target_link_libraries(emfe_plugin_mc68030 PRIVATE
    ws2_32      # For SlirpNetworkHandler
)

# Statically link the MSVC runtime so the plugin DLL is self-contained.
# Matches em8 and z8000 plugins — no msvcp140.dll/vcruntime140.dll deployment.
set_target_properties(emfe_plugin_mc68030 PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
