# C++ Plugin Quickstart (CMake + MSVC)

This guide walks through creating a new **C++** emfe plugin from scratch.
The target is a Windows DLL statically linked against the MSVC runtime.

For the contract details, see [`plugin_developer_guide.md`](plugin_developer_guide.md).

Existing plugins you can copy from:
- `emfe_plugins/em8/` — smallest plugin (8-bit toy CPU, ~1400 LoC)
- `emfe_plugins/z8000/` — mid-size (Z8000 family, self-implemented)
- `emfe_plugins/mc68030/` — largest (wraps external Em68030 C++ emulator)

---

## 1. Directory scaffold

```
emfe_plugins/foo/
├── CMakeLists.txt          # top-level
├── README.md
├── docs/
│   ├── foo_reference.md
│   └── foo_reference_ja.md
├── src/
│   ├── CMakeLists.txt      # target definition
│   ├── pch.h               # MSVC pch
│   ├── pch.cpp             #   (includes pch.h)
│   ├── plugin_foo.cpp      # all emfe_* entry points
│   └── plugin_foo.def      # DLL exports
└── tests/
    ├── CMakeLists.txt
    └── test_plugin.cpp
```

## 2. Top-level `CMakeLists.txt`

```cmake
cmake_minimum_required(VERSION 3.20)
if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()

project(emfe_plugin_foo
    VERSION 0.1.0
    DESCRIPTION "FOO CPU plugin for emfe"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Path to the shared emfe header (emfe_plugin.h). It lives in the sibling
# `emfe_plugins/api/` directory.
set(EMFE_HEADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../api"
    CACHE PATH "Path to the emfe shared header directory")

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

add_subdirectory(src)

option(EMFE_PLUGIN_BUILD_TESTS "Build tests" ON)
if(EMFE_PLUGIN_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
```

## 3. `src/CMakeLists.txt`

```cmake
add_library(emfe_plugin_foo SHARED
    plugin_foo.cpp
    pch.cpp
)

target_include_directories(emfe_plugin_foo PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${EMFE_HEADER_DIR}
)

target_compile_definitions(emfe_plugin_foo PRIVATE EMFE_PLUGIN_EXPORTS)

if(MSVC)
    target_compile_options(emfe_plugin_foo PRIVATE /W4 /O2 /GL)
    target_link_options(emfe_plugin_foo PRIVATE /LTCG)
    # Statically link MSVC runtime — avoids VCREDIST deployment.
    set_target_properties(emfe_plugin_foo PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

    set(MODULE_DEF "${CMAKE_CURRENT_SOURCE_DIR}/plugin_foo.def")
    if(EXISTS "${MODULE_DEF}")
        target_sources(emfe_plugin_foo PRIVATE "${MODULE_DEF}")
    endif()
endif()
```

## 4. `src/plugin_foo.def`

The `.def` file lists every exported function. Each `emfe_*` function from
the header must appear. The shortest working list is in
`emfe_plugins/em8/src/plugin_em8.def` — copy it and rename the `LIBRARY`
line to match your DLL.

```
LIBRARY emfe_plugin_foo
EXPORTS
    emfe_negotiate
    emfe_get_board_info
    emfe_create
    emfe_destroy
    ; ... all emfe_* functions your plugin implements ...
```

## 5. `src/pch.h`

```cpp
#pragma once

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#ifdef GetObject
#undef GetObject
#endif
#ifdef GetCurrentTime
#undef GetCurrentTime
#endif

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <atomic>
#include <mutex>
#include <thread>
#include <memory>
```

## 6. `src/plugin_foo.cpp` — minimum skeleton

```cpp
#include "pch.h"
#include "emfe_plugin.h"

// ============================================================================
// Instance
// ============================================================================

struct EmfeInstanceData {
    // CPU / memory / peripherals
    uint8_t memory[65536] = {};
    uint16_t pc = 0;
    std::atomic<EmfeState> state{EMFE_STATE_STOPPED};

    // Callbacks
    EmfeConsoleCharCallback console_cb = nullptr;
    void* console_user = nullptr;

    // UI-driven storage
    std::vector<EmfeRegisterDef> reg_defs;

    void BuildRegisterDefs() {
        reg_defs.push_back({0, "PC", "CPU", EMFE_REG_INT, 16, EMFE_REG_FLAG_PC});
        // ... add more registers ...
    }
};

// ============================================================================
// Board info (static — lives for the DLL's lifetime)
// ============================================================================

static EmfeBoardInfo s_board_info = {
    "FOO",          // board_name
    "FOOCPU",       // cpu_name
    "FOO CPU plugin (16-bit toy)",   // description
    "0.1.0",        // version
};

// ============================================================================
// Discovery & lifecycle
// ============================================================================

extern "C" EmfeResult EMFE_CALL emfe_negotiate(const EmfeNegotiateInfo* info) {
    if (!info) return EMFE_ERR_INVALID;
    if (info->api_version_major != EMFE_API_VERSION_MAJOR)
        return EMFE_ERR_UNSUPPORTED;
    return EMFE_OK;
}

extern "C" EmfeResult EMFE_CALL emfe_get_board_info(EmfeBoardInfo* out) {
    if (!out) return EMFE_ERR_INVALID;
    *out = s_board_info;
    return EMFE_OK;
}

extern "C" EmfeResult EMFE_CALL emfe_create(EmfeInstance* out_instance) {
    if (!out_instance) return EMFE_ERR_INVALID;
    auto inst = new (std::nothrow) EmfeInstanceData();
    if (!inst) return EMFE_ERR_MEMORY;
    inst->BuildRegisterDefs();
    *out_instance = reinterpret_cast<EmfeInstance>(inst);
    return EMFE_OK;
}

extern "C" EmfeResult EMFE_CALL emfe_destroy(EmfeInstance instance) {
    if (!instance) return EMFE_ERR_INVALID;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    // Stop any worker thread first (omitted in this skeleton).
    delete inst;
    return EMFE_OK;
}

// ============================================================================
// Registers
// ============================================================================

extern "C" int32_t EMFE_CALL emfe_get_register_defs(
    EmfeInstance instance, const EmfeRegisterDef** out) {
    if (!instance || !out) return 0;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    *out = inst->reg_defs.data();
    return static_cast<int32_t>(inst->reg_defs.size());
}

extern "C" EmfeResult EMFE_CALL emfe_get_registers(
    EmfeInstance instance, EmfeRegValue* values, int32_t count) {
    if (!instance || !values) return EMFE_ERR_INVALID;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    for (int32_t i = 0; i < count; i++) {
        auto& v = values[i];
        switch (v.reg_id) {
        case 0: v.value.u64 = inst->pc; break;
        default: v.value.u64 = 0;
        }
    }
    return EMFE_OK;
}

// ... many more entry points — see em8 / z8000 plugins for complete list ...

// ============================================================================
// DLL entry point
// ============================================================================

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }
```

## 7. Build

```bash
cd emfe_plugins/foo
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
```

Output: `build/bin/Release/emfe_plugin_foo.dll`.

Verify it doesn't depend on VCRUNTIME140.dll etc:

```bash
dumpbin /dependents build/bin/Release/emfe_plugin_foo.dll
```

## 8. Wire into frontends

Add `<Content>` entry to
`emfe_WinUI3Cpp/emfe/emfe.vcxproj` and `<None>` entry to
`emfe_CsWPF/emfe/emfe.csproj`:

```xml
<Content Include="$(ProjectDir)..\..\emfe_plugins\foo\build\bin\Release\emfe_plugin_foo.dll"
         Condition="Exists('$(ProjectDir)..\..\emfe_plugins\foo\build\bin\Release\emfe_plugin_foo.dll')">
  <CopyToOutputDirectory>PreserveNewest</CopyToOutputDirectory>
  <DeploymentContent>true</DeploymentContent>
  <TargetPath>plugins\%(Filename)%(Extension)</TargetPath>
</Content>
```

Rebuild the frontend; the DLL will be deployed to `plugins\` next to the
exe and discovered automatically.

## 9. Test harness (optional but recommended)

See `emfe_plugins/em8/tests/test_em8.cpp` for a self-contained test binary
that loads the plugin via the C ABI and exercises all entry points.

## 10. Next steps

- Read [`plugin_developer_guide.md`](plugin_developer_guide.md) for the
  contract details (threading, string ownership, state machine, ...)
- Copy a similar plugin and adapt: em8 for simple toy CPUs, z8000 for
  self-implemented ISAs, mc68030 for wrapping an external engine.
